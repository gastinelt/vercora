#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""
lidar_to_heightmap.py — dalle LiDAR HD IGN (.laz / .copc.laz) -> heightmap .vchm

PHASE 7. Premier maillon reel du pipeline : remplace le bruit procedural de la
Phase 4 par du terrain mesure. L'outil est VOLONTAIREMENT SEPARE du moteur — il
tourne une fois, hors ligne, et le jeu ne lit qu'une grille de float deja propre.
Mettre un decodeur LAZ dans le moteur aurait ajoute une dependance lourde pour
un travail qui n'a aucune raison d'etre refait a chaque lancement.

    python tools/lidar_to_heightmap.py ENTREE.copc.laz -o SORTIE.vchm [options]

Exemple reel (celui de la Phase 7) :

    python tools/lidar_to_heightmap.py ^
        data/lidar/LHD_FXX_0907_6440_PTS_LAMB93_IGN69.copc.laz ^
        -o assets/terrain/LHD_0907_6440_1m.vchm --cell 1.0

Options utiles :
    --cell N        taille de cellule en metres (defaut 1.0)
    --stat S        median (defaut) | mean | min — agregation des points d'un noeud
    --classes L     classes LAS retenues (defaut 2 = sol)
    --no-despike    desactive le filtre de pics
    --zero-base     ramene l'altitude minimale a 0 au lieu de garder l'IGN69
    --report-only   analyse et statistiques, sans ecrire de fichier

Dependances : laspy + lazrs + numpy
    python -m pip install "laspy[lazrs]" numpy

Pourquoi laspy/lazrs et pas PDAL : PDAL est bien le standard du domaine, mais sur
Windows il ne se livre en pratique que par conda, absent de cette machine.
laspy 2.x avec le backend lazrs (l'implementation Rust de LASzip) lit exactement
le meme LAZ 1.4 classifie, s'installe en une commande par pip, et tout le travail
qu'on lui demanderait ici — filtrer une classe, binner sur une grille, agreger —
est de toute facon du numpy. Le jour ou PDAL serait disponible, il produirait la
meme grille : le format .vchm ci-dessous ne depend pas du lecteur.


FORMAT DE SORTIE .vchm ("Vercora HeightMap"), petit-boutiste, en-tete de 120
octets suivi des altitudes :

    offset taille champ
    0      4      magie "VCHM"
    4      4      uint32  version = 1
    8      4      uint32  samplesX   (echantillons d'ouest en est)
    12     4      uint32  samplesZ   (echantillons du nord vers le sud)
    16     4      float32 spacing    (metres entre deux echantillons voisins)
    20     4      uint32  reserve = 0
    24     8      float64 originEast   coordonnee X (Lambert 93) de l'echantillon ix=0
    32     8      float64 originNorth  coordonnee Y (Lambert 93) de l'echantillon iz=0
    40     4      float32 minHeight
    44     4      float32 maxHeight
    48     4      uint32  crsEpsg    (2154 pour le Lambert 93)
    52     4      uint32  flags      bit 0 : les altitudes sont absolues (IGN69)
    56     64     char[64] label     nom de la dalle, complete par des zeros
    120    ...    float32 heights[samplesZ * samplesX], index = iz * samplesX + ix

SENS DES AXES — le point delicat, et la raison pour laquelle les lignes sont
stockees du NORD vers le SUD. Le moteur est en repere main droite avec Y vers le
haut, donc X ^ Y = Z. En posant X = Est et Y = Haut, on obtient
Z = Est ^ Haut = -Nord : le nord est en -Z. Ecrire les lignes du nord vers le sud
fait donc correspondre iz croissant a Z croissant, et le terrain s'affiche dans
le bon sens. Stocker du sud vers le nord donnerait un relief en MIROIR — une
erreur invisible sur du bruit procedural, et tres difficile a diagnostiquer plus
tard sur une orthophoto.

Les altitudes restent ABSOLUES (IGN69) par defaut : c'est la donnee mesuree, et
c'est ce que originEast/originNorth pretendent decrire. Le recentrage horizontal
(la dalle est centree sur l'origine du monde) est fait par le moteur au
chargement, pas ici — ainsi le fichier reste geo-reference et deux dalles
voisines resteront raccordables.
"""

from __future__ import annotations

import argparse
import math
import os
import struct
import sys
import time

import numpy as np

try:
    import laspy
except ImportError:  # pragma: no cover - message d'aide, pas de la logique
    sys.exit("laspy est absent. Installer :\n"
             '    python -m pip install "laspy[lazrs]" numpy')


MAGIC = b"VCHM"
VERSION = 1
HEADER_SIZE = 120
LABEL_SIZE = 64
FLAG_ABSOLUTE_ALTITUDE = 1 << 0

# Nomenclature LAS standard, telle qu'utilisee par l'IGN pour LiDAR HD.
# Sert uniquement aux messages : le filtre, lui, prend les numeros bruts.
CLASS_NAMES = {
    1: "non classe",
    2: "sol",
    3: "vegetation basse",
    4: "vegetation moyenne",
    5: "vegetation haute",
    6: "bati",
    9: "eau",
    17: "tablier de pont",
    64: "sursol perenne",
    65: "artefact",
    66: "point virtuel",
    67: "divers bati",
}


def human_size(n: int) -> str:
    for unit in ("o", "Ko", "Mo", "Go"):
        if n < 1024 or unit == "Go":
            return f"{n:.1f} {unit}" if unit != "o" else f"{n} o"
        n /= 1024.0
    return f"{n} o"


# ---------------------------------------------------------------------------
# 1. Lecture du nuage
# ---------------------------------------------------------------------------

def read_ground_points(path: str, classes: set[int], chunk_size: int):
    """Renvoie (x, y, z) des points des classes demandees, plus des statistiques.

    Lecture PAR MORCEAUX : une dalle LiDAR HD fait ~29 millions de points, dont
    on ne garde ici que le quart (le sol). Tout charger d'un bloc marcherait sur
    cette machine, mais garder seulement les points retenus borne la memoire a la
    taille du RESULTAT et non a celle du fichier — ce qui reste vrai le jour ou
    l'on enchainera plusieurs dalles.
    """
    xs, ys, zs = [], [], []
    histogram = {}
    total = 0

    with laspy.open(path) as reader:
        header = reader.header
        for chunk in reader.chunk_iterator(chunk_size):
            cls = np.asarray(chunk.classification)
            total += cls.size

            values, counts = np.unique(cls, return_counts=True)
            for value, count in zip(values.tolist(), counts.tolist()):
                histogram[value] = histogram.get(value, 0) + count

            keep = np.isin(cls, list(classes))
            if not keep.any():
                continue
            xs.append(np.asarray(chunk.x)[keep])
            ys.append(np.asarray(chunk.y)[keep])
            zs.append(np.asarray(chunk.z)[keep].astype(np.float64))

    if not xs:
        raise SystemExit(
            f"Aucun point des classes {sorted(classes)} dans {os.path.basename(path)}. "
            "Cette dalle est-elle bien la version CLASSIFIEE ?")

    return (np.concatenate(xs), np.concatenate(ys), np.concatenate(zs),
            header, total, histogram)


# ---------------------------------------------------------------------------
# 2. Grille
# ---------------------------------------------------------------------------

def bounds_from_name(path: str):
    """(x_min, y_min, x_max, y_max) nominal deduit du nom d'une dalle LiDAR HD.

    'LHD_FXX_0899_6440_PTS_LAMB93_IGN69.copc.laz' -> coin NORD-OUEST a
    X = 899 km, Y = 6440 km, donc la dalle couvre X 899000..900000 et
    Y 6439000..6440000 : le Y du nom est le HAUT de la dalle, pas le bas.
    Retourne None si le nom ne suit pas la nomenclature.
    """
    parts = os.path.basename(path).split("_")
    for index in range(len(parts) - 1):
        a, b = parts[index], parts[index + 1]
        if a.isdigit() and b.isdigit() and len(a) == 4 and len(b) == 4:
            x_min = int(a) * 1000
            y_max = int(b) * 1000
            return (x_min, y_max - 1000, x_min + 1000, y_max)
    return None


def build_grid(x, y, z, x_min, y_min, x_max, y_max, cell, stat):
    """Agrege les points sur une grille reguliere, un echantillon par NOEUD.

    Les echantillons sont sur les noeuds (ix = 0 tombe pile sur x_min) et non au
    centre des cellules : les bords de la grille coincident alors exactement avec
    les bords de la dalle, donc deux dalles voisines partageront leurs
    echantillons de bord au lieu de se chevaucher d'une demi-cellule. Chaque
    point est affecte au noeud le plus PROCHE, ce qui revient a un carre de cote
    `cell` centre sur le noeud.

    Renvoie (heights, counts) en shape (nz, nx), lignes du NORD vers le SUD,
    heights valant NaN la ou aucun point n'est tombe.
    """
    nx = int(round((x_max - x_min) / cell)) + 1
    nz = int(round((y_max - y_min) / cell)) + 1

    # REGLE D'AFFECTATION, valable pour les deux axes : un point appartient au
    # noeud dont l'intervalle [c - cell/2, c + cell/2[ contient sa coordonnee
    # PROJETEE. C'est cette definition, et non ce code, que rejoue
    # tools/check_terrain_file.py.
    #
    # floor(t + 0.5) et NON np.rint : rint arrondit les ex aequo vers l'entier
    # PAIR, si bien qu'un point tombant pile sur la frontiere entre deux noeuds
    # part a gauche ou a droite selon la PARITE de l'indice. Les coordonnees LAZ
    # etant quantifiees au centimetre, ces ex aequo sont frequents et non
    # marginaux.
    #
    # Le renversement nord-sud est fait APRES l'arrondi, sur l'indice entier, et
    # c'est essentiel : arrondir directement (y_max - y) inverserait aussi le
    # sens de l'intervalle semi-ouvert, qui deviendrait ]c - cell/2, c + cell/2]
    # sur cet axe et [c - cell/2, c + cell/2[ sur l'autre. Deux dalles voisines
    # raccordees le long d'une frontiere est-ouest se disputeraient alors leur
    # ligne commune. Erreur reellement commise et attrapee par le controle croise
    # (des noeuds "vides" pour l'un contenaient un point pour l'autre) : sur une
    # dalle isolee elle ne deplacait qu'une poignee de points, donc rien de
    # visible a l'ecran.
    ix = np.floor((x - x_min) / cell + 0.5).astype(np.int64)
    iz_from_south = np.floor((y - y_min) / cell + 0.5).astype(np.int64)
    iz = (nz - 1) - iz_from_south

    inside = (ix >= 0) & (ix < nx) & (iz >= 0) & (iz < nz)
    ix, iz, z = ix[inside], iz[inside], z[inside]

    key = iz * nx + ix
    counts_flat = np.bincount(key, minlength=nx * nz)

    if stat == "mean":
        sums = np.bincount(key, weights=z, minlength=nx * nz)
        with np.errstate(invalid="ignore", divide="ignore"):
            values = sums / counts_flat
    elif stat == "min":
        values = np.full(nx * nz, np.inf)
        np.minimum.at(values, key, z)
        values[np.isinf(values)] = np.nan
    else:  # median
        # Tri lexicographique (cle, puis altitude) : les points d'un meme noeud
        # se retrouvent contigus ET tries, donc la mediane est une simple
        # indexation au milieu de chaque groupe. Entierement vectorise, la ou
        # une boucle Python sur le million de noeuds couterait des minutes.
        order = np.lexsort((z, key))
        key_sorted = key[order]
        z_sorted = z[order]

        unique_keys, starts, group_counts = np.unique(
            key_sorted, return_index=True, return_counts=True)
        middle = starts + group_counts // 2

        median = z_sorted[middle]
        even = (group_counts % 2) == 0
        if even.any():
            median = median.copy()
            median[even] = 0.5 * (z_sorted[middle[even]] + z_sorted[middle[even] - 1])

        values = np.full(nx * nz, np.nan)
        values[unique_keys] = median

    values = np.where(counts_flat > 0, values, np.nan)
    return values.reshape(nz, nx), counts_flat.reshape(nz, nx)


def neighbour_stack(grid):
    """Empile les 8 voisins de chaque cellule, bords prolonges par recopie."""
    padded = np.pad(grid, 1, mode="edge")
    nz, nx = grid.shape
    out = np.empty((8, nz, nx), dtype=grid.dtype)
    k = 0
    for dz in (0, 1, 2):
        for dx in (0, 1, 2):
            if dz == 1 and dx == 1:
                continue
            out[k] = padded[dz:dz + nz, dx:dx + nx]
            k += 1
    return out


def fill_holes(heights, max_passes=512):
    """Bouche les trous par dilatation iterative : chaque passe remplit une
    cellule vide par la MOYENNE de ses voisins deja connus.

    Aucun trou n'est tolere dans le maillage final — un NaN qui arrive au GPU
    donne un triangle degenere et un terrain qui clignote. Le nombre de passes
    effectuees est renvoye : il vaut exactement le rayon du plus gros trou, ce
    qui est le diagnostic le plus parlant sur la qualite de la dalle.
    """
    filled = heights.copy()
    known = ~np.isnan(filled)
    passes = 0

    while not known.all() and passes < max_passes:
        neighbours = neighbour_stack(np.where(known, filled, 0.0))
        valid = neighbour_stack(known.astype(np.float32))

        total = neighbours.sum(axis=0)
        weight = valid.sum(axis=0)

        newly = (~known) & (weight > 0)
        if not newly.any():
            break

        filled[newly] = total[newly] / weight[newly]
        known |= newly
        passes += 1

    remaining = int((~known).sum())
    if remaining:
        # Ne peut arriver que si la grille est entierement vide, ou si un trou
        # depasse max_passes cellules de rayon. Plutot que de laisser passer des
        # NaN, on rabat sur la mediane globale et on le dit tres fort.
        fallback = float(np.nanmedian(filled)) if np.isfinite(filled).any() else 0.0
        filled[~known] = fallback
        print(f"  !! {remaining} cellules jamais atteintes apres {passes} passes, "
              f"rabattues sur {fallback:.2f} m")

    return filled, passes, remaining


def despike(heights, measured, abs_tolerance, mad_factor):
    """Remplace les cellules isolees qui s'ecartent trop de leur voisinage.

    Le vrai risque avec une dalle reelle n'est pas le bruit — la mediane par
    noeud l'a deja absorbe — mais la cellule dont TOUS les points sont faux
    (echo parasite classe sol par erreur). Le critere doit donc etre severe
    envers un point isole et indulgent envers une falaise, car cette dalle en a
    de vraies : 562 m de denivele sur 1 km. D'ou le double seuil — il faut
    depasser A LA FOIS une tolerance absolue et un multiple de la dispersion
    LOCALE. Sur un versant raide la dispersion locale est grande, donc le filtre
    s'y desactive de lui-meme ; sur un replat elle est proche de zero et le
    moindre pic ressort.

    Seules les cellules MESUREES sont candidates : celles issues du bouchage de
    trous sont deja lisses par construction.
    """
    neighbours = neighbour_stack(heights)
    local_median = np.median(neighbours, axis=0)
    deviation = np.abs(heights - local_median)

    # MAD -> ecart-type par le facteur 1.4826 (loi normale).
    local_mad = np.median(np.abs(neighbours - local_median), axis=0) * 1.4826

    threshold = np.maximum(abs_tolerance, mad_factor * local_mad)
    spikes = measured & (deviation > threshold)

    cleaned = heights.copy()
    cleaned[spikes] = local_median[spikes]
    return cleaned, int(spikes.sum()), float(deviation[spikes].max()) if spikes.any() else 0.0


# ---------------------------------------------------------------------------
# 3. Ecriture
# ---------------------------------------------------------------------------

def write_vchm(path, heights, spacing, origin_east, origin_north, label,
               epsg, absolute):
    nz, nx = heights.shape
    data = np.ascontiguousarray(heights, dtype="<f4")

    header = struct.pack(
        "<4sIIIfI dd ffII",
        MAGIC, VERSION, nx, nz, float(spacing), 0,
        float(origin_east), float(origin_north),
        float(data.min()), float(data.max()),
        int(epsg), FLAG_ABSOLUTE_ALTITUDE if absolute else 0,
    )
    header += label.encode("ascii", "replace")[:LABEL_SIZE].ljust(LABEL_SIZE, b"\0")
    assert len(header) == HEADER_SIZE, len(header)

    os.makedirs(os.path.dirname(os.path.abspath(path)), exist_ok=True)
    with open(path, "wb") as f:
        f.write(header)
        f.write(data.tobytes())


# ---------------------------------------------------------------------------

def main() -> int:
    parser = argparse.ArgumentParser(
        description="Convertit une dalle LiDAR HD IGN en heightmap .vchm.")
    parser.add_argument("input", help="fichier .laz / .copc.laz classifie")
    parser.add_argument("-o", "--output", help="fichier .vchm a ecrire")
    parser.add_argument("--cell", type=float, default=1.0,
                        help="taille de cellule en metres (defaut 1.0)")
    parser.add_argument("--stat", choices=("median", "mean", "min"), default="median",
                        help="agregation des points d'un noeud (defaut median)")
    parser.add_argument("--classes", default="2",
                        help="classes LAS retenues, separees par des virgules "
                             "(defaut 2 = sol)")
    parser.add_argument("--chunk", type=int, default=4_000_000,
                        help="points lus par morceau (defaut 4 millions)")
    parser.add_argument("--no-despike", action="store_true",
                        help="desactive le filtre de pics")
    parser.add_argument("--spike-tolerance", type=float, default=5.0,
                        help="ecart absolu minimal pour qu'une cellule soit un "
                             "pic, en metres (defaut 5.0)")
    parser.add_argument("--spike-factor", type=float, default=6.0,
                        help="multiple de la dispersion locale au-dela duquel "
                             "une cellule est un pic (defaut 6.0)")
    parser.add_argument("--zero-base", action="store_true",
                        help="ramene l'altitude minimale a 0 (defaut : garder "
                             "les altitudes absolues IGN69)")
    parser.add_argument("--report-only", action="store_true",
                        help="analyse seulement, n'ecrit aucun fichier")
    parser.add_argument("--bounds-from-name", action="store_true",
                        help="imposer l'emprise NOMINALE de la dalle deduite de "
                             "son nom (LHD_FXX_0899_6440_... -> X 899000..900000, "
                             "Y 6439000..6440000) au lieu de l'emprise des points. "
                             "Indispensable en assemblage multi-dalles.")
    args = parser.parse_args()

    if args.cell <= 0:
        return parser.error("--cell doit etre > 0")
    if not args.report_only and not args.output:
        return parser.error("-o/--output est requis (ou utiliser --report-only)")

    classes = {int(c) for c in args.classes.split(",") if c.strip()}

    started = time.time()
    size = os.path.getsize(args.input)
    print(f"Entree  : {os.path.basename(args.input)}  ({human_size(size)})")

    x, y, z, header, total, histogram = read_ground_points(
        args.input, classes, args.chunk)

    print(f"  {total:,} points lus, point format {header.point_format.id}, "
          f"LAS {header.version}")
    print("  classification :")
    for value in sorted(histogram):
        name = CLASS_NAMES.get(value, "?")
        mark = " <-- retenu" if value in classes else ""
        print(f"      {value:3d} {name:<18s} {histogram[value]:>11,}  "
              f"({100.0 * histogram[value] / total:5.2f} %){mark}")

    print(f"  retenus : {x.size:,} points "
          f"({100.0 * x.size / total:.2f} %), "
          f"soit {x.size / max(1.0, (x.max() - x.min()) * (y.max() - y.min())):.2f} pts/m2")

    # Emprise : celle declaree par l'en-tete LAS, ecretee sur des metres entiers.
    # On ne prend PAS l'emprise des points retenus : elle varierait selon la
    # classe demandee, et deux dalles voisines ne se raccorderaient plus.
    x_min = math.floor(header.mins[0])
    y_min = math.floor(header.mins[1])
    x_max = math.ceil(header.maxs[0])
    y_max = math.ceil(header.maxs[1])

    if args.bounds_from_name:
        # ASSEMBLAGE MULTI-DALLES : l'emprise ci-dessus vient des POINTS, pas de
        # la dalle. Une dalle dont les echos s'arretent 30 cm avant le bord (bord
        # de mission, plan d'eau non reflechissant, trou de vol) donnerait une
        # grille de 1000 au lieu de 1001 echantillons : elle serait refusee comme
        # non carree, ou pire, se raccorderait decalee d'un metre a ses voisines.
        # Le nom de la dalle, lui, porte l'emprise nominale garantie par l'IGN.
        nominal = bounds_from_name(args.input)
        if nominal is None:
            return parser.error(
                f"--bounds-from-name : impossible de lire les coordonnees dans "
                f"'{os.path.basename(args.input)}' (attendu LHD_FXX_<X>_<Y>_...)")
        forced_x_min, forced_y_min, forced_x_max, forced_y_max = nominal
        if (forced_x_min, forced_y_min, forced_x_max, forced_y_max) != \
                (x_min, y_min, x_max, y_max):
            print(f"  emprise des points : X {x_min} .. {x_max}, "
                  f"Y {y_min} .. {y_max}  (ecartee)")
        x_min, y_min, x_max, y_max = nominal
        print(f"  emprise NOMINALE (nom de dalle) : X {x_min} .. {x_max}, "
              f"Y {y_min} .. {y_max} ({x_max - x_min} x {y_max - y_min} m)")
    else:
        print(f"  emprise (Lambert 93) : X {x_min} .. {x_max}, Y {y_min} .. {y_max} "
              f"({x_max - x_min} x {y_max - y_min} m)")

    heights, counts = build_grid(x, y, z, x_min, y_min, x_max, y_max,
                                 args.cell, args.stat)
    nz, nx = heights.shape
    measured = counts > 0
    holes = int((~measured).sum())
    print(f"\nGrille  : {nx} x {nz} echantillons, pas de {args.cell} m, "
          f"agregation '{args.stat}'")
    print(f"  points par cellule : min {counts[measured].min()}, "
          f"median {int(np.median(counts[measured]))}, max {counts.max()}")
    print(f"  trous : {holes:,} cellules sans aucun point "
          f"({100.0 * holes / counts.size:.3f} %)")

    heights, passes, unreached = fill_holes(heights)
    if holes:
        print(f"  bouches en {passes} passes "
              f"(rayon du plus gros trou : {passes} cellules)")

    if not args.no_despike:
        heights, spikes, worst = despike(heights, measured,
                                         args.spike_tolerance, args.spike_factor)
        print(f"  pics corriges : {spikes:,} cellules "
              f"({100.0 * spikes / heights.size:.4f} %)"
              + (f", ecart max {worst:.1f} m" if spikes else ""))

    if not np.isfinite(heights).all():
        raise SystemExit("BUG : des valeurs non finies subsistent dans la grille.")

    absolute = not args.zero_base
    if args.zero_base:
        heights = heights - heights.min()

    print(f"\nAltitudes : {heights.min():.2f} .. {heights.max():.2f} m "
          f"(denivele {heights.max() - heights.min():.2f} m)"
          + ("" if absolute else "  [ramenees a zero]"))

    label = os.path.splitext(os.path.splitext(os.path.basename(args.input))[0])[0]
    if args.report_only:
        print("\n--report-only : rien n'a ete ecrit.")
        return 0

    epsg = 2154  # Lambert 93. Les dalles LiDAR HD metropolitaines y sont toutes.
    write_vchm(args.output, heights, args.cell, x_min, y_max, label, epsg, absolute)

    written = os.path.getsize(args.output)
    print(f"\nSortie  : {args.output}  ({human_size(written)})")
    print(f"  origine (echantillon 0,0 = coin NORD-OUEST) : "
          f"E {x_min}, N {y_max}  [EPSG:{epsg}]")
    print(f"  emprise monde : {(nx - 1) * args.cell:.0f} x {(nz - 1) * args.cell:.0f} m")
    print(f"Termine en {time.time() - started:.1f} s.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
