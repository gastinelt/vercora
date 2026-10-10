#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""
find_climb_walls.py — reperage des PAROIS grimpables dans une dalle .vchm.

    python tools/find_climb_walls.py assets/terrain/LHD_0903_6439_1m.vchm
    python tools/find_climb_walls.py DALLE.vchm --half-base 2 --threshold 1.0

Pourquoi cet outil existe (Phase 14, escalade).

Le seuil de pente a partir duquel une zone est une PAROI et non un versant raide
ne peut pas etre pose a priori : il faut savoir ce que le terrain reel contient
reellement. Le meme piege qu'en Phase 13 (velo) est present ici, en pire - un
DTM LiDAR a 1 m porte du bruit decimetrique, donc lire la pente sur une base
d'un metre fabrique des "parois" partout ou il n'y a qu'un caillou. La pente est
donc lue par differences finies CENTREES sur une demi-base reglable, exactement
comme world/Bike (BikeParams::slopeSampleStep), et la valeur par defaut est la
meme : 2 m, soit une base de 4 m.

Ce que le script produit :
  1. la DISTRIBUTION des pentes de la dalle, qui dit si un seuil separe vraiment
     deux populations ou s'il coupe au milieu d'un continuum ;
  2. les meilleures PAROIS CANDIDATES, c'est-a-dire les zones qui reunissent les
     trois conditions qu'une paroi doit remplir pour etre jouable :
       - pente soutenue au-dessus du seuil sur toute la zone (pas un pixel isole),
       - denivele suffisant pour que grimper ait un sens,
       - du terrain MARCHABLE au pied, sinon on ne peut pas y arriver.
  3. pour chaque candidate, les coordonnees Lambert 93 ET les coordonnees du
     monde moteur (terrain centre sur l'origine, cf. Heightmap), donc directement
     utilisables avec --start=est,nord.

Le georeferencement est lu dans l'en-tete du fichier, jamais recopie a la main
(meme regle qu'en Phase 11 pour l'orthophoto).
"""

from __future__ import annotations

import argparse
import os
import struct
import sys

import numpy as np

HEADER_SIZE = 120


def read_vchm(path):
    """Relit un .vchm. Format identique a tools/check_terrain_file.py."""
    with open(path, "rb") as f:
        header = f.read(HEADER_SIZE)
        if len(header) != HEADER_SIZE or header[:4] != b"VCHM":
            sys.exit(f"{path} n'est pas un fichier .vchm.")
        (_magic, version, nx, nz, spacing, _reserved,
         origin_e, origin_n, hmin, hmax, epsg, flags) = struct.unpack_from(
            "<4sIIIfI dd ffII", header, 0)
        label = header[56:120].split(b"\0", 1)[0].decode("ascii", "replace")
        heights = np.frombuffer(f.read(), dtype="<f4")

    if heights.size != nx * nz:
        sys.exit(f"{path} : {heights.size} altitudes pour {nx}x{nz} attendues.")
    return {
        "version": version, "nx": nx, "nz": nz, "spacing": float(spacing),
        "origin_e": origin_e, "origin_n": origin_n, "min": hmin, "max": hmax,
        "epsg": epsg, "flags": flags, "label": label,
        # grid[iz, ix] : ix croissant vers l'EST, iz croissant vers le SUD.
        "grid": heights.reshape(nz, nx).astype(np.float64),
    }


def slope_field(grid, cell, half_base_m):
    """Pente (magnitude du gradient, en rise par metre horizontal).

    Differences finies CENTREES sur +/- half_base_m, comme Bike::terrainGradient.
    Une difference AVANT donnerait une pente differente selon le sens de lecture
    sur le meme versant ; une difference centree est symetrique.

    Le decalage est exprime en ECHANTILLONS, donc half_base_m doit etre un
    multiple du pas de la grille - sinon on interpolerait, ce qui lisserait une
    seconde fois et melangerait deux effets.
    """
    step = int(round(half_base_m / cell))
    if step < 1:
        sys.exit(f"--half-base {half_base_m} m est plus petit que le pas de "
                 f"la grille ({cell} m).")
    if abs(step * cell - half_base_m) > 1e-6:
        sys.exit(f"--half-base {half_base_m} m n'est pas un multiple du pas "
                 f"({cell} m) : on interpolerait, donc on lisserait deux fois.")

    base = 2.0 * step * cell
    # np.roll ferait passer le bord oppose ; on travaille donc sur l'INTERIEUR et
    # on marque le bord comme non evaluable (NaN), plutot que d'y inventer une
    # pente. Une paroi collee au bord de dalle n'est de toute facon pas jouable.
    dh_dx = np.full(grid.shape, np.nan)
    dh_dz = np.full(grid.shape, np.nan)
    dh_dx[:, step:-step] = (grid[:, 2 * step:] - grid[:, :-2 * step]) / base
    dh_dz[step:-step, :] = (grid[2 * step:, :] - grid[:-2 * step, :]) / base
    return np.hypot(dh_dx, dh_dz)


def describe_distribution(slope, threshold):
    """La distribution des pentes, qui dit si le seuil separe deux populations."""
    valid = slope[np.isfinite(slope)]
    print("\n--- Distribution des pentes de la dalle ---")
    print(f"  echantillons evaluables : {valid.size}")
    for q in (50, 75, 90, 95, 99, 99.9):
        v = np.percentile(valid, q)
        print(f"    p{q:<5} {100.0 * v:8.1f} %   ({np.degrees(np.arctan(v)):5.1f} deg)")
    print(f"    max    {100.0 * valid.max():8.1f} %   "
          f"({np.degrees(np.arctan(valid.max())):5.1f} deg)")

    print("\n  Part de la dalle au-dessus de quelques seuils candidats :")
    for t in (0.30, 0.50, 0.70, 1.00, 1.50, 2.00, 3.00):
        share = 100.0 * float(np.count_nonzero(valid >= t)) / valid.size
        mark = "  <== seuil retenu" if abs(t - threshold) < 1e-9 else ""
        print(f"    >= {100.0 * t:6.0f} % ({np.degrees(np.arctan(t)):4.1f} deg) : "
              f"{share:6.2f} % de la dalle{mark}")


def find_walls(m, slope, threshold, patch_m, walkable, min_relief, top_n,
               min_foot=0.0, max_relief=1e9, rank_by="relief"):
    """Cherche les meilleures parois candidates.

    Balayage par PATCHS carres de patch_m metres de cote, pas d'un demi-patch.
    Un patch est retenu s'il reunit les trois conditions d'une paroi jouable :

      - couverture : une part dominante du patch depasse le seuil de pente. Un
        seul pixel raide au milieu d'un pierrier n'est pas une paroi ;
      - denivele : le patch monte d'au moins min_relief metres, sinon grimper
        n'a pas de sens ;
      - PIED MARCHABLE : il existe, dans l'anneau qui entoure le patch, du
        terrain sous le seuil de marche. C'est la condition qu'on oublie et qui
        rend une paroi injouable : une falaise dont le pied est lui-meme une
        falaise ne s'atteint pas a pied.
    """
    cell = m["spacing"]
    nx, nz = m["nx"], m["nz"]
    grid = m["grid"]
    patch = max(2, int(round(patch_m / cell)))
    stride = max(1, patch // 2)

    candidates = []
    for iz in range(0, nz - patch, stride):
        for ix in range(0, nx - patch, stride):
            sub = slope[iz:iz + patch, ix:ix + patch]
            if not np.all(np.isfinite(sub)):
                continue  # touche le bord non evaluable

            covered = float(np.count_nonzero(sub >= threshold)) / sub.size
            if covered < 0.80:
                continue

            hsub = grid[iz:iz + patch, ix:ix + patch]
            relief = float(hsub.max() - hsub.min())
            if relief < min_relief or relief > max_relief:
                continue

            # Anneau de 10 m autour du patch : y a-t-il de quoi poser les pieds ?
            pad = max(1, int(round(10.0 / cell)))
            z0, z1 = max(0, iz - pad), min(nz, iz + patch + pad)
            x0, x1 = max(0, ix - pad), min(nx, ix + patch + pad)
            ring = slope[z0:z1, x0:x1]
            ring_ok = np.isfinite(ring) & (ring <= walkable)
            foot = float(np.count_nonzero(ring_ok)) / max(1, ring.size)
            if foot <= min_foot:
                continue

            candidates.append({
                "ix": ix, "iz": iz, "patch": patch,
                "covered": covered, "relief": relief, "foot": foot,
                "median_slope": float(np.median(sub)),
                "hmin": float(hsub.min()), "hmax": float(hsub.max()),
            })

    # Classement. Deux criteres, et le choix n'est PAS cosmetique :
    #  - "relief" trouve les parois les plus spectaculaires. Sur ce massif elles
    #    sont toutes injouables : 250 m de haut et 0 % de pied marchable, on ne
    #    peut meme pas les atteindre a pied ;
    #  - "foot" trouve les parois qu'on peut REELLEMENT aborder. C'est ce qu'il
    #    faut pour poser un scenario de test, et c'est le classement qui a servi
    #    a choisir la paroi de reference de la Phase 14.
    if rank_by == "foot":
        candidates.sort(key=lambda c: (c["foot"], c["relief"]), reverse=True)
    else:
        candidates.sort(key=lambda c: (c["relief"], c["median_slope"]), reverse=True)

    # On evite de rendre dix fois la meme falaise : deux candidates dont les
    # centres sont a moins d'un patch l'une de l'autre sont la meme paroi.
    kept = []
    for c in candidates:
        cx, cz = c["ix"] + c["patch"] / 2, c["iz"] + c["patch"] / 2
        if any(abs(cx - k["ix"] - k["patch"] / 2) < patch and
               abs(cz - k["iz"] - k["patch"] / 2) < patch for k in kept):
            continue
        kept.append(c)
        if len(kept) >= top_n:
            break
    return kept



def find_routes(m, slope, threshold, walkable, top_n, min_rise, max_rise):
    """Cherche des VOIES : un pied marchable, un cap, et de la paroi au-dessus.

    Pourquoi ce mode existe en plus de find_walls. find_walls repond a "ou y
    a-t-il de la paroi ?", ce qui ne suffit pas a poser un scenario jouable : il
    faut aussi savoir PAR OU on l'aborde. C'est la lecon de la Phase 13, ou le
    scenario velo a echoue trois fois avant qu'on evalue la pente LE LONG DE
    CHAQUE CAP - "faire demi-tour ne redonne pas la pente inverse", le terrain de
    l'autre cote d'un point est un AUTRE terrain.

    Pour chaque point de depart marchable et chacun des 16 caps, on remonte le
    terrain pas a pas et on mesure :
      - la hauteur reellement gagnee avant que ca ne s'aplatisse,
      - la part du trajet qui est au-dessus du seuil de paroi.
    Une bonne voie monte franchement ET est majoritairement de la paroi ; un
    simple versant raide monte aussi, mais sans jamais depasser le seuil.
    """
    cell = m["spacing"]
    nx, nz = m["nx"], m["nz"]
    grid = m["grid"]
    half = 0.5 * (nx - 1) * cell

    def h_at(wx, wz):
        ix = int(round((wx + half) / cell))
        iz = int(round((wz + half) / cell))
        return grid[min(max(iz, 0), nz - 1), min(max(ix, 0), nx - 1)]

    def slope_at(wx, wz):
        ix = int(round((wx + half) / cell))
        iz = int(round((wz + half) / cell))
        v = slope[min(max(iz, 0), nz - 1), min(max(ix, 0), nx - 1)]
        return v if np.isfinite(v) else 0.0

    headings = [i * 22.5 for i in range(16)]
    routes = []
    # Pas de 10 m sur la grille des departs : on cherche un point de depart, pas
    # a cartographier la dalle au metre pres.
    span = int(0.5 * (nx - 1) * cell) - 60
    for wz in range(-span, span, 10):
        for wx in range(-span, span, 10):
            if slope_at(wx, wz) > walkable:
                continue  # on ne part que de terrain ou l'on tient debout
            for heading in headings:
                rad = np.radians(heading)
                dx, dz = np.cos(rad), np.sin(rad)
                x, z = float(wx), float(wz)
                y0 = h_at(x, z)
                wall_steps = 0
                steps = 0
                # On avance de 1 m tant que ca monte ; des que ca redescend ou
                # que ca s'aplatit durablement, la voie est finie.
                flat_run = 0
                while steps < 120:
                    x += dx
                    z += dz
                    steps += 1
                    if slope_at(x, z) >= threshold:
                        wall_steps += 1
                        flat_run = 0
                    else:
                        flat_run += 1
                        if flat_run > 6:
                            break
                    if h_at(x, z) < y0:
                        break
                rise = h_at(x, z) - y0
                if rise < min_rise or rise > max_rise:
                    continue
                wall_share = wall_steps / max(1, steps)
                if wall_share < 0.60:
                    continue
                routes.append({
                    "wx": wx, "wz": wz, "heading": heading, "rise": rise,
                    "wall_share": wall_share, "steps": steps,
                    "y0": y0, "y1": h_at(x, z),
                })

    routes.sort(key=lambda r: (r["wall_share"], r["rise"]), reverse=True)
    kept = []
    for r in routes:
        if any(abs(r["wx"] - k["wx"]) < 40 and abs(r["wz"] - k["wz"]) < 40 for k in kept):
            continue
        kept.append(r)
        if len(kept) >= top_n:
            break

    print()
    print("--- Voies d'escalade (pied marchable + paroi au-dessus) ---")
    if not kept:
        print("  Aucune.")
        return
    for rank, r in enumerate(kept, 1):
        east = m["origin_e"] + (r["wx"] + half)
        north = m["origin_n"] - (r["wz"] + half)
        print()
        print(f"  #{rank}  depart monde x {r['wx']:+7.1f}  z {r['wz']:+7.1f}   "
              f"(E {east:.0f}  N {north:.0f})")
        print(f"      cap {r['heading']:5.1f} deg, {r['rise']:.1f} m gagnes sur "
              f"{r['steps']} m de trajet")
        print(f"      {100.0 * r['wall_share']:.0f} % du trajet est au-dessus du "
              f"seuil de paroi")
        print(f"      altitudes {r['y0']:.1f} -> {r['y1']:.1f} m, pente au depart "
              f"{100.0 * slope_at(r['wx'], r['wz']):.0f} %")


def report_walls(m, kept, threshold, walkable):
    cell = m["spacing"]
    nx, nz = m["nx"], m["nz"]
    half = 0.5 * (nx - 1) * cell

    print(f"\n--- Parois candidates (seuil {100.0 * threshold:.0f} %, "
          f"marchable <= {100.0 * walkable:.0f} %) ---")
    if not kept:
        print("  Aucune. Assouplir --threshold, --patch ou --min-relief.")
        return

    for rank, c in enumerate(kept, 1):
        cx = c["ix"] + c["patch"] / 2.0
        cz = c["iz"] + c["patch"] / 2.0
        # Lambert 93 : ix croissant vers l'est, iz croissant vers le SUD.
        east = m["origin_e"] + cx * cell
        north = m["origin_n"] - cz * cell
        # Monde moteur : terrain centre sur l'origine (cf. Heightmap).
        wx = cx * cell - half
        wz = cz * cell - half
        print(f"\n  #{rank}  E {east:9.1f}  N {north:10.1f}   "
              f"(monde moteur x {wx:+8.1f}  z {wz:+8.1f})")
        print(f"      pente mediane {100.0 * c['median_slope']:7.1f} %  "
              f"({np.degrees(np.arctan(c['median_slope'])):.1f} deg), "
              f"{100.0 * c['covered']:.0f} % du patch au-dessus du seuil")
        print(f"      altitudes {c['hmin']:.1f} .. {c['hmax']:.1f} m  "
              f"=> {c['relief']:.1f} m de denivele sur "
              f"{c['patch'] * cell:.0f} m de cote")
        print(f"      pied marchable : {100.0 * c['foot']:.0f} % de l'anneau "
              f"a 10 m est sous {100.0 * walkable:.0f} % de pente")


def profile_at(m, slope, east, north, half_base_m):
    """Detail d'un point precis, en Lambert 93. Sert a verifier une coordonnee
    citee dans les notes plutot qu'a la croire sur parole."""
    cell = m["spacing"]
    ix = int(round((east - m["origin_e"]) / cell))
    iz = int(round((m["origin_n"] - north) / cell))
    if not (0 <= ix < m["nx"] and 0 <= iz < m["nz"]):
        print(f"\n--- Sonde E {east} N {north} : HORS de cette dalle ---")
        return

    print(f"\n--- Sonde E {east:.0f} N {north:.0f} "
          f"(ix {ix}, iz {iz}, base {2 * half_base_m:.0f} m) ---")
    print(f"  altitude {m['grid'][iz, ix]:.2f} m")
    s = slope[iz, ix]
    if np.isfinite(s):
        print(f"  pente locale {100.0 * s:.1f} % "
              f"({np.degrees(np.arctan(s)):.1f} deg)")
    # Voisinage de 40 m : une paroi se juge sur son emprise, pas sur un point.
    pad = int(round(20.0 / cell))
    z0, z1 = max(0, iz - pad), min(m["nz"], iz + pad + 1)
    x0, x1 = max(0, ix - pad), min(m["nx"], ix + pad + 1)
    sub = slope[z0:z1, x0:x1]
    hsub = m["grid"][z0:z1, x0:x1]
    valid = sub[np.isfinite(sub)]
    if valid.size:
        print(f"  voisinage 40 x 40 m : pente mediane {100.0 * np.median(valid):.1f} %, "
              f"max {100.0 * valid.max():.1f} %")
        print(f"                        altitudes {hsub.min():.1f} .. {hsub.max():.1f} m "
              f"({hsub.max() - hsub.min():.1f} m de denivele)")


def main():
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("vchm", help="dalle ou terrain assemble .vchm")
    p.add_argument("--half-base", type=float, default=2.0,
                   help="demi-base des differences centrees, en m (defaut 2, "
                        "comme BikeParams::slopeSampleStep)")
    p.add_argument("--threshold", type=float, default=1.00,
                   help="seuil de PAROI en rise/run (defaut 1.00 = 45 deg)")
    p.add_argument("--walkable", type=float, default=0.60,
                   help="pente au-dessus de laquelle on ne marche plus "
                        "(defaut 0.60 = 31 deg), pour juger le pied de paroi")
    p.add_argument("--patch", type=float, default=20.0,
                   help="cote du patch analyse, en m (defaut 20)")
    p.add_argument("--min-relief", type=float, default=15.0,
                   help="denivele minimal d'un patch pour etre une paroi (m)")
    p.add_argument("--top", type=int, default=5, help="nombre de candidates")
    p.add_argument("--min-foot", type=float, default=0.0,
                   help="part MINIMALE de l'anneau qui doit etre marchable, dans "
                        "[0, 1]. C'est le filtre de REACHABILITE : sans lui on ne "
                        "trouve que des falaises qu'on ne peut pas aborder a pied.")
    p.add_argument("--max-relief", type=float, default=1e9,
                   help="denivele MAXIMAL d'un patch, en m. Une paroi de 250 m ne "
                        "se grimpe pas avec une jauge d'energie de premiere session.")
    p.add_argument("--rank", choices=("relief", "foot"), default="relief",
                   help="critere de classement (defaut relief ; 'foot' privilegie "
                        "les parois reellement abordables)")
    p.add_argument("--probe", nargs=2, type=float, metavar=("EST", "NORD"),
                   help="detaille un point Lambert 93 precis")
    p.add_argument("--routes", action="store_true",
                   help="cherche des VOIES (pied marchable + cap + paroi au-dessus) "
                        "au lieu de simples zones de paroi. C'est ce mode qui donne "
                        "un point de depart jouable.")
    args = p.parse_args()

    m = read_vchm(args.vchm)
    print(f"Fichier : {os.path.basename(args.vchm)}")
    print(f"  label   : {m['label']}")
    print(f"  {m['nx']} x {m['nz']} echantillons, pas {m['spacing']} m, EPSG:{m['epsg']}")
    print(f"  origine (coin nord-ouest) : E {m['origin_e']:.1f}  N {m['origin_n']:.1f}")
    print(f"  altitudes {m['grid'].min():.1f} .. {m['grid'].max():.1f} m")

    slope = slope_field(m["grid"], m["spacing"], args.half_base)
    print(f"\n  Pente lue par differences centrees sur une base de "
          f"{2 * args.half_base:.0f} m.")

    describe_distribution(slope, args.threshold)

    if args.probe:
        profile_at(m, slope, args.probe[0], args.probe[1], args.half_base)

    if args.routes:
        find_routes(m, slope, args.threshold, args.walkable, args.top,
                    args.min_relief, args.max_relief)
        return

    kept = find_walls(m, slope, args.threshold, args.patch, args.walkable,
                      args.min_relief, args.top, args.min_foot, args.max_relief,
                      args.rank)
    report_walls(m, kept, args.threshold, args.walkable)


if __name__ == "__main__":
    main()
