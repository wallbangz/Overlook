#pragma once
#include <windows.h>
#include <string>

struct WallpaperSample {
    double lightness = 0;      // mean perceived lightness, CIE L* (0..100)
    double r = 0, g = 0, b = 0; // mean colour, linear-light sRGB (0..1)
};

// Samples the desktop wallpaper inside `region` (physical screen pixels, PerMonitorV2
// coordinates) on the monitor whose full rect is `monitorRect`. Honours the wallpaper
// position mode (fill/fit/stretch/tile/center/span) and the solid background colour.
// Caches the decoded image; cheap to call every minute.
// Must be called on a thread with COM initialised. Returns false if it can't tell.
bool SampleWallpaper(const RECT& monitorRect, const RECT& region, WallpaperSample& out);

// Drops the cache (e.g. after WM_SETTINGCHANGE / SPI_SETDESKWALLPAPER).
void InvalidateWallpaperCache();

// The image the desktop shows on that monitor (empty = solid colour only), its position mode
// (DESKTOP_WALLPAPER_POSITION) and the colour around it. Needs COM. For --preview.
bool WallpaperFile(const RECT& monitorRect, std::wstring& path, int& position, COLORREF& bg);
