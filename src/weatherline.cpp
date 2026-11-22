#include "weatherline.h"
#include "render.h"   // kSunriseIcon / kSunsetIcon
#include "icons.h"
#include <cmath>
#include <cwctype>

namespace {

constexpr ULONGLONG kTicksPerSecond = 10000000ULL;
constexpr ULONGLONG kTicksPerHour = 3600ULL * kTicksPerSecond;
constexpr ULONGLONG kTicksPerDay = 24ULL * kTicksPerHour;

// WMO weather interpretation code -> one of the app's icons (icons.h). moon: the icon for a
// clear night (the current phase).
wchar_t WeatherIcon(int code, bool day, wchar_t moon) {
    switch (code) {
    case 0:  return day ? icon::Clear : moon;
    case 1:  return day ? icon::MainlyClear : icon::MainlyClearNight;
    case 2:  return day ? icon::PartlyCloudy : icon::PartlyCloudyNight;
    case 3:  return icon::Overcast;
    case 45: case 48: return icon::Fog;
    case 51: case 53: case 55: case 56: case 57: return icon::Drizzle;
    case 61: case 63: case 65: case 66: case 67: return icon::Rain;
    case 80: case 81: case 82: return day ? icon::Showers : icon::ShowersNight;
    case 71: case 73: case 75: case 77: case 85: case 86: return icon::Snow;
    case 95: case 96: case 99: return icon::Thunder;
    default: return icon::Overcast;
    }
}

std::wstring Degrees(double v) {
    const long r = lround(v);
    wchar_t b[16];
    swprintf_s(b, r < 0 ? L"−%ld°" : L"%ld°", r < 0 ? -r : r);   // typographic minus
    return b;
}

// Minutes after local midnight (the PC's time zone) of a UTC instant, or -1.
int LocalMinutes(ULONGLONG utc) {
    const FILETIME ft{ (DWORD)utc, (DWORD)(utc >> 32) };
    SYSTEMTIME su, sl;
    if (!FileTimeToSystemTime(&ft, &su) || !SystemTimeToTzSpecificLocalTime(nullptr, &su, &sl)) return -1;
    return sl.wHour * 60 + sl.wMinute;
}

std::wstring ClockText(int minutes, const Settings& s) {   // "07:38" / "7:38 AM", like the clock
    const int h = minutes / 60 % 24, m = minutes % 60;
    wchar_t b[24];
    if (s.hour24) swprintf_s(b, s.leadingZero ? L"%02d:%02d" : L"%d:%02d", h, m);
    else          swprintf_s(b, L"%d:%02d %s", (h + 11) % 12 + 1, m, h < 12 ? L"AM" : L"PM");
    return b;
}

// The first sunrise/sunset after nowUtc. A cached reading can be days old: its times are moved
// on by whole days until one lies ahead (sun times drift only a few minutes a day).
bool NextSunEvent(const WeatherReading& w, ULONGLONG nowUtc, ULONGLONG& at, bool& sunrise) {
    for (ULONGLONG days = 0; days < 8; ++days) {
        bool found = false;
        for (int e = 0; e < 4; ++e) {
            if (!w.sunUtc[e]) continue;
            const ULONGLONG t = w.sunUtc[e] + days * kTicksPerDay;
            if (t > nowUtc && (!found || t < at)) { found = true; at = t; sunrise = (e % 2) == 0; }
        }
        if (found) return true;
    }
    return false;   // no sun times (polar day/night, or an old cache)
}

}  // namespace

ULONGLONG NowUtc() {
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    return (ULONGLONG(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
}

bool SunNight(const WeatherReading& w, ULONGLONG nowUtc, bool& night) {
    ULONGLONG at = 0;
    bool sunrise = false;
    if (!w.valid || !NextSunEvent(w, nowUtc, at, sunrise)) return false;
    night = sunrise;
    return true;
}

std::wstring FormatWeatherLine(const WeatherReading& w, ULONGLONG nowUtc, const Settings& s, bool fahrenheit,
                               unsigned omit, unsigned* shown) {
    if (shown) *shown = 0;
    auto use = [&](unsigned extra) {   // an extra that is due: show it unless it is to be left out
        if (omit & extra) return false;
        if (shown) *shown |= extra;
        return true;
    };
    if (!w.valid || !s.showWeather) return L"";
    if (w.fahrenheit != fahrenheit) return L"";   // units just changed: wait for the new reading
    const ULONGLONG thenU = (ULONGLONG(w.fetchedUtc.dwHighDateTime) << 32) | w.fetchedUtc.dwLowDateTime;
    // (A reading "from the future" means the PC clock was set back: keep showing it.)
    if (nowUtc > thenU && (nowUtc - thenU) / kTicksPerSecond > kWeatherStaleSeconds) return L"";

    const wchar_t* en = L"\u2002";   // en space (escaped: an invisible character is easily lost in editing)
    const wchar_t* em = L"\u2002\u2009";   // between groups: en + thin space (~0.7 em; a full em left holes)
    const wchar_t moon = (wchar_t)(icon::MoonPhase0 + MoonPhaseIndex(nowUtc, w.latitude < 0));
    std::wstring line = std::wstring(1, WeatherIcon(w.code, w.isDay, moon)) + en + Degrees(w.temp);
    if (s.feelsLike && w.hasApparent && std::fabs(w.apparent - w.temp) >= s.feelsLikeDiff &&
        lround(w.apparent) != lround(w.temp) && use(kExtraFeels))
        line += std::wstring(em) + L"feels " + Degrees(w.apparent);
    line += std::wstring(em) + L"↑" + Degrees(w.high) + en + L"↓" + Degrees(w.low);   // ↑ high  ↓ low
    if (s.wind && w.hasWind && w.windSpeed >= s.windThreshold && use(kExtraWind)) {
        static const wchar_t* kCompass[] = { L"N", L"NE", L"E", L"SE", L"S", L"SW", L"W", L"NW" };
        wchar_t b[48];
        swprintf_s(b, L"%ld %s %s", lround(w.windSpeed), w.fahrenheit ? L"mph" : L"km/h",
                   kCompass[((w.windDir % 360 + 360) % 360 * 2 + 45) / 90 % 8]);   // the direction it blows from
        line += std::wstring(em) + icon::Wind + en + b;
    }
    // Rain: the hour slot with the highest chance within RainHours. "now" while that slot runs;
    // a slot that has already ended (the reading is up to 30 min old) is dropped.
    if (w.rainChance >= 0 && w.rainChance >= s.rainThreshold && w.rainAtUtc &&
        w.rainAtUtc + kTicksPerHour > nowUtc &&
        w.rainAtUtc < nowUtc + (ULONGLONG)(s.rainHours * (double)kTicksPerHour) && use(kExtraRain)) {
        wchar_t b[48];
        const int at = w.rainAtUtc <= nowUtc ? 0 : LocalMinutes(w.rainAtUtc);
        if (w.rainAtUtc <= nowUtc) swprintf_s(b, L"%d%% now", w.rainChance);
        else if (at >= 0)          swprintf_s(b, L"%d%% at %s", w.rainChance, ClockText(at, s).c_str());
        else                       swprintf_s(b, L"%d%%", w.rainChance);   // (the time couldn't be converted)
        line += std::wstring(em) + icon::Umbrella + en + b;
    }
    // Air quality from "poor": European index, or the US index with Fahrenheit.
    if (s.airQuality) {
        const wchar_t* air = nullptr;
        if (!fahrenheit && w.euAqi >= 60)
            air = w.euAqi >= 100 ? L"Air extremely poor" : w.euAqi >= 80 ? L"Air very poor" : L"Air poor";
        if (fahrenheit && w.usAqi >= 101)
            air = w.usAqi >= 301 ? L"Air hazardous" : w.usAqi >= 201 ? L"Air very unhealthy"
                : w.usAqi >= 151 ? L"Air unhealthy" : L"Air poor";
        if (air && use(kExtraAir)) line += std::wstring(em) + air;
    }
    if (s.uv && w.uvIndex >= 0 && w.uvIndex >= s.uvThreshold && lround(w.uvIndex) > 0 && use(kExtraUv)) {
        wchar_t b[16];
        swprintf_s(b, L"UV %ld", lround(w.uvIndex));
        line += std::wstring(em) + b;
    }
    ULONGLONG at = 0;
    bool sunrise = false;
    if (s.sunTimes && NextSunEvent(w, nowUtc, at, sunrise) &&
        (s.sunHours <= 0 || at - nowUtc <= (ULONGLONG)(s.sunHours * (double)kTicksPerHour)) && LocalMinutes(at) >= 0 &&
        use(kExtraSun))
        line += std::wstring(em) + (sunrise ? kSunriseIcon : kSunsetIcon) + en + ClockText(LocalMinutes(at), s);
    return line;
}

bool PrepareFitted(ClockRenderer& r, const Settings& s, UINT dpi, const std::wstring& date, const std::wstring& time,
                   const WeatherReading* w, ULONGLONG nowUtc, bool fahrenheit, FrameLayout& f,
                   std::wstring* line, unsigned* dropped, const std::wstring& media) {
    static const unsigned kDropOrder[] = { kExtraSun, kExtraWind, kExtraUv, kExtraAir, kExtraFeels, kExtraRain };   // least important first
    unsigned omit = 0, shown = 0;
    std::wstring text = w ? FormatWeatherLine(*w, nowUtc, s, fahrenheit, omit, &shown) : std::wstring();
    if (!r.Prepare(s, dpi, date, time, text, f, media)) return false;
    for (unsigned extra : kDropOrder) {
        if (!f.weather || f.weatherWidth <= f.timeRefWidth * (float)(s.weatherMaxWidth / 100.0)) break;
        if (!(shown & extra)) continue;
        omit |= extra;
        text = FormatWeatherLine(*w, nowUtc, s, fahrenheit, omit, &shown);
        if (!r.Prepare(s, dpi, date, time, text, f, media)) return false;
    }
    r.CentreLines(f);
    if (line) *line = text;
    if (dropped) *dropped = omit;
    return true;
}

int MoonPhaseIndex(ULONGLONG nowUtc, bool southern) {
    // Days since a known new moon (2000-01-06 18:14 UTC), modulo the mean synodic month.
    constexpr double kSynodic = 29.530588853;
    constexpr ULONGLONG kNewMoon2000 = 125916560400000000ULL;   // 2000-01-06 18:14 UTC as FILETIME ticks
    const double days = ((double)nowUtc - (double)kNewMoon2000) / (double)kTicksPerDay;
    double age = std::fmod(days, kSynodic);
    if (age < 0) age += kSynodic;
    const int i = (int)std::floor(age / kSynodic * 8.0 + 0.5) % 8;
    return southern ? (8 - i) % 8 : i;   // seen from the south, the lit side is mirrored
}

std::wstring CleanTrackTitle(const std::wstring& title) {
    auto lower = [](std::wstring s) { for (auto& ch : s) ch = (wchar_t)towlower(ch); return s; };
    auto crowds = [&](const std::wstring& inside) {   // content of a bracketed group
        static const wchar_t* kStarts[] = { L"feat", L"ft.", L"ft ", L"featuring", L"with ", L"prod", L"from " };
        std::wstring s = lower(inside);
        while (!s.empty() && iswspace(s.front())) s.erase(s.begin());
        for (const wchar_t* k : kStarts) if (s.rfind(k, 0) == 0) return true;
        return s.find(L"remaster") != std::wstring::npos;
    };
    std::wstring t = title;
    for (size_t from = 0;;) {
        const size_t open = t.find_first_of(L"([", from);
        if (open == std::wstring::npos) break;
        const size_t close = t.find(t[open] == L'(' ? L')' : L']', open + 1);
        if (close == std::wstring::npos) break;
        if (crowds(t.substr(open + 1, close - open - 1))) {
            size_t start = open;
            while (start > 0 && iswspace(t[start - 1])) --start;   // with the space before it
            t.erase(start, close + 1 - start);
            from = start;
        } else {
            from = close + 1;
        }
    }
    // Spotify-style " - 2011 Remaster" / " - Remastered 2009" suffixes.
    const size_t dash = t.rfind(L" - ");
    if (dash != std::wstring::npos && lower(t.substr(dash)).find(L"remaster") != std::wstring::npos) t.erase(dash);
    while (!t.empty() && iswspace(t.back())) t.pop_back();
    return t.empty() ? title : t;
}
