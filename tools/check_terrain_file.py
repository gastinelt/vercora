#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""
check_terrain_file.py — controle croise d'un .vchm contre le .laz d'origine.

    python tools/check_terrain_file.py DALLE.vchm NUAGE.copc.laz

Relit le fichier produit par lidar_to_heightmap.py et, pour une serie de points
de sonde, compare l'altitude stockee a la MEDIANE DES POINTS SOL BRUTS du nuage
dans un rayon d'une demi-cellule. Rien n'est partage avec le convertisseur a part
le format du fichier : une erreur d'indexation ou d'orientation dans l'un ne peut
pas se compenser dans l'autre.

CE QUE CE CONTROLE ATTRAPE, ET QUE RIEN D'AUTRE N'ATTRAPE : le relief en MIROIR.
Les lignes du .vchm vont du nord vers le sud (cf. l'en-tete de
lidar_to_heightmap.py). Se tromper de sens produit un terrain parfaitement
plausible - meme denivele, memes pentes, meme histogramme - mais retourne,
d'autant plus indetectable qu'il n'y a pas encore d'orthophoto a plaquer dessus.
Les sondes sont donc placees ASYMETRIQUEMENT : sur cette dalle les quatre bords
different de plusieurs centaines de metres, donc une inversion nord/sud creuse
un ecart enorme au lieu de passer inapercue.

Affiche aussi la conversion monde moteur <-> Lambert 93 que fait
Heightmap::toProjected, pour verifier a la main les coordonnees journalisees par
le jeu au demarrage.
"""

from __future__ import annotations

import os
import struct
import sys

import numpy as np

try:
    import laspy
except ImportError:
    sys.exit('laspy est absent : python -m pip install "laspy[lazrs]" numpy')

HEADER_SIZE = 120


def read_vchm(path):
    with open(path, "rb") as f:
        header = f.read(HEADER_SIZE)
        if len(header) != HEADER_SIZE or header[:4] != b"VCHM":
            sys.exit(f"{path} n'est pas un fichier .vchm.")
        (magic, version, nx, nz, spacing, _reserved,
         origin_e, origin_n, hmin, hmax, epsg, flags) = struct.unpack_from(
            "<4sIIIfI dd ffII", header, 0)
        label = header[56:120].split(b"\0", 1)[0].decode("ascii", "replace")
        heights = np.frombuffer(f.read(), dtype="<f4")

    if heights.size != nx * nz:
        sys.exit(f"{path} : {heights.size} altitudes pour {nx}x{nz} attendues.")
    return {
        "version": version, "nx": nx, "nz": nz, "spacing": spacing,
        "origin_e": origin_e, "origin_n": origin_n, "min": hmin, "max": hmax,
        "epsg": epsg, "flags": flags, "label": label,
        "grid": heights.reshape(nz, nx),
    }


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__.strip())
    vchm_path, laz_path = sys.argv[1], sys.argv[2]

    m = read_vchm(vchm_path)
    nx, nz, cell = m["nx"], m["nz"], m["spacing"]
    grid = m["grid"]

    print(f"Fichier : {os.path.basename(vchm_path)}")
    print(f"  label      : {m['label']}")
    print(f"  version {m['version']}, {nx} x {nz} echantillons, pas {cell} m, "
          f"EPSG:{m['epsg']}, flags {m['flags']}")
    print(f"  origine (ix=0, iz=0) : E {m['origin_e']:.1f}  N {m['origin_n']:.1f}  "
          f"(coin nord-ouest)")
    print(f"  bornes en-tete {m['min']:.2f} .. {m['max']:.2f} m   |   "
          f"donnees {grid.min():.2f} .. {grid.max():.2f} m")
    assert abs(m["min"] - grid.min()) < 0.01 and abs(m["max"] - grid.max()) < 0.01, \
        "les bornes de l'en-tete ne correspondent pas aux donnees"

    half = 0.5 * (nx - 1) * cell
    print(f"\nRepere moteur : terrain centre sur l'origine, "
          f"X et Z dans [{-half:.0f}, {half:.0f}]")
    print("  ix croissant -> EST (+X)     iz croissant -> SUD (+Z)")

    # --- Points de sonde, volontairement asymetriques ----------------------
    q = (nx - 1) // 4
    c = (nx - 1) // 2
    probes = [
        ("centre",       c,          c),
        ("nord",         c,          q),
        ("sud",          c,          3 * q),
        ("ouest",        q,          c),
        ("est",          3 * q,      c),
        ("nord-ouest",   q,          q),
        ("sud-est",      3 * q,      3 * q),
        ("coin NO",      2,          2),
        ("coin SE",      nx - 3,     nz - 3),
    ]

    print(f"\nLecture du nuage brut {os.path.basename(laz_path)} (classe 2)...")
    xs, ys, zs = [], [], []
    with laspy.open(laz_path) as reader:
        for chunk in reader.chunk_iterator(4_000_000):
            keep = np.asarray(chunk.classification) == 2
            if keep.any():
                xs.append(np.asarray(chunk.x)[keep])
                ys.append(np.asarray(chunk.y)[keep])
                zs.append(np.asarray(chunk.z)[keep])
    x = np.concatenate(xs); y = np.concatenate(ys); z = np.concatenate(zs)
    print(f"  {x.size:,} points sol")

    print(f"\n{'sonde':<12} {'ix':>5} {'iz':>5} {'monde X':>9} {'monde Z':>9} "
          f"{'E (L93)':>11} {'N (L93)':>11} {'.vchm':>9} {'brut':>9} {'ecart':>7} {'n':>5}")
    print("-" * 106)

    worst = 0.0
    checked = 0
    for name, ix, iz in probes:
        world_x = -half + ix * cell
        world_z = -half + iz * cell
        # Exactement ce que fait Heightmap::toProjected.
        east = m["origin_e"] + (world_x + half)
        north = m["origin_n"] - (world_z + half)

        stored = float(grid[iz, ix])

        # Mediane des points bruts appartenant au noeud : c'est la DEFINITION du
        # noeud - la demi-cellule [E - c/2, E + c/2[ - rejouee ici depuis le
        # nuage, sans rien emprunter au convertisseur. L'intervalle est
        # semi-ouvert des deux cotes, comme la regle d'arrondi "moitie vers le
        # haut" employee la-bas : un point pile sur la frontiere doit tomber du
        # meme cote dans les deux calculs, faute de quoi les deux medianes
        # different legerement et le controle accuse a tort le convertisseur.
        r = 0.5 * cell
        near = ((x - east >= -r) & (x - east < r) &
                (y - north >= -r) & (y - north < r))
        count = int(near.sum())
        if count == 0:
            print(f"{name:<12} {ix:5d} {iz:5d} {world_x:9.1f} {world_z:9.1f} "
                  f"{east:11.1f} {north:11.1f} {stored:9.2f} {'(trou)':>9} "
                  f"{'-':>7} {0:5d}")
            continue

        raw = float(np.median(z[near]))
        delta = stored - raw
        worst = max(worst, abs(delta))
        checked += 1
        print(f"{name:<12} {ix:5d} {iz:5d} {world_x:9.1f} {world_z:9.1f} "
              f"{east:11.1f} {north:11.1f} {stored:9.2f} {raw:9.2f} "
              f"{delta:+7.3f} {count:5d}")

    # --- Test d'orientation, sur un large echantillon de noeuds -------------
    # Comparer la moyenne d'une BANDE de la grille a celle des points bruts de la
    # meme bande serait tentant mais FAUX : la grille est ponderee par la
    # surface, alors que le nuage est ponderee par les points, dont la densite
    # s'effondre sous le couvert forestier. Sur cette dalle l'ecart atteint 30 m
    # sans qu'aucune erreur ne soit en cause.
    #
    # On generalise donc la comparaison PAR NOEUD, la seule qui compare deux fois
    # la meme chose, et on la rejoue contre les deux symetriques possibles :
    #   - lignes inversees   -> le relief serait retourne nord/sud ;
    #   - colonnes inversees -> retourne est/ouest.
    # Une orientation correcte donne un ecart direct nul et des ecarts miroir
    # enormes. C'est ce contraste qui fait la preuve, pas la valeur absolue.
    print("\nControle d'orientation sur un large echantillon de noeuds :")
    rng = np.random.default_rng(20260816)
    sample = 600
    sample_ix = rng.integers(0, nx, sample)
    sample_iz = rng.integers(0, nz, sample)

    direct, flip_ns, flip_ew, used = [], [], [], 0
    r = 0.5 * cell
    for ix, iz in zip(sample_ix.tolist(), sample_iz.tolist()):
        east = m["origin_e"] + ix * cell
        north = m["origin_n"] - iz * cell
        near = ((x - east >= -r) & (x - east < r) &
                (y - north >= -r) & (y - north < r))
        if not near.any():
            continue
        raw = float(np.median(z[near]))
        used += 1
        direct.append(abs(float(grid[iz, ix]) - raw))
        flip_ns.append(abs(float(grid[nz - 1 - iz, ix]) - raw))
        flip_ew.append(abs(float(grid[iz, nx - 1 - ix]) - raw))

    direct = np.array(direct); flip_ns = np.array(flip_ns); flip_ew = np.array(flip_ew)
    # Quelques noeuds PEUVENT s'ecarter legitimement : ce sont ceux que le filtre
    # de pics a remplaces par la mediane de leur voisinage, donc justement ceux
    # ou la valeur stockee ne DOIT pas etre celle du nuage brut. Ils sont de
    # l'ordre de 1 sur 100 000, donc invisibles sur 600 tirages, mais on compte
    # les depassements plutot que de juger sur le seul maximum.
    outliers = int((direct > 0.01).sum())
    print(f"  {used} noeuds mesures sur {sample} tires")
    print(f"  lecture DIRECTE               : ecart moyen {direct.mean():8.4f} m, "
          f"max {direct.max():8.4f} m, {outliers} noeud(s) au-dessus de 1 cm")
    print(f"  si les lignes etaient inversees (nord/sud) : ecart moyen "
          f"{flip_ns.mean():8.3f} m")
    print(f"  si les colonnes etaient inversees (est/ouest) : ecart moyen "
          f"{flip_ew.mean():8.3f} m")

    faithful = direct.mean() < 0.001 and outliers <= 2
    discriminating = flip_ns.mean() > 10.0 and flip_ew.mean() > 10.0
    ok_orientation = faithful and discriminating
    if ok_orientation:
        print("  -> lecture directe exacte, symetriques tres fausses : "
              "orientation CORRECTE et discriminee.")
    elif not faithful:
        print("  -> la lecture directe ne retrouve pas le nuage : "
              "indexation ou geo-referencement errone.")
    else:
        print("  -> le terrain est trop symetrique pour que ce test discrimine "
              "quoi que ce soit ; changer de dalle.")

    print(f"\nEcart maximal sur les {checked} sondes nommees : {worst:.3f} m")
    ok = ok_orientation and worst < 0.05 and checked >= 5
    print("[OK] Le fichier decrit bien le nuage d'origine." if ok
          else "[ECHEC] Voir ci-dessus.")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
