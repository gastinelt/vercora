#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""
make_ortho_texture.py — assemble les tuiles BD ORTHO en une texture .vcto prete
a etre envoyee au GPU : mosaique, chaine de mipmaps, compression BC1.

PHASE 11. Pendant du couple fetch_dalles.py / assemble_terrain.py de la Phase 9 :
le telechargement et la MISE EN FORME sont deux outils separes, parce qu'on
re-encode bien plus souvent qu'on ne retelecharge.

    python tools/make_ortho_texture.py assets/terrain/vercora_zone_1m.vchm \
        -i data/ortho -o assets/terrain/vercora_zone_ortho.vcto
    python tools/make_ortho_texture.py ... --size 4096 --format rgba

POURQUOI TOUT EST PRECALCULE ICI, MIPMAPS COMPRISES. Le chemin habituel cote
moteur (generer les mipmaps sur le GPU par vkCmdBlitImage) est INAPPLICABLE a une
texture compressee : on ne peut pas filtrer des blocs BC1. Il faudrait donc soit
renoncer a la compression, soit reduire puis compresser chaque niveau — c'est
exactement ce que fait ce script. Le moteur, lui, se contente de recopier des
octets deja prets, ce qui rend son code de chargement plus simple, pas moins.

POURQUOI BC1. La zone fait 5 x 5 km. A 8192 px de cote (0,61 m/texel) :
  - en RGBA8 non compresse : 268 Mo, plus 89 Mo de mipmaps ;
  - en BC1 (6:1, blocs de 4x4 sur 8 octets)  :  33 Mo, plus 11 Mo de mipmaps.
Sur le GPU integre de cette machine, dont la memoire est prise sur la RAM
systeme et dont la Phase 10 avait ramene l'empreinte totale a 246 Mo, la
difference n'est pas cosmetique. BC1 est disponible sur tout GPU de bureau
(feature Vulkan textureCompressionBC) ; le moteur le VERIFIE et refuse de charger
une texture BC1 sur un GPU qui ne la gere pas, plutot que d'afficher n'importe
quoi. --format rgba reste disponible comme repli.

QUALITE DE L'ENCODAGE. BC1 est destructif. Le script mesure et AFFICHE le PSNR
du niveau 0 apres aller-retour : sur de la photo aerienne (contenu a haute
frequence, sans grands aplats degrades) on attend environ 35 dB, ce qui est
visuellement indiscernable a la distance ou le terrain est regarde. Un chiffre
nettement plus bas serait le signe d'un defaut d'encodage, pas d'une limite du
format — d'ou son affichage systematique plutot qu'une confiance aveugle.

FORMAT .vcto ("Vercora TexturE Ortho"), petit-boutiste, en-tete de 128 octets :

    0    "VCTO"                 magie
    4    uint32  version = 1
    8    uint32  width          largeur du niveau 0
    12   uint32  height
    16   uint32  mipLevels      niveaux stockes, du plus grand au plus petit
    20   uint32  format         0 = RGBA8 brut, 1 = BC1 (RGB, blocs de 8 octets)
    24   float64 eastMin        \
    32   float64 northMin        | emprise Lambert 93 COUVERTE par l'image,
    40   float64 eastMax         | bord a bord (et non centres de texels)
    48   float64 northMax       /
    56   uint32  epsg           2154
    60   uint32  flags          reserve
    64   char[64] label
    128  donnees des mipmaps, consecutives, sans remplissage

L'emprise est recopiee dans l'en-tete pour que le moteur puisse VERIFIER qu'elle
correspond a celle de la heightmap chargee, au lieu de supposer que l'utilisateur
a associe les bons fichiers. Une texture d'une autre zone est refusee au
chargement avec un message explicite.

Dependances : Pillow, numpy.
"""

from __future__ import annotations

import argparse
import glob
import os
import re
import struct
import sys
import time

try:
    import numpy as np
    from PIL import Image
except ImportError:
    sys.exit("Pillow et numpy sont requis : python -m pip install Pillow numpy")

Image.MAX_IMAGE_PIXELS = None

FORMAT_RGBA8 = 0
FORMAT_BC1 = 1
HEADER_SIZE = 128
TILE_RE = re.compile(r"ortho_(\d+)_(\d+)_(\d+)px\.(jpg|png)$", re.IGNORECASE)

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from fetch_ortho import Extent  # noqa: E402  (meme definition d'emprise, pas de copie)


# ---------------------------------------------------------------------------
# Compression BC1
# ---------------------------------------------------------------------------
def _pack565(rgb: np.ndarray) -> np.ndarray:
    """(N,3) uint8 -> (N,) uint16 au format RGB565."""
    r = (rgb[:, 0].astype(np.uint16) >> 3) << 11
    g = (rgb[:, 1].astype(np.uint16) >> 2) << 5
    b = (rgb[:, 2].astype(np.uint16) >> 3)
    return (r | g | b).astype(np.uint16)


def _unpack565(packed: np.ndarray) -> np.ndarray:
    """(N,) uint16 -> (N,3) uint8 : la couleur REELLEMENT vue par le GPU.

    La replication des bits de poids fort dans les bits perdus (r5<<3 | r5>>2)
    n'est pas une coquetterie : c'est ce que fait le materiel, et choisir les
    indices contre une autre valeur que celle-la ferait diverger l'encodeur du
    decodeur sur les couleurs extremes.
    """
    p = packed.astype(np.uint32)
    r5 = (p >> 11) & 0x1F
    g6 = (p >> 5) & 0x3F
    b5 = p & 0x1F
    r = (r5 << 3) | (r5 >> 2)
    g = (g6 << 2) | (g6 >> 4)
    b = (b5 << 3) | (b5 >> 2)
    return np.stack([r, g, b], axis=1).astype(np.uint8)


def encode_bc1(image: np.ndarray, chunk_blocks: int = 1 << 18) -> bytes:
    """image (H,W,3) uint8, H et W multiples de 4 -> octets BC1.

    Methode dite de la BOITE ENGLOBANTE avec retrait (le chemin rapide de
    stb_dxt) : les deux extremites de la palette sont les coins min/max du pave
    des couleurs du bloc, rentres d'un seizieme de leur ecart. Le retrait compte
    — sans lui les deux couleurs interpolees de la palette sont systematiquement
    trop rapprochees des extremes et les blocs bruites se delavent.

    Le traitement est decoupe en paquets de blocs : la matrice des distances
    (blocs x 16 pixels x 4 couleurs) pese sinon plus d'un gigaoctet sur une
    texture de 8192 px.
    """
    height, width, _ = image.shape
    assert height % 4 == 0 and width % 4 == 0

    # (H,W,3) -> (nblocks, 16, 3), les 16 pixels d'un bloc dans l'ordre BC1
    # (ligne par ligne, de gauche a droite).
    blocks = (image.reshape(height // 4, 4, width // 4, 4, 3)
                   .transpose(0, 2, 1, 3, 4)
                   .reshape(-1, 16, 3))
    total = blocks.shape[0]
    out = np.empty(total * 8, dtype=np.uint8)

    for start in range(0, total, chunk_blocks):
        chunk = blocks[start:start + chunk_blocks].astype(np.int16)
        n = chunk.shape[0]

        lo = chunk.min(axis=1)
        hi = chunk.max(axis=1)
        inset = (hi - lo) >> 4
        lo = np.clip(lo + inset, 0, 255).astype(np.uint8)
        hi = np.clip(hi - inset, 0, 255).astype(np.uint8)

        packed_hi = _pack565(hi)
        packed_lo = _pack565(lo)

        # BC1 n'entre en mode 4 couleurs que si c0 > c1 (strictement). On force
        # donc l'ordre ; l'egalite designe un bloc uni, dont tous les indices
        # tomberont naturellement sur 0.
        c0 = np.maximum(packed_hi, packed_lo)
        c1 = np.minimum(packed_hi, packed_lo)

        e0 = _unpack565(c0).astype(np.int16)
        e1 = _unpack565(c1).astype(np.int16)

        palette = np.empty((n, 4, 3), dtype=np.int16)
        palette[:, 0] = e0
        palette[:, 1] = e1
        palette[:, 2] = (2 * e0 + e1) // 3
        palette[:, 3] = (e0 + 2 * e1) // 3

        # (n,16,1,3) - (n,1,4,3) -> (n,16,4)
        # int32 OBLIGATOIRE : une distance au carre monte a 3 x 255^2 = 195 075,
        # tres au-dela des 32 767 d'un int16. En int16 l'accumulation deborde en
        # silence, l'argmin choisit une couleur au hasard, et le PSNR tombe de
        # ~36 a ~22 dB sans qu'aucune erreur ne soit levee.
        diff = (chunk[:, :, None, :].astype(np.int32)
                - palette[:, None, :, :].astype(np.int32))
        distance = np.einsum("nikc,nikc->nik", diff, diff)
        indices = distance.argmin(axis=2).astype(np.uint32)  # (n,16)

        # Le pixel 0 occupe les bits de poids faible du mot d'indices.
        shifts = (2 * np.arange(16, dtype=np.uint32))[None, :]
        packed_indices = np.bitwise_or.reduce(indices << shifts, axis=1)

        block_bytes = np.empty((n, 8), dtype=np.uint8)
        block_bytes[:, 0] = c0 & 0xFF
        block_bytes[:, 1] = c0 >> 8
        block_bytes[:, 2] = c1 & 0xFF
        block_bytes[:, 3] = c1 >> 8
        block_bytes[:, 4] = packed_indices & 0xFF
        block_bytes[:, 5] = (packed_indices >> 8) & 0xFF
        block_bytes[:, 6] = (packed_indices >> 16) & 0xFF
        block_bytes[:, 7] = (packed_indices >> 24) & 0xFF

        out[start * 8:(start + n) * 8] = block_bytes.reshape(-1)

    return out.tobytes()


def decode_bc1(data: bytes, width: int, height: int) -> np.ndarray:
    """Decodeur de controle, utilise uniquement pour mesurer le PSNR.

    Ecrit INDEPENDAMMENT de l'encodeur (il repart des octets produits, comme le
    ferait le GPU) : c'est ce qui lui permet d'attraper une erreur d'ordre des
    bits ou d'endianness, qu'un simple aller-retour interne masquerait.
    """
    raw = np.frombuffer(data, dtype=np.uint8).reshape(-1, 8)
    c0 = raw[:, 0].astype(np.uint16) | (raw[:, 1].astype(np.uint16) << 8)
    c1 = raw[:, 2].astype(np.uint16) | (raw[:, 3].astype(np.uint16) << 8)
    bits = (raw[:, 4].astype(np.uint32) | (raw[:, 5].astype(np.uint32) << 8) |
            (raw[:, 6].astype(np.uint32) << 16) | (raw[:, 7].astype(np.uint32) << 24))

    e0 = _unpack565(c0).astype(np.int16)
    e1 = _unpack565(c1).astype(np.int16)
    n = raw.shape[0]
    palette = np.empty((n, 4, 3), dtype=np.int16)
    palette[:, 0] = e0
    palette[:, 1] = e1
    four = (c0 > c1)[:, None]
    palette[:, 2] = np.where(four, (2 * e0 + e1) // 3, (e0 + e1) // 2)
    palette[:, 3] = np.where(four, (e0 + 2 * e1) // 3, 0)

    shifts = (2 * np.arange(16, dtype=np.uint32))[None, :]
    indices = (bits[:, None] >> shifts) & 0x3
    pixels = np.take_along_axis(palette, indices[:, :, None].astype(np.intp), axis=1)

    return (pixels.reshape(height // 4, width // 4, 4, 4, 3)
                  .transpose(0, 2, 1, 3, 4)
                  .reshape(height, width, 3)).astype(np.uint8)


def psnr(a: np.ndarray, b: np.ndarray) -> float:
    mse = np.mean((a.astype(np.float64) - b.astype(np.float64)) ** 2)
    if mse <= 0.0:
        return float("inf")
    return 10.0 * np.log10(255.0 * 255.0 / mse)


# ---------------------------------------------------------------------------
# Mosaique
# ---------------------------------------------------------------------------
def build_mosaic(tile_dir: str, extent: Extent) -> tuple[Image.Image, float]:
    """Recolle les tuiles telechargees. Renvoie (image, metres par pixel).

    Les tuiles sont placees par leurs COORDONNEES, lues dans leur nom de
    fichier, et non par ordre alphabetique : c'est ce qui garantit que le nord
    est en haut. Une tuile manquante est une erreur franche — une mosaique
    trouee produirait une texture noire par endroits, defaut qu'on ne
    remarquerait qu'en jeu, longtemps apres.
    """
    tiles = {}
    tile_px = None
    for path in glob.glob(os.path.join(tile_dir, "ortho_*px.*")):
        match = TILE_RE.search(os.path.basename(path))
        if not match:
            continue
        east, north, px = int(match.group(1)), int(match.group(2)), int(match.group(3))
        if tile_px is None:
            tile_px = px
        elif px != tile_px:
            sys.exit(f"{tile_dir} contient des tuiles de tailles differentes "
                     f"({tile_px} et {px} px). Vider le repertoire et retelecharger.")
        tiles[(east, north)] = path

    if not tiles:
        sys.exit(f"Aucune tuile ortho_*px.* dans {tile_dir}. "
                 "Lancer d'abord tools/fetch_ortho.py.")

    easts = sorted({e for e, _ in tiles})
    norths = sorted({n for _, n in tiles})
    tile_m = (easts[1] - easts[0]) if len(easts) > 1 else int(extent.width_m)
    cols, rows = len(easts), len(norths)

    expected_cols = int(round(extent.width_m / tile_m))
    expected_rows = int(round(extent.height_m / tile_m))
    if cols != expected_cols or rows != expected_rows or len(tiles) != cols * rows:
        sys.exit(f"Couverture incomplete : {len(tiles)} tuiles pour une grille "
                 f"{expected_cols}x{expected_rows} attendue. Relancer fetch_ortho.py.")

    mosaic = Image.new("RGB", (cols * tile_px, rows * tile_px))
    for column, east in enumerate(easts):
        for row, north in enumerate(norths):
            # Les nords CROISSENT vers le haut, les lignes d'image vers le bas :
            # d'ou l'inversion. C'est le seul endroit ou le sens du nord est
            # decide, et c'est exactement l'erreur que la Phase 9 ne pouvait pas
            # detecter faute d'orthophoto.
            y = (rows - 1 - row) * tile_px
            with Image.open(tiles[(east, north)]) as tile:
                mosaic.paste(tile.convert("RGB"), (column * tile_px, y))

    return mosaic, tile_m / float(tile_px)


# ---------------------------------------------------------------------------
def main() -> int:
    parser = argparse.ArgumentParser(
        description="Assemble les tuiles BD ORTHO en texture .vcto (mipmaps + BC1).")
    parser.add_argument("terrain", help="fichier .vchm dont on reprend l'emprise")
    parser.add_argument("-i", "--input", default="data/ortho", help="repertoire des tuiles")
    parser.add_argument("-o", "--output", required=True, help="fichier .vcto a ecrire")
    parser.add_argument("--size", type=int, default=8192,
                        help="cote de la texture GPU, en texels (defaut 8192). "
                             "Doit etre une puissance de deux et un multiple de 4.")
    parser.add_argument("--format", default="bc1", choices=("bc1", "rgba"))
    parser.add_argument("--no-psnr", action="store_true",
                        help="saute la mesure de qualite (elle double la duree)")
    args = parser.parse_args()

    if args.size & (args.size - 1) or args.size < 4:
        sys.exit("--size doit etre une puissance de deux >= 4.")

    extent = Extent(args.terrain)
    print(extent.describe())

    started = time.time()
    print(f"\nAssemblage depuis {args.input} ...")
    mosaic, source_res = build_mosaic(args.input, extent)
    print(f"  mosaique {mosaic.size[0]} x {mosaic.size[1]} px "
          f"a {source_res:g} m/texel")

    if mosaic.size[0] != args.size:
        print(f"  redimensionnement -> {args.size} x {args.size} "
              f"({extent.width_m / args.size:.2f} m/texel) [Lanczos]")
        mosaic = mosaic.resize((args.size, args.size), Image.LANCZOS)

    # Chaine de mipmaps complete, jusqu'au texel unique. Chaque niveau est reduit
    # depuis le PRECEDENT (et non depuis le niveau 0) : c'est la definition d'une
    # mipmap, et c'est deux fois moins de travail.
    levels = []
    current = mosaic
    while True:
        levels.append(current)
        if current.size[0] == 1 and current.size[1] == 1:
            break
        current = current.resize((max(1, current.size[0] // 2),
                                  max(1, current.size[1] // 2)), Image.LANCZOS)
    print(f"  {len(levels)} niveaux de mipmap ({levels[0].size[0]} -> 1)")

    fmt = FORMAT_BC1 if args.format == "bc1" else FORMAT_RGBA8
    payload = bytearray()
    for level, image in enumerate(levels):
        width, height = image.size
        if fmt == FORMAT_RGBA8:
            payload += image.convert("RGBA").tobytes()
            continue
        # BC1 travaille par blocs de 4x4 : les derniers niveaux (2x2, 1x1) sont
        # completes par replication du bord, ce qui est ce que produit n'importe
        # quel compresseur et ce que le GPU attend (l'image reste declaree a sa
        # taille reelle, le materiel ignore les texels de remplissage).
        array = np.asarray(image.convert("RGB"))
        pad_h = (-height) % 4
        pad_w = (-width) % 4
        if pad_h or pad_w:
            array = np.pad(array, ((0, pad_h), (0, pad_w), (0, 0)), mode="edge")
        encoded = encode_bc1(array)
        payload += encoded
        if level == 0 and not args.no_psnr:
            decoded = decode_bc1(encoded, array.shape[1], array.shape[0])
            quality = psnr(array, decoded)
            verdict = "bon" if quality >= 33.0 else "FAIBLE - a verifier"
            print(f"  qualite BC1 du niveau 0 : PSNR {quality:.1f} dB ({verdict})")

    header = bytearray(HEADER_SIZE)
    struct.pack_into("<4sIIIII", header, 0, b"VCTO", 1,
                     levels[0].size[0], levels[0].size[1], len(levels), fmt)
    struct.pack_into("<dddd", header, 24,
                     extent.east_min, extent.north_min, extent.east_max, extent.north_max)
    struct.pack_into("<II", header, 56, extent.epsg, 0)
    label = (extent.label + "_ortho").encode("utf-8")[:63]
    header[64:64 + len(label)] = label

    os.makedirs(os.path.dirname(os.path.abspath(args.output)), exist_ok=True)
    with open(args.output, "wb") as f:
        f.write(header)
        f.write(payload)

    size_mo = (HEADER_SIZE + len(payload)) / 1e6
    print(f"\nEcrit : {args.output}")
    print(f"  {args.size} x {args.size}, {len(levels)} mipmaps, "
          f"format {'BC1' if fmt == FORMAT_BC1 else 'RGBA8'}, {size_mo:.1f} Mo")
    print(f"  {extent.width_m / args.size:.2f} m/texel "
          f"(source BD ORTHO a {source_res:g} m/texel)")
    print(f"  en {time.time() - started:.0f} s")
    return 0


if __name__ == "__main__":
    sys.exit(main())
