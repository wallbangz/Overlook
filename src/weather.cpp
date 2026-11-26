// Weather from Open-Meteo via WinHTTP, on a worker thread. See weather.h.
#include "weather.h"
#include "settings.h"
#include "net.h"

#include <winhttp.h>
#include <atomic>
#include <cmath>
#include <charconv>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#pragma comment(lib, "winhttp.lib")

namespace {

// ---------------------------------------------------------------- small utils

std::wstring Widen(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    if (n <= 0) return {};
    std::wstring w((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
    return w;
}

std::string Narrow(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    if (n <= 0) return {};
    std::string s((size_t)n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}

// A number from the network as an int, clamped first: casting a double outside int's range (the
// JSON reader accepts 1e300) is undefined behaviour.
int ClampInt(double v, int lo, int hi) {
    if (!(v >= lo)) return lo;
    if (v > hi) return hi;
    return (int)v;
}

std::wstring Trim(const std::wstring& s) {
    size_t b = 0, e = s.size();
    while (b < e && iswspace(s[b])) ++b;
    while (e > b && iswspace(s[e - 1])) --e;
    return s.substr(b, e - b);
}

std::wstring Lower(std::wstring s) {
    if (!s.empty()) CharLowerBuffW(s.data(), (DWORD)s.size());
    return s;
}

bool SameCity(const std::wstring& a, const std::wstring& b) {
    return Lower(Trim(a)) == Lower(Trim(b));
}

std::string UrlEncode(const std::string& s) {
    static const char* hex = "0123456789ABCDEF";
    std::string o;
    for (unsigned char c : s) {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
            c == '-' || c == '_' || c == '.' || c == '~') {
            o.push_back((char)c);
        } else {
            o.push_back('%');
            o.push_back(hex[c >> 4]);
            o.push_back(hex[c & 15]);
        }
    }
    return o;
}

// Invariant (locale independent) number <-> text.
std::wstring NumToW(double v, int fixedDigits = -1) {
    char buf[64];
    std::to_chars_result r = fixedDigits >= 0
        ? std::to_chars(buf, buf + sizeof buf, v, std::chars_format::fixed, fixedDigits)
        : std::to_chars(buf, buf + sizeof buf, v);
    if (r.ec != std::errc()) return L"0";
    return Widen(std::string(buf, r.ptr));
}

std::wstring NumToW(long long v) { return std::to_wstring(v); }

bool WToNum(const std::wstring& w, double& out) {
    std::string s = Narrow(Trim(w));
    if (s.empty()) return false;
    const char* b = s.data();
    const char* e = b + s.size();
    double v = 0;
    auto r = std::from_chars(b, e, v);
    if (r.ec != std::errc() || r.ptr != e) return false;
    out = v;
    return true;
}

bool WToInt(const std::wstring& w, long long& out) {
    std::string s = Narrow(Trim(w));
    if (s.empty()) return false;
    long long v = 0;
    auto r = std::from_chars(s.data(), s.data() + s.size(), v);
    if (r.ec != std::errc() || r.ptr != s.data() + s.size()) return false;
    out = v;
    return true;
}

using namespace net;

// ---------------------------------------------------------------- state & ini cache

SRWLOCK g_lock = SRWLOCK_INIT;
WeatherReading g_reading;          // guarded by g_lock
std::wstring g_readingCity;        // city the in-memory reading belongs to
std::wstring g_lastError;
std::atomic<bool> g_busy{false};

struct Lock {
    Lock() { AcquireSRWLockExclusive(&g_lock); }
    ~Lock() { ReleaseSRWLockExclusive(&g_lock); }
};

std::wstring CachePath() { return ExeBase() + L".weather.ini"; }   // Overlook.weather.ini

// A new ini is created as UTF-16 LE (BOM) so place names with any characters survive.
void EnsureUnicodeIni(const std::wstring& path) {
    if (GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES) return;
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    const unsigned char bom[2] = { 0xFF, 0xFE };
    DWORD w = 0;
    WriteFile(h, bom, 2, &w, nullptr);
    CloseHandle(h);
}

std::wstring IniGet(const std::wstring& path, const wchar_t* sec, const wchar_t* key) {
    std::wstring buf(2048, L'\0');
    DWORD n = GetPrivateProfileStringW(sec, key, L"", buf.data(), (DWORD)buf.size(), path.c_str());
    buf.resize(n);
    return buf;
}

void IniSet(const std::wstring& path, const wchar_t* sec, const wchar_t* key, const std::wstring& v) {
    WritePrivateProfileStringW(sec, key, v.c_str(), path.c_str());
}

bool LoadCachedReading(const std::wstring& city, WeatherReading& out) {
    std::wstring path = CachePath();
    if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) return false;
    std::wstring c = IniGet(path, L"Reading", L"City");
    if (c.empty() || !SameCity(c, city)) return false;
    WeatherReading r;
    long long i = 0;
    if (!WToNum(IniGet(path, L"Reading", L"Temp"), r.temp)) return false;
    if (!WToNum(IniGet(path, L"Reading", L"High"), r.high)) return false;
    if (!WToNum(IniGet(path, L"Reading", L"Low"), r.low)) return false;
    if (!WToInt(IniGet(path, L"Reading", L"Code"), i)) return false;
    r.code = (int)i;
    r.isDay = WToInt(IniGet(path, L"Reading", L"IsDay"), i) ? (i != 0) : true;
    r.rainChance = WToInt(IniGet(path, L"Reading", L"RainChance"), i) ? (int)i : -1;
    r.rainAtUtc = WToInt(IniGet(path, L"Reading", L"RainAtUtc"), i) && i > 0 ? (ULONGLONG)i : 0;
    r.fahrenheit = WToInt(IniGet(path, L"Reading", L"Fahrenheit"), i) && i != 0;
    r.utcOffset = WToInt(IniGet(path, L"Reading", L"UtcOffset"), i) ? (int)i : 0;
    if (!WToNum(IniGet(path, L"Reading", L"Latitude"), r.latitude)) r.latitude = 0;
    if (!WToNum(IniGet(path, L"Reading", L"UvIndex"), r.uvIndex)) r.uvIndex = -1;
    r.euAqi = WToInt(IniGet(path, L"Reading", L"EuAqi"), i) ? (int)i : -1;
    r.usAqi = WToInt(IniGet(path, L"Reading", L"UsAqi"), i) ? (int)i : -1;
    r.hasApparent = WToNum(IniGet(path, L"Reading", L"Apparent"), r.apparent);
    r.hasWind = WToNum(IniGet(path, L"Reading", L"WindSpeed"), r.windSpeed) &&
                WToInt(IniGet(path, L"Reading", L"WindDir"), i);
    if (r.hasWind) r.windDir = (int)i;
    for (int e = 0; e < 4; ++e)
        r.sunUtc[e] = WToInt(IniGet(path, L"Reading", (L"SunUtc" + std::to_wstring(e)).c_str()), i) && i > 0 ? (ULONGLONG)i : 0;
    if (WToInt(IniGet(path, L"Reading", L"FetchedUtc"), i) && i > 0) {
        ULARGE_INTEGER u;
        u.QuadPart = (ULONGLONG)i;
        r.fetchedUtc.dwLowDateTime = u.LowPart;
        r.fetchedUtc.dwHighDateTime = u.HighPart;
    }
    r.place = IniGet(path, L"Reading", L"Place");
    r.fetchedAt = 0;
    r.valid = true;
    out = std::move(r);
    return true;
}

void SaveCachedReading(const std::wstring& city, const WeatherRequest& req, const WeatherReading& r) {
    std::wstring path = CachePath();
    EnsureUnicodeIni(path);
    ULARGE_INTEGER u;
    u.LowPart = r.fetchedUtc.dwLowDateTime;
    u.HighPart = r.fetchedUtc.dwHighDateTime;
    IniSet(path, L"Reading", L"City", city);
    IniSet(path, L"Reading", L"Place", r.place);
    IniSet(path, L"Reading", L"Temp", NumToW(r.temp));
    IniSet(path, L"Reading", L"High", NumToW(r.high));
    IniSet(path, L"Reading", L"Low", NumToW(r.low));
    IniSet(path, L"Reading", L"Code", NumToW((long long)r.code));
    IniSet(path, L"Reading", L"IsDay", r.isDay ? L"1" : L"0");
    IniSet(path, L"Reading", L"RainChance", NumToW((long long)r.rainChance));
    IniSet(path, L"Reading", L"RainAtUtc", NumToW((long long)r.rainAtUtc));
    IniSet(path, L"Reading", L"UtcOffset", NumToW((long long)r.utcOffset));
    IniSet(path, L"Reading", L"Latitude", NumToW(r.latitude, 4));
    IniSet(path, L"Reading", L"UvIndex", NumToW(r.uvIndex));
    IniSet(path, L"Reading", L"EuAqi", NumToW((long long)r.euAqi));
    IniSet(path, L"Reading", L"UsAqi", NumToW((long long)r.usAqi));
    IniSet(path, L"Reading", L"Apparent", r.hasApparent ? NumToW(r.apparent) : L"");
    IniSet(path, L"Reading", L"WindSpeed", r.hasWind ? NumToW(r.windSpeed) : L"");
    IniSet(path, L"Reading", L"WindDir", r.hasWind ? NumToW((long long)r.windDir) : L"");
    for (int e = 0; e < 4; ++e)
        IniSet(path, L"Reading", (L"SunUtc" + std::to_wstring(e)).c_str(), NumToW((long long)r.sunUtc[e]));
    IniSet(path, L"Reading", L"Fahrenheit", req.fahrenheit ? L"1" : L"0");
    IniSet(path, L"Reading", L"FetchedUtc", NumToW((long long)u.QuadPart));
}

// ---------------------------------------------------------------- geocoding

struct GeoResult {
    double lat = 0, lon = 0;
    std::wstring place;
};

bool Geocode(const std::wstring& cityIn, GeoResult& out, std::wstring& err, bool useCache = true) {
    std::wstring city = Trim(cityIn);
    std::wstring path = CachePath();

    std::wstring cached = useCache ? IniGet(path, L"Geocode", L"City") : std::wstring();
    if (!cached.empty() && SameCity(cached, city)) {
        GeoResult g;
        if (WToNum(IniGet(path, L"Geocode", L"Lat"), g.lat) && WToNum(IniGet(path, L"Geocode", L"Lon"), g.lon)) {
            g.place = IniGet(path, L"Geocode", L"Place");
            out = std::move(g);
            Log(L"weather: geocode cache hit for '%s' -> %s", Private(city).c_str(), Private(out.place).c_str());
            return true;
        }
    }

    std::wstring name = city, qualifier;
    size_t comma = city.find(L',');
    if (comma != std::wstring::npos) {
        name = Trim(city.substr(0, comma));
        qualifier = Lower(Trim(city.substr(comma + 1)));
    }
    if (name.empty()) { err = L"geocoding: no match for '" + city + L"'"; return false; }

    std::wstring q = L"/v1/search?name=" + Widen(UrlEncode(Narrow(name))) +
                     L"&count=" + (qualifier.empty() ? L"1" : L"10") + L"&language=en&format=json";
    JVal root;
    if (!HttpGetJson(L"geocoding-api.open-meteo.com", q, root, err)) { err = L"geocoding: " + err; return false; }

    const JVal* results = Get(&root, "results");
    if (!results || results->type != JVal::Array || results->arr.empty()) {
        err = L"geocoding: no match for '" + city + L"'";
        return false;
    }
    const JVal* pick = &results->arr[0];
    if (!qualifier.empty()) {
        for (const JVal& r : results->arr) {
            std::string cc, cn;
            bool hit = false;
            if (GetStr(Get(&r, "country_code"), cc) && Lower(Widen(cc)) == qualifier) hit = true;
            if (!hit && GetStr(Get(&r, "country"), cn) && Lower(Widen(cn)) == qualifier) hit = true;
            if (hit) { pick = &r; break; }
        }
    }
    GeoResult g;
    std::string nm, country;
    if (!GetNum(Get(pick, "latitude"), g.lat) || !GetNum(Get(pick, "longitude"), g.lon) ||
        !GetStr(Get(pick, "name"), nm)) {
        err = L"geocoding: unexpected response";
        return false;
    }
    g.place = Widen(nm);
    if (GetStr(Get(pick, "country"), country) && !country.empty()) g.place += L", " + Widen(country);

    if (!useCache) { out = std::move(g); return true; }
    EnsureUnicodeIni(path);
    IniSet(path, L"Geocode", L"City", city);
    IniSet(path, L"Geocode", L"Lat", NumToW(g.lat, 4));
    IniSet(path, L"Geocode", L"Lon", NumToW(g.lon, 4));
    IniSet(path, L"Geocode", L"Place", g.place);
    wchar_t at[64] = L"";   // the coordinates pin the place down even better than its name
    if (g_logPrivate) swprintf_s(at, L" (%.4f, %.4f)", g.lat, g.lon);
    Log(L"weather: geocoded '%s' -> %s%s", Private(city).c_str(), Private(g.place).c_str(), at);
    out = std::move(g);
    return true;
}

// ---------------------------------------------------------------- forecast

// "YYYY-MM-DDTHH:MM" -> abstract timeline value (100 ns ticks, no timezone), via FILETIME arithmetic.
bool ParseIsoLocal(const std::string& s, ULONGLONG& ticks, int& hour) {
    if (s.size() < 13 || s[4] != '-' || s[7] != '-' || (s[10] != 'T' && s[10] != ' '))
        return false;
    auto num = [&](size_t pos, size_t len, int& v) {
        v = 0;
        for (size_t i = 0; i < len; ++i) {
            char c = s[pos + i];
            if (c < '0' || c > '9') return false;
            v = v * 10 + (c - '0');
        }
        return true;
    };
    int y, mo, d, h, mi = 0;
    if (!num(0, 4, y) || !num(5, 2, mo) || !num(8, 2, d) || !num(11, 2, h)) return false;
    if (s.size() >= 16 && s[13] == ':' && !num(14, 2, mi)) return false;
    SYSTEMTIME st{};
    st.wYear = (WORD)y; st.wMonth = (WORD)mo; st.wDay = (WORD)d; st.wHour = (WORD)h; st.wMinute = (WORD)mi;
    FILETIME ft;
    if (!SystemTimeToFileTime(&st, &ft)) return false;
    ULARGE_INTEGER u;
    u.LowPart = ft.dwLowDateTime;
    u.HighPart = ft.dwHighDateTime;
    ticks = u.QuadPart;
    hour = h;
    return true;
}

// Location-local "YYYY-MM-DDTHH:MM" -> UTC FILETIME ticks, given the location's UTC offset.
bool IsoToUtc(const JVal* v, int utcOffset, ULONGLONG& utc) {
    std::string s;
    ULONGLONG ticks;
    int hour;
    if (!GetStr(v, s) || !ParseIsoLocal(s, ticks, hour)) return false;
    utc = ticks - (ULONGLONG)((long long)utcOffset * 10000000LL);
    return true;
}

// Current European and US air-quality indices. Optional: a failure only leaves them unknown.
void AirQuality(const GeoResult& geo, WeatherReading& r) {
    std::wstring err;
    JVal root;
    const std::wstring q = L"/v1/air-quality?latitude=" + NumToW(geo.lat, 4) + L"&longitude=" + NumToW(geo.lon, 4) +
                           L"&current=european_aqi,us_aqi";
    if (!HttpGetJson(L"air-quality-api.open-meteo.com", q, root, err)) {
        Log(L"weather: air quality unavailable: %s", err.c_str());
        return;
    }
    const JVal* cur = Get(&root, "current");
    double v;
    if (GetNum(Get(cur, "european_aqi"), v)) r.euAqi = ClampInt(std::round(v), 0, 1000);
    if (GetNum(Get(cur, "us_aqi"), v)) r.usAqi = ClampInt(std::round(v), 0, 1000);
}

bool Forecast(const GeoResult& geo, const WeatherRequest& req, WeatherReading& out, std::wstring& err) {
    std::wstring q = L"/v1/forecast?latitude=" + NumToW(geo.lat, 4) + L"&longitude=" + NumToW(geo.lon, 4) +
        L"&current=temperature_2m,apparent_temperature,weather_code,is_day,wind_speed_10m,wind_direction_10m,uv_index"
        L"&daily=temperature_2m_max,temperature_2m_min,sunrise,sunset"
        L"&hourly=precipitation_probability&forecast_days=2&timezone=auto";
    if (req.fahrenheit) q += L"&temperature_unit=fahrenheit&wind_speed_unit=mph";

    JVal root;
    if (!HttpGetJson(L"api.open-meteo.com", q, root, err)) return false;

    const JVal* cur = Get(&root, "current");
    double temp, code, isDay, hi, lo;
    if (!GetNum(Get(cur, "temperature_2m"), temp) || !GetNum(Get(cur, "weather_code"), code) ||
        !GetNum(Get(cur, "is_day"), isDay)) {
        err = L"forecast: missing current data";
        return false;
    }
    const JVal* daily = Get(&root, "daily");
    if (!GetNum(At(Get(daily, "temperature_2m_max"), 0), hi) || !GetNum(At(Get(daily, "temperature_2m_min"), 0), lo)) {
        err = L"forecast: missing daily data";
        return false;
    }

    WeatherReading r;
    r.temp = temp;
    r.high = hi;
    r.low = lo;
    r.code = ClampInt(code, 0, 99);   // WMO codes
    r.isDay = isDay != 0;
    r.fahrenheit = req.fahrenheit;
    r.latitude = geo.lat;
    if (!GetNum(Get(cur, "uv_index"), r.uvIndex)) r.uvIndex = -1;
    double offset = 0;
    GetNum(Get(&root, "utc_offset_seconds"), offset);
    r.utcOffset = ClampInt(offset, -18 * 3600, 18 * 3600);   // seconds; real zones are within ±14 h
    r.hasApparent = GetNum(Get(cur, "apparent_temperature"), r.apparent);
    double wdir = 0;
    r.hasWind = GetNum(Get(cur, "wind_speed_10m"), r.windSpeed) && GetNum(Get(cur, "wind_direction_10m"), wdir);
    r.windDir = ClampInt(std::round(wdir), 0, 360) % 360;
    for (size_t d = 0; d < 2; ++d) {
        ULONGLONG rise = 0, set = 0;
        // Polar day/night: Open-Meteo still sends times, but sunrise == sunset or they sit at midnight.
        if (IsoToUtc(At(Get(daily, "sunrise"), d), r.utcOffset, rise) && IsoToUtc(At(Get(daily, "sunset"), d), r.utcOffset, set) &&
            set > rise) {
            r.sunUtc[d * 2] = rise;
            r.sunUtc[d * 2 + 1] = set;
        }
    }

    // Rain (degrades to -1/-1)
    std::string nowStr;
    ULONGLONG now = 0;
    int nowHour = 0;
    const JVal* hourly = Get(&root, "hourly");
    const JVal* times = Get(hourly, "time");
    const JVal* probs = Get(hourly, "precipitation_probability");
    if (GetStr(Get(cur, "time"), nowStr) && ParseIsoLocal(nowStr, now, nowHour) &&
        times && probs && times->type == JVal::Array && probs->type == JVal::Array) {
        const ULONGLONG hourTicks = 3600ULL * 10000000ULL;
        ULONGLONG start = now - (now % hourTicks);      // FILETIME epoch is hour-aligned
        int rh = req.rainHours < 1 ? 1 : req.rainHours;
        ULONGLONG end = now + (ULONGLONG)rh * hourTicks;
        size_t n = times->arr.size() < probs->arr.size() ? times->arr.size() : probs->arr.size();
        int best = -1;
        ULONGLONG bestTicks = 0;
        for (size_t i = 0; i < n; ++i) {
            ULONGLONG t;
            int h;
            if (times->arr[i].type != JVal::String || !ParseIsoLocal(times->arr[i].str, t, h)) continue;
            if (t < start || t >= end) continue;
            double p;
            if (!GetNum(&probs->arr[i], p)) continue;
            int pi = ClampInt(p + 0.5, 0, 100);
            if (pi > best) { best = pi; bestTicks = t; }
        }
        r.rainChance = best;
        r.rainAtUtc = best >= 0 ? bestTicks - (ULONGLONG)((long long)r.utcOffset * 10000000LL) : 0;
    }

    GetSystemTimeAsFileTime(&r.fetchedUtc);
    r.fetchedAt = GetTickCount64();
    r.valid = true;
    out = std::move(r);
    return true;
}

// ---------------------------------------------------------------- worker

struct Job {
    HWND notify;
    UINT msg;
    WeatherRequest req;
};

void RunJob(const Job& job) {
    bool ok = false;
    std::wstring err;
    ULONGLONG t0 = GetTickCount64();
    try {
        GeoResult geo;
        WeatherReading rd;
        if (Geocode(job.req.city, geo, err) && Forecast(geo, job.req, rd, err)) {
            if (job.req.airQuality) AirQuality(geo, rd);
            rd.place = geo.place;
            std::wstring city = Trim(job.req.city);
            SaveCachedReading(city, job.req, rd);
            {
                Lock l;
                g_reading = rd;
                g_readingCity = city;
                g_lastError.clear();
            }
            ok = true;
            Log(L"weather: fetched %s (UTC%+d min) %.1f (feels %.1f) (H %.1f L %.1f) code %d rain %d%% wind %.0f from %d in %llu ms",
                Private(rd.place).c_str(), rd.utcOffset / 60, rd.temp, rd.apparent, rd.high, rd.low, rd.code, rd.rainChance, rd.windSpeed,
                rd.windDir, GetTickCount64() - t0);
        }
    } catch (...) {
        err = L"internal error while fetching weather";
    }
    if (!ok) {
        Lock l;
        g_lastError = err.empty() ? L"unknown error" : err;
        Log(L"weather: failed: %s", PrivateIn(g_lastError, Trim(job.req.city)).c_str());
    }
    HWND w = job.notify;
    UINT m = job.msg;
    g_busy.store(false);
    if (w) PostMessageW(w, m, ok ? 1 : 0, 0);
}

} // namespace

// ---------------------------------------------------------------- public API

bool WeatherFetchAsync(HWND notify, UINT msg, const WeatherRequest& req) {
    if (Trim(req.city).empty()) return false;
    bool expected = false;
    if (!g_busy.compare_exchange_strong(expected, true)) return false;
    try {
        std::thread([job = Job{ notify, msg, req }]() {
            try { RunJob(job); }
            catch (...) { g_busy.store(false); }
        }).detach();
    } catch (...) {
        g_busy.store(false);
        Lock l;
        g_lastError = L"could not start weather thread";
        return false;
    }
    return true;
}

bool WeatherFetchOnce(const WeatherRequest& req, WeatherReading& out, std::wstring& err) {
    GeoResult geo;
    if (!Geocode(req.city, geo, err, false) || !Forecast(geo, req, out, err)) return false;
    if (req.airQuality) AirQuality(geo, out);
    out.place = geo.place;
    return true;
}

bool WeatherLatest(const std::wstring& city, WeatherReading& out) {
    {
        Lock l;
        if (g_reading.valid && SameCity(g_readingCity, city)) { out = g_reading; return true; }
    }
    WeatherReading r;
    if (!LoadCachedReading(city, r)) return false;
    Lock l;
    if (!g_reading.valid) { g_reading = r; g_readingCity = Trim(city); }
    out = std::move(r);
    return true;
}

std::wstring WeatherLastError() {
    Lock l;
    return g_lastError;
}
