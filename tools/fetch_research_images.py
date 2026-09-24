#!/usr/bin/env python3
"""
Store the research icons (or the buildings' pictures) where the web console can show them:
webui/static/img/research/ (webui/static/img/buildings/ with --set buildings).

The list of icons (and where each one is) comes from gamedata/research_images.json, written by
tools/import_wiki_research.py (gamedata/building_images.json by tools/import_wiki_buildings.py). This tool downloads them, one every half second, from the wiki's
image server, and writes webui/static/img/research/index.json (icon name -> stored file) which the
console reads to know what is available.

    python3 tools/fetch_research_images.py               download what is missing
    python3 tools/fetch_research_images.py --status      count what is stored / missing, download nothing
    python3 tools/fetch_research_images.py --from-dir D  copy icons you saved by hand from a browser
    python3 tools/fetch_research_images.py --set buildings   the same for the buildings' pictures

The wiki's image server sometimes answers scripts with a browser check (HTTP 403, header
"cf-mitigated: challenge"). This tool does NOT try to get around it: it stops and says so. Then save
the icons yourself from the wiki in a browser (or use the console's hot-linking of the wiki's own
addresses, which a browser is allowed to do) and import them with --from-dir: the files are matched to
the icons by name, whatever the case or the use of spaces / underscores.

The icons are game artwork that belongs to IGG, taken from the community wiki: keep them for the
console's own use.
"""

import argparse
import json
import os
import re
import shutil
import sys
import time
import urllib.error
import urllib.parse
import urllib.request

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
# what can be stored: the list written by the importer, and where the console looks for the files
SETS = {
    "research": ("research_images.json", os.path.join("webui", "static", "img", "research")),
    "buildings": ("building_images.json", os.path.join("webui", "static", "img", "buildings")),
}
MANIFEST = os.path.join(REPO, "gamedata", SETS["research"][0])
DEST = os.path.join(REPO, SETS["research"][1])
USER_AGENT = "lmbot-research-images/1.0 (icons for a local web console, one request per half second)"
DELAY = 0.5
EXTENSIONS = (".png", ".jpg", ".jpeg", ".gif", ".webp")


class Challenged(Exception):
    """The image server asked for a browser check."""


def stored_name(key):
    """File name an icon is stored under: safe on every filesystem, still recognisable."""
    name = urllib.parse.unquote(key)
    name = re.sub(r"[^A-Za-z0-9._-]+", "_", name)
    return name.strip("_") or "icon"


def normalized(name):
    """What two spellings of the same file name have in common (case, spaces, underscores, extension)."""
    base = os.path.splitext(urllib.parse.unquote(name))[0]
    return re.sub(r"[^a-z0-9]+", "", base.lower())


def load_manifest():
    if not os.path.exists(MANIFEST):
        sys.exit("%s is missing: run tools/import_wiki_research.py (or import_wiki_buildings.py) first." % MANIFEST)
    with open(MANIFEST, encoding="utf-8") as f:
        return json.load(f)["images"]


def read_index():
    path = os.path.join(DEST, "index.json")
    if os.path.exists(path):
        with open(path, encoding="utf-8") as f:
            return json.load(f)
    return {}


def write_index(index):
    os.makedirs(DEST, exist_ok=True)
    with open(os.path.join(DEST, "index.json"), "w", encoding="utf-8") as f:
        json.dump(index, f, ensure_ascii=False, indent=0, sort_keys=True)
        f.write("\n")


def already_stored(images, index):
    return {k for k in images if k in index and os.path.exists(os.path.join(DEST, index[k]))}


def download(url, path):
    request = urllib.request.Request(url, headers={"User-Agent": USER_AGENT})
    try:
        with urllib.request.urlopen(request, timeout=60) as response:
            kind = response.headers.get("Content-Type", "")
            data = response.read()
    except urllib.error.HTTPError as error:
        if error.headers.get("cf-mitigated") == "challenge" or error.code in (403, 429):
            raise Challenged("HTTP %d, %s" % (error.code, error.headers.get("cf-mitigated") or "refused"))
        raise
    if not kind.startswith("image/"):
        raise Challenged("the server sent %r instead of an image" % kind)
    with open(path, "wb") as f:
        f.write(data)


def command_status(images, index):
    have = already_stored(images, index)
    print("%d icons listed, %d stored in %s, %d missing." % (len(images), len(have), DEST, len(images) - len(have)))


def command_download(images, index):
    os.makedirs(DEST, exist_ok=True)
    todo = sorted(k for k in images if k not in already_stored(images, index))
    print("%d icons to download." % len(todo))
    done = 0
    for key in todo:
        name = stored_name(key)
        try:
            download(images[key]["url"], os.path.join(DEST, name))
        except Challenged as why:
            write_index(index)
            print("\nStopped after %d icons: the image server answered with a browser check (%s)." % (done, why))
            print("This tool does not try to get around it. Save the icons from the wiki in a browser into one")
            print("folder, then import them with:  python3 tools/fetch_research_images.py --from-dir <folder>")
            sys.exit(2)
        except (urllib.error.URLError, OSError) as error:
            print("  %s: %s (skipped)" % (key, error))
            continue
        index[key] = name
        done += 1
        if done % 25 == 0:
            print("  %d / %d" % (done, len(todo)))
            write_index(index)
        time.sleep(DELAY)
    write_index(index)
    print("Done: %d downloaded, %d stored in total." % (done, len(already_stored(images, index))))


def command_from_dir(images, index, folder):
    if not os.path.isdir(folder):
        sys.exit("%s is not a folder." % folder)
    wanted = {}
    for key in images:
        wanted.setdefault(normalized(key), key)
    os.makedirs(DEST, exist_ok=True)
    copied, unknown = 0, []
    for entry in sorted(os.listdir(folder)):
        source = os.path.join(folder, entry)
        if not os.path.isfile(source) or not entry.lower().endswith(EXTENSIONS):
            continue
        key = wanted.get(normalized(entry))
        if key is None:
            unknown.append(entry)
            continue
        name = stored_name(key)
        shutil.copyfile(source, os.path.join(DEST, name))
        index[key] = name
        copied += 1
    write_index(index)
    print("%d icons imported, %d files did not match any icon." % (copied, len(unknown)))
    for entry in unknown[:10]:
        print("  not an icon of the list: %s" % entry)
    command_status(images, index)


def main():
    parser = argparse.ArgumentParser(description="Store the research icons for the web console.")
    parser.add_argument("--status", action="store_true", help="only count what is stored")
    parser.add_argument("--from-dir", metavar="FOLDER", help="import icons saved by hand from a browser")
    parser.add_argument("--set", choices=sorted(SETS), default="research", help="which pictures (default: research)")
    args = parser.parse_args()

    global MANIFEST, DEST
    MANIFEST = os.path.join(REPO, "gamedata", SETS[args.set][0])
    DEST = os.path.join(REPO, SETS[args.set][1])

    images = load_manifest()
    index = read_index()
    if args.status:
        command_status(images, index)
    elif args.from_dir:
        command_from_dir(images, index, args.from_dir)
    else:
        command_download(images, index)


if __name__ == "__main__":
    main()
