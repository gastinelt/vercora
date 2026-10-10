#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""
fetch_ortho.py — telecharge la BD ORTHO de l'IGN sur l'emprise d'un terrain .vchm.

PHASE 11. Le terrain LiDAR ne porte QUE de la geometrie : aucune couleur. Ce
script va chercher la photo aerienne reelle qui recouvre exactement la meme
emprise, par le service WMS raster de la Geoplateforme (data.geopf.fr), couche
HR.ORTHOIMAGERY.ORTHOPHOTOS ("Ortho 20 cm", BD ORTHO V3, licence Etalab).

    python tools/fetch_ortho.py assets/terrain/vercora_zone_1m.vchm -o data/ortho
    python tools/fetch_ortho.py assets/terrain/vercora_zone_1m.vchm -o data/ortho --res 0.2
    python tools/fetch_ortho.py assets/terrain/vercora_zone_1m.vchm --dry-run

POURQUOI L'EMPRISE EST LUE DANS LE .vchm ET NON PASSEE EN ARGUMENT. C'est le
seul moyen d'etre certain que la texture recouvre le terrain au metre pres. Une
bbox recopiee a la main dans une ligne de commande derive au premier changement
de zone, et une orthophoto decalee de quelques metres est exactement le genre de
defaut qu'on ne voit pas sur une capture d'ecran mais qui met les arbres a cote
des arbres. L'en-tete .vchm donne l'origine (noeud ix=0, iz=0 = coin NORD-OUEST),
le pas et le nombre d'echantillons : l'emprise en decoule sans ambiguite.

    E0 = originEast                     E1 = originEast  + (samplesX - 1) * spacing
    N1 = originNorth                    N0 = originNorth - (samplesZ - 1) * spacing

Attention a la difference de NATURE entre les deux jeux de donnees, qui est la
seule vraie subtilite de l'alignement :
  - la heightmap est un semis de NOEUDS ; le noeud ix=0 est exactement a E0 ;
  - l'orthophoto est une grille de PIXELS-SURFACES ; le pixel 0 COUVRE
    [E0, E0+res] et son centre est a E0 + res/2.
La bbox demandee au WMS est donc bien [E0, E1] x [N0, N1] : l'image couvre
exactement le carre delimite par les noeuds extremes du terrain, bord a bord.
Le demi-texel de decalage residuel entre "bord de l'image" et "centre du premier
texel" est assume et documente dans NOTES-PHASE11.md (il vaut res/2, soit 25 cm
au pas de 50 cm, tres en dessous de la precision de la BD ORTHO elle-meme).

ORDRE DES AXES. Le service ne parle QUE WMS 1.3.0 (une requete 1.1.1 est
rejetee : VersionNegotiationFailed). En 1.3.0 l'ordre des axes depend du CRS, ce
qui est la source d'erreur classique. Verifie sur ce serveur, pas suppose :
BBOX=minEast,minNorth,maxEast,maxNorth renvoie l'image attendue, tandis que
l'ordre inverse renvoie une image BLANCHE UNIFORME. Le controle est reproduit a
chaque execution par --self-test.

DECOUPAGE. MaxWidth/MaxHeight annonces par GetCapabilities valent 5010 px : une
requete unique sur 5 km a 50 cm (10 000 px) est donc impossible. Les tuiles font
par defaut 1000 m de cote, ce qui aligne le decoupage sur celui des dalles LiDAR
(meme grille kilometrique Lambert 93) et donne 2000 px par tuile a 50 cm — deux
fois moins que la limite, avec de la marge pour --res 0.2 (5000 px, juste sous
la limite).

REPRISE : une tuile deja presente et lisible est sautee. Un telechargement
interrompu ne laisse pas de fichier tronque (ecriture dans un .part renomme
seulement une fois l'image validee par Pillow).

Dependances : Pillow.
    python -m pip install Pillow
"""

from __future__ import annotations

import argparse
import io
import os
import shutil
import struct
import sys
import time
import urllib.error
import urllib.parse
import urllib.request

try:
    from PIL import Image
except ImportError:
    sys.exit("Pillow est requis : python -m pip install Pillow")

WMS_URL = "https://data.geopf.fr/wms-r/wms"
LAYER = "HR.ORTHOIMAGERY.ORTHOPHOTOS"
USER_AGENT = "Vercora/0.11 (+projet de jeu personnel; habillage terrain)"
TIMEOUT = 120
WMS_MAX_PIXELS = 5010  # annonce par GetCapabilities

# Pillow refuse par defaut les images de plus de ~89 Mpx (garde anti-bombe).
# Nos tuiles restent tres en dessous, mais l'assemblage aval, lui, ne l'est pas :
# on releve la limite ici pour que les deux outils se comportent pareil.
Image.MAX_IMAGE_PIXELS = None


# ---------------------------------------------------------------------------
# Emprise
# ---------------------------------------------------------------------------
class Extent:
    """Emprise Lambert 93 d'un terrain, lue dans l'en-tete .vchm."""

    def __init__(self, path: str):
        with open(path, "rb") as f:
            head = f.read(120)
        if len(head) < 120 or head[:4] != b"VCHM":
            raise ValueError(f"{path} : ce n'est pas un fichier .vchm (magie absente).")
        (_magic, version, samples_x, samples_z, spacing, _reserved,
         origin_e, origin_n, self.min_h, self.max_h,
         self.epsg, _flags) = struct.unpack_from("<4sIIIfIddffII", head, 0)
        if version != 1:
            raise ValueError(f"{path} : version {version} non geree (attendu 1).")
        if self.epsg != 2154:
            raise ValueError(
                f"{path} : EPSG {self.epsg}. Seul le Lambert 93 (2154) est gere ; "
                "un terrain synthetique (epsg 0) n'a pas d'emprise reelle a "
                "photographier.")
        self.label = head[56:120].split(b"\0")[0].decode("utf-8", "replace")
        self.samples_x = samples_x
        self.samples_z = samples_z
        self.spacing = spacing
        # Coin nord-ouest = noeud (0, 0).
        self.east_min = origin_e
        self.north_max = origin_n
        self.east_max = origin_e + (samples_x - 1) * spacing
        self.north_min = origin_n - (samples_z - 1) * spacing

    @property
    def width_m(self) -> float:
        return self.east_max - self.east_min

    @property
    def height_m(self) -> float:
        return self.north_max - self.north_min

    def describe(self) -> str:
        return (f"{self.label} : {self.samples_x}x{self.samples_z} au pas de "
                f"{self.spacing:g} m\n"
                f"  emprise Lambert 93  E {self.east_min:.0f} .. {self.east_max:.0f}"
                f"   N {self.north_min:.0f} .. {self.north_max:.0f}"
                f"   ({self.width_m:.0f} x {self.height_m:.0f} m)")


# ---------------------------------------------------------------------------
# WMS
# ---------------------------------------------------------------------------
def getmap_url(e0: float, n0: float, e1: float, n1: float,
               width: int, height: int, fmt: str) -> str:
    """URL GetMap. BBOX en ordre EST,NORD — verifie sur ce serveur, voir l'en-tete."""
    params = {
        "SERVICE": "WMS",
        "VERSION": "1.3.0",
        "REQUEST": "GetMap",
        "LAYERS": LAYER,
        "STYLES": "",
        "CRS": "EPSG:2154",
        "BBOX": f"{e0:.3f},{n0:.3f},{e1:.3f},{n1:.3f}",
        "WIDTH": str(width),
        "HEIGHT": str(height),
        "FORMAT": fmt,
    }
    return WMS_URL + "?" + urllib.parse.urlencode(params)


def http_get(url: str) -> bytes:
    request = urllib.request.Request(url, headers={"User-Agent": USER_AGENT})
    with urllib.request.urlopen(request, timeout=TIMEOUT) as response:
        return response.read()


def self_test() -> bool:
    """Re-verifie l'ordre des axes. Un serveur qui changerait d'avis se voit ici,
    pas trois heures plus tard sur une texture en miroir."""
    e0, n0 = 899000.0, 6435000.0
    side = 512.0
    try:
        good = Image.open(io.BytesIO(http_get(
            getmap_url(e0, n0, e0 + side, n0 + side, 256, 256, "image/png")))).convert("RGB")
        swapped = Image.open(io.BytesIO(http_get(
            getmap_url(n0, e0, n0 + side, e0 + side, 256, 256, "image/png")))).convert("RGB")
    except (urllib.error.URLError, OSError, ValueError) as error:
        print(f"  [self-test] injoignable ({error}) — controle non concluant.")
        return False

    def is_blank(image) -> bool:
        lo, hi = image.convert("L").getextrema()
        return lo == hi

    if is_blank(good):
        print("  [self-test] ECHEC : l'ordre EST,NORD renvoie une image uniforme.")
        return False
    if not is_blank(swapped):
        print("  [self-test] AVERTISSEMENT : l'ordre NORD,EST renvoie aussi une "
              "image non uniforme ; le controle ne discrimine plus.")
        return False
    print("  [self-test] ordre des axes EST,NORD confirme "
          "(l'ordre inverse renvoie du blanc uniforme).")
    return True


# ---------------------------------------------------------------------------
# Telechargement
# ---------------------------------------------------------------------------
def tile_name(e0: int, n0: int, px: int, ext: str) -> str:
    return f"ortho_{e0:07d}_{n0:07d}_{px}px.{ext}"


def download_tile(url: str, destination: str, retries: int) -> tuple[bool, str]:
    """Telecharge et VALIDE une tuile. L'ecriture passe par un .part : un fichier
    final present est donc toujours une image complete et decodable."""
    partial = destination + ".part"
    delay = 2.0
    for attempt in range(1, retries + 1):
        try:
            data = http_get(url)
            if data[:1] == b"<":
                # Le WMS signale ses erreurs par un ServiceExceptionReport XML
                # renvoye avec un code 200 : sans ce test, on ecrirait le message
                # d'erreur dans un fichier .jpg.
                message = data[:300].decode("utf-8", "replace")
                return False, f"reponse XML du service : {message}"
            image = Image.open(io.BytesIO(data))
            image.verify()  # leve si le flux est tronque
            with open(partial, "wb") as f:
                f.write(data)
            os.replace(partial, destination)
            return True, f"{len(data) / 1024.0:.0f} Ko"
        except (urllib.error.URLError, OSError, ValueError, TimeoutError) as error:
            if attempt == retries:
                return False, f"{type(error).__name__}: {error}"
            time.sleep(delay)
            delay *= 2.0
    return False, "epuise"


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Telecharge la BD ORTHO IGN sur l'emprise d'un terrain .vchm.")
    parser.add_argument("terrain", help="fichier .vchm dont on reprend l'emprise")
    parser.add_argument("-o", "--output", default="data/ortho",
                        help="repertoire des tuiles (defaut : data/ortho)")
    parser.add_argument("--res", type=float, default=0.5,
                        help="resolution au sol en metres/pixel (defaut 0,5 ; "
                             "la source native est a 0,2)")
    parser.add_argument("--tile-m", type=float, default=1000.0,
                        help="cote d'une tuile en metres (defaut 1000, soit la "
                             "grille kilometrique des dalles LiDAR)")
    parser.add_argument("--format", default="jpeg", choices=("jpeg", "png"),
                        help="format demande au WMS (defaut jpeg : la BD ORTHO "
                             "est elle-meme une photo, le png ne restitue rien "
                             "de plus et pese 6 fois plus)")
    parser.add_argument("--retries", type=int, default=4)
    parser.add_argument("--dry-run", action="store_true",
                        help="affiche le plan et la place necessaire, sans rien ecrire")
    parser.add_argument("--margin-go", type=float, default=2.0,
                        help="marge de disque libre exigee en plus du lot")
    parser.add_argument("--no-self-test", action="store_true")
    args = parser.parse_args()

    try:
        extent = Extent(args.terrain)
    except (OSError, ValueError) as error:
        return int(bool(sys.stderr.write(f"{error}\n"))) or 2

    print("BD ORTHO IGN — couche " + LAYER)
    print(extent.describe())

    if args.res <= 0:
        sys.exit("--res doit etre strictement positif.")

    tile_px = int(round(args.tile_m / args.res))
    if tile_px > WMS_MAX_PIXELS:
        sys.exit(f"--tile-m {args.tile_m:g} a --res {args.res:g} demande "
                 f"{tile_px} px par tuile, au-dela de la limite du service "
                 f"({WMS_MAX_PIXELS}). Reduire --tile-m.")

    cols = int(round(extent.width_m / args.tile_m))
    rows = int(round(extent.height_m / args.tile_m))
    if abs(cols * args.tile_m - extent.width_m) > 1e-6 or \
       abs(rows * args.tile_m - extent.height_m) > 1e-6:
        sys.exit(f"L'emprise ({extent.width_m:g} x {extent.height_m:g} m) n'est pas "
                 f"un multiple entier de --tile-m {args.tile_m:g}. Choisir un cote "
                 "de tuile qui la divise, sinon les tuiles deborderaient ou "
                 "laisseraient une bande non couverte.")

    total_px = (cols * tile_px, rows * tile_px)
    print(f"  decoupage : {cols} x {rows} = {cols * rows} tuiles de {tile_px} px "
          f"({args.tile_m:g} m a {args.res:g} m/px)")
    print(f"  mosaique complete : {total_px[0]} x {total_px[1]} px "
          f"= {total_px[0] * total_px[1] / 1e6:.1f} Mpx")

    if not args.no_self_test and not args.dry_run:
        self_test()

    # Estimation de place : une tuile de BD ORTHO en jpeg qualite service pese
    # de l'ordre de 0,25 octet par pixel (mesure sur les premieres tuiles). On
    # majore volontairement.
    bytes_per_px = 0.5 if args.format == "jpeg" else 2.0
    estimate = total_px[0] * total_px[1] * bytes_per_px
    print(f"  place estimee : {estimate / 1e6:.0f} Mo (majoree)")

    if args.dry_run:
        print("\n--dry-run : rien n'a ete ecrit.")
        return 0

    os.makedirs(args.output, exist_ok=True)
    free = shutil.disk_usage(args.output).free
    needed = estimate + args.margin_go * 1e9
    print(f"  disque libre : {free / 1e9:.1f} Go "
          f"(exige : {needed / 1e9:.1f} Go avec la marge)")
    if free < needed:
        sys.exit("Place insuffisante — telechargement non demarre.")

    ext = "jpg" if args.format == "jpeg" else "png"
    todo = []
    skipped = 0
    for row in range(rows):
        for col in range(cols):
            e0 = extent.east_min + col * args.tile_m
            n0 = extent.north_min + row * args.tile_m
            destination = os.path.join(args.output, tile_name(int(e0), int(n0), tile_px, ext))
            if os.path.exists(destination) and os.path.getsize(destination) > 0:
                skipped += 1
                continue
            url = getmap_url(e0, n0, e0 + args.tile_m, n0 + args.tile_m,
                             tile_px, tile_px, "image/" + args.format)
            todo.append((e0, n0, url, destination))

    print(f"\n{len(todo)} tuile(s) a telecharger, {skipped} deja presente(s).")
    if not todo:
        print("Rien a faire.")
        return 0

    started = time.time()
    failures = []
    written = 0
    for index, (e0, n0, url, destination) in enumerate(todo, 1):
        label = f"E{int(e0)} N{int(n0)}"
        print(f"  [{index}/{len(todo)}] {label} ... ", end="", flush=True)
        ok, message = download_tile(url, destination, args.retries)
        if ok:
            written += os.path.getsize(destination)
            print(message)
        else:
            failures.append((label, message))
            print("ECHEC — " + message)

    elapsed = time.time() - started
    print(f"\nTermine en {elapsed:.0f} s — {written / 1e6:.1f} Mo ecrits "
          f"({written / 1e6 / max(elapsed, 1e-3):.1f} Mo/s).")
    if failures:
        print(f"{len(failures)} echec(s) :")
        for label, message in failures:
            print(f"  {label} : {message}")
        print("Relancer la meme commande : les tuiles deja recues sont sautees.")
        return 1
    print(f"Couverture complete : {cols * rows} tuiles dans {args.output}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
