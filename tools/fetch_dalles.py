#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""
fetch_dalles.py — telecharge un lot de dalles LiDAR HD IGN decrites par un JSON.

PHASE 9. La Phase 7 avait telecharge UNE dalle a la main ; ici il y en a 25, et
un telechargement de plusieurs gigaoctets qui echoue a la 23e sans rien dire
serait le pire resultat possible. D'ou : taille totale connue AVANT de commencer,
reprise sur octets deja recus, reessais avec attente croissante, et un bilan
final qui NOMME ce qui a echoue au lieu de s'arreter en silence.

    python tools/fetch_dalles.py vercora_dalles.json -o data/lidar
    python tools/fetch_dalles.py vercora_dalles.json -o data/lidar --only 0899-6440 0900-6440
    python tools/fetch_dalles.py vercora_dalles.json -o data/lidar --dry-run

Options :
    --only C [C ...]  ne traiter que ces dalles (coordonnees_NW, ex. 0899-6440)
    --dry-run         interroge les tailles et affiche le bilan, sans rien ecrire
    --retries N       tentatives par dalle (defaut 4)
    --margin-go N     marge de disque libre exigee en plus du lot (defaut 5)

FORMAT D'ENTREE attendu (celui exporte depuis data.geopf.fr) : un objet dont
chaque valeur porte "url" et "metadata", cette derniere etant une CHAINE de JSON
a re-parser (l'IGN imbrique du JSON dans du JSON) contenant au moins
"coordonnees_NW" ("0899-6440" = coin nord-ouest en km Lambert 93) et
"nombre_points".

REPRISE : un fichier deja present et complet (taille == Content-Length) est
saute, pas retelecharge. Un fichier partiel est repris par requete Range si le
serveur l'accepte, sinon reparti de zero. C'est ce qui rend le script rejouable
apres une coupure sans repayer les gigaoctets deja recus.
"""

from __future__ import annotations

import argparse
import json
import os
import shutil
import sys
import time
import urllib.error
import urllib.request

USER_AGENT = "Vercora/0.9 (+projet de jeu personnel; laspy pipeline)"
TIMEOUT = 60
CHUNK = 1 << 20  # 1 Mo


def human(n: float) -> str:
    for unit in ("o", "Ko", "Mo", "Go"):
        if abs(n) < 1024 or unit == "Go":
            return f"{n:.1f} {unit}" if unit != "o" else f"{int(n)} o"
        n /= 1024.0
    return f"{n:.1f} Go"


def human_time(seconds: float) -> str:
    seconds = int(seconds)
    if seconds < 60:
        return f"{seconds} s"
    if seconds < 3600:
        return f"{seconds // 60} min {seconds % 60:02d} s"
    return f"{seconds // 3600} h {(seconds % 3600) // 60:02d} min"


def load_manifest(path: str, only: list[str] | None):
    """Retourne [(coord, nom, url, nombre_points)] trie du nord-ouest au sud-est."""
    with open(path, "r", encoding="utf-8") as f:
        raw = json.load(f)

    entries = []
    for name, item in raw.items():
        url = item["url"]
        # L'IGN imbrique le bloc de metadonnees comme une CHAINE de JSON.
        meta = item["metadata"]
        if isinstance(meta, str):
            meta = json.loads(meta)
        coord = meta["coordonnees_NW"]
        entries.append((coord, name, url, int(meta.get("nombre_points", 0))))

    if only:
        wanted = set(only)
        kept = [e for e in entries if e[0] in wanted]
        missing = wanted - {e[0] for e in kept}
        if missing:
            sys.exit(f"--only : dalle(s) absente(s) du manifeste : "
                     f"{', '.join(sorted(missing))}")
        entries = kept

    # Nord-ouest -> sud-est : X croissant, Y DECROISSANT (le nord est en haut).
    def key(e):
        x, y = e[0].split("-")
        return (-int(y), int(x))

    entries.sort(key=key)
    return entries


def remote_size(url: str) -> int | None:
    """Content-Length sans telecharger le corps. None si le serveur se tait."""
    request = urllib.request.Request(url, method="HEAD",
                                     headers={"User-Agent": USER_AGENT})
    try:
        with urllib.request.urlopen(request, timeout=TIMEOUT) as response:
            length = response.headers.get("Content-Length")
            return int(length) if length is not None else None
    except (urllib.error.URLError, ValueError, TimeoutError):
        return None


def download_one(url: str, destination: str, expected: int | None,
                 retries: int, index: int, total: int, label: str) -> tuple[bool, str]:
    """Telecharge avec reprise et reessais. Retourne (succes, message)."""
    temporary = destination + ".part"

    for attempt in range(1, retries + 1):
        have = os.path.getsize(temporary) if os.path.exists(temporary) else 0

        if expected is not None and have == expected:
            os.replace(temporary, destination)
            return True, "deja complet (.part)"
        if expected is not None and have > expected:
            # Fichier partiel incoherent (manifeste change, disque corrompu) :
            # mieux vaut repartir que concatener n'importe quoi.
            os.remove(temporary)
            have = 0

        headers = {"User-Agent": USER_AGENT}
        if have > 0:
            headers["Range"] = f"bytes={have}-"

        try:
            request = urllib.request.Request(url, headers=headers)
            with urllib.request.urlopen(request, timeout=TIMEOUT) as response:
                # 200 sur une requete Range = le serveur ignore la reprise et
                # renvoie tout : il faut alors reecrire depuis le debut, sinon
                # on colle le fichier entier a la suite du morceau deja recu.
                resuming = (response.status == 206)
                if have > 0 and not resuming:
                    have = 0

                mode = "ab" if resuming and have > 0 else "wb"
                started = time.time()
                last_print = 0.0
                received = have

                with open(temporary, mode) as f:
                    while True:
                        chunk = response.read(CHUNK)
                        if not chunk:
                            break
                        f.write(chunk)
                        received += len(chunk)

                        now = time.time()
                        if now - last_print >= 0.5:
                            last_print = now
                            elapsed = max(now - started, 1e-6)
                            speed = (received - have) / elapsed
                            if expected:
                                pct = 100.0 * received / expected
                                eta = ((expected - received) / speed
                                       if speed > 0 else 0)
                                bar_width = 24
                                filled = int(bar_width * received / expected)
                                bar = "#" * filled + "." * (bar_width - filled)
                                sys.stdout.write(
                                    f"\r  [{index}/{total}] {label} "
                                    f"[{bar}] {pct:5.1f}%  "
                                    f"{human(received)}/{human(expected)}  "
                                    f"{human(speed)}/s  reste {human_time(eta)}   ")
                            else:
                                sys.stdout.write(
                                    f"\r  [{index}/{total}] {label} "
                                    f"{human(received)}  {human(speed)}/s   ")
                            sys.stdout.flush()

            sys.stdout.write("\r" + " " * 100 + "\r")
            sys.stdout.flush()

            size = os.path.getsize(temporary)
            if expected is not None and size != expected:
                raise IOError(f"taille finale {size} != attendu {expected}")

            # Un .laz commence par "LASF" : un HTML d'erreur enregistre sous le
            # bon nom passerait sinon inapercu jusqu'a la conversion.
            with open(temporary, "rb") as f:
                magic = f.read(4)
            if magic != b"LASF":
                raise IOError(f"en-tete inattendu {magic!r} (pas un LAS/LAZ)")

            os.replace(temporary, destination)
            elapsed = time.time() - started
            return True, f"{human(size)} en {human_time(elapsed)}"

        except (urllib.error.URLError, IOError, TimeoutError, OSError) as error:
            sys.stdout.write("\r" + " " * 100 + "\r")
            sys.stdout.flush()
            if attempt >= retries:
                return False, f"{type(error).__name__}: {error}"
            wait = min(2 ** attempt, 30)
            print(f"  [{index}/{total}] {label} : echec ({error}), "
                  f"nouvelle tentative {attempt + 1}/{retries} dans {wait} s")
            time.sleep(wait)

    return False, "tentatives epuisees"


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Telecharge les dalles LiDAR HD decrites par un manifeste JSON.")
    parser.add_argument("manifest")
    parser.add_argument("-o", "--output", default="data/lidar")
    parser.add_argument("--only", nargs="+", default=None)
    parser.add_argument("--dry-run", action="store_true")
    parser.add_argument("--retries", type=int, default=4)
    parser.add_argument("--margin-go", type=float, default=5.0)
    args = parser.parse_args()

    entries = load_manifest(args.manifest, args.only)
    print(f"Manifeste : {args.manifest}  ->  {len(entries)} dalle(s)")

    coords = [e[0] for e in entries]
    xs = sorted({int(c.split("-")[0]) for c in coords})
    ys = sorted({int(c.split("-")[1]) for c in coords})
    print(f"  emprise (coins NO) : X {xs[0]}..{xs[-1]} km, Y {ys[0]}..{ys[-1]} km "
          f"({len(xs)} x {len(ys)} dalles)")
    print(f"  points annonces : {sum(e[3] for e in entries):,}")

    os.makedirs(args.output, exist_ok=True)

    # --- Tailles reelles AVANT de telecharger quoi que ce soit ---------------
    print(f"\nInterrogation des tailles ({len(entries)} requetes HEAD)...")
    sizes: dict[str, int | None] = {}
    unknown = 0
    for index, (coord, name, url, points) in enumerate(entries, 1):
        size = remote_size(url)
        sizes[coord] = size
        if size is None:
            unknown += 1
        sys.stdout.write(f"\r  {index}/{len(entries)}   ")
        sys.stdout.flush()
    print("\r" + " " * 40 + "\r", end="")

    known = [s for s in sizes.values() if s is not None]
    total_known = sum(known)
    if unknown and known:
        # Estimation pour les manquantes, a partir de la moyenne des connues.
        average = total_known / len(known)
        estimated_total = total_known + unknown * average
    else:
        estimated_total = total_known

    print(f"  taille totale : {human(total_known)}"
          + (f"  (+{unknown} dalle(s) de taille inconnue, "
             f"estimation totale {human(estimated_total)})" if unknown else ""))

    already = 0
    todo = []
    for coord, name, url, points in entries:
        destination = os.path.join(args.output, os.path.basename(url))
        size = sizes[coord]
        if os.path.exists(destination) and (size is None
                                            or os.path.getsize(destination) == size):
            already += os.path.getsize(destination)
            continue
        todo.append((coord, name, url, destination, size))

    partial = sum(os.path.getsize(t[3] + ".part")
                  for t in todo if os.path.exists(t[3] + ".part"))
    remaining = max(estimated_total - already - partial, 0)

    print(f"  deja sur disque : {human(already)}"
          + (f" (+{human(partial)} de fragments repris)" if partial else ""))
    print(f"  a telecharger : {human(remaining)}  ({len(todo)} dalle(s))")

    # --- Verification du disque ---------------------------------------------
    free = shutil.disk_usage(os.path.abspath(args.output)).free
    needed = remaining + args.margin_go * (1 << 30)
    print(f"\nDisque : {human(free)} libres, besoin {human(remaining)} "
          f"+ {args.margin_go:g} Go de marge = {human(needed)}")
    if free < needed:
        print("  INSUFFISANT — rien n'a ete telecharge.", file=sys.stderr)
        return 2
    print("  OK")

    if args.dry_run:
        print("\n--dry-run : rien n'a ete telecharge.")
        return 0
    if not todo:
        print("\nToutes les dalles sont deja presentes. Rien a faire.")
        return 0

    # --- Telechargement ------------------------------------------------------
    print(f"\nTelechargement de {len(todo)} dalle(s) vers {args.output}/\n")
    started = time.time()
    failures = []
    done_bytes = 0

    for index, (coord, name, url, destination, size) in enumerate(todo, 1):
        ok, message = download_one(url, destination, size, args.retries,
                                   index, len(todo), coord)
        if ok:
            done_bytes += os.path.getsize(destination)
            elapsed = time.time() - started
            speed = done_bytes / elapsed if elapsed > 0 else 0
            left = max(remaining - done_bytes, 0)
            eta = left / speed if speed > 0 else 0
            print(f"  [{index}/{len(todo)}] {coord}  OK  {message}"
                  f"   | lot : {100.0 * index / len(todo):.0f}%, "
                  f"reste ~{human_time(eta)}")
        else:
            failures.append((coord, message))
            print(f"  [{index}/{len(todo)}] {coord}  ECHEC  {message}")

    elapsed = time.time() - started
    print(f"\nTermine en {human_time(elapsed)} — "
          f"{len(todo) - len(failures)}/{len(todo)} reussie(s), "
          f"{human(done_bytes)} recus"
          + (f", {human(done_bytes / elapsed)}/s de moyenne" if elapsed > 0 else ""))

    if failures:
        print(f"\n{len(failures)} ECHEC(S) — relancer la meme commande reprendra "
              f"ces dalles la ou elles en sont :")
        for coord, message in failures:
            print(f"  {coord} : {message}")
        return 1

    return 0


if __name__ == "__main__":
    sys.exit(main())
