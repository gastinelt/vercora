#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""
assemble_terrain.py — raccorde N heightmaps .vchm voisines en UN seul terrain.

PHASE 9. La Phase 7 avait valide le pipeline sur une dalle isolee ; c'est ici que
se joue ce que ses deux bugs d'arrondi auraient casse en silence (arrondi
bancaire, intervalle semi-ouvert inverse sur l'axe nord). Rien n'est suppose :
chaque dalle est placee par son GEOREFERENCEMENT (originEast/originNorth lus dans
son en-tete), jamais par son ordre dans la ligne de commande ou son nom.

    python tools/assemble_terrain.py assets/terrain/LHD_*_1m.vchm \
        -o assets/terrain/vercora_zone_1m.vchm --label vercora_zone_5x5

Options :
    --label L     nom inscrit dans l'en-tete du fichier produit
    --allow-gaps  accepter une grille incomplete (trous combles par voisinage)
    --quiet-seams n'afficher que le bilan des jointures, pas le detail

LE NOEUD PARTAGE — le point non evident de tout l'assemblage. Les echantillons
sont sur les NOEUDS : une dalle de 1 km au pas de 1 m fait 1001 echantillons, de
X=899000 a X=900000 INCLUS. La dalle voisine commence a X=900000, elle aussi
inclus. La colonne X=900000 existe donc DANS LES DEUX, et 5 dalles alignees font
5*1001-4 = 5001 echantillons, pas 5005.

Concatener betement produirait 5005 colonnes : le terrain serait 4 m trop large
et decalerait tout ce qui est a l'est d'une jointure. C'est precisement la
"duplication de ligne/colonne a la jointure" a ne pas commettre.

Mais ces deux valeurs partagees ne sont PAS identiques, et c'est normal : le
noeud X=900000 agrege les points de [899999.5, 900000.5[. La dalle ouest n'en
voit que la moitie gauche, la dalle est que la moitie droite. Chacune calcule
donc une mediane sur la moitie des points. L'ecart entre les deux est une mesure
DIRECTE de la coherence du raccord — c'est le chiffre que ce script rapporte, et
il doit rester de l'ordre du bruit de mesure (centimetres), pas du metre. La
valeur retenue est leur MOYENNE.
"""

from __future__ import annotations

import argparse
import os
import struct
import sys

import numpy as np

MAGIC = b"VCHM"
VERSION = 1
HEADER_SIZE = 120
LABEL_SIZE = 64
FLAG_ABSOLUTE_ALTITUDE = 1 << 0


def read_vchm(path: str):
    with open(path, "rb") as f:
        header = f.read(HEADER_SIZE)
        if len(header) != HEADER_SIZE or header[:4] != MAGIC:
            sys.exit(f"{path} : ce n'est pas un fichier .vchm")
        version = struct.unpack_from("<I", header, 4)[0]
        if version != VERSION:
            sys.exit(f"{path} : version {version} inconnue")
        samples_x = struct.unpack_from("<I", header, 8)[0]
        samples_z = struct.unpack_from("<I", header, 12)[0]
        spacing = struct.unpack_from("<f", header, 16)[0]
        origin_east = struct.unpack_from("<d", header, 24)[0]
        origin_north = struct.unpack_from("<d", header, 32)[0]
        epsg = struct.unpack_from("<I", header, 48)[0]
        flags = struct.unpack_from("<I", header, 52)[0]
        label = header[56:56 + LABEL_SIZE].split(b"\x00")[0].decode("utf-8", "replace")

        count = samples_x * samples_z
        data = f.read(count * 4)
        if len(data) != count * 4:
            sys.exit(f"{path} : fichier tronque")
        heights = np.frombuffer(data, dtype="<f4").reshape(samples_z, samples_x)

    return {
        "path": path, "nx": samples_x, "nz": samples_z, "spacing": spacing,
        "east": origin_east, "north": origin_north, "epsg": epsg,
        "flags": flags, "label": label, "heights": heights,
    }


def write_vchm(path, heights, spacing, origin_east, origin_north, label, epsg, flags):
    nz, nx = heights.shape
    header = bytearray(HEADER_SIZE)
    header[0:4] = MAGIC
    struct.pack_into("<I", header, 4, VERSION)
    struct.pack_into("<I", header, 8, nx)
    struct.pack_into("<I", header, 12, nz)
    struct.pack_into("<f", header, 16, float(spacing))
    struct.pack_into("<I", header, 20, 0)
    struct.pack_into("<d", header, 24, float(origin_east))
    struct.pack_into("<d", header, 32, float(origin_north))
    struct.pack_into("<f", header, 40, float(np.nanmin(heights)))
    struct.pack_into("<f", header, 44, float(np.nanmax(heights)))
    struct.pack_into("<I", header, 48, int(epsg))
    struct.pack_into("<I", header, 52, int(flags))
    encoded = label.encode("utf-8")[:LABEL_SIZE - 1]
    header[56:56 + len(encoded)] = encoded

    with open(path, "wb") as f:
        f.write(header)
        f.write(np.ascontiguousarray(heights, dtype="<f4").tobytes())


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Raccorde N heightmaps .vchm voisines en un seul terrain.")
    parser.add_argument("inputs", nargs="+")
    parser.add_argument("-o", "--output", required=True)
    parser.add_argument("--label", default="")
    parser.add_argument("--allow-gaps", action="store_true")
    parser.add_argument("--quiet-seams", action="store_true")
    args = parser.parse_args()

    tiles = [read_vchm(p) for p in args.inputs]
    print(f"Entree  : {len(tiles)} dalle(s)")

    # --- Coherence globale ---------------------------------------------------
    spacing = tiles[0]["spacing"]
    epsg = tiles[0]["epsg"]
    flags = tiles[0]["flags"]
    for t in tiles:
        if abs(t["spacing"] - spacing) > 1e-6:
            sys.exit(f"{t['path']} : pas de {t['spacing']} m, attendu {spacing} m. "
                     f"Toutes les dalles doivent avoir la meme resolution.")
        if t["epsg"] != epsg:
            sys.exit(f"{t['path']} : EPSG {t['epsg']}, attendu {epsg}.")
        if t["flags"] != flags:
            sys.exit(f"{t['path']} : flags {t['flags']}, attendu {flags} "
                     f"(melange d'altitudes absolues et relatives ?)")

    # --- Reseau global : chaque dalle placee par son georeferencement ---------
    global_east = min(t["east"] for t in tiles)
    global_north = max(t["north"] for t in tiles)

    for t in tiles:
        # Decalage en NOEUDS depuis le coin nord-ouest global. L'axe nord
        # DESCEND quand iz monte (lignes stockees du nord vers le sud), d'ou le
        # sens oppose des deux expressions.
        offset_x = (t["east"] - global_east) / spacing
        offset_z = (global_north - t["north"]) / spacing
        # Une dalle qui ne tombe pas sur le reseau (decalage non entier) rendrait
        # tout l'assemblage faux d'une fraction de cellule, sans rien casser de
        # visible : on refuse.
        if abs(offset_x - round(offset_x)) > 1e-6 or abs(offset_z - round(offset_z)) > 1e-6:
            sys.exit(f"{t['path']} : origine (E {t['east']}, N {t['north']}) hors "
                     f"du reseau au pas de {spacing} m "
                     f"(decalage {offset_x}, {offset_z} noeuds).")
        t["ox"] = int(round(offset_x))
        t["oz"] = int(round(offset_z))

    total_nx = max(t["ox"] + t["nx"] for t in tiles)
    total_nz = max(t["oz"] + t["nz"] for t in tiles)

    print(f"  pas {spacing} m, EPSG {epsg}")
    print(f"  coin nord-ouest global : E {global_east:.0f}, N {global_north:.0f}")
    print(f"  grille assemblee : {total_nx} x {total_nz} echantillons "
          f"({(total_nx - 1) * spacing:.0f} x {(total_nz - 1) * spacing:.0f} m)")

    naive = sum(t["nx"] for t in tiles if t["oz"] == 0)
    if naive and naive != total_nx:
        print(f"  (une concatenation naive aurait donne {naive} colonnes : "
              f"les {naive - total_nx} noeuds de bord sont PARTAGES, pas dupliques)")

    # --- Report ---------------------------------------------------------------
    accumulator = np.zeros((total_nz, total_nx), dtype=np.float64)
    contributions = np.zeros((total_nz, total_nx), dtype=np.int32)

    for t in tiles:
        z0, x0 = t["oz"], t["ox"]
        view = t["heights"].astype(np.float64)
        if not np.isfinite(view).all():
            sys.exit(f"{t['path']} : contient des valeurs non finies (NaN/inf). "
                     f"Le convertisseur aurait du les boucher.")
        accumulator[z0:z0 + t["nz"], x0:x0 + t["nx"]] += view
        contributions[z0:z0 + t["nz"], x0:x0 + t["nx"]] += 1

    covered = contributions > 0
    missing = int((~covered).sum())
    if missing:
        message = (f"{missing} noeud(s) ({100.0 * missing / covered.size:.2f} %) "
                   f"ne sont couverts par aucune dalle")
        if not args.allow_gaps:
            sys.exit(f"  ERREUR : {message}. La grille de dalles est incomplete. "
                     f"Utiliser --allow-gaps pour passer outre.")
        print(f"  ATTENTION : {message} — combles par voisinage.")

    with np.errstate(invalid="ignore"):
        heights = accumulator / np.maximum(contributions, 1)
    heights[~covered] = np.nan

    # --- LE CONTROLE : ecart entre les deux versions d'un noeud partage -------
    shared = contributions > 1
    shared_count = int(shared.sum())
    print(f"\nJointures : {shared_count:,} noeud(s) partage(s) par "
          f"{contributions.max()} dalles au plus")

    worst_overall = 0.0
    if shared_count:
        # Pour chaque paire de dalles qui se recouvrent, mesurer l'ecart entre
        # ce que chacune dit du meme point du sol. C'est la mesure directe de la
        # coherence du raccord : elle ne peut pas etre "compensee" par le
        # placement, puisqu'elle compare deux valeurs a la MEME position.
        seams = []
        for i, a in enumerate(tiles):
            for b in tiles[i + 1:]:
                x0 = max(a["ox"], b["ox"])
                x1 = min(a["ox"] + a["nx"], b["ox"] + b["nx"])
                z0 = max(a["oz"], b["oz"])
                z1 = min(a["oz"] + a["nz"], b["oz"] + b["nz"])
                if x1 <= x0 or z1 <= z0:
                    continue
                va = a["heights"][z0 - a["oz"]:z1 - a["oz"],
                                  x0 - a["ox"]:x1 - a["ox"]].astype(np.float64)
                vb = b["heights"][z0 - b["oz"]:z1 - b["oz"],
                                  x0 - b["ox"]:x1 - b["ox"]].astype(np.float64)
                delta = np.abs(va - vb)
                kind = ("colonne" if (x1 - x0) == 1 else
                        "ligne" if (z1 - z0) == 1 else "bloc")
                seams.append((a["label"] or os.path.basename(a["path"]),
                              b["label"] or os.path.basename(b["path"]),
                              kind, delta.size, float(delta.mean()),
                              float(np.median(delta)), float(delta.max())))
                worst_overall = max(worst_overall, float(delta.max()))

        seams.sort(key=lambda s: -s[6])
        if not args.quiet_seams:
            print(f"  {'dalle A':<20s} {'dalle B':<20s} {'type':<8s} "
                  f"{'noeuds':>7s} {'moyen':>8s} {'median':>8s} {'max':>8s}")
            for a, b, kind, n, mean, med, mx in seams:
                print(f"  {a[:20]:<20s} {b[:20]:<20s} {kind:<8s} {n:>7,} "
                      f"{mean:>7.3f}m {med:>7.3f}m {mx:>7.3f}m")

        all_delta = np.concatenate([np.array([s[4]]) for s in seams])
        print(f"  ecart moyen sur l'ensemble des jointures : "
              f"{all_delta.mean():.3f} m, pire ecart ponctuel : {worst_overall:.3f} m")

    # --- Continuite mesuree sur le produit fini ------------------------------
    # Test independant du placement : le long d'une jointure, la difference
    # entre deux colonnes voisines doit ressembler a celle qu'on observe
    # PARTOUT ailleurs. Un decalage d'un metre la rendrait anormalement GRANDE ;
    # une colonne dupliquee la rendrait anormalement PETITE (deux colonnes
    # identiques = pente nulle). Les deux defauts sont donc attrapes, et par une
    # mesure faite sur le fichier final, sans rien savoir des dalles d'origine.
    print("\nContinuite du terrain assemble (gradient aux jointures vs partout) :")
    finite = np.isfinite(heights)
    grad_x = np.abs(np.diff(heights, axis=1))
    grad_z = np.abs(np.diff(heights, axis=0))
    reference_x = float(np.nanmedian(grad_x))
    reference_z = float(np.nanmedian(grad_z))
    print(f"  reference : pente mediane est-ouest {reference_x:.3f} m/m, "
          f"nord-sud {reference_z:.3f} m/m")

    seam_columns = sorted({t["ox"] for t in tiles if t["ox"] > 0})
    seam_rows = sorted({t["oz"] for t in tiles if t["oz"] > 0})
    anomalies = 0

    for column in seam_columns:
        # Le raccord se joue entre les colonnes column-1 et column+1 autour du
        # noeud partage `column` : on regarde les deux transitions qui l'encadrent.
        left = float(np.nanmedian(grad_x[:, column - 1]))
        right = float(np.nanmedian(grad_x[:, column]))
        flag = ""
        for value in (left, right):
            if reference_x > 0 and (value > 6.0 * reference_x or value < reference_x / 6.0):
                flag = "  <-- ANOMALIE"
                anomalies += 1
        print(f"  jointure verticale x={column:>5d} : "
              f"{left:.3f} / {right:.3f} m/m{flag}")

    for row in seam_rows:
        top = float(np.nanmedian(grad_z[row - 1, :]))
        bottom = float(np.nanmedian(grad_z[row, :]))
        flag = ""
        for value in (top, bottom):
            if reference_z > 0 and (value > 6.0 * reference_z or value < reference_z / 6.0):
                flag = "  <-- ANOMALIE"
                anomalies += 1
        print(f"  jointure horizontale z={row:>5d} : "
              f"{top:.3f} / {bottom:.3f} m/m{flag}")

    if anomalies:
        print(f"  {anomalies} ANOMALIE(S) : une jointure ne se comporte pas comme "
              f"le reste du terrain (decalage ou duplication).")
    elif seam_columns or seam_rows:
        print("  OK — aucune jointure ne se distingue du relief environnant.")

    # --- Ecriture -------------------------------------------------------------
    if missing:
        # Meme dilatation que le convertisseur, pour ne jamais livrer de NaN.
        from math import inf
        passes = 0
        while not np.isfinite(heights).all() and passes < 512:
            passes += 1
            filled = np.where(np.isfinite(heights), heights, 0.0)
            valid = np.isfinite(heights).astype(np.float64)
            total = np.zeros_like(filled)
            weight = np.zeros_like(valid)
            for dz, dx in ((-1, 0), (1, 0), (0, -1), (0, 1)):
                total += np.roll(np.roll(filled, dz, 0), dx, 1)
                weight += np.roll(np.roll(valid, dz, 0), dx, 1)
            candidate = np.where(weight > 0, total / np.maximum(weight, 1), np.nan)
            heights = np.where(np.isfinite(heights), heights, candidate)
        print(f"  trous combles en {passes} passes")

    if not np.isfinite(heights).all():
        sys.exit("ERREUR : des valeurs non finies subsistent, refus d'ecrire.")

    label = args.label or f"assemblage {len(tiles)} dalles"
    write_vchm(args.output, heights.astype(np.float32), spacing,
               global_east, global_north, label, epsg, flags)
    written = os.path.getsize(args.output)

    print(f"\nSortie  : {args.output}  ({written / (1 << 20):.1f} Mo)")
    print(f"  {total_nx} x {total_nz} echantillons, pas {spacing} m")
    print(f"  origine (echantillon 0,0 = coin NORD-OUEST) : "
          f"E {global_east:.0f}, N {global_north:.0f}  [EPSG:{epsg}]")
    print(f"  altitudes : {np.nanmin(heights):.2f} .. {np.nanmax(heights):.2f} m "
          f"(denivele {np.nanmax(heights) - np.nanmin(heights):.2f} m)")

    return 1 if anomalies else 0


if __name__ == "__main__":
    sys.exit(main())
