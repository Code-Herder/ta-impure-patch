"""unditherer -- restore Total Annihilation's 256-colour dither back to true colour.

Classical stages (bilateral undither, deband, enhance) live in `classical`;
the learned restorer (a small residual CNN) in `model` / `infer`; `restore`
runs one image through either; `cli` is the command-line tool; `train`,
`evaluate`, `pipeline` reproduce the shipped models.  README.md explains it,
LEARNINGS.md records how the recipe was arrived at, CREDITS.md names the
people whose CC0 textures it was trained on.
"""
__version__ = "0.1.0"
