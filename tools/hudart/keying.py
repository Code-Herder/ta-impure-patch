"""Despill the magenta key out of a painting's colours.
An edge pixel is the paint mixed with the key, p * a + (255, 0, 255) * (1 - a). For neutral paint (r = g = b)
the key's share is exactly (min(r, b) - g) / 255, so it can be taken back out; paint with no red-and-blue
excess over green (greys, khaki, the reds and oranges of TA's indicators) is left as it is. Without this a dark
outline anti-aliased into the key survives as an opaque dark-purple line around the part."""
import numpy as np

def despill(im):
    r, g, b = im[..., 0], im[..., 1], im[..., 2]
    s = np.clip((np.minimum(r, b) - g - 6) / 255, 0, .7)[..., None]
    return np.clip((im - s * np.array([255., 0., 255.])) / (1 - s), 0, 255)
