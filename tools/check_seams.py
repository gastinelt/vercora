#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""
check_seams.py — controle croise d'un terrain ASSEMBLE contre les nuages bruts,
le long de TOUTES les frontieres entre dalles.

    python tools/check_seams.py assets/terrain/vercora_zone_1m.vchm data/lidar/*.laz

Pendant de tools/check_terrain_file.py (une dalle, sondes nommees) pour le cas
multi-dalles. Meme principe, et c'est ce qui en fait une preuve : on relit le
.vchm produit et on le confronte aux points SOL BRUTS des .laz, sans rien
partager avec assemble_terrain.py ni avec lidar_to_heightmap.py. Une erreur
d'indexation dans l'un ne peut donc pas se compenser dans l'autre.

CE QUI EST VERIFIE, ET POURQUOI C'EST LE SUJET DE LA PHASE 9

1. JUSTESSE — a chaque sonde placee sur une jointure, l'altitude assemblee doit
   valoir la mediane des points sol reels du voisinage, points pris dans les DEUX
   dalles voisines indifferemment.

2. ALIGNEMENT (le controle decisif) — la meme comparaison est refaite en
   DECALANT la lecture d'un noeud vers l'est, l'ouest, le nord et le sud. Un
   assemblage decale d'une colonne resterait plausible sur du relief doux : c'est
   le CONTRASTE entre la lecture directe et les quatre lectures decalees qui
   prouve l'alignement, exactement comme le test nord/sud de la Phase 7. Si un
   decalage colle aussi bien que la lecture directe, la sonde ne prouve rien et
   c'est signale.

3. TEMOINS — les memes mesures sur des sondes prises LOIN de toute jointure. Si
   l'ecart aux jointures ressemble a l'ecart des temoins, le raccord n'introduit
   aucune degradation. C'est la comparaison qui compte, pas la valeur absolue :
   sous couvert forestier dense, meme au milieu d'une dalle, l'ecart n'est jamais
   nul.

Les frontieres sont deduites des NOMS des .laz (nomenclature IGN
LHD_FXX_<Xkm>_<Ykm>_...), pas de la maniere dont le terrain a ete assemble.
"""

from __future__ import annotations

import argparse
import os
import struct
import sys

import numpy as np

try:
    import laspy
except ImportError:
    sys.exit('laspy est absent : python -m pip install "laspy[lazrs]" numpy')

HEADER_SIZE = 120
GROUND_CLASS = 2


def read_vchm(path):
    with open(path, "rb") as f:
        header = f.read(HEADER_SIZE)
        if header[:4] != b"VCHM":
            sys.exit(f"{path} : ce n'est pas un .vchm")
        nx = struct.unpack_from("<I", header, 8)[0]
        nz = struct.unpack_from("<I", header, 12)[0]
        spacing = struct.unpack_from("<f", header, 16)[0]
        east = struct.unpack_from("<d", header, 24)[0]
        north = struct.unpack_from("<d", header, 32)[0]
        epsg = struct.unpack_from("<I", header, 48)[0]
        label = header[56:120].split(b"\x00")[0].decode("utf-8", "replace")
        grid = np.frombuffer(f.read(nx * nz * 4), dtype="<f4").reshape(nz, nx)
    return dict(nx=nx, nz=nz, spacing=float(spacing), east=east, north=north,
                epsg=epsg, label=label, grid=grid)


def tile_bounds(path):
    """(x_min, y_min, x_max, y_max) nominal d'une dalle, lu dans son nom."""
    parts = os.path.basename(path).split("_")
    for i in range(len(parts) - 1):
        a, b = parts[i], parts[i + 1]
        if a.isdigit() and b.isdigit() and len(a) == 4 and len(b) == 4:
            x = int(a) * 1000
            y = int(b) * 1000
            return (x, y - 1000, x + 1000, y)
    return None


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Verifie les jointures d'un terrain assemble contre les nuages bruts.")
    parser.add_argument("vchm")
    parser.add_argument("laz", nargs="+")
    parser.add_argument("--step", type=float, default=25.0,
                        help="espacement des sondes le long d'une jointure, en m "
                             "(defaut 25)")
    parser.add_argument("--min-points", type=int, default=4,
                        help="points sol minimum pour qu'une sonde compte (defaut 4)")
    parser.add_argument("--tolerance", type=float, default=0.50,
                        help="ecart median tolere aux jointures, en m (defaut 0.50)")
    args = parser.parse_args()

    m = read_vchm(args.vchm)
    nx, nz, cell = m["nx"], m["nz"], m["spacing"]
    grid = m["grid"].astype(np.float64)
    east0, north0 = m["east"], m["north"]

    print(f"Terrain : {os.path.basename(args.vchm)}  ('{m['label']}')")
    print(f"  {nx} x {nz} echantillons, pas {cell} m, EPSG:{m['epsg']}")
    print(f"  coin nord-ouest : E {east0:.0f}, N {north0:.0f}")
    print(f"  emprise : E {east0:.0f}..{east0 + (nx - 1) * cell:.0f}, "
          f"N {north0 - (nz - 1) * cell:.0f}..{north0:.0f}")

    bounds = {}
    for path in args.laz:
        b = tile_bounds(path)
        if b is None:
            print(f"  ATTENTION : nom non reconnu, dalle ignoree : "
                  f"{os.path.basename(path)}")
            continue
        bounds[path] = b

    if not bounds:
        sys.exit("Aucune dalle exploitable.")

    xs = sorted({b[0] for b in bounds.values()})
    ys = sorted({b[3] for b in bounds.values()})
    print(f"  {len(bounds)} dalle(s), grille {len(xs)} x {len(ys)}")

    # --- Frontieres interieures ---------------------------------------------
    # Une frontiere verticale existe a chaque X de debut de dalle qui n'est pas
    # le bord ouest du terrain ; idem au sud pour les horizontales.
    seam_x = [x for x in xs if x > min(xs)]
    seam_y = [y - 1000 for y in ys if y - 1000 > min(b[1] for b in bounds.values())]

    print(f"  frontieres interieures : {len(seam_x)} verticale(s) "
          f"{[f'E{v}' for v in seam_x]}, {len(seam_y)} horizontale(s) "
          f"{[f'N{v}' for v in seam_y]}")

    if not seam_x and not seam_y:
        print("\nAucune frontiere interieure : rien a verifier.")
        return 0

    # --- Sondes ---------------------------------------------------------------
    # Chaque sonde est un NOEUD de la grille, repere par ses coordonnees
    # Lambert 93. On evite les 2 noeuds extremes de chaque jointure (les coins du
    # terrain n'ont pas de voisinage complet).
    e_min, e_max = east0, east0 + (nx - 1) * cell
    n_min, n_max = north0 - (nz - 1) * cell, north0

    probes = []  # (categorie, E, N)
    for sx in seam_x:
        n = n_min + args.step
        while n <= n_max - args.step:
            probes.append((f"V E{sx}", float(sx), float(round(n))))
            n += args.step
    for sy in seam_y:
        e = e_min + args.step
        while e <= e_max - args.step:
            probes.append((f"H N{sy}", float(round(e)), float(sy)))
            e += args.step

    # Temoins : loin de toute jointure (au moins 100 m), semis regulier decale
    # pour ne jamais retomber sur une frontiere.
    controls = 0
    e = e_min + 137.0
    while e <= e_max - 137.0:
        n = n_min + 211.0
        while n <= n_max - 211.0:
            far_x = all(abs(e - sx) > 100 for sx in seam_x) if seam_x else True
            far_y = all(abs(n - sy) > 100 for sy in seam_y) if seam_y else True
            if far_x and far_y:
                probes.append(("temoin", float(round(e)), float(round(n))))
                controls += 1
            n += 500.0
        e += 500.0

    print(f"  sondes : {len(probes) - controls} sur jointure, {controls} temoins "
          f"(pas de {args.step:g} m)")

    # Index de grille de chaque sonde (meme regle d'affectation que le
    # convertisseur : plus proche noeud, floor(t + 0.5)).
    probe_ix = np.array([int(np.floor((p[1] - east0) / cell + 0.5)) for p in probes])
    probe_iz = np.array([int(np.floor((north0 - p[2]) / cell + 0.5)) for p in probes])

    inside = (probe_ix >= 1) & (probe_ix < nx - 1) & (probe_iz >= 1) & (probe_iz < nz - 1)

    # Le croisement de deux jointures est engendre une fois par chacune : on ne
    # garde qu'une sonde par noeud (la premiere, donc la jointure verticale).
    seen: set[tuple[int, int]] = set()
    unique = []
    for index, keep in enumerate(inside):
        node = (int(probe_ix[index]), int(probe_iz[index]))
        if keep and node not in seen:
            seen.add(node)
            unique.append(index)
    unique = np.asarray(unique, dtype=np.int64)

    duplicates = int(inside.sum()) - len(unique)
    if duplicates:
        print(f"  ({duplicates} sonde(s) en double au croisement de deux "
              f"jointures, dedoublonnee(s))")

    probes = [probes[i] for i in unique]
    probe_ix, probe_iz = probe_ix[unique], probe_iz[unique]
    count = len(probes)

    # --- Collecte des points sol bruts autour de chaque sonde ----------------
    # Une passe par .laz, en flux : chaque point est rattache au noeud le plus
    # proche, et on ne garde que ceux qui tombent sur une sonde. Les points d'une
    # sonde placee sur une jointure viennent donc des DEUX dalles voisines, ce
    # qui est exactement le voisinage que le noeud partage devrait decrire.
    keys = probe_ix.astype(np.int64) * np.int64(1 << 20) + probe_iz.astype(np.int64)
    order = np.argsort(keys)
    sorted_keys = keys[order]
    if len(np.unique(sorted_keys)) != len(sorted_keys):
        sys.exit("Sondes en double : ajuster --step.")

    buckets: list[list[np.ndarray]] = [[] for _ in range(count)]

    print(f"\nLecture des nuages bruts (classe {GROUND_CLASS})...")
    for index, path in enumerate(sorted(bounds), 1):
        total = 0
        with laspy.open(path) as reader:
            for chunk in reader.chunk_iterator(4_000_000):
                keep = np.asarray(chunk.classification) == GROUND_CLASS
                if not keep.any():
                    continue
                px = np.asarray(chunk.x)[keep]
                py = np.asarray(chunk.y)[keep]
                pz = np.asarray(chunk.z)[keep]

                ix = np.floor((px - east0) / cell + 0.5).astype(np.int64)
                iz = np.floor((north0 - py) / cell + 0.5).astype(np.int64)
                key = ix * np.int64(1 << 20) + iz

                position = np.searchsorted(sorted_keys, key)
                position[position >= len(sorted_keys)] = 0
                hit = sorted_keys[position] == key
                if not hit.any():
                    continue
                for slot, value in zip(position[hit], pz[hit]):
                    buckets[order[slot]].append(value)
                total += int(hit.sum())

        sys.stdout.write(f"\r  {index}/{len(bounds)} {os.path.basename(path)[:44]:<44s} "
                         f"{total:>7,} point(s) retenus   ")
        sys.stdout.flush()
    print("\r" + " " * 90 + "\r", end="")

    # --- Comparaison ----------------------------------------------------------
    shifts = {"direct": (0, 0), "est +1": (1, 0), "ouest -1": (-1, 0),
              "sud +1": (0, 1), "nord -1": (0, -1)}
    errors: dict[str, dict[str, list[float]]] = {
        name: {"jointure": [], "temoin": []} for name in shifts}

    weak = 0
    used = 0
    per_seam: dict[str, list[float]] = {}

    for i, (category, e, n) in enumerate(probes):
        values = buckets[i]
        if len(values) < args.min_points:
            weak += 1
            continue
        reference = float(np.median(np.asarray(values, dtype=np.float64)))
        used += 1
        group = "temoin" if category == "temoin" else "jointure"

        for name, (dx, dz) in shifts.items():
            jx, jz = probe_ix[i] + dx, probe_iz[i] + dz
            errors[name][group].append(abs(float(grid[jz, jx]) - reference))

        if group == "jointure":
            per_seam.setdefault(category, []).append(
                abs(float(grid[probe_iz[i], probe_ix[i]]) - reference))

    print(f"Sondes exploitables : {used}/{count} "
          f"({weak} rejetee(s) faute de points sol — zone sous couvert ou hors dalle)")

    if not used:
        sys.exit("Aucune sonde exploitable : verifier que les .laz couvrent bien "
                 "l'emprise du terrain.")

    # 1 + 2 : justesse et alignement, par le contraste
    print(f"\n{'lecture':<12} {'jointures: median':>18} {'max':>9} "
          f"{'| temoins: median':>20} {'max':>9}")
    print("-" * 72)
    direct_seam = None
    for name in shifts:
        seam_values = errors[name]["jointure"]
        control_values = errors[name]["temoin"]
        seam_median = float(np.median(seam_values)) if seam_values else float("nan")
        seam_max = float(np.max(seam_values)) if seam_values else float("nan")
        control_median = float(np.median(control_values)) if control_values else float("nan")
        control_max = float(np.max(control_values)) if control_values else float("nan")
        if name == "direct":
            direct_seam = seam_median
        mark = "  <- lecture reelle" if name == "direct" else ""
        print(f"{name:<12} {seam_median:>17.3f}m {seam_max:>8.3f}m "
              f"{control_median:>19.3f}m {control_max:>8.3f}m{mark}")

    # 3 : detail par frontiere — aucune ne doit se distinguer des autres
    print(f"\nDetail par frontiere ({'V' } = verticale, 'H' = horizontale) :")
    print(f"  {'frontiere':<14} {'sondes':>7} {'median':>9} {'moyen':>9} {'max':>9}")
    worst_seam = 0.0
    for name in sorted(per_seam):
        values = np.asarray(per_seam[name])
        worst_seam = max(worst_seam, float(np.median(values)))
        print(f"  {name:<14} {values.size:>7} {np.median(values):>8.3f}m "
              f"{values.mean():>8.3f}m {values.max():>8.3f}m")

    # --- Verdict --------------------------------------------------------------
    print()
    failures = []

    shifted = [float(np.median(errors[name]["jointure"]))
               for name in shifts if name != "direct"]
    best_shift = min(shifted)
    if direct_seam is None or not (direct_seam < best_shift):
        failures.append(
            f"ALIGNEMENT : la lecture directe ({direct_seam:.3f} m) n'est pas "
            f"meilleure que le meilleur decalage ({best_shift:.3f} m). "
            f"L'assemblage est probablement decale d'un noeud.")
    else:
        ratio = best_shift / direct_seam if direct_seam > 0 else float("inf")
        print(f"ALIGNEMENT OK : lecture directe {direct_seam:.3f} m contre "
              f"{best_shift:.3f} m pour le meilleur decalage "
              f"(x{ratio:.1f}). Le contraste prouve que les jointures tombent "
              f"exactement sur le bon noeud.")

    if worst_seam > args.tolerance:
        failures.append(f"JUSTESSE : une frontiere depasse la tolerance "
                        f"({worst_seam:.3f} m > {args.tolerance:.2f} m).")
    else:
        print(f"JUSTESSE OK : toutes les frontieres sous {args.tolerance:.2f} m "
              f"(pire mediane {worst_seam:.3f} m).")

    control_median = float(np.median(errors["direct"]["temoin"])) \
        if errors["direct"]["temoin"] else float("nan")
    if np.isfinite(control_median):
        if direct_seam > max(3.0 * control_median, control_median + 0.30):
            failures.append(
                f"DEGRADATION : l'ecart aux jointures ({direct_seam:.3f} m) est "
                f"nettement pire qu'en plein milieu de dalle "
                f"({control_median:.3f} m).")
        else:
            print(f"NON-DEGRADATION OK : jointures {direct_seam:.3f} m vs "
                  f"plein milieu de dalle {control_median:.3f} m — le raccord "
                  f"n'est pas moins fidele que le reste du terrain.")

    if failures:
        print()
        for f in failures:
            print(f"  ECHEC — {f}")
        return 1

    print("\nToutes les frontieres sont conformes.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
