#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""
make_bench_zone.py — fabrique une zone PROXY de 25 km2 pour MESURER le moteur
sur une machine qui n'a pas les donnees LiDAR (session cloud, CI, nouveau poste).

PHASE 30. Les dalles LiDAR (data/lidar, plusieurs Go) et les .vchm qui en sont
tires ne sont pas versionnes ; seule l'orthophoto vercora_zone_ortho.vcto l'est.
Sans terrain de zone, le jeu retombe sur le terrain synthetique 128x128 : un
banc de mesure n'y exerce ni le streaming de 8 000 noeuds, ni la vegetation a
l'echelle, ni rien de ce qui pese reellement en memoire.

Ce script ecrit, dans assets/terrain/ (ignore par git, *.vchm) :
    vercora_zone_1m.vchm                    5001 x 5001 au pas de 1 m
    vercora_zone_canopy.vchm                couvert 0..1, 1001 x 1001 au pas de 5 m
    vercora_zone_canopy_height.vchm         hauteur de canopee (m)
    vercora_zone_canopy_lowveg.vchm         fraction de vegetation basse (F3)
    vercora_zone_canopy_lowveg_height.vchm  hauteur de vegetation basse (m)

CE QUI EST VRAI ET CE QUI NE L'EST PAS, sans detour :
  - L'EMPRISE, la RESOLUTION, la georeference et la plage d'altitudes
    (1029-2285 m) sont celles de la vraie zone : le quadtree, le streaming,
    l'orthophoto (qui s'y applique, orthoMatchesTerrain passe) et tous les
    budgets memoire travaillent donc a la vraie echelle.
  - Le RELIEF est un bruit fractal (fBm + cretes, la recette de
    Heightmap::Params), pas le Vercors.
  - Le COUVERT est tire de la VRAIE orthophoto (indice de vert sur le niveau de
    mipmap 2048 x 2048) : les arbres poussent la ou la photo montre de la foret,
    mais sur un relief qui n'est pas le sien.
Les chiffres de MEMOIRE et de COUT obtenus sur cette zone sont donc
representatifs a l'ordre de grandeur et comparables ENTRE EUX (avant / apres
une optimisation) ; ils ne remplacent pas une mesure sur les vraies donnees.

    python tools/make_bench_zone.py            # ecrit dans assets/terrain/
    python tools/make_bench_zone.py --force    # ecrase des fichiers existants

Refuse d'ecraser un fichier existant sans --force : sur la machine de
developpement, ces noms sont ceux des VRAIES donnees.

Dependances : numpy.
"""

from __future__ import annotations

import argparse
import os
import struct
import sys
import time

import numpy as np

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TERRAIN_DIR = os.path.join(ROOT, "assets", "terrain")
ORTHO = os.path.join(TERRAIN_DIR, "vercora_zone_ortho.vcto")

# Emprise de la vraie zone (en-tete de vercora_zone_ortho.vcto).
ORIGIN_EAST = 899000.0
ORIGIN_NORTH = 6440000.0
EXTENT_M = 5000.0
EPSG = 2154
ALT_MIN = 1029.0
ALT_MAX = 2285.0
SEED = 20261013


def write_vchm(path: str, grid: np.ndarray, spacing: float, label: str,
               absolute: bool) -> None:
    """Ecrit un .vchm version 1 (format : src/assets/TerrainFile.hpp)."""
    grid = np.ascontiguousarray(grid, dtype="<f4")
    n = grid.shape[0]
    assert grid.shape == (n, n)
    header = bytearray(120)
    header[0:4] = b"VCHM"
    struct.pack_into("<IIIf", header, 4, 1, n, n, spacing)
    struct.pack_into("<dd", header, 24, ORIGIN_EAST, ORIGIN_NORTH)
    struct.pack_into("<ffII", header, 40, float(grid.min()), float(grid.max()),
                     EPSG, 1 if absolute else 0)
    raw = label.encode("ascii")[:64]
    header[56:56 + len(raw)] = raw
    with open(path, "wb") as f:
        f.write(header)
        f.write(grid.tobytes())


def smooth(t: np.ndarray) -> np.ndarray:
    return t * t * (3.0 - 2.0 * t)


def value_noise(n: int, cells: int, rng: np.random.Generator) -> np.ndarray:
    """Bruit de valeur n x n a `cells` cellules par cote, interpolation
    separable (d'abord le long de X pour chaque ligne du reseau, puis le long
    de Z) : 3 operations par echantillon au lieu de 4 lectures + 3 lerps."""
    lattice = rng.random((cells + 2, cells + 2), dtype=np.float32)
    x = np.linspace(0.0, cells, n, dtype=np.float64)
    i0 = np.minimum(np.floor(x).astype(np.int64), cells)
    t = smooth((x - i0).astype(np.float32))
    along_x = lattice[:, i0] * (1.0 - t) + lattice[:, i0 + 1] * t  # (cells+2, n)
    return along_x[i0, :] * (1.0 - t)[:, None] + along_x[i0 + 1, :] * t[:, None]


def fbm(n: int, base: int, octaves: int, ridged: bool, rng) -> np.ndarray:
    total = np.zeros((n, n), dtype=np.float32)
    norm = 0.0
    amplitude = 1.0
    cells = base
    for _ in range(octaves):
        octave = value_noise(n, cells, rng)
        if ridged:
            octave = 1.0 - np.abs(2.0 * octave - 1.0)
        total += amplitude * octave
        norm += amplitude
        amplitude *= 0.5
        cells *= 2
    return total / norm


def make_relief(n: int) -> np.ndarray:
    rng = np.random.default_rng(SEED)
    soft = fbm(n, 3, 9, False, rng) ** 1.4
    ridge = fbm(n, 3, 9, True, rng)
    shaped = soft + (ridge * ridge - soft) * 0.6
    # Barres rocheuses facon Vercors : une marche raide la ou un bruit tres
    # large franchit un seuil. Sans elles le relief n'offrirait aucune paroi
    # d'escalade (pente >= 0,84) ni de pierrier (0,60-0,84).
    band = value_noise(n, 4, rng)
    cliff = smooth(np.clip((band - 0.55) / 0.02, 0.0, 1.0))
    shaped = shaped + 0.12 * cliff
    lo, hi = float(shaped.min()), float(shaped.max())
    return (ALT_MIN + (shaped - lo) / (hi - lo) * (ALT_MAX - ALT_MIN)).astype(np.float32)


def decode_bc1_level(path: str, level: int) -> np.ndarray:
    """Decode le niveau `level` d'un .vcto BC1 en RGB float [0,1]."""
    with open(path, "rb") as f:
        data = f.read()
    if data[:4] != b"VCTO":
        raise SystemExit(f"{path} n'est pas un .vcto")
    width, height, mips, fmt = struct.unpack_from("<IIII", data, 8)
    if fmt != 1:
        raise SystemExit("orthophoto attendue en BC1")
    offset = 128
    for k in range(level):
        w, h = max(1, width >> k), max(1, height >> k)
        offset += ((w + 3) // 4) * ((h + 3) // 4) * 8
    w, h = max(1, width >> level), max(1, height >> level)
    bx, by = (w + 3) // 4, (h + 3) // 4
    blocks = np.frombuffer(data, dtype="<u2", count=bx * by * 4, offset=offset)
    blocks = blocks.reshape(by, bx, 4)
    c0 = blocks[..., 0].astype(np.uint32)
    c1 = blocks[..., 1].astype(np.uint32)
    bits = blocks[..., 2].astype(np.uint32) | (blocks[..., 3].astype(np.uint32) << 16)

    def rgb565(c):
        r = ((c >> 11) & 31) / 31.0
        g = ((c >> 5) & 63) / 63.0
        b = (c & 31) / 31.0
        return np.stack([r, g, b], axis=-1)

    p0, p1 = rgb565(c0), rgb565(c1)
    four = (c0 > c1)[..., None]
    p2 = np.where(four, (2 * p0 + p1) / 3, (p0 + p1) / 2)
    p3 = np.where(four, (p0 + 2 * p1) / 3, 0.0)
    palette = np.stack([p0, p1, p2, p3], axis=2)  # (by, bx, 4, 3)
    out = np.zeros((by * 4, bx * 4, 3), dtype=np.float32)
    for py in range(4):
        for px in range(4):
            idx = (bits >> (2 * (py * 4 + px))) & 3
            texel = np.take_along_axis(palette, idx[..., None, None].repeat(3, -1),
                                       axis=2)[:, :, 0, :]
            out[py::4, px::4, :] = texel
    return out[:h, :w]


def resample(image: np.ndarray, n: int) -> np.ndarray:
    """Echantillonne l'image (rangee du nord au sud) aux n x n points de grille
    (coins compris), par moyenne d'une fenetre 3 x 3 autour du texel."""
    h, w = image.shape[:2]
    coords = np.linspace(0.0, 1.0, n)
    ys = np.clip(np.round(coords * (h - 1)).astype(np.int64), 1, h - 2)
    xs = np.clip(np.round(coords * (w - 1)).astype(np.int64), 1, w - 2)
    acc = np.zeros((n, n) + image.shape[2:], dtype=np.float32)
    for dy in (-1, 0, 1):
        for dx in (-1, 0, 1):
            acc += image[(ys + dy)[:, None], (xs + dx)[None, :]]
    return acc / 9.0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[1])
    parser.add_argument("--force", action="store_true",
                        help="ecraser des fichiers existants")
    args = parser.parse_args()

    names = {
        "terrain": "vercora_zone_1m.vchm",
        "cover": "vercora_zone_canopy.vchm",
        "height": "vercora_zone_canopy_height.vchm",
        "lowveg": "vercora_zone_canopy_lowveg.vchm",
        "lowveg_height": "vercora_zone_canopy_lowveg_height.vchm",
    }
    paths = {k: os.path.join(TERRAIN_DIR, v) for k, v in names.items()}
    existing = [p for p in paths.values() if os.path.exists(p)]
    if existing and not args.force:
        print("Refus : ces fichiers existent deja (vraies donnees ?) :")
        for p in existing:
            print("  " + p)
        print("Relancer avec --force pour les ecraser.")
        return 1
    if not os.path.exists(ORTHO):
        print(f"Orthophoto introuvable : {ORTHO}")
        return 1

    t0 = time.time()
    relief = make_relief(5001)
    write_vchm(paths["terrain"], relief, 1.0, "vercora_zone PROXY bench (relief fBm)", True)
    print(f"terrain : {paths['terrain']} ({relief.min():.1f} .. {relief.max():.1f} m, "
          f"{time.time() - t0:.1f} s)")

    rgb = decode_bc1_level(ORTHO, 2)  # 2048 x 2048, 2,44 m/texel
    rgb5 = resample(rgb, 1001)
    r, g, b = rgb5[..., 0], rgb5[..., 1], rgb5[..., 2]
    lum = (r + g + b) / 3.0
    exg = 2.0 * g - r - b
    # Foret = sombre ET verte ; prairie = verte mais claire ; roche = grise.
    green = np.clip(exg / 0.06, 0.0, 1.0)
    dark = np.clip((0.45 - lum) / 0.18, 0.0, 1.0)
    cover = np.clip(green * dark * 1.6, 0.0, 1.0).astype(np.float32)
    rng = np.random.default_rng(SEED + 1)
    tall = 14.0 + 16.0 * value_noise(1001, 40, rng)
    canopy = np.where(cover > 0.15, tall * np.clip(cover * 1.5, 0.3, 1.0), 0.0)
    shrub = np.clip(green * (1.0 - dark) * 0.35 + cover * (1.0 - cover) * 0.5, 0.0, 0.6)
    lowveg = shrub.astype(np.float32)
    lowveg_h = np.where(lowveg > 0.05, 0.6 + 3.6 * value_noise(1001, 60, rng), 0.0)

    write_vchm(paths["cover"], cover, 5.0, "vercora_zone PROXY couvert (ortho)", False)
    write_vchm(paths["height"], canopy.astype(np.float32), 5.0,
               "vercora_zone PROXY canopee", False)
    write_vchm(paths["lowveg"], lowveg, 5.0, "vercora_zone PROXY vegetation basse", False)
    write_vchm(paths["lowveg_height"], lowveg_h.astype(np.float32), 5.0,
               "vercora_zone PROXY h vegetation basse", False)
    print(f"couvert : moyenne {cover.mean():.3f}, cellules > 0,5 : "
          f"{(cover > 0.5).mean() * 100:.1f} % ; F3 moyenne {lowveg.mean():.3f}")
    print(f"total {time.time() - t0:.1f} s")
    return 0


if __name__ == "__main__":
    sys.exit(main())
