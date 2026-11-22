#pragma once
#include <windows.h>
#include "wallpaper.h"

// Tints `base` toward the hue of the wallpaper behind the clock, like the macOS lock screen's
// vibrant text. Works in OKLab: keeps the base colour's lightness (moved only as far as needed
// for the colour to fit in sRGB), takes the wallpaper's hue, and scales chroma by `strength`
// (0..1) and by how colourful the wallpaper actually is, so grey wallpapers leave text neutral.
// E.g. white over a dark purple night sky at strength 0.35 -> #E2DFFE (lavender).
COLORREF TintColor(COLORREF base, const WallpaperSample& wall, double strength);
