#pragma once
#include <windows.h>
#include <string>
#include "settings.h"
#include "weather.h"

// Building the weather line under the time from a reading. Pure functions (no globals), so the
// live clock and the --weather-test gallery produce exactly the same text.

// Hide the line once the reading is this old.
constexpr ULONGLONG kWeatherStaleSeconds = 3 * 60 * 60;

// "<cloud> 12°  feels 9°  ↑15° ↓7°  <wind> 32 km/h NW  <umbrella> 60% at 15:00  <sunset> 17:21"
// (icons are icons.h / render.h placeholders), or "" when the line
// is off for this monitor, the reading is invalid, stale (older than kWeatherStaleSeconds at
// nowUtc), or in other units than `fahrenheit` asks for. Each extra appears only when it
// matters (FeelsLike/FeelsLikeDiff, Wind/WindThreshold, RainThreshold/RainHours,
// SunTimes/SunHours in `s`). Times are shown in the PC's local time zone, in the clock's
// 12/24-hour format. Sunrise/sunset icons are the kSunriseIcon / kSunsetIcon placeholders
// from render.h. nowUtc: FILETIME ticks, UTC.
// omit: extras to leave out (WeatherExtra flags); shown (optional): the extras the line has.
enum WeatherExtra : unsigned { kExtraFeels = 1, kExtraWind = 2, kExtraRain = 4, kExtraSun = 8, kExtraAir = 16, kExtraUv = 32 };
std::wstring FormatWeatherLine(const WeatherReading& w, ULONGLONG nowUtc, const Settings& s, bool fahrenheit,
                               unsigned omit = 0, unsigned* shown = nullptr);

// Prepare() a frame (see render.h) whose weather line is no wider than the time: on a busy day
// extras are left out, least important first (sunrise/sunset, wind, UV, air, feels-like, rain), until it
// fits. The reference is the time with every digit as "0", so the result doesn't change with the
// digits from minute to minute. w may be null (no line). line/dropped (optional): the text used
// and the extras left out.
class ClockRenderer;
struct FrameLayout;
bool PrepareFitted(ClockRenderer& r, const Settings& s, UINT dpi, const std::wstring& date, const std::wstring& time,
                   const WeatherReading* w, ULONGLONG nowUtc, bool fahrenheit, FrameLayout& f,
                   std::wstring* line = nullptr, unsigned* dropped = nullptr, const std::wstring& media = std::wstring());

// A track title without the extras that crowd the now-playing line: bracketed "(feat. ...)",
// "(with ...)", "[From ...]", "(prod. ...)", remaster notes, and a " - 2011 Remaster" suffix.
// Version info that changes the music ("(Live)", "(Remix)", "(Acoustic)") stays.
std::wstring CleanTrackTitle(const std::wstring& title);

// Moon phase 0..7 (0 new, 4 full) at nowUtc, as seen from the given hemisphere.
int MoonPhaseIndex(ULONGLONG nowUtc, bool southern);

// Whether it is night at nowUtc according to the reading's sunrise/sunset times (the next sun
// event is a sunrise). Returns false when the reading has no usable sun times.
bool SunNight(const WeatherReading& w, ULONGLONG nowUtc, bool& night);

// Current time as FILETIME ticks (UTC).
ULONGLONG NowUtc();
