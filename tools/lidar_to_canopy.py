#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""
lidar_to_canopy.py — extrait des dalles LiDAR HD une CARTE DE COUVERT FORESTIER.

PHASE 11, tache 2. Jusqu'ici les arbres etaient semes par tirage-rejet uniforme
(pente + espacement) sur une zone de test de quelques dizaines de metres. A
l'echelle des 25 km2 reels, semer uniformement donnerait des arbres sur les
lapiaz, sur les eboulis et au milieu des pistes de ski — c'est-a-dire un paysage
faux, alors que la donnee qui dit ou sont VRAIMENT les arbres est deja sur le
disque : les nuages LiDAR sont CLASSIFIES.

    python tools/lidar_to_canopy.py data/lidar -o assets/terrain/vercora_zone_canopy.vchm \\
        --extent assets/terrain/vercora_zone_1m.vchm

CE QUE DIT LA CLASSIFICATION (norme ASPRS, verifie sur ces dalles) :
    2 = sol           ~90 %        3 = vegetation basse   ~1,9 %
    4 = vegetation moyenne ~0,7 %  5 = vegetation haute   ~4,7 %
    1 = non classe    ~2,8 %       6 = bati, 64 = bruit

DEUX GRANDEURS SONT PRODUITES, dans deux fichiers :
  - le COUVERT (--out) : part des retours de vegetation (classes 3, 4 et 5) parmi
    tous les retours valides de la cellule. C'est une fraction entre 0 et 1, donc
    insensible a la densite de vol, contrairement a un simple comptage.
  - la HAUTEUR DE CANOPEE (--out-height) : altitude du plus haut retour de classe
    5 moins l'altitude du plus bas retour de sol, dans la meme cellule. C'est la
    hauteur du plus grand arbre de la cellule, en metres.
Le jeu se sert du premier pour savoir OU semer, du second pour savoir QUELLE
TAILLE donner aux arbres — deux questions distinctes que la meme carte ne peut
pas trancher.

LE MAXIMUM PLUTOT QU'UN PERCENTILE. Un percentile de hauteur demanderait de
conserver tous les points d'une cellule ; le maximum se calcule en un seul
passage, sans rien stocker. Sur des cellules de 5 m il designe le plus grand
arbre de la cellule, ce qui est exactement la grandeur voulue. Contrepartie
assumee : un point aberrant non filtre (classe 64) gonflerait la valeur, d'ou
l'exclusion explicite du bruit et l'ecretage a --max-height.

FORMAT DE SORTIE : le meme .vchm que le terrain (voir tools/lidar_to_heightmap.py).
C'est volontaire et ce n'est pas un abus : un .vchm est une GRILLE DE FLOTTANTS
GEOREFERENCEE, et rien dans le format n'impose que le flottant soit une altitude.
Le moteur relit donc ces cartes avec loadTerrainFile() sans une ligne de code
nouvelle, et obtient gratuitement l'interpolation bilineaire de heightAt() et la
conversion monde <-> Lambert 93. La grille produite a EXACTEMENT la meme origine
et la meme emprise que le terrain, seul son pas differe : un point du monde s'y
lit donc avec la meme formule, sans decalage a corriger.

Dependances : laspy + lazrs + numpy.
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
    import laspy
except ImportError:
    sys.exit('laspy et numpy sont requis : python -m pip install "laspy[lazrs]" numpy')

HEADER_SIZE = 120
CHUNK = 4_000_000

CLASS_GROUND = 2
CLASSES_VEGETATION = (3, 4, 5)
CLASS_HIGH_VEGETATION = 5
# Classes a EXCLURE du denominateur : ce ne sont pas des retours de surface.
CLASSES_REJECTED = (7, 18, 64, 65, 66)


def read_extent(path: str):
    """(originEast, originNorth, samples, spacing) d'un .vchm de reference."""
    with open(path, "rb") as f:
        head = f.read(HEADER_SIZE)
    if len(head) < HEADER_SIZE or head[:4] != b"VCHM":
        sys.exit(f"{path} n'est pas un fichier .vchm.")
    _m, _v, sx, sz, spacing, _r, origin_e, origin_n = struct.unpack_from("<4sIIIfIdd", head, 0)
    if sx != sz:
        sys.exit(f"{path} : grille non carree ({sx}x{sz}), non gere.")
    return origin_e, origin_n, sx, spacing


def write_vchm(path: str, grid: np.ndarray, spacing: float,
               origin_e: float, origin_n: float, label: str) -> None:
    samples = grid.shape[0]
    header = bytearray(HEADER_SIZE)
    struct.pack_into("<4sIIIfI", header, 0, b"VCHM", 1, samples, samples, spacing, 0)
    struct.pack_into("<dd", header, 24, origin_e, origin_n)
    struct.pack_into("<ff", header, 40, float(grid.min()), float(grid.max()))
    # epsg 2154, flags 0 : ce ne sont PAS des altitudes absolues, et le dire evite
    # qu'un outil de controle croise ces valeurs avec un modele de terrain.
    struct.pack_into("<II", header, 48, 2154, 0)
    encoded = label.encode("utf-8")[:63]
    header[56:56 + len(encoded)] = encoded
    with open(path, "wb") as f:
        f.write(header)
        f.write(grid.astype(np.float32).tobytes())


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Carte de couvert forestier et de hauteur de canopee depuis le LiDAR.")
    parser.add_argument("lidar_dir", help="repertoire des dalles .copc.laz")
    parser.add_argument("--extent", required=True,
                        help="terrain .vchm dont on reprend origine et emprise")
    parser.add_argument("-o", "--out", required=True, help=".vchm de couvert (0..1)")
    parser.add_argument("--out-height", default=None,
                        help=".vchm de hauteur de canopee (m). Par defaut, "
                             "le nom de --out avec _height avant l'extension.")
    parser.add_argument("--cell", type=float, default=5.0,
                        help="pas de la grille produite, en metres (defaut 5)")
    parser.add_argument("--max-height", type=float, default=45.0,
                        help="ecretage de la hauteur de canopee (defaut 45 m)")
    args = parser.parse_args()

    origin_e, origin_n, terrain_samples, terrain_spacing = read_extent(args.extent)
    span = (terrain_samples - 1) * terrain_spacing
    if abs(span / args.cell - round(span / args.cell)) > 1e-9:
        sys.exit(f"L'emprise ({span:g} m) n'est pas un multiple de --cell {args.cell:g}.")

    samples = int(round(span / args.cell)) + 1
    east_max = origin_e + span
    north_min = origin_n - span
    print(f"Reference : {args.extent}")
    print(f"  emprise E {origin_e:.0f}..{east_max:.0f}  N {north_min:.0f}..{origin_n:.0f}")
    print(f"  grille produite : {samples}x{samples} au pas de {args.cell:g} m")

    # Un accumulateur par grandeur. Les noeuds sont les CENTRES des cellules
    # d'agregation : c'est ce qui permet a la grille produite d'avoir exactement
    # la meme origine et la meme emprise que le terrain, donc de se lire avec la
    # meme formule monde <-> grille, sans demi-cellule a corriger quelque part.
    total_count = np.zeros((samples, samples), dtype=np.int32)
    veg_count = np.zeros((samples, samples), dtype=np.int32)
    high_max = np.full((samples, samples), -np.inf, dtype=np.float32)
    ground_min = np.full((samples, samples), np.inf, dtype=np.float32)

    tiles = sorted(glob.glob(os.path.join(args.lidar_dir, "*.laz")))
    if not tiles:
        sys.exit(f"Aucune dalle .laz dans {args.lidar_dir}.")
    print(f"\n{len(tiles)} dalle(s) a depouiller.")

    started = time.time()
    for index, path in enumerate(tiles, 1):
        name = os.path.basename(path)
        tile_started = time.time()
        kept = 0
        with laspy.open(path) as reader:
            for chunk in reader.chunk_iterator(CHUNK):
                cls = np.asarray(chunk.classification)
                valid = ~np.isin(cls, CLASSES_REJECTED)
                if not valid.any():
                    continue
                cls = cls[valid]
                x = np.asarray(chunk.x)[valid]
                y = np.asarray(chunk.y)[valid]
                z = np.asarray(chunk.z)[valid].astype(np.float32)

                # Indices de noeud : ARRONDI (et non partie entiere), puisque le
                # noeud est le centre de sa cellule. iz croit vers le SUD, comme
                # dans le .vchm du terrain.
                ix = np.rint((x - origin_e) / args.cell).astype(np.int64)
                iz = np.rint((origin_n - y) / args.cell).astype(np.int64)
                inside = (ix >= 0) & (ix < samples) & (iz >= 0) & (iz < samples)
                if not inside.any():
                    continue
                ix, iz, cls, z = ix[inside], iz[inside], cls[inside], z[inside]
                flat = iz * samples + ix
                kept += flat.size

                np.add.at(total_count.reshape(-1), flat, 1)

                veg = np.isin(cls, CLASSES_VEGETATION)
                if veg.any():
                    np.add.at(veg_count.reshape(-1), flat[veg], 1)

                high = cls == CLASS_HIGH_VEGETATION
                if high.any():
                    np.maximum.at(high_max.reshape(-1), flat[high], z[high])

                ground = cls == CLASS_GROUND
                if ground.any():
                    np.minimum.at(ground_min.reshape(-1), flat[ground], z[ground])

        print(f"  [{index}/{len(tiles)}] {name} : {kept:,} points retenus "
              f"en {time.time() - tile_started:.1f} s")

    # --- Couvert ------------------------------------------------------------
    # Une cellule sans aucun retour (hors emprise du vol, ou trou de donnees)
    # reste a zero : mieux vaut ne pas y semer d'arbre que d'en inventer.
    cover = np.zeros((samples, samples), dtype=np.float32)
    seen = total_count > 0
    cover[seen] = veg_count[seen].astype(np.float32) / total_count[seen]

    # --- Hauteur de canopee -------------------------------------------------
    height = np.zeros((samples, samples), dtype=np.float32)
    usable = np.isfinite(high_max) & np.isfinite(ground_min)
    height[usable] = high_max[usable] - ground_min[usable]
    np.clip(height, 0.0, args.max_height, out=height)

    out_height = args.out_height
    if out_height is None:
        stem, extension = os.path.splitext(args.out)
        out_height = stem + "_height" + extension

    for path, grid, label in ((args.out, cover, "couvert_vegetal"),
                              (out_height, height, "hauteur_canopee")):
        os.makedirs(os.path.dirname(os.path.abspath(path)), exist_ok=True)
        write_vchm(path, grid, args.cell, origin_e, origin_n, label)

    covered = float((cover > 0.15).mean())
    wooded = float((height >= 3.0).mean())
    print(f"\nEcrit : {args.out}")
    print(f"  couvert : moyenne {cover.mean():.3f}, max {cover.max():.3f} ; "
          f"{100 * covered:.1f} % des cellules au-dessus de 0,15")
    print(f"Ecrit : {out_height}")
    print(f"  canopee : moyenne des cellules boisees "
          f"{height[height >= 3.0].mean() if wooded else 0:.1f} m, "
          f"max {height.max():.1f} m ; {100 * wooded:.1f} % de cellules a 3 m ou plus")
    print(f"  cellules sans aucun retour : {100 * float((~seen).mean()):.2f} %")
    print(f"  en {time.time() - started:.0f} s")
    return 0


if __name__ == "__main__":
    sys.exit(main())
