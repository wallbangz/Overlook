#pragma once
#include <windows.h>
#include <d2d1_1.h>
#include <wrl/client.h>

// Weather icons drawn by the app, in one style with the sunrise/sunset icons: filled shapes and
// round strokes, with a small gap where one shape sits in front of another. In the weather
// line's text each icon is a placeholder character from the private use area.
namespace icon {

constexpr wchar_t Clear = 0xE010, ClearNight = 0xE011, MainlyClear = 0xE012, MainlyClearNight = 0xE013,
                  PartlyCloudy = 0xE014, PartlyCloudyNight = 0xE015, Overcast = 0xE016, Fog = 0xE017,
                  Drizzle = 0xE018, Rain = 0xE019, Showers = 0xE01A, ShowersNight = 0xE01B, Snow = 0xE01C,
                  Thunder = 0xE01D, Wind = 0xE01E, Umbrella = 0xE01F;
// Moon phases, as seen from the northern hemisphere: new, waxing crescent, first quarter, waxing
// gibbous, full, waning gibbous, last quarter, waning crescent.
constexpr wchar_t MoonPhase0 = 0xE020;   // MoonPhase0 + 0..7
constexpr wchar_t Music = 0xE028;        // the now-playing line

inline bool IsDrawn(wchar_t c) { return c >= Clear && c <= Music; }

// Width of the icon's box in icon units (1 = the icon's height).
float Width(wchar_t c);

// The whole icon as one filled geometry in icon units: x in [0, Width(c)], y in [0, 1].
// Shapes are unioned, so a translucent brush covers every pixel once.
Microsoft::WRL::ComPtr<ID2D1Geometry> Build(ID2D1Factory1* factory, wchar_t c);

// "[rain]" etc., for text reports.
const wchar_t* Name(wchar_t c);

}  // namespace icon
