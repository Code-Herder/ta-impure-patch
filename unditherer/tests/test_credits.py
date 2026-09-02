import json

from unditherer.credits import SOURCES, attribution_markdown, main, summarise, write_attribution

MANIFEST = [
    {"file": "images/ambientcg/Ground001.jpg", "source": "ambientCG", "id": "Ground001", "licence": "CC0 1.0",
     "url": "https://ambientcg.com/a/Ground001", "bytes": 10},
    {"file": "images/polyhaven/rock_01.jpg", "source": "Poly Haven", "id": "rock_01", "licence": "CC0 1.0",
     "url": "https://polyhaven.com/a/rock_01", "authors": ["Someone"], "bytes": 20},
    {"file": "images/sbs-tiny/a/Brick 1.png", "source": "OpenGameArt", "id": "pack.zip:Dir/Brick 1.png",
     "licence": "CC0 1.0", "url": "https://opengameart.org/content/tiny-texture-pack",
     "authors": ["Screaming Brain Studios"], "bytes": 30},
]


def test_sources_have_links_and_licences():
    for s in SOURCES.values():
        assert s["site"].startswith("https://") and s["licence_url"].startswith("https://")
        assert "CC0" in s["licence"] and s["credit_line"]


def test_summarise_counts():
    s = summarise(MANIFEST)
    assert s["Poly Haven"]["authors"]["Someone"] == 1 and s["ambientCG"]["images"] == 1
    assert s["OpenGameArt"]["bytes"] == 30


def test_attribution_markdown_lists_every_asset():
    md = attribution_markdown(MANIFEST)
    for needle in ("## ambientCG", "## Poly Haven", "Screaming Brain Studios", "<https://ambientcg.com/a/Ground001>",
                   "| Brick 1.png |", "Someone (1)", "CC0 1.0 Universal"):
        assert needle in md, needle


def test_write_and_main(tmp_path, capsys):
    m = tmp_path / "manifest.json"
    m.write_text(json.dumps(MANIFEST))
    out = write_attribution(m, tmp_path / "ATTRIBUTION.md")
    assert out.exists() and "Training corpus attribution" in out.read_text()
    main(["--manifest", str(m), "--out", str(tmp_path / "again.md")])
    captured = capsys.readouterr().out
    assert "Poly Haven" in captured and (tmp_path / "again.md").exists()
