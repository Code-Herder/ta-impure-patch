"""Where things live.

The package ships its own palette, configs and final models.  The corpus and
the training runs are large and sit OUTSIDE git, at the main checkout root
(shared by every worktree, like the venv), unless UNDITHERER_DATA points
somewhere else.
"""
import os
import subprocess
from pathlib import Path

PKG = Path(__file__).resolve().parent
REPO = PKG.parent
MODELS = PKG / "models"
CONFIGS = PKG / "configs"
PALETTE = PKG / "ta-palette.json"
CORPUS = PKG / "corpus"                      # the shipped corpus manifest + attribution
SHIPPED_MANIFEST = CORPUS / "manifest.json"
LICENSES = PKG / "LICENSES"


def main_checkout_root():
    """The primary checkout of this repository (a worktree's parent)."""
    try:
        common = subprocess.run(["git", "rev-parse", "--git-common-dir"], capture_output=True,
                                text=True, check=True, cwd=PKG).stdout.strip()
        p = Path(common)
        if not p.is_absolute():
            p = PKG / p
        return p.resolve().parent
    except Exception:
        return REPO


def data_root():
    """Corpus root: images/, manifest.json, raw/, runs/."""
    env = os.environ.get("UNDITHERER_DATA")
    return Path(env).expanduser().resolve() if env else main_checkout_root() / ".data" / "undither-train"


def runs_root():
    return data_root() / "runs"
