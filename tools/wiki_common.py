"""
Shared by tools/import_wiki_research.py and tools/import_wiki_buildings.py: reading the Lords Mobile
fandom wiki through its public MediaWiki API (the wiki's pages are blocked to scripts, its API is not).

One request per second, an identifying User-Agent, standard library only. The wiki is edited by players:
a value that cannot be read without guessing is returned as None, never corrected here.
"""

import json
import re
import sys
import time
import urllib.error
import urllib.parse
import urllib.request

WIKI = "https://lordsmobile.fandom.com"
API = WIKI + "/api.php"
USER_AGENT = "lmbot-wiki-import/1.0 (reads the public MediaWiki API, one request per second)"
DELAY = 1.0


def api(**params):
    params["format"] = "json"
    url = API + "?" + urllib.parse.urlencode(params)
    request = urllib.request.Request(url, headers={"User-Agent": USER_AGENT})
    try:
        with urllib.request.urlopen(request, timeout=60) as response:
            data = json.load(response)
    except urllib.error.HTTPError as error:
        sys.exit("The wiki refused the request (HTTP %d): %s\n%s" % (error.code, url, error.reason))
    time.sleep(DELAY)
    if "error" in data:
        sys.exit("The wiki API returned an error: %s" % data["error"])
    return data


# --------------------------------------------------------------------------
# Wikitext helpers
# --------------------------------------------------------------------------

def clean(text):
    """Wikitext cell -> plain text."""
    text = re.sub(r"\{\{Pic 2\|(?:1=)?[^|}]*\|(?:2=)?([^|}]*)\}\}", r"\1", text)
    text = re.sub(r"\{\{[^{}]*\}\}", "", text)
    text = re.sub(r"\[\[(?:[^|\]]*\|)?([^\]]*)\]\]", r"\1", text)
    text = re.sub(r"<br\s*/?>", "; ", text)
    text = re.sub(r"'{2,}", "", text)
    text = re.sub(r"<[^>]+>", "", text)
    text = re.sub(r'^\s*(?:(?:style|class|colspan|rowspan|width|align|bgcolor)="[^"]*"\s*)+\|\s*', "", text)
    return text.strip()


def wikitables(wikitext):
    """Every {| ... |} table as a list of rows, each row a list of (kind, text), kind 'h' or 'd'."""
    tables = []
    for match in re.finditer(r"\{\|(.*?)\n\|\}", wikitext, re.S):
        rows, current = [], []
        for line in match.group(1).split("\n")[1:]:
            line = line.rstrip()
            if line.startswith("|-"):
                if current:
                    rows.append(current)
                    current = []
            elif line.startswith("!"):
                current += [("h", clean(cell)) for cell in line[1:].split("!!")]
            elif line.startswith("|") and not line.startswith("|+"):
                current += [("d", clean(cell)) for cell in line[1:].split("||")]
            elif line.strip() and current and not line.startswith(("{", "}")):
                # a cell written over several lines ("Castle Lv25" then "Gold Hammer 1")
                kind, text = current[-1]
                current[-1] = (kind, (text + "; " + clean(line)).strip("; "))
        if current:
            rows.append(current)
        tables.append(rows)
    return tables


def to_int(text):
    """'1,616' / '279.026' / '4' -> int; anything else (a typo such as '503,9695') -> None."""
    text = text.strip().rstrip(";").strip()
    if re.fullmatch(r"\d+", text):
        return int(text)
    if re.fullmatch(r"\d{1,3}(?:[.,]\d{3})+", text):
        return int(re.sub(r"[.,]", "", text))
    return None


def to_seconds(text):
    """'2d 02:29:00' / '1,023d 23:17:00' -> seconds, None when it is not a duration."""
    text = text.strip().rstrip(";").strip()
    match = re.fullmatch(r"(?:(\d{1,3}(?:,\d{3})*|\d+)d\s*)?(\d+):(\d\d):(\d\d)", text)
    if not match:
        return None
    days, hours, minutes, seconds = match.groups()
    return int((days or "0").replace(",", "")) * 86400 + int(hours) * 3600 + int(minutes) * 60 + int(seconds)


def requirements(text):
    found = []
    for part in re.split(r"[;,]", text):
        match = (re.search(r"([A-Za-z][A-Za-z' .\-]*?)\s*Lv\.?\s*(\d+)", part)
                 or re.fullmatch(r"\s*([A-Za-z][A-Za-z' .\-]*?)\s+(\d+)\s*", part))   # "Castle 15"
        if match:
            found.append({"name": match.group(1).strip(), "level": int(match.group(2))})
    return found


def intro(wikitext):
    head = re.split(r"^==", wikitext, 1, flags=re.M)[0]
    head = re.sub(r"\[\[File:[^\]]*\]\]", "", head)
    return re.sub(r"\s+", " ", clean(head)).strip()


def fetch_wikitext(titles):
    texts = {}
    for start in range(0, len(titles), 20):
        batch = titles[start:start + 20]
        data = api(action="query", prop="revisions", rvprop="content", rvslots="main",
                   titles="|".join(batch), redirects=1)
        for page in data["query"]["pages"].values():
            if "revisions" in page:
                texts[page["title"]] = page["revisions"][0]["slots"]["main"]["*"]
            else:
                print("  missing page: %s" % page.get("title"))
    return texts


def fetch_image_info(files):
    info = {}
    names = sorted(files)
    for start in range(0, len(names), 40):
        batch = names[start:start + 40]
        data = api(action="query", prop="imageinfo", iiprop="url|size|mime",
                   titles="|".join("File:" + name.replace("_", " ") for name in batch))
        normalized = {n["to"]: n["from"] for n in data["query"].get("normalized", [])}
        for page in data["query"]["pages"].values():
            title = page["title"][len("File:"):]
            if "imageinfo" not in page:
                continue
            picture = page["imageinfo"][0]
            key = title.replace(" ", "_")
            info[key] = {"url": picture["url"], "width": picture["width"], "height": picture["height"],
                         "bytes": picture["size"], "mime": picture["mime"]}
    return info


def category_members(category):
    """(pages, subcategories) of a wiki category."""
    data = api(action="query", list="categorymembers", cmtitle="Category:" + category, cmlimit=200)
    members = data["query"]["categorymembers"]
    pages = [m["title"] for m in members if m["ns"] == 0]
    sub = [m["title"][len("Category:"):] for m in members if m["ns"] == 14]
    return pages, sub
