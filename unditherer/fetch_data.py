#!/usr/bin/env python3
"""Fetch the CC0 true-colour texture corpus the learned restorer was trained on.

Two modes:

  manifest (default)   reproduce the SHIPPED corpus exactly: every asset listed in
                       unditherer/corpus/manifest.json, by id, from its source
  discover             build a corpus afresh from the sources' category listings
                       (what produced the shipped manifest on 2026-09-01; the sites
                       change, so a new discover run will not give the same set)

Sources, all CC0 1.0 (verified on their pages, see CREDITS.md):

  ambientCG   https://ambientcg.com          API v2 -> 1K-JPG zip per material,
                                              only the *_Color.jpg map is kept
  Poly Haven  https://polyhaven.com          API   -> 1k diffuse JPG per texture
  Screaming Brain Studios, "Tiny Texture Pack" 1-3 on OpenGameArt
              https://opengameart.org/content/tiny-texture-pack
                                              painted textures at pixel-art scale

Output (outside git, see .gitignore; or $UNDITHERER_DATA):
  <main checkout>/.data/undither-train/images/<source>/...    the corpus
  <main checkout>/.data/undither-train/manifest.json          one record per image
  <main checkout>/.data/undither-train/ATTRIBUTION.md         per-asset credits

    python -m unditherer.fetch_data --dry-run        # what is present / missing, no downloads
    python -m unditherer.fetch_data                  # the shipped corpus, resumable
    python -m unditherer.fetch_data --verify         # every manifest file present, sizes match
    python -m unditherer.fetch_data --discover --limit 3   # a few per source, from the listings
"""
import argparse
import io
import json
import sys
import time
import zipfile
from pathlib import Path
from urllib.error import HTTPError, URLError
from urllib.parse import quote
from urllib.request import Request, urlopen

from .paths import SHIPPED_MANIFEST, data_root

UA = "ta-undither-research/0.1 (CC0 texture corpus fetch)"   # a project string, never a person's address
PAUSE = 0.5            # seconds between API calls
PAUSE_ZIP = 1.0        # seconds between bulk downloads

# Every asset this script fetches is CC0 1.0 Universal (checked 2026-09-01 on the
# sources' own pages).  Poly Haven's API terms ask that software surfacing their
# content say so; we do, in CREDITS.md and at the end of every run.
LICENCE = "CC0 1.0"
LICENCE_URLS = {
    "ambientCG": "https://docs.ambientcg.com/license/",
    "Poly Haven": "https://polyhaven.com/license",
    "OpenGameArt": "https://opengameart.org/content/tiny-texture-pack",
}

# ambientCG categories that map onto TA tilesets.  Bricks / PavingStones / Tiles /
# Wood are left out: Poly Haven covers the urban ground surfaces more cheaply
# (its diffuse map is a single ~0.7 MB file; an ambientCG zip is ~8 MB).
AMBIENTCG_CATEGORIES = ["Ground", "Rock", "Grass", "Snow", "Ice", "Lava", "Gravel", "Moss",
                        "Metal", "Rust", "Concrete", "Asphalt"]

POLYHAVEN_CATEGORIES = {"terrain", "rock", "sand", "gravel", "snow", "aerial", "moss",
                        "collection: moon", "brick", "concrete", "cobblestone", "road",
                        "metal", "asphalt", "tiles", "roofing"}

OGA_PACKS = [
    ("tiny-texture-pack",   "sbs_-_tiny_texture_pack_512x512_a.zip"),
    ("tiny-texture-pack",   "sbs_-_tiny_texture_pack_512x512_b.zip"),
    ("tiny-texture-pack-2", "sbs_-_tiny_texture_pack_2_-_512x512.zip"),
    ("tiny-texture-pack-3", "sbs_-_tiny_texture_pack_-_large_part_1.zip"),
    ("tiny-texture-pack-3", "sbs_-_tiny_texture_pack_3_-_large_part_2.zip"),
]
OGA_FILES = "https://opengameart.org/sites/default/files/"
SOURCE_KEYS = {"ambientcg": "ambientCG", "polyhaven": "Poly Haven", "oga": "OpenGameArt"}


# --------------------------------------------------------------------------- http

def get(url, retries=3, binary=False):
    last = None
    for attempt in range(retries):
        try:
            with urlopen(Request(url, headers={"User-Agent": UA}), timeout=120) as r:
                data = r.read()
            return data if binary else json.loads(data)
        except (HTTPError, URLError, TimeoutError, json.JSONDecodeError) as e:
            last = e
            time.sleep(2.0 * (attempt + 1))
    raise RuntimeError(f"failed: {url} ({last})")


def download(url, dest, retries=3):
    dest = Path(dest)
    dest.parent.mkdir(parents=True, exist_ok=True)
    tmp = dest.with_suffix(dest.suffix + ".part")
    last = None
    for attempt in range(retries):
        try:
            with urlopen(Request(url, headers={"User-Agent": UA}), timeout=300) as r, open(tmp, "wb") as fh:
                while True:
                    chunk = r.read(1 << 20)
                    if not chunk:
                        break
                    fh.write(chunk)
            tmp.replace(dest)
            return dest
        except (HTTPError, URLError, TimeoutError) as e:
            time.sleep(2.0 * (attempt + 1))
            last = e
    raise RuntimeError(f"failed: {url} ({last})")


def image_ok(path, min_side=256):
    try:
        from PIL import Image
        with Image.open(path) as im:
            return im.mode in ("RGB", "RGBA", "P", "L") and min(im.size) >= min_side
    except Exception:
        return False


# --------------------------------------------------------------------------- ambientCG

def ambientcg_list(category):
    out, offset = [], 0
    while True:
        d = get(f"https://ambientcg.com/api/v2/full_json?type=Material&category={quote(category)}"
                f"&limit=100&offset={offset}&include=downloadData")
        found = d.get("foundAssets") or []
        out.extend(found)
        offset += len(found)
        time.sleep(PAUSE)
        if not found or offset >= d.get("numberOfResults", 0):
            return out


def ambientcg_by_ids(ids, batch=50):
    """assetId -> asset record, resolved in batches through the API's id filter."""
    out = {}
    for i in range(0, len(ids), batch):
        chunk = ids[i:i + batch]
        d = get(f"https://ambientcg.com/api/v2/full_json?type=Material&id={quote(','.join(chunk))}"
                f"&limit={max(100, len(chunk))}&include=downloadData")
        for a in d.get("foundAssets") or []:
            out[a["assetId"]] = a
        time.sleep(PAUSE)
    return out


def ambientcg_1k(asset):
    folders = asset.get("downloadFolders") or {}
    for f in folders.values():
        for cat in (f.get("downloadFiletypeCategories") or {}).values():
            for dl in cat.get("downloads") or []:
                if dl.get("attribute") == "1K-JPG":
                    return dl["downloadLink"], int(dl.get("size") or 0)
    return None, 0


def ambientcg_save(url, dest):
    """Download the 1K-JPG zip and keep only its *_Color.jpg.  False if absent."""
    data = get(url, binary=True)
    time.sleep(PAUSE_ZIP)
    with zipfile.ZipFile(io.BytesIO(data)) as z:
        colour = [m for m in z.namelist() if m.lower().endswith("_color.jpg")]
        if not colour:
            return False
        dest = Path(dest)
        dest.parent.mkdir(parents=True, exist_ok=True)
        dest.write_bytes(z.read(colour[0]))
    return True


def ambientcg_record(root, dest, asset, category=None):
    return {"file": str(dest.relative_to(root)), "source": "ambientCG", "id": asset["assetId"],
            "licence": LICENCE, "url": asset.get("shortLink") or f"https://ambientcg.com/a/{asset['assetId']}",
            "category": asset.get("displayCategory") or category, "tags": asset.get("tags", []),
            "bytes": dest.stat().st_size}


def discover_ambientcg(root, manifest, limit, dry):
    img_dir = root / "images" / "ambientcg"
    seen = {m["id"] for m in manifest if m["source"] == "ambientCG"}
    total, n = 0, 0
    for cat in AMBIENTCG_CATEGORIES:
        assets = ambientcg_list(cat)
        print(f"ambientCG {cat}: {len(assets)} materials")
        for a in assets:
            aid = a["assetId"]
            url, size = ambientcg_1k(a)
            if not url:
                continue
            total += size
            if limit and n >= limit:
                continue
            if aid in seen or dry:
                n += 1
                continue
            dest = img_dir / f"{aid}.jpg"
            if not dest.exists() and not ambientcg_save(url, dest):
                print(f"  !! {aid}: no Color map in zip")
                continue
            if not image_ok(dest):
                print(f"  !! {aid}: unusable image, dropped")
                dest.unlink(missing_ok=True)
                continue
            manifest.append(ambientcg_record(root, dest, a, cat))
            seen.add(aid)
            n += 1
            if n % 25 == 0:      # keep a partial corpus usable while the fetch runs
                (root / "manifest.json").write_text(json.dumps(manifest, indent=1))
    print(f"ambientCG: {n} images, {total / 1e6:.0f} MB of zips listed")
    return n


# --------------------------------------------------------------------------- Poly Haven

def polyhaven_save(tid, dest_stem):
    """Fetch the 1k diffuse map; returns the Path written or None."""
    files = get(f"https://api.polyhaven.com/files/{tid}")
    time.sleep(PAUSE)
    key = next((k for k in files if "diff" in k.lower()), None)
    if not key:
        return None
    res = files[key].get("1k") or next(iter(files[key].values()))
    entry = res.get("jpg") or res.get("png")
    if not entry:
        return None
    dest = Path(str(dest_stem) + ("." + ("jpg" if "jpg" in res else "png")))
    if not dest.exists():
        download(entry["url"], dest)
        time.sleep(PAUSE)
    return dest


def polyhaven_record(root, dest, tid, meta):
    return {"file": str(dest.relative_to(root)), "source": "Poly Haven", "id": tid,
            "licence": LICENCE, "url": f"https://polyhaven.com/a/{tid}",
            "category": meta.get("categories", meta.get("category", [])), "tags": meta.get("tags", []),
            "authors": list((meta.get("authors") or {}).keys()) if isinstance(meta.get("authors"), dict)
            else list(meta.get("authors") or []),
            "bytes": dest.stat().st_size}


def discover_polyhaven(root, manifest, limit, dry):
    img_dir = root / "images" / "polyhaven"
    seen = {m["id"] for m in manifest if m["source"] == "Poly Haven"}
    assets = get("https://api.polyhaven.com/assets?t=textures")
    time.sleep(PAUSE)
    wanted = {k: v for k, v in assets.items() if set(v.get("categories", [])) & POLYHAVEN_CATEGORIES}
    print(f"Poly Haven: {len(wanted)} of {len(assets)} textures in the wanted categories")
    n, total = 0, 0
    for tid, meta in sorted(wanted.items()):
        if limit and n >= limit:
            break
        if tid in seen:
            n += 1
            continue
        if dry:
            n += 1
            total += 700_000
            continue
        dest = polyhaven_save(tid, img_dir / tid)
        if dest is None:
            print(f"  !! {tid}: no diffuse map")
            continue
        if not image_ok(dest):
            print(f"  !! {tid}: unusable image, dropped")
            dest.unlink(missing_ok=True)
            continue
        total += dest.stat().st_size
        manifest.append(polyhaven_record(root, dest, tid, meta))
        seen.add(tid)
        n += 1
    print(f"Poly Haven: {n} images, {total / 1e6:.0f} MB")
    return n


# --------------------------------------------------------------------------- OpenGameArt

def oga_zip(root, fname):
    raw = root / "raw" / fname
    if not raw.exists():
        download(OGA_FILES + fname, raw)
        time.sleep(PAUSE_ZIP)
    return raw


def oga_record(root, dest, tid, page):
    return {"file": str(dest.relative_to(root)), "source": "OpenGameArt", "id": tid,
            "licence": LICENCE, "url": f"https://opengameart.org/content/{page}",
            "authors": ["Screaming Brain Studios"], "bytes": dest.stat().st_size}


def discover_oga(root, manifest, limit, dry):
    img_dir = root / "images" / "sbs-tiny"
    seen = {m["id"] for m in manifest if m["source"] == "OpenGameArt"}
    n = 0
    for page, fname in OGA_PACKS:
        if dry:
            print(f"OpenGameArt: {fname}")
            continue
        with zipfile.ZipFile(oga_zip(root, fname)) as z:
            for m in z.namelist():
                if not m.lower().endswith((".png", ".jpg", ".jpeg")) or m.startswith("__MACOSX"):
                    continue
                tid = f"{fname}:{m}"
                if tid in seen or (limit and n >= limit):
                    continue
                dest = img_dir / fname.replace(".zip", "") / Path(m).name
                if not dest.exists():
                    dest.parent.mkdir(parents=True, exist_ok=True)
                    dest.write_bytes(z.read(m))
                if not image_ok(dest, min_side=128):
                    dest.unlink(missing_ok=True)
                    continue
                manifest.append(oga_record(root, dest, tid, page))
                seen.add(tid)
                n += 1
    print(f"OpenGameArt: {n} images")
    return n


# --------------------------------------------------------------------------- manifest mode

def load_manifest(path):
    return json.loads(Path(path).read_text())


def present(root, rec):
    return (root / rec["file"]).exists()


def fetch_from_manifest(root, records, sources, limit=0, dry=False):
    """Download exactly the listed assets that are not on disk yet.
    Returns (kept records with refreshed byte counts, missing records + reason)."""
    kept, missing = [], []
    by_source = {}
    for r in records:
        by_source.setdefault(r["source"], []).append(r)

    def take(source):
        recs = by_source.get(SOURCE_KEYS[source], []) if source in sources else []
        return recs[:limit] if limit else recs

    def keep(rec):
        rec = dict(rec)
        rec["bytes"] = (root / rec["file"]).stat().st_size
        kept.append(rec)

    # ambientCG: resolve ids through the API in batches, then the same zip dance
    recs = take("ambientcg")
    todo = [r for r in recs if not present(root, r)]
    for r in recs:
        if present(root, r):
            keep(r)
    print(f"ambientCG: {len(recs) - len(todo)} present, {len(todo)} to fetch")
    if todo:
        assets = ambientcg_by_ids([r["id"] for r in todo])
        for r in todo:
            a = assets.get(r["id"])
            url = ambientcg_1k(a)[0] if a else None
            if not url:
                missing.append({**r, "reason": "not listed by the API any more" if not a else "no 1K-JPG download"})
                continue
            if dry:
                continue
            dest = root / r["file"]
            try:
                ok = ambientcg_save(url, dest) and image_ok(dest)
            except Exception as e:
                ok, err = False, str(e)
            if ok:
                keep(r)
                if len(kept) % 25 == 0:
                    write_manifest(root, kept)
            else:
                dest.unlink(missing_ok=True)
                missing.append({**r, "reason": "download or image failed"})
        if dry:
            print(f"ambientCG: {len(todo) - len([m for m in missing if m['source'] == 'ambientCG'])} of {len(todo)} "
                  f"resolve through the API")

    # Poly Haven: per-asset files API, as before
    recs = take("polyhaven")
    todo = [r for r in recs if not present(root, r)]
    for r in recs:
        if present(root, r):
            keep(r)
    print(f"Poly Haven: {len(recs) - len(todo)} present, {len(todo)} to fetch")
    if not dry:
        for r in todo:
            dest = root / r["file"]
            try:
                got = polyhaven_save(r["id"], dest.with_suffix(""))
            except Exception as e:
                got = None
            if got is None or not image_ok(got):
                if got:
                    got.unlink(missing_ok=True)
                missing.append({**r, "reason": "no diffuse map / download failed"})
                continue
            if got != dest:                  # extension changed at the source
                r = {**r, "file": str(got.relative_to(root))}
            keep(r)
            if len(kept) % 25 == 0:
                write_manifest(root, kept)

    # OpenGameArt: the zips are fixed files; extract only the listed members
    recs = take("oga")
    todo = [r for r in recs if not present(root, r)]
    for r in recs:
        if present(root, r):
            keep(r)
    print(f"OpenGameArt: {len(recs) - len(todo)} present, {len(todo)} to fetch")
    if not dry:
        by_zip = {}
        for r in todo:
            by_zip.setdefault(r["id"].split(":", 1)[0], []).append(r)
        for fname, rs in by_zip.items():
            try:
                z = zipfile.ZipFile(oga_zip(root, fname))
            except Exception as e:
                missing += [{**r, "reason": f"zip unavailable: {e}"} for r in rs]
                continue
            with z:
                names = set(z.namelist())
                for r in rs:
                    member = r["id"].split(":", 1)[1]
                    if member not in names:
                        missing.append({**r, "reason": "member not in zip"})
                        continue
                    dest = root / r["file"]
                    dest.parent.mkdir(parents=True, exist_ok=True)
                    dest.write_bytes(z.read(member))
                    if not image_ok(dest, min_side=128):
                        dest.unlink(missing_ok=True)
                        missing.append({**r, "reason": "unusable image"})
                        continue
                    keep(r)
    return kept, missing


def write_manifest(root, records):
    (root / "manifest.json").write_text(json.dumps(records, indent=1))


def verify(root, records):
    """Every listed file present with the listed size.  Returns (missing, size_mismatch)."""
    missing, mismatch = [], []
    for r in records:
        p = root / r["file"]
        if not p.exists():
            missing.append(r["file"])
        elif r.get("bytes") and p.stat().st_size != r["bytes"]:
            mismatch.append((r["file"], r["bytes"], p.stat().st_size))
    return missing, mismatch


# --------------------------------------------------------------------------- entry

def summarise(manifest):
    by = {}
    for m in manifest:
        by[m["source"]] = by.get(m["source"], 0) + 1
    total = sum(m.get("bytes", 0) for m in manifest)
    return by, total


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--out", help=f"corpus root (default {data_root()})")
    ap.add_argument("--from-manifest", default=str(SHIPPED_MANIFEST), metavar="PATH",
                    help="reproduce the corpus this manifest lists (default: the shipped one)")
    ap.add_argument("--discover", action="store_true", help="build a new corpus from the sources' listings instead")
    ap.add_argument("--sources", default="ambientcg,polyhaven,oga")
    ap.add_argument("--limit", type=int, default=0, help="max images per source (0 = all)")
    ap.add_argument("--dry-run", action="store_true", help="report, download nothing")
    ap.add_argument("--verify", action="store_true", help="check every manifest file is present with its size; no network")
    args = ap.parse_args(argv)

    root = Path(args.out) if args.out else data_root()
    root.mkdir(parents=True, exist_ok=True)
    mpath = root / "manifest.json"
    sources = set(args.sources.split(","))
    bad = sources - set(SOURCE_KEYS)
    if bad:
        ap.error(f"unknown source(s) {sorted(bad)}; choose from {sorted(SOURCE_KEYS)}")

    if args.verify:
        records = load_manifest(args.from_manifest)
        missing, mismatch = verify(root, records)
        by, total = summarise(records)
        print(f"{args.from_manifest}: {len(records)} records {by}, {total / 1e6:.0f} MB expected under {root}")
        for f in missing[:20]:
            print(f"  missing  {f}")
        for f, want, got in mismatch[:20]:
            print(f"  size     {f}: manifest {want}, on disk {got}")
        print(f"{len(records) - len(missing)} present, {len(missing)} missing, {len(mismatch)} size mismatches")
        return 1 if missing else 0

    if args.discover:
        manifest = load_manifest(mpath) if mpath.exists() else []
        print(f"discover mode -> {root}  ({len(manifest)} images already listed)")
        try:
            if "ambientcg" in sources:
                discover_ambientcg(root, manifest, args.limit, args.dry_run)
            if "polyhaven" in sources:
                discover_polyhaven(root, manifest, args.limit, args.dry_run)
            if "oga" in sources:
                discover_oga(root, manifest, args.limit, args.dry_run)
        finally:
            if not args.dry_run:
                write_manifest(root, manifest)
                finish(root, manifest)
        return 0

    records = load_manifest(args.from_manifest)
    by, total = summarise(records)
    print(f"manifest mode: {args.from_manifest} -> {root}: {len(records)} records {by}, {total / 1e6:.0f} MB")
    kept, missing = fetch_from_manifest(root, records, sources, args.limit, args.dry_run)
    if args.dry_run:
        for m in missing:
            print(f"  !! {m['source']} {m['id']}: {m['reason']}")
        print(f"dry run: {len(kept)} present, {len(records) - len(kept) - len(missing)} would be fetched, "
              f"{len(missing)} unavailable")
        return 0
    # the written manifest is everything the shipped list knows that is on disk, plus any
    # extra records an earlier discover run added, so a --limit run never shrinks a full corpus
    known = {r["file"] for r in records}
    on_disk = [r for r in kept]
    listed = {r["file"] for r in on_disk}
    for r in records:
        if r["file"] not in listed and present(root, r):
            r = dict(r); r["bytes"] = (root / r["file"]).stat().st_size
            on_disk.append(r); listed.add(r["file"])
    if mpath.exists():
        for r in load_manifest(mpath):
            if r["file"] not in known and r["file"] not in listed and present(root, r):
                on_disk.append(r); listed.add(r["file"])
    order = {r["file"]: i for i, r in enumerate(records)}
    on_disk.sort(key=lambda r: order.get(r["file"], len(order)))
    write_manifest(root, on_disk)
    if missing:
        (root / "fetch-missing.json").write_text(json.dumps(missing, indent=1))
        print(f"!! {len(missing)} listed assets could not be fetched (see {root / 'fetch-missing.json'}):")
        for m in missing[:20]:
            print(f"   {m['source']} {m['id']}: {m['reason']}")
    finish(root, on_disk)
    return 0


def finish(root, manifest):
    by, total = summarise(manifest)
    print(f"manifest: {len(manifest)} images, {total / 1e6:.0f} MB on disk, by source {by}")
    from .credits import write_attribution
    print(f"attribution -> {write_attribution(root / 'manifest.json', root / 'ATTRIBUTION.md')}")
    print("Textures: ambientCG (CC0), Poly Haven (CC0, powered by the Poly Haven API), "
          "Screaming Brain Studios via OpenGameArt (CC0). Thank you.")


if __name__ == "__main__":
    sys.exit(main())
