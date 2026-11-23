#include "selftest.h"
#include "render.h"
#include "icons.h"
#include "settings.h"
#include "weather.h"
#include "weatherline.h"
#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <string>
#include <vector>

namespace {

constexpr ULONGLONG kSec = 10000000ULL, kMin = 60 * kSec, kHour = 60 * kMin;

ULONGLONG ToTicks(const FILETIME& ft) { return ((ULONGLONG)ft.dwHighDateTime << 32) | ft.dwLowDateTime; }
FILETIME ToFileTime(ULONGLONG t) { return { (DWORD)t, (DWORD)(t >> 32) }; }

// The line shows the sunrise/sunset icons as private-use placeholders; make them readable in text.
std::wstring Visible(std::wstring s) {
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == kSunriseIcon || s[i] == kSunsetIcon || icon::IsDrawn(s[i])) {
            const wchar_t* name = s[i] == kSunriseIcon ? L"[sunrise]" : s[i] == kSunsetIcon ? L"[sunset]" : icon::Name(s[i]);
            s.replace(i, 1, name);
            i += wcslen(name) - 1;
        }
    }
    return s;
}

std::wstring Fmt(const wchar_t* fmt, ...) {
    wchar_t buf[512];
    va_list ap;
    va_start(ap, fmt);
    _vsnwprintf_s(buf, _TRUNCATE, fmt, ap);
    va_end(ap);
    return buf;
}

// UTC ticks as "YYYY-MM-DD HH:MM" in the PC's local zone, or at a fixed offset (seconds from UTC).
std::wstring LocalStr(ULONGLONG utc) {
    if (!utc) return L"-";
    const FILETIME ft = ToFileTime(utc);
    SYSTEMTIME u{}, l{};
    if (!FileTimeToSystemTime(&ft, &u) || !SystemTimeToTzSpecificLocalTime(nullptr, &u, &l)) return L"?";
    return Fmt(L"%04d-%02d-%02d %02d:%02d", l.wYear, l.wMonth, l.wDay, l.wHour, l.wMinute);
}
std::wstring AtOffsetStr(ULONGLONG utc, int offsetSec) {
    if (!utc) return L"-";
    const FILETIME ft = ToFileTime((ULONGLONG)((LONGLONG)utc + (LONGLONG)offsetSec * (LONGLONG)kSec));
    SYSTEMTIME u{};
    if (!FileTimeToSystemTime(&ft, &u)) return L"?";
    return Fmt(L"%04d-%02d-%02d %02d:%02d", u.wYear, u.wMonth, u.wDay, u.wHour, u.wMinute);
}
std::wstring OffsetName(int sec) {
    const int a = sec < 0 ? -sec : sec;
    return Fmt(L"UTC%c%d:%02d", sec < 0 ? L'-' : L'+', a / 3600, a % 3600 / 60);
}

std::string Utf8(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}

// One gallery entry: a reading, the settings/units/time to format it with, and what came out.
struct Case {
    std::wstring label;
    WeatherReading w;
    Settings s;
    bool fahrenheit = false;
    ULONGLONG now = 0;
    bool expectEmpty = false;
    std::wstring line;       // FormatWeatherLine result (everything that is due)
    std::wstring fitted;     // what the clock shows: extras left out until it fits WeatherMaxWidth
    unsigned dropped = 0;
    std::wstring media;      // now-playing line drawn under the weather line ("" = none)
    float mediaAlpha = 1;    // < 1: paused (dimmed)
    float fullRatio = 0, fittedRatio = 0;   // line width / time width (all-zero digits), in %
    std::wstring error;      // non-empty: a fetch that failed (label-only row)
    std::wstring detail;     // extra lines for the report (real cities)
};

struct Builder {
    ULONGLONG now;            // synthetic "current time"
    std::vector<Case> cases;

    // A plausible reading with every optional extra off; tests switch on what they exercise.
    WeatherReading Base() const {
        WeatherReading w;
        w.valid = true;
        w.place = L"Fakeville";
        w.temp = 14; w.high = 17; w.low = 9;
        w.code = 0; w.isDay = true;
        w.rainChance = -1;
        w.fetchedUtc = ToFileTime(now - 10 * kMin);
        return w;
    }
    Settings Defaults() const {
        Settings s;
        s.weatherCity = L"Fakeville";
        s.adaptiveLegibility = false;   // no wallpaper here; and its shadow room would make the galleries too tall to save
        return s;
    }
    void Add(const std::wstring& label, const WeatherReading& w, const Settings& s, bool f = false, bool expectEmpty = false,
             ULONGLONG at = 0) {
        Case c;
        c.label = label; c.w = w; c.s = s; c.fahrenheit = f; c.expectEmpty = expectEmpty;
        c.now = at ? at : now;
        cases.push_back(std::move(c));
    }
    void Add(const std::wstring& label, const WeatherReading& w, bool f = false, bool expectEmpty = false) {
        Add(label, w, Defaults(), f, expectEmpty);
    }
};

void BuildSynthetic(Builder& b) {
    const ULONGLONG now = b.now;

    // Every WMO family, day and night.
    struct Code { int code; const wchar_t* name; };
    const Code codes[] = { { 0, L"clear" }, { 1, L"mainly clear" }, { 2, L"partly cloudy" }, { 3, L"overcast" }, { 45, L"fog" },
                           { 61, L"rain" }, { 80, L"showers" }, { 71, L"snow" }, { 95, L"thunderstorm" } };
    for (bool day : { true, false })
        for (const Code& c : codes) {
            WeatherReading w = b.Base();
            w.code = c.code; w.isDay = day;
            b.Add(Fmt(L"code %d %s, %s", c.code, c.name, day ? L"day" : L"night"), w);
        }

    // Temperatures (typographic minus, zero, single/double digit).
    const struct { double t, hi, lo; } temps[] = { { -12, -8, -15 }, { -3, 0, -6 }, { 0, 3, -2 }, { 9, 12, 4 }, { 23, 26, 17 }, { 38, 41, 27 } };
    for (const auto& t : temps) {
        WeatherReading w = b.Base();
        w.temp = t.t; w.high = t.hi; w.low = t.lo;
        b.Add(Fmt(L"temp %g (high %g, low %g)", t.t, t.hi, t.lo), w);
    }
    const struct { double t, hi, lo; } ftemps[] = { { 101, 106, 88 }, { -4, 2, -10 } };
    for (const auto& t : ftemps) {
        WeatherReading w = b.Base();
        w.fahrenheit = true;
        w.temp = t.t; w.high = t.hi; w.low = t.lo;
        b.Add(Fmt(L"Fahrenheit %g (high %g, low %g)", t.t, t.hi, t.lo), w, true);
    }

    // Feels-like: hidden below the difference threshold (2), shown at or above it.
    const struct { double t, a; } feels[] = { { 3, 2 }, { 3, 1.5 }, { 3, 1 }, { 3, -2 }, { 30, 35 } };
    for (const auto& f : feels) {
        WeatherReading w = b.Base();
        w.temp = f.t; w.hasApparent = true; w.apparent = f.a;
        b.Add(Fmt(L"feels-like: temp %g, apparent %g (diff %g, %s)", f.t, f.a, std::abs(f.a - f.t), std::abs(f.a - f.t) >= 2 ? L"shown" : L"hidden"), w);
    }
    {
        WeatherReading w = b.Base();
        w.temp = 3; w.hasApparent = true; w.apparent = -2;
        Settings s = b.Defaults(); s.feelsLike = false;
        b.Add(L"feels-like switched off (FeelsLike=0), temp 3, apparent -2", w, s);
    }

    // Wind.
    {
        WeatherReading w = b.Base();
        w.hasWind = true; w.windSpeed = 12; w.windDir = 200;
        b.Add(L"wind 12 km/h S (below 20, hidden)", w);
        w.windSpeed = 20; w.windDir = 180;
        b.Add(L"wind 20 km/h from 180 (threshold, shown)", w);
        w.windSpeed = 35; w.windDir = 315;
        b.Add(L"wind 35 km/h from 315 (NW)", w);
        w.windSpeed = 48; w.windDir = 0;
        b.Add(L"wind 48 km/h from 0 (N)", w);
        w.fahrenheit = true; w.temp = 61; w.high = 66; w.low = 50;
        w.windSpeed = 60; w.windDir = 90;
        b.Add(L"wind 60 mph from 90 (E), Fahrenheit", w, true);
    }

    // Rain: chance within RainHours (6), time of the hour where the maximum occurs.
    {
        // Start of the synthetic local hour: "now" is 14:20 local, so 14:00.
        const ULONGLONG hour = now - 20 * kMin;
        WeatherReading w = b.Base();
        w.rainChance = 20; w.rainAtUtc = hour + 3 * kHour;
        b.Add(L"rain 20% in 3 h (below 30, hidden)", w);
        w.rainChance = 30; w.rainAtUtc = hour + kHour;
        b.Add(L"rain 30% in 1 h (threshold, shown)", w);
        w.rainChance = 60; w.rainAtUtc = hour + 2 * kHour;
        b.Add(L"rain 60% in 2 h", w);
        w.rainChance = 90; w.rainAtUtc = hour;
        b.Add(L"rain 90% now (start of the current hour)", w);
        w.rainChance = 75; w.rainAtUtc = hour + 8 * kHour;
        b.Add(L"rain 75% in 8 h (beyond RainHours 6, hidden)", w);
    }

    // Sun: next event within SunHours (3).
    {
        WeatherReading w = b.Base();
        w.sunUtc[0] = now + 90 * kMin;  w.sunUtc[1] = now + 13 * kHour;
        w.sunUtc[2] = now + 25 * kHour + 30 * kMin; w.sunUtc[3] = now + 37 * kHour;
        b.Add(L"sun: sunrise in 1.5 h (shown)", w);
        w.sunUtc[0] = now - 8 * kHour;  w.sunUtc[1] = now + 2 * kHour;
        w.sunUtc[2] = now + 16 * kHour; w.sunUtc[3] = now + 26 * kHour;
        b.Add(L"sun: sunset in 2 h (shown)", w);
        w.sunUtc[0] = now - 5 * kHour;  w.sunUtc[1] = now + 7 * kHour;
        w.sunUtc[2] = now + 19 * kHour; w.sunUtc[3] = now + 31 * kHour;
        b.Add(L"sun: sunset in 7 h (beyond 3 h, hidden)", w);
        w.sunUtc[0] = w.sunUtc[1] = w.sunUtc[2] = w.sunUtc[3] = 0;
        b.Add(L"sun: polar (all sun times 0, nothing)", w);
        w.sunUtc[0] = now - 8 * kHour;  w.sunUtc[1] = now + 2 * kHour;
        w.sunUtc[2] = now + 16 * kHour; w.sunUtc[3] = now + 26 * kHour;
        Settings s = b.Defaults(); s.sunTimes = false;
        b.Add(L"sun: sunset in 2 h but SunTimes=0 (hidden)", w, s);
    }

    // Longest line: everything at once, then the 12-hour variants.
    {
        WeatherReading w = b.Base();
        w.code = 95; w.temp = -3; w.high = 1; w.low = -8;
        w.hasApparent = true; w.apparent = -9;
        w.hasWind = true; w.windSpeed = 52; w.windDir = 225;
        w.rainChance = 90; w.rainAtUtc = now - 20 * kMin + 2 * kHour;   // now is 14:20 local, so 16:00
        w.sunUtc[0] = now - 8 * kHour;  w.sunUtc[1] = now + 2 * kHour;
        w.sunUtc[2] = now + 16 * kHour; w.sunUtc[3] = now + 26 * kHour;
        b.Add(L"all extras at once (longest line)", w);
        Settings s = b.Defaults(); s.hour24 = false;
        b.Add(L"12-hour: all extras", w, s);
        s.showAmPm = true;
        b.Add(L"12-hour with AM/PM: all extras", w, s);
        s.leadingZero = false;
        b.Add(L"12-hour with AM/PM, no leading zero: all extras", w, s);
    }

    // Busy days: two or three extras, to see which ones give way first.
    {
        WeatherReading w = b.Base();
        w.code = 61; w.temp = 4; w.high = 7; w.low = 1;
        w.hasApparent = true; w.apparent = -1;
        w.hasWind = true; w.windSpeed = 45; w.windDir = 270;
        w.rainChance = 80; w.rainAtUtc = now - 20 * kMin + kHour;
        WeatherReading x = w; x.hasApparent = false;
        b.Add(L"busy: rain + wind", x);
        x = w; x.rainChance = -1;
        b.Add(L"busy: feels + wind", x);
        b.Add(L"busy: rain + feels + wind", w);
        x = w; x.sunUtc[0] = now - 8 * kHour; x.sunUtc[1] = now + kHour; x.sunUtc[2] = now + 16 * kHour; x.sunUtc[3] = now + 26 * kHour;
        x.rainChance = -1;
        b.Add(L"busy: feels + wind + sunset", x);
        x.temp = -12; x.apparent = -21; x.high = -9; x.low = -18;
        b.Add(L"busy: cold, feels + wind + sunset (wide negatives)", x);
    }

    // Moon phases on a clear night, a quarter synodic month apart in eighths; then the southern hemisphere.
    {
        WeatherReading w = b.Base();
        w.isDay = false;
        const ULONGLONG kDay = 24 * kHour;
        for (int i = 0; i < 8; ++i) {
            const ULONGLONG at = now + (ULONGLONG)(i * 29.530588853 / 8.0 * (double)kDay);
            w.fetchedUtc = ToFileTime(at - 10 * kMin);
            b.Add(Fmt(L"clear night, moon phase step %d (north)", i), w, b.Defaults(), false, false, at);
        }
        w.latitude = -33.9;   // Sydney
        const ULONGLONG at = now + (ULONGLONG)(2 * 29.530588853 / 8.0 * (double)kDay);
        w.fetchedUtc = ToFileTime(at - 10 * kMin);
        b.Add(L"clear night, same phase as step 2 seen from the south (mirrored)", w, b.Defaults(), false, false, at);
    }

    // UV and air quality.
    {
        WeatherReading w = b.Base();
        w.uvIndex = 5.4; b.Add(L"UV 5 (below 6, hidden)", w);
        w.uvIndex = 7.2; b.Add(L"UV 7", w);
        w.uvIndex = -1;
        w.euAqi = 45; b.Add(L"EU AQI 45 (moderate, hidden)", w);
        w.euAqi = 65; b.Add(L"EU AQI 65: Air poor", w);
        w.euAqi = 85; b.Add(L"EU AQI 85: Air very poor", w);
        w.euAqi = 120; b.Add(L"EU AQI 120: Air extremely poor", w);
        WeatherReading f = w; f.fahrenheit = true; f.temp = 70; f.high = 75; f.low = 60;
        f.usAqi = 160; b.Add(L"US AQI 160 (Fahrenheit): Air unhealthy", f, true);
        w.euAqi = 70; w.uvIndex = 8; w.hasApparent = true; w.apparent = 8; w.rainChance = 70; w.rainAtUtc = now - 20 * kMin + kHour;
        w.hasWind = true; w.windSpeed = 40; w.windDir = 300;
        b.Add(L"busy: rain + feels + air + UV + wind (air ranks above UV and wind)", w);
    }

    // Now-playing line under the weather line (and alone, without weather).
    {
        WeatherReading w = b.Base();
        Case* c;
        b.Add(L"now playing: short", w); c = &b.cases.back();
        c->media = std::wstring(1, icon::Music) + L"\u2002Travis Scott \u2014 RHYNO";
        b.Add(L"now playing: long title (cut with an ellipsis)", w); c = &b.cases.back();
        c->media = std::wstring(1, icon::Music) +
                   L"\u2002Godspeed You! Black Emperor \u2014 Storm: Lift Yr. Skinny Fists Like Antennas to Heaven";
        b.Add(L"now playing: title only (no artist)", w); c = &b.cases.back();
        c->media = std::wstring(1, icon::Music) + L"\u2002Lofi beats to study to";
        b.Add(L"now playing: paused (dimmed to 50%)", w); c = &b.cases.back();
        c->media = std::wstring(1, icon::Music) + L"\u2002Travis Scott \u2014 RHYNO";
        c->mediaAlpha = 0.5f;
    }

    // The same busy line at different WeatherMaxWidth settings (what is kept, and the spacing).
    {
        WeatherReading w = b.Base();
        w.code = 61; w.temp = 4; w.high = 7; w.low = 1;
        w.hasApparent = true; w.apparent = -1;
        w.hasWind = true; w.windSpeed = 45; w.windDir = 270;
        w.rainChance = 80; w.rainAtUtc = now - 20 * kMin + kHour;
        w.uvIndex = 7; w.euAqi = 65;
        w.sunUtc[0] = now - 8 * kHour; w.sunUtc[1] = now + kHour; w.sunUtc[2] = now + 16 * kHour; w.sunUtc[3] = now + 26 * kHour;
        for (double mw : { 100.0, 130.0, 150.0, 180.0, 250.0 }) {
            Settings s = b.Defaults();
            s.weatherMaxWidth = mw;
            b.Add(Fmt(L"WeatherMaxWidth=%.0f: rain + feels + air + UV + wind + sunset", mw), w, s);
        }
    }

    // Lines that must come out empty.
    {
        WeatherReading w = b.Base();
        w.fetchedUtc = ToFileTime(now - 4 * kHour);
        b.Add(L"stale reading (fetched 4 h ago)", w, false, true);
        w.fetchedUtc = ToFileTime(now - (3 * kHour - kMin));
        b.Add(L"fresh enough reading (fetched 2 h 59 min ago, shown)", w);
        w = b.Base();
        b.Add(L"wrong units: reading Celsius, Fahrenheit requested", w, true, true);
        w.fahrenheit = true; w.temp = 57;
        b.Add(L"wrong units: reading Fahrenheit, Celsius requested", w, false, true);
        w = b.Base();
        w.valid = false;
        b.Add(L"invalid reading", w, false, true);
        w = b.Base();
        Settings s = b.Defaults(); s.showWeather = false;
        b.Add(L"showWeather=0 for this monitor", w, s, false, true);
    }
}

struct City { const wchar_t* name; bool fahrenheit; };

void BuildReal(Builder& b, ULONGLONG nowReal) {
    const City cities[] = { { L"Lisbon", false }, { L"Tokyo", false }, { L"New York", true }, { L"Reykjavik", false },
                            { L"Sydney", false }, { L"Tromsø", false }, { L"Kolkata", false } };
    for (const City& c : cities) {
        Case cs;
        cs.s = b.Defaults();
        cs.s.weatherCity = c.name;
        cs.fahrenheit = c.fahrenheit;
        WeatherRequest req;
        req.city = c.name; req.fahrenheit = c.fahrenheit; req.rainHours = (int)cs.s.rainHours;
        WeatherReading w;
        std::wstring err;
        if (!WeatherFetchOnce(req, w, err)) {
            cs.label = Fmt(L"%s: fetch failed: %s", c.name, err.c_str());
            cs.error = err.empty() ? L"unknown error" : err;
            b.cases.push_back(std::move(cs));
            continue;
        }
        if (!w.fetchedUtc.dwHighDateTime && !w.fetchedUtc.dwLowDateTime) w.fetchedUtc = ToFileTime(nowReal);   // fetched just now
        cs.w = w;
        cs.now = NowUtc();   // after the fetch: the reading must not look like it comes from the future
        cs.label = Fmt(L"%s (%s), %s%s", c.name, w.place.c_str(), OffsetName(w.utcOffset).c_str(), c.fahrenheit ? L", Fahrenheit" : L"");
        cs.detail = Fmt(L"    reading: temp %g high %g low %g code %d isDay %d apparent %s%g wind %s%g@%d rain %d%% at %s\r\n"
                        L"    place offset %s; sun events (PC local | location local):\r\n",
                        w.temp, w.high, w.low, w.code, w.isDay ? 1 : 0, w.hasApparent ? L"" : L"(n/a) ", w.apparent,
                        w.hasWind ? L"" : L"(n/a) ", w.windSpeed, w.windDir, w.rainChance, LocalStr(w.rainAtUtc).c_str(),
                        OffsetName(w.utcOffset).c_str());
        const wchar_t* names[4] = { L"sunrise", L"sunset ", L"sunrise", L"sunset " };
        for (int i = 0; i < 4; ++i)
            cs.detail += Fmt(L"      %s  %s  |  %s\r\n", names[i], LocalStr(w.sunUtc[i]).c_str(), AtOffsetStr(w.sunUtc[i], w.utcOffset).c_str());
        b.cases.push_back(std::move(cs));
    }
}

}  // namespace

int RunWeatherTest(const std::wstring& outDirArg) {
    std::wstring dir = outDirArg.empty() ? ExeDir() : outDirArg;
    if (!dir.empty() && dir.back() != L'\\' && dir.back() != L'/') dir += L'\\';
    CreateDirectoryW(dir.c_str(), nullptr);   // no-op if it exists

    ClockRenderer r;
    if (!r.Init()) { Log(L"weather-test: renderer init failed"); return 1; }

    // Synthetic "now": today 14:20 local, so hour-rounded rain times and sun offsets are easy to read.
    SYSTEMTIME lt{}; GetLocalTime(&lt);
    lt.wHour = 14; lt.wMinute = 20; lt.wSecond = 0; lt.wMilliseconds = 0;
    SYSTEMTIME ut{}; FILETIME ft{};
    ULONGLONG synthNow = NowUtc();
    if (TzSpecificLocalTimeToSystemTime(nullptr, &lt, &ut) && SystemTimeToFileTime(&ut, &ft)) synthNow = ToTicks(ft);

    Builder b;
    b.now = synthNow;
    BuildSynthetic(b);
    BuildReal(b, NowUtc());

    // Format every line once; the text doesn't depend on the DPI.
    std::wstring report = L"Weather line self-test\r\n";
    report += Fmt(L"Synthetic cases: now = %s (PC local), readings fetched 10 min earlier unless stated.\r\n", LocalStr(synthNow).c_str());
    report += Fmt(L"Real cities: fetched live at %s (PC local).\r\n\r\n", LocalStr(NowUtc()).c_str());
    int n = 0;
    for (Case& c : b.cases) {
        ++n;
        std::wstring status;
        if (c.error.empty()) {
            c.line = FormatWeatherLine(c.w, c.now, c.s, c.fahrenheit);
            // What the clock would show (widths are in DIPs, so any DPI gives the same answer).
            FrameLayout f;
            if (!c.line.empty() && r.Prepare(c.s, 120, L" ", L"14:20", c.line, f) && f.timeRefWidth > 0)
                c.fullRatio = 100.f * f.weatherWidth / f.timeRefWidth;
            PrepareFitted(r, c.s, 120, L" ", L"14:20", &c.w, c.now, c.fahrenheit, f, &c.fitted, &c.dropped, c.media);
            if (f.weather && f.timeRefWidth > 0) c.fittedRatio = 100.f * f.weatherWidth / f.timeRefWidth;
            if (c.expectEmpty) status = c.line.empty() ? L" (empty, as expected)" : L" UNEXPECTED: " + Visible(c.line);
            else if (c.line.empty()) status = L" UNEXPECTED: empty line";
        }
        c.label += status;
        report += Fmt(L"%02d. %s\r\n", n, c.label.c_str());
        report += c.error.empty() ? Fmt(L"    line: \"%s\"  (%.0f%% of the time's width)\r\n", Visible(c.line).c_str(), c.fullRatio)
                                  : std::wstring(L"    (no line: fetch failed)\r\n");
        if (c.dropped) {
            std::wstring what;
            if (c.dropped & kExtraSun) what += L" sun";
            if (c.dropped & kExtraWind) what += L" wind";
            if (c.dropped & kExtraUv) what += L" UV";
            if (c.dropped & kExtraAir) what += L" air";
            if (c.dropped & kExtraFeels) what += L" feels";
            if (c.dropped & kExtraRain) what += L" rain";
            report += Fmt(L"    shown: \"%s\"  (%.0f%%; left out:%s)\r\n", Visible(c.fitted).c_str(), c.fittedRatio, what.c_str());
        }
        if (!c.media.empty()) report += L"    media: \"" + Visible(c.media) + L"\"\r\n";
        report += c.detail;
        report += L"\r\n";
    }
    report += L"Track titles on the now-playing line (CleanTrackTitle):\r\n";
    for (const wchar_t* t : { L"Flower In A Hurricane (feat. Tame Impala, SZA, King Gizzard & the Lizard Wizard) [From GTAVI: Original Score]",
                              L"Die With A Smile (with Bruno Mars)", L"Bohemian Rhapsody - Remastered 2011",
                              L"Here Comes The Sun (2019 Mix) [Remastered]", L"Shape of You (Acoustic)",
                              L"Levitating (feat. DaBaby) (Remix)", L"Time (Live)", L"(Sittin' On) The Dock of the Bay",
                              L"[feat. Only]" })
        report += L"    \"" + std::wstring(t) + L"\"  ->  \"" + CleanTrackTitle(t) + L"\"\r\n";
    report += L"\r\n";
    {
        const std::string u8 = "\xEF\xBB\xBF" + Utf8(report);
        HANDLE h = CreateFileW((dir + L"weather-test.txt").c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h != INVALID_HANDLE_VALUE) {
            DWORD wr = 0;
            WriteFile(h, u8.data(), (DWORD)u8.size(), &wr, nullptr);
            CloseHandle(h);
        } else {
            Log(L"weather-test: cannot write the report (error %lu)", GetLastError());
        }
    }

    // Render the galleries at the user's two monitor DPIs.
    int rc = 0;
    for (UINT dpi : { 120u, 192u }) {
        std::vector<GalleryRow> rows;
        for (const Case& c : b.cases) {
            GalleryRow row;
            row.label = c.label;
            if (c.error.empty() && !c.fitted.empty()) {
                // Date/time aren't drawn (drawDate=false, timeMask=0); a space keeps their layouts valid.
                // The row shows what the clock would show (the fitted line).
                FrameLayout f;
                // The time's text only sets the width budget (it isn't drawn).
                if (r.Prepare(c.s, dpi, L" ", L"14:20", c.fitted, f, c.media) && f.weather) {
                    f.mediaAlpha = c.mediaAlpha;
                    row.bmp = r.RenderFrame(f, RGB(255, 255, 255), false, false, 0, true);
                    row.srcTop = std::max(0.f, f.weatherOrigin.y * dpi / 96.f - 6.f);
                }
                if (!row.bmp) { row.label += L" (RENDER FAILED)"; rc = 1; }
            }
            rows.push_back(std::move(row));
        }
        const std::wstring path = dir + Fmt(L"weather-test-%u.png", dpi);
        if (!r.SaveGallery(path, rows, (float)dpi)) { Log(L"weather-test: could not save %s", path.c_str()); rc = 1; }
    }

    // Worst case, drawn as the whole clock: a long date, every weather extra due at once and a
    // long track title, at the default width limit, at 180% and with no limit.
    {
        WeatherReading w = b.Base();
        w.code = 95; w.temp = -4; w.high = 7; w.low = -11;
        w.hasApparent = true; w.apparent = -12;
        w.hasWind = true; w.windSpeed = 65; w.windDir = 315;
        w.rainChance = 90; w.rainAtUtc = synthNow - 20 * kMin + kHour;
        w.uvIndex = 11; w.euAqi = 85;
        w.sunUtc[0] = synthNow - 8 * kHour; w.sunUtc[1] = synthNow + kHour;
        w.sunUtc[2] = synthNow + 16 * kHour; w.sunUtc[3] = synthNow + 26 * kHour;
        const std::wstring media = std::wstring(1, icon::Music) +
            L" Godspeed You! Black Emperor — Storm: Lift Yr. Skinny Fists Like Antennas to Heaven";
        const std::wstring date = L"Wednesday, September 30", time = L"20:08";
        for (UINT dpi : { 120u, 192u }) {
            std::vector<GalleryRow> rows;
            for (double mw : { 150.0, 180.0, 1000.0 }) {
                Settings s = b.Defaults();
                s.weatherMaxWidth = mw;
                FrameLayout f;
                std::wstring line;
                unsigned dropped = 0;
                GalleryRow row;
                row.label = mw >= 1000 ? std::wstring(L"Worst case, no width limit") : Fmt(L"Worst case, WeatherMaxWidth=%.0f", mw);
                if (PrepareFitted(r, s, dpi, date, time, &w, synthNow, false, f, &line, &dropped, media))
                    row.bmp = r.RenderFrame(f, RGB(255, 255, 255), false);
                if (!row.bmp) { row.label += L" (RENDER FAILED)"; rc = 1; }
                rows.push_back(std::move(row));
            }
            const std::wstring path = dir + Fmt(L"worst-case-%u.png", dpi);
            if (!r.SaveGallery(path, rows, (float)dpi)) { Log(L"weather-test: could not save %s", path.c_str()); rc = 1; }
        }
    }

    // Centring: every line's ink, as drawn in the frame (icons included), against the centre
    // line, at both monitor DPIs, in physical pixels. Lines start on whole pixels (so a minute's
    // unchanged digits land on the same pixels), which alone allows half a pixel, and edges are
    // estimated to a fraction of a pixel. More than kTol px off is UNEXPECTED.
    {
        constexpr float kTol = 0.6f;   // px
        struct Check { std::wstring what; int n = 0, bad = 0; float worst = 0; std::wstring worstAt, badList, note; };
        std::vector<Check> checks;
        UINT dpiNow = 96;
        auto record = [&](Check& c, const std::wstring& at, float offDip) {
            const float off = offDip * dpiNow / 96.f;
            ++c.n;
            if (std::fabs(off) >= std::fabs(c.worst)) { c.worst = off; c.worstAt = at; }
            if (std::fabs(off) > kTol && ++c.bad <= 12) c.badList += Fmt(L" %s %+.2f", at.c_str(), off);
        };
        // Same text as the clock's FormatTime (main.cpp).
        auto timeText = [](int h, int m, const Settings& s) {
            const wchar_t* suffix = L"";
            if (!s.hour24) { suffix = h < 12 ? L" AM" : L" PM"; h %= 12; if (h == 0) h = 12; }
            return Fmt(s.leadingZero ? L"%02d:%02d%s" : L"%d:%02d%s", h, m, (!s.hour24 && s.showAmPm) ? suffix : L"");
        };
        struct TimeFormat { const wchar_t* name; bool h24, lz, ampm; };
        const TimeFormat formats[] = { { L"24-hour 07:05", true, true, false }, { L"24-hour 7:05", true, false, false },
                                       { L"12-hour 7:05 PM", false, false, true } };
        for (UINT dpi : { 120u, 192u }) {
            dpiNow = dpi;
            for (const auto& tf : formats) {
                Check c; c.what = Fmt(L"Time, %s, every minute, %u DPI", tf.name, dpi);
                Settings s = b.Defaults();
                s.hour24 = tf.h24; s.leadingZero = tf.lz; s.showAmPm = tf.ampm;
                for (int m = 0; m < 24 * 60; ++m) {
                    const std::wstring t = timeText(m / 60, m % 60, s);
                    FrameLayout f;
                    float off = 0;
                    if (!r.Prepare(s, dpi, L" ", t, L"", f)) { rc = 1; continue; }
                    r.CentreLines(f);   // as PrepareFitted does for the clock
                    if (r.LineInkOffset(f, ClockRenderer::kTimeLine, off)) record(c, t, off);
                    else rc = 1;
                }
                checks.push_back(std::move(c));
            }
            for (const auto& tf : formats) {   // CentreOn=colon: the colon on the centre line
                Check c; c.what = Fmt(L"Colon (CentreOn=colon), %s, every minute, %u DPI", tf.name, dpi);
                Settings s = b.Defaults();
                s.hour24 = tf.h24; s.leadingZero = tf.lz; s.showAmPm = tf.ampm; s.centreOnColon = true;
                float lean = 0; std::wstring leanAt;   // how far the whole time then sits off (by design)
                for (int m = 0; m < 24 * 60; ++m) {
                    const std::wstring t = timeText(m / 60, m % 60, s);
                    FrameLayout f;
                    float off = 0;
                    if (!r.Prepare(s, dpi, L" ", t, L"", f) || f.colonIndex < 0) { rc = 1; continue; }
                    r.CentreLines(f);
                    if (r.LineInkOffset(f, ClockRenderer::kTimeLine, off, 1u << f.colonIndex)) record(c, t, off);
                    else rc = 1;
                    if (r.LineInkOffset(f, ClockRenderer::kTimeLine, off) && std::fabs(off) > std::fabs(lean)) { lean = off; leanAt = t; }
                }
                c.note = Fmt(L"; the whole time then leans up to %+.1f px (%s)", lean * dpi / 96.f, leanAt.c_str());
                checks.push_back(std::move(c));
            }
            {   // Every date of a year, as the default DateFormat writes it.
                Check c; c.what = Fmt(L"Date, every day of 2027, %u DPI", dpi);
                const Settings s = b.Defaults();
                SYSTEMTIME st{}; st.wYear = 2027; st.wMonth = 1; st.wDay = 1;
                FILETIME jan1{};
                SystemTimeToFileTime(&st, &jan1);
                for (int d = 0; d < 365; ++d) {
                    const ULONGLONG t = ToTicks(jan1) + (ULONGLONG)d * 24ULL * kHour;
                    SYSTEMTIME day{};
                    const FILETIME dft = ToFileTime(t);
                    FileTimeToSystemTime(&dft, &day);
                    wchar_t buf[96] = {};
                    GetDateFormatEx(s.dateLocale.c_str(), 0, &day, s.dateFormat.c_str(), buf, 96, nullptr);
                    FrameLayout f;
                    float off = 0;
                    if (PrepareFitted(r, s, dpi, buf, L"14:20", nullptr, synthNow, false, f, nullptr, nullptr, L"") &&
                        r.LineInkOffset(f, ClockRenderer::kDateLine, off)) record(c, buf, off);
                    else rc = 1;
                }
                checks.push_back(std::move(c));
            }
            {   // Every weather and now-playing line of the cases above.
                Check cw, cm;
                cw.what = Fmt(L"Weather line, every case, %u DPI", dpi);
                cm.what = Fmt(L"Now-playing line, every case, %u DPI", dpi);
                int idx = 0;
                for (const Case& c : b.cases) {
                    ++idx;
                    if (!c.error.empty() || c.fitted.empty()) continue;
                    FrameLayout f;
                    float off = 0;
                    if (!PrepareFitted(r, c.s, dpi, L" ", L"14:20", &c.w, c.now, c.fahrenheit, f, nullptr, nullptr, c.media)) { rc = 1; continue; }
                    if (f.weather && r.LineInkOffset(f, ClockRenderer::kWeatherLine, off)) record(cw, Fmt(L"#%02d", idx), off);
                    if (f.media && r.LineInkOffset(f, ClockRenderer::kMediaLine, off)) record(cm, Fmt(L"#%02d", idx), off);
                }
                checks.push_back(std::move(cw));
                checks.push_back(std::move(cm));
            }
        }
        std::wstring rep = Fmt(L"Centring self-test: each line's ink box against the centre line, in physical pixels (+ = right).\r\n"
                               L"120 DPI = 125%% scaling, 192 DPI = 200%%. Over %.1f px = UNEXPECTED.\r\n\r\n", kTol);
        for (const Check& c : checks) {
            rep += Fmt(L"%s: %d checked, worst %+.2f (%s)%s", c.what.c_str(), c.n, c.worst, c.worstAt.c_str(), c.note.c_str());
            rep += c.bad ? Fmt(L"  UNEXPECTED: %d over %.1f:%s\r\n", c.bad, kTol, c.badList.c_str()) : std::wstring(L"  OK\r\n");
        }

        // A sheet of hard times: the ink box centred, half-way, the centre of mass centred
        // (OpticalCentring 0 / 50 / 100) and the colon centred (CentreOn=colon), with the centre line drawn.
        for (UINT dpi : { 120u, 192u }) {
            std::vector<GalleryRow> sheet;
            for (const wchar_t* t : { L"13:23", L"17:38", L"10:41", L"11:11", L"20:08", L"21:47" }) {
                for (double oc : { 0.0, 50.0, 100.0, -1.0 }) {
                    Settings so = b.Defaults();
                    so.opticalCentring = oc < 0 ? 0 : oc;
                    so.centreOnColon = oc < 0;
                    FrameLayout f;
                    GalleryRow row;
                    row.label = oc < 0 ? Fmt(L"%s  CentreOn=colon", t) : Fmt(L"%s  OpticalCentring=%.0f", t, oc);
                    if (PrepareFitted(r, so, dpi, L"Sunday, November 22", t, nullptr, synthNow, false, f, nullptr, nullptr, L"")) {
                        f.guide = true;
                        row.bmp = r.RenderFrame(f, RGB(255, 255, 255), false);
                    }
                    if (!row.bmp) { row.label += L" (RENDER FAILED)"; rc = 1; }
                    sheet.push_back(std::move(row));
                }
            }
            const std::wstring path = dir + Fmt(L"time-centring-%u.png", dpi);
            if (!r.SaveGallery(path, sheet, (float)dpi)) rc = 1;
        }
        const std::string u8 = "\xEF\xBB\xBF" + Utf8(rep);
        HANDLE h = CreateFileW((dir + L"centring.txt").c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h != INVALID_HANDLE_VALUE) { DWORD wr = 0; WriteFile(h, u8.data(), (DWORD)u8.size(), &wr, nullptr); CloseHandle(h); }
    }
    return rc;
}
