"""fetch_data: the shipped manifest's integrity, and manifest mode end to end
with the network replaced by fakes (no downloads)."""
import io
import json
import zipfile
from pathlib import Path

import numpy as np
import pytest
from PIL import Image

from unditherer import fetch_data as F
from unditherer.paths import SHIPPED_MANIFEST


def test_shipped_manifest_integrity():
    m = json.loads(SHIPPED_MANIFEST.read_text())
    assert len(m) == 1548
    assert {r["source"] for r in m} == {"ambientCG", "Poly Haven", "OpenGameArt"}
    assert all(r["licence"] == "CC0 1.0" for r in m)
    assert len({r["file"] for r in m}) == len(m)
    for r in m:
        assert r["id"] and r["url"].startswith("https://") and r["file"].startswith("images/") and r["bytes"] > 0
    by = {}
    for r in m:
        by[r["source"]] = by.get(r["source"], 0) + 1
    assert by == {"ambientCG": 496, "Poly Haven": 527, "OpenGameArt": 525}
    assert all(r["id"].split(":", 1)[0] in {f for _, f in F.OGA_PACKS} for r in m if r["source"] == "OpenGameArt")


# ---- a fake internet ---------------------------------------------------------

def _jpeg_bytes(side=256, seed=0):
    rng = np.random.default_rng(seed)
    buf = io.BytesIO()
    Image.fromarray(rng.integers(0, 255, (side, side, 3), dtype=np.uint8)).save(buf, "JPEG")
    return buf.getvalue()


def _png_bytes(side=256, seed=0):
    rng = np.random.default_rng(seed)
    buf = io.BytesIO()
    Image.fromarray(rng.integers(0, 255, (side, side, 3), dtype=np.uint8)).save(buf, "PNG")
    return buf.getvalue()


def _zip_bytes(members):
    buf = io.BytesIO()
    with zipfile.ZipFile(buf, "w") as z:
        for name, data in members.items():
            z.writestr(name, data)
    return buf.getvalue()


MANIFEST = [
    {"file": "images/ambientcg/Ground001.jpg", "source": "ambientCG", "id": "Ground001", "licence": "CC0 1.0",
     "url": "https://ambientcg.com/a/Ground001", "category": "Ground", "tags": ["dirt"], "bytes": 1},
    {"file": "images/ambientcg/Gone999.jpg", "source": "ambientCG", "id": "Gone999", "licence": "CC0 1.0",
     "url": "https://ambientcg.com/a/Gone999", "category": "Ground", "tags": [], "bytes": 1},
    {"file": "images/polyhaven/rock_01.jpg", "source": "Poly Haven", "id": "rock_01", "licence": "CC0 1.0",
     "url": "https://polyhaven.com/a/rock_01", "category": ["rock"], "tags": [], "authors": ["Someone"], "bytes": 1},
    {"file": "images/sbs-tiny/sbs_-_tiny_texture_pack_512x512_a/Brick 1.png", "source": "OpenGameArt",
     "id": "sbs_-_tiny_texture_pack_512x512_a.zip:SBS/Bricks/Brick 1.png", "licence": "CC0 1.0",
     "url": "https://opengameart.org/content/tiny-texture-pack", "authors": ["Screaming Brain Studios"], "bytes": 1},
    {"file": "images/sbs-tiny/sbs_-_tiny_texture_pack_512x512_a/Missing.png", "source": "OpenGameArt",
     "id": "sbs_-_tiny_texture_pack_512x512_a.zip:SBS/Missing.png", "licence": "CC0 1.0",
     "url": "https://opengameart.org/content/tiny-texture-pack", "authors": ["Screaming Brain Studios"], "bytes": 1},
]


@pytest.fixture
def fake_net(monkeypatch):
    calls = {"get": [], "download": []}
    acg_zip = _zip_bytes({"Ground001_1K-JPG_Color.jpg": _jpeg_bytes(seed=1), "Ground001_1K-JPG_Normal.jpg": b"x"})
    oga_zip = _zip_bytes({"SBS/Bricks/Brick 1.png": _png_bytes(seed=2), "__MACOSX/._x": b""})

    def fake_get(url, retries=3, binary=False):
        calls["get"].append(url)
        if "ambientcg.com/api/v2/full_json" in url:
            ids = url.split("id=")[1].split("&")[0].replace("%2C", ",").split(",")
            found = [{"assetId": i, "shortLink": f"https://ambientcg.com/a/{i}", "displayCategory": "Ground",
                      "tags": ["dirt"],
                      "downloadFolders": {"default": {"downloadFiletypeCategories": {"zip": {"downloads": [
                          {"attribute": "1K-JPG", "downloadLink": f"https://fake/{i}.zip", "size": 100}]}}}}}
                     for i in ids if i != "Gone999"]
            return {"foundAssets": found, "numberOfResults": len(found)}
        if url.endswith(".zip") and binary:
            return acg_zip
        if "api.polyhaven.com/files/" in url:
            return {"Diffuse": {"1k": {"jpg": {"url": "https://fake/rock_01.jpg"}}}}
        raise AssertionError(f"unexpected GET {url}")

    def fake_download(url, dest, retries=3):
        calls["download"].append(url)
        dest = Path(dest)
        dest.parent.mkdir(parents=True, exist_ok=True)
        dest.write_bytes(oga_zip if url.endswith(".zip") else _jpeg_bytes(seed=3))
        return dest

    monkeypatch.setattr(F, "get", fake_get)
    monkeypatch.setattr(F, "download", fake_download)
    monkeypatch.setattr(F.time, "sleep", lambda s: None)
    return calls


def test_manifest_mode_end_to_end(tmp_path, fake_net, capsys):
    mpath = tmp_path / "shipped.json"
    mpath.write_text(json.dumps(MANIFEST))
    root = tmp_path / "corpus"
    rc = F.main(["--out", str(root), "--from-manifest", str(mpath)])
    out = capsys.readouterr().out
    assert rc == 0
    assert (root / "images/ambientcg/Ground001.jpg").exists()
    assert (root / "images/polyhaven/rock_01.jpg").exists()
    assert (root / "images/sbs-tiny/sbs_-_tiny_texture_pack_512x512_a/Brick 1.png").exists()
    assert (root / "raw/sbs_-_tiny_texture_pack_512x512_a.zip").exists()
    written = json.loads((root / "manifest.json").read_text())
    assert [r["id"] for r in written] == ["Ground001", "rock_01", MANIFEST[3]["id"]]
    assert all(r["bytes"] > 1 for r in written)                      # refreshed from disk
    missing = json.loads((root / "fetch-missing.json").read_text())
    assert {m["id"] for m in missing} == {"Gone999", MANIFEST[4]["id"]}
    assert "not listed by the API" in missing[0]["reason"] or "member not in zip" in missing[0]["reason"]
    assert (root / "ATTRIBUTION.md").exists() and "Thank you" in out
    # second run: everything present, no network traffic
    n_get, n_dl = len(fake_net["get"]), len(fake_net["download"])
    assert F.main(["--out", str(root), "--from-manifest", str(mpath), "--sources", "polyhaven,oga"]) == 0
    assert len(fake_net["get"]) == n_get and len(fake_net["download"]) == n_dl
    assert len(json.loads((root / "manifest.json").read_text())) == 3    # a partial run never shrinks the manifest


def test_verify_reports_missing_and_sizes(tmp_path, capsys):
    mpath = tmp_path / "m.json"
    root = tmp_path / "c"
    (root / "images/ambientcg").mkdir(parents=True)
    (root / "images/ambientcg/Ground001.jpg").write_bytes(b"12345")
    mpath.write_text(json.dumps([MANIFEST[0] | {"bytes": 5}, MANIFEST[2]]))
    assert F.main(["--out", str(root), "--from-manifest", str(mpath), "--verify"]) == 1
    assert "1 present, 1 missing, 0 size mismatches" in capsys.readouterr().out
    mpath.write_text(json.dumps([MANIFEST[0] | {"bytes": 4}]))
    assert F.main(["--out", str(root), "--from-manifest", str(mpath), "--verify"]) == 0
    assert "1 size mismatches" in capsys.readouterr().out


def test_dry_run_touches_only_the_listing_api(tmp_path, fake_net, capsys):
    mpath = tmp_path / "m.json"
    mpath.write_text(json.dumps(MANIFEST))
    assert F.main(["--out", str(tmp_path / "c"), "--from-manifest", str(mpath), "--dry-run"]) == 0
    out = capsys.readouterr().out
    assert "would be fetched" in out and "Gone999" in out
    assert all("full_json" in u for u in fake_net["get"]) and not fake_net["download"]
    assert not (tmp_path / "c" / "manifest.json").exists()


def test_bad_source_is_a_usage_error(tmp_path):
    with pytest.raises(SystemExit) as e:
        F.main(["--out", str(tmp_path), "--sources", "shutterstock"])
    assert e.value.code == 2
