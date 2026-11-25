#pragma once
#include <windows.h>
#include <string>

class ClockRenderer;

// Weather effects: abstract shapes (clouds, rain, ...) over the wallpaper, below the clock text
// and the desktop icons. Each effect is drawn ONCE (on the renderer's WARP device) into a few
// small composition surfaces; DirectComposition animations then move, scale and fade them
// forever, so DWM does the per-frame work and this process stays asleep.
namespace weatherfx {

enum class Kind { None, Clouds, Rain, Snow, Fog };

struct Params {
    Kind kind = Kind::None;
    int width = 0, height = 0;   // the monitor, physical px (the clock window covers it)
    float dpi = 96;
    float opacity = 0.4f;        // the most opaque a shape gets, 0..1
    COLORREF color = RGB(255, 255, 255);   // the shapes' colour (already chosen against the wallpaper)
    bool isDay = true;
    float windVx = 0;            // horizontal drift, DIPs per second (+ = to the right)
    float density = 1;           // 1 = normal; scales the number of particles
    bool lightning = false;      // thunderstorm: soft glows in the sky now and then (rain only)
    bool icy = false;            // freezing drizzle/rain: paler, slower, heavier drops (rain only)
    unsigned seed = 1;           // the same seed draws the same scene (self-tests)
};

// Short description for the log and for change detection, e.g. "rain 2560x1440@120 #DDE4F0 a40 w-35 d100".
std::wstring Describe(const Params& p);

// Replaces whatever the clock window `hwnd` shows below its text with the effect in `p`, fading
// it in over fadeSeconds. Kind::None (or a failure) leaves the layer empty. Commits.
bool Show(ClockRenderer& r, HWND hwnd, const Params& p, double fadeSeconds = 1.5);

// Removes the effect at once and frees its surfaces. Commits.
void Clear(ClockRenderer& r, HWND hwnd);

}   // namespace weatherfx
