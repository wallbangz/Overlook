#include "color.h"
#include <cmath>
#include <algorithm>

namespace {

struct Lab { double L, a, b; };
struct Rgb { double r, g, b; };   // linear-light sRGB

double ToLinear(int v) {
    double c = v / 255.0;
    return c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4);
}

int ToSrgb8(double c) {
    c = std::clamp(c, 0.0, 1.0);
    c = c <= 0.0031308 ? c * 12.92 : 1.055 * std::pow(c, 1 / 2.4) - 0.055;
    return (int)std::lround(c * 255.0);
}

// Björn Ottosson's OKLab (https://bottosson.github.io/posts/oklab/).
Lab ToOklab(const Rgb& c) {
    double l = 0.4122214708 * c.r + 0.5363325363 * c.g + 0.0514459929 * c.b;
    double m = 0.2119034982 * c.r + 0.6806995451 * c.g + 0.1073969566 * c.b;
    double s = 0.0883024619 * c.r + 0.2817188376 * c.g + 0.6299787005 * c.b;
    l = std::cbrt(l); m = std::cbrt(m); s = std::cbrt(s);
    return { 0.2104542553 * l + 0.7936177850 * m - 0.0040720468 * s,
             1.9779984951 * l - 2.4285922050 * m + 0.4505937099 * s,
             0.0259040371 * l + 0.7827717662 * m - 0.8086757660 * s };
}

Rgb FromOklab(const Lab& c) {
    double l = c.L + 0.3963377774 * c.a + 0.2158037573 * c.b;
    double m = c.L - 0.1055613458 * c.a - 0.0638541728 * c.b;
    double s = c.L - 0.0894841775 * c.a - 1.2914855480 * c.b;
    l = l * l * l; m = m * m * m; s = s * s * s;
    return { +4.0767416621 * l - 3.3077115913 * m + 0.2309699292 * s,
             -1.2684380046 * l + 2.6097574011 * m - 0.3413193965 * s,
             -0.0041960863 * l - 0.7034186147 * m + 1.7076147010 * s };
}

bool InGamut(const Rgb& c) {
    const double e = 1e-4;
    return c.r >= -e && c.r <= 1 + e && c.g >= -e && c.g <= 1 + e && c.b >= -e && c.b <= 1 + e;
}

}  // namespace

COLORREF TintColor(COLORREF base, const WallpaperSample& wall, double strength) {
    if (strength <= 0) return base;
    const Lab b = ToOklab({ ToLinear(GetRValue(base)), ToLinear(GetGValue(base)), ToLinear(GetBValue(base)) });
    const Lab w = ToOklab({ wall.r, wall.g, wall.b });
    const double wallChroma = std::hypot(w.a, w.b);
    if (wallChroma < 1e-4) return base;

    // Full tint once the wallpaper is moderately colourful (OKLCH chroma ~0.05);
    // at strength 1 the text gets chroma 0.12 (clearly coloured, still light).
    const double kMaxChroma = 0.12, kWallRef = 0.05;
    double C = std::min(1.0, strength) * kMaxChroma * std::min(1.0, wallChroma / kWallRef);
    const double ha = w.a / wallChroma, hb = w.b / wallChroma;   // hue direction

    // Near white (or black) there is little room for colour in sRGB, so light text is darkened
    // (dark text lightened) just as far as needed for the tint to fit, by at most 0.12 L;
    // only if it still doesn't fit is the chroma reduced.
    const bool light = b.L > 0.5;
    for (int i = 0; i < 16; ++i, C *= 0.85) {
        for (int k = 0; k <= 24; ++k) {
            const double L = light ? b.L - 0.005 * k : b.L + 0.005 * k;
            const Rgb out = FromOklab({ L, b.a + C * ha, b.b + C * hb });
            if (InGamut(out)) return RGB(ToSrgb8(out.r), ToSrgb8(out.g), ToSrgb8(out.b));
        }
    }
    return base;
}
