#include "settings.h"
#include <cstdio>
#include <cstdarg>
#include <cwchar>
#include <cwctype>
#include <vector>

bool g_logEnabled = false;

static const wchar_t* kSection = L"Clock";
static const wchar_t kMonitorPrefix[] = L"Monitor ";   // [Monitor <spec>] override sections

namespace {
struct Src { std::wstring path; std::wstring section; };   // where a key is read from
}
static const wchar_t* kRunKey  = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
static const wchar_t* kRunName = L"Overlook";
static const wchar_t* kOldName = L"DesktopClock";   // the app's name before it became Overlook

std::wstring ExePath() {
    wchar_t buf[MAX_PATH * 2];
    DWORD n = GetModuleFileNameW(nullptr, buf, ARRAYSIZE(buf));
    return std::wstring(buf, n);
}

std::wstring ExeDir() {
    std::wstring p = ExePath();
    size_t s = p.find_last_of(L"\\/");
    return s == std::wstring::npos ? L"." : p.substr(0, s);
}

std::wstring ExeBase() {
    std::wstring p = ExePath();
    const size_t dot = p.find_last_of(L'.'), slash = p.find_last_of(L"\\/");
    return dot == std::wstring::npos || (slash != std::wstring::npos && dot < slash) ? p : p.substr(0, dot);
}

std::wstring IniPath() { return ExeBase() + L".ini"; }

void Log(const wchar_t* fmt, ...) {
    if (!g_logEnabled) return;
    wchar_t msg[1024];
    va_list ap; va_start(ap, fmt);
    _vsnwprintf_s(msg, _TRUNCATE, fmt, ap);
    va_end(ap);
    SYSTEMTIME st; GetLocalTime(&st);
    wchar_t line[1200];
    int n = swprintf_s(line, L"%02u:%02u:%02u.%03u  %s\r\n", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, msg);
    const std::wstring path = ExeBase() + L".log";
    HANDLE h = CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    int bytes = WideCharToMultiByte(CP_UTF8, 0, line, n, nullptr, 0, nullptr, nullptr);
    std::vector<char> u8(bytes);
    WideCharToMultiByte(CP_UTF8, 0, line, n, u8.data(), bytes, nullptr, nullptr);
    DWORD w; WriteFile(h, u8.data(), bytes, &w, nullptr);
    CloseHandle(h);
}

static const wchar_t kDefaultIni[] =
L"; Overlook settings. Saved changes are picked up automatically.\r\n"
L"; The common ones are also in the settings window (tray menu > Settings...), which edits this file in place.\r\n"
L"; Sizes are in DIPs (device-independent pixels; 1 DIP = 2 physical pixels at 200% scaling).\r\n"
L"\r\n"
L"[Clock]\r\n"
L"; Which monitor(s): all, or a comma separated list of primary | top | bottom | left | right |\r\n"
L";   1,2,... | \\\\.\\DISPLAY2 | part of the monitor's name. Example: top,primary\r\n"
L"Monitor=all\r\n"
L"; Centre of the date+time block, as % of the monitor height from the top\r\n"
L"VerticalPosition=16.5\r\n"
L"; Horizontal nudge in DIPs (+ = right)\r\n"
L"HorizontalOffset=0\r\n"
L"\r\n"
L"; Font sizes (em size, DIPs). Time is ~4.5x the date.\r\n"
L"TimeSize=145\r\n"
L"DateSize=32\r\n"
L"; Space between the date baseline and the top of the time digits (DIPs)\r\n"
L"Gap=25\r\n"
L"; Letter spacing in % of the font size (negative = tighter)\r\n"
L"TimeLetterSpacing=-2\r\n"
L"DateLetterSpacing=0\r\n"
L"\r\n"
L"; Colour (#RRGGBB) and opacity (%)\r\n"
L"Color=#FFFFFF\r\n"
L"Opacity=80\r\n"
L"; Auto-contrast: sample the wallpaper behind the clock and use DarkColor when it is bright\r\n"
L"AutoContrast=1\r\n"
L"DarkColor=#1A1A1A\r\n"
L"; Perceived lightness (0-100) of the wallpaper above which DarkColor is used\r\n"
L"BrightThreshold=60\r\n"
L"; Tint the text toward the wallpaper's colour behind it, like macOS (0 = neutral, 100 = strong)\r\n"
L"Tint=35\r\n"
L"\r\n"
L"; Soft shadow for legibility\r\n"
L"Shadow=1\r\n"
L"ShadowOpacity=30\r\n"
L"ShadowBlur=6\r\n"
L"ShadowOffsetY=1\r\n"
L"\r\n"
L"; Weather line under the time, from Open-Meteo (free, no account). Leave WeatherCity empty to turn it off.\r\n"
L"; The city name (e.g. Lisbon, \"Porto, PT\", or a postcode) is looked up once; the coordinates are\r\n"
L"; sent to open-meteo.com every 30 minutes.\r\n"
L"WeatherCity=\r\n"
L"; C or F\r\n"
L"WeatherUnits=C\r\n"
L"WeatherSize=28\r\n"
L"; Space between the time and the weather line (DIPs)\r\n"
L"WeatherGap=35\r\n"
L"; Show the chance of rain when it reaches RainThreshold % within the next RainHours hours\r\n"
L"RainThreshold=30\r\n"
L"RainHours=6\r\n"
L"; Show the weather line (ShowWeather=0 in a [Monitor ...] section hides it on that monitor)\r\n"
L"ShowWeather=1\r\n"
L"; Extras, each shown only when it matters:\r\n"
L";   \"feels 12°\" when the apparent temperature differs by FeelsLikeDiff degrees or more\r\n"
L"FeelsLike=1\r\n"
L"FeelsLikeDiff=2\r\n"
L";   wind speed and direction from WindThreshold (km/h, or mph with WeatherUnits=F)\r\n"
L"Wind=1\r\n"
L"WindThreshold=20\r\n"
L";   the next sunrise or sunset when it is less than SunHours away (0 = always)\r\n"
L"SunTimes=1\r\n"
L"SunHours=3\r\n"
L"; On a busy day the line may take up to WeatherMaxWidth % of the time's width; beyond that the extras\r\n"
L"; give way in this order: sunrise/sunset, wind, UV, air, feels-like, rain\r\n"
L"WeatherMaxWidth=150\r\n"
L";   \"UV 7\" from UVThreshold; \"Air poor\" and worse (European index, US index with WeatherUnits=F)\r\n"
L"UV=1\r\n"
L"UVThreshold=6\r\n"
L"AirQuality=1\r\n"
L"\r\n"
L"; Now-playing line under the weather, from Windows' media controls.\r\n"
L"; After playback pauses it dims to NowPlayingPausedOpacity % and stays NowPlayingLinger seconds.\r\n"
L"NowPlaying=1\r\n"
L"; Apps it listens to, by name (part of the app's id, case and spaces ignored). Videos in a browser\r\n"
L"; are left out unless you add the browser (e.g. chrome, msedge, firefox); all = every app.\r\n"
L"NowPlayingApps=Spotify, Apple Music, iTunes, TIDAL, Deezer, Amazon Music, Qobuz, YouTube Music, Pandora, foobar2000, MusicBee, AIMP, Winamp, Plexamp, Cider\r\n"
L"NowPlayingLinger=60\r\n"
L"NowPlayingPausedOpacity=50\r\n"
L"; The weather and now-playing lines' opacity, % of Opacity (below 100 the time stands out more)\r\n"
L"SecondaryOpacity=70\r\n"
L"\r\n"
L"; OLED care, usually set in a [Monitor ...] section for the OLED only:\r\n"
L";   PixelShift moves the clock along a slow loop, 1 px every 10 minutes, within PixelShiftRange px\r\n"
L"PixelShift=0\r\n"
L"PixelShiftRange=4\r\n"
L";   NightDim lowers the opacity to NightOpacity between sunset and sunrise (from the weather data,\r\n"
L";   or NightStart-NightEnd when the weather line is off)\r\n"
L"NightDim=0\r\n"
L"NightOpacity=45\r\n"
L"NightStart=20:00\r\n"
L"NightEnd=07:00\r\n"
L"\r\n"
L"; Crossfade when the time changes, in milliseconds (0 = instant)\r\n"
L"TransitionMs=500\r\n"
L"\r\n"
L"; 24-hour clock (0 = 12-hour)\r\n"
L"Hour24=1\r\n"
L"; Pad the hour with a zero (07:05 instead of 7:05)\r\n"
L"LeadingZero=1\r\n"
L"; Show AM/PM in 12-hour mode\r\n"
L"ShowAmPm=0\r\n"
L"; Date picture, Windows format: dddd=weekday, ddd=short weekday, MMMM=month, MMM=short month, d/dd=day, yyyy=year\r\n"
L"DateFormat=dddd, MMMM d\r\n"
L"; Language for day/month names (e.g. en-US, en-GB, pt-PT)\r\n"
L"DateLocale=en-US\r\n"
L"; Optional OpenType features, comma separated (e.g. tnum, ss01, cv08)\r\n"
L"TimeFeatures=\r\n"
L"DateFeatures=\r\n"
L"; Centring of the time: 0 = its outline (ink box) on the centre line; 100 = its visual weight\r\n"
L"; (a thin 1 or 7 makes the outline look off-centre); in between = a blend\r\n"
L"OpticalCentring=0\r\n"
L"; What sits on the centre line: ink = every line's outline (default); colon = the time's colon, so it\r\n"
L"; never moves and only changed digits slide, but a lopsided time (11:08, 20:11) sits off-centre\r\n"
L"CentreOn=ink\r\n"
L"\r\n"
L"; Start with Windows (HKCU Run key). Also toggled from the tray menu.\r\n"
L"StartWithWindows=0\r\n"
L"; Notification-area icon with Settings / Edit settings file / Reload / Start with Windows / Exit\r\n"
L"TrayIcon=1\r\n"
L"; Hide every clock (also from the tray menu). Show=0 in a [Monitor ...] section also hides one monitor's clock.\r\n"
L"HideClocks=0\r\n"
L"; Monitors whose clock is switched off in the tray menu, by name, separated by ;\r\n"
L"HiddenMonitors=\r\n"
L"; Write Overlook.log next to the exe\r\n"
L"Log=0\r\n"
L"\r\n"
L"; Per-monitor overrides: a [Monitor <which>] section (same names as Monitor=) can set any key above\r\n"
L"; except Monitor, WeatherCity, WeatherUnits, RainHours, NowPlayingApps, StartWithWindows, TrayIcon,\r\n"
L"; HideClocks, HiddenMonitors and Log.\r\n"
L"; For example:\r\n"
L";\r\n"
L"; [Monitor primary]\r\n"
L"; VerticalPosition=30\r\n"
L"; ShowWeather=0\r\n"
L"; PixelShift=1\r\n"
L"; NightDim=1\r\n";

void WriteDefaultIni(const std::wstring& path) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    // UTF-16LE with BOM: the format GetPrivateProfileString/WritePrivateProfileString handle natively.
    const WORD bom = 0xFEFF;
    DWORD w;
    WriteFile(h, &bom, sizeof(bom), &w, nullptr);
    WriteFile(h, kDefaultIni, (DWORD)(wcslen(kDefaultIni) * sizeof(wchar_t)), &w, nullptr);
    CloseHandle(h);
}

static std::wstring ReadStr(const Src& ini, const wchar_t* key, const std::wstring& def) {
    wchar_t buf[512];
    GetPrivateProfileStringW(ini.section.c_str(), key, def.c_str(), buf, ARRAYSIZE(buf), ini.path.c_str());
    std::wstring s = buf;
    // strip inline comments and whitespace
    size_t sc = s.find(L" ;");
    if (sc != std::wstring::npos) s.resize(sc);
    while (!s.empty() && iswspace(s.back())) s.pop_back();
    size_t b = 0; while (b < s.size() && iswspace(s[b])) ++b;
    return s.substr(b);
}

static double ReadNum(const Src& ini, const wchar_t* key, double def) {
    std::wstring s = ReadStr(ini, key, L"");
    if (s.empty()) return def;
    if (!s.empty() && s.back() == L'%') s.pop_back();
    wchar_t* end = nullptr;
    double v = wcstod(s.c_str(), &end);
    return end == s.c_str() ? def : v;
}

static bool ReadBool(const Src& ini, const wchar_t* key, bool def) {
    std::wstring s = ReadStr(ini, key, L"");
    if (s.empty()) return def;
    wchar_t c = (wchar_t)towlower(s[0]);
    if (c == L'1' || c == L't' || c == L'y') return true;
    if (c == L'0' || c == L'f' || c == L'n') return false;
    if (_wcsicmp(s.c_str(), L"on") == 0) return true;
    if (_wcsicmp(s.c_str(), L"off") == 0) return false;
    return def;
}

static COLORREF ReadColor(const Src& ini, const wchar_t* key, COLORREF def) {
    std::wstring s = ReadStr(ini, key, L"");
    if (!s.empty() && s[0] == L'#') s = s.substr(1);
    if (s.size() == 3) s = { s[0], s[0], s[1], s[1], s[2], s[2] };
    if (s.size() != 6) return def;
    wchar_t* end = nullptr;
    unsigned long v = wcstoul(s.c_str(), &end, 16);
    if (*end) return def;
    return RGB((v >> 16) & 0xFF, (v >> 8) & 0xFF, v & 0xFF);
}

static double Clamp(double v, double lo, double hi) { return v < lo ? lo : (v > hi ? hi : v); }

// Keys that can also be set per monitor; defaults (and missing keys) come from `d`.
static void ReadClockKeys(const Src& ini, const Settings& d, Settings& s) {
    s.verticalPos      = Clamp(ReadNum(ini, L"VerticalPosition", d.verticalPos), 0, 100);
    s.horizontalOffset = ReadNum(ini, L"HorizontalOffset", d.horizontalOffset);
    s.timeSize         = Clamp(ReadNum(ini, L"TimeSize", d.timeSize), 4, 1000);
    s.dateSize         = Clamp(ReadNum(ini, L"DateSize", d.dateSize), 4, 400);
    s.gap              = Clamp(ReadNum(ini, L"Gap", d.gap), -500, 1000);
    s.timeTracking     = Clamp(ReadNum(ini, L"TimeLetterSpacing", d.timeTracking), -50, 100);
    s.dateTracking     = Clamp(ReadNum(ini, L"DateLetterSpacing", d.dateTracking), -50, 100);
    s.color            = ReadColor(ini, L"Color", d.color);
    s.darkColor        = ReadColor(ini, L"DarkColor", d.darkColor);
    s.opacity          = Clamp(ReadNum(ini, L"Opacity", d.opacity), 0, 100);
    s.autoContrast     = ReadBool(ini, L"AutoContrast", d.autoContrast);
    s.brightThreshold  = Clamp(ReadNum(ini, L"BrightThreshold", d.brightThreshold), 0, 100);
    s.tint             = Clamp(ReadNum(ini, L"Tint", d.tint), 0, 100);
    s.shadow           = ReadBool(ini, L"Shadow", d.shadow);
    s.shadowOpacity    = Clamp(ReadNum(ini, L"ShadowOpacity", d.shadowOpacity), 0, 100);
    s.shadowBlur       = Clamp(ReadNum(ini, L"ShadowBlur", d.shadowBlur), 0, 100);
    s.shadowOffsetY    = Clamp(ReadNum(ini, L"ShadowOffsetY", d.shadowOffsetY), -100, 100);
    s.showWeather      = ReadBool(ini, L"ShowWeather", d.showWeather);
    s.feelsLike        = ReadBool(ini, L"FeelsLike", d.feelsLike);
    s.feelsLikeDiff    = Clamp(ReadNum(ini, L"FeelsLikeDiff", d.feelsLikeDiff), 0, 50);
    s.wind             = ReadBool(ini, L"Wind", d.wind);
    s.windThreshold    = Clamp(ReadNum(ini, L"WindThreshold", d.windThreshold), 0, 500);
    s.sunTimes         = ReadBool(ini, L"SunTimes", d.sunTimes);
    s.sunHours         = Clamp(ReadNum(ini, L"SunHours", d.sunHours), 0, 24);
    s.weatherMaxWidth  = ReadNum(ini, L"WeatherMaxWidth", d.weatherMaxWidth);
    if (s.weatherMaxWidth > 0 && s.weatherMaxWidth <= 5) s.weatherMaxWidth *= 100;   // "1.8" = 1.8x the time = 180%
    s.weatherMaxWidth  = Clamp(s.weatherMaxWidth, 50, 1000);
    s.pixelShift       = ReadBool(ini, L"PixelShift", d.pixelShift);
    s.pixelShiftRange  = Clamp(ReadNum(ini, L"PixelShiftRange", d.pixelShiftRange), 1, 20);
    s.nightDim         = ReadBool(ini, L"NightDim", d.nightDim);
    s.nightOpacity     = Clamp(ReadNum(ini, L"NightOpacity", d.nightOpacity), 0, 100);
    s.nightStart       = ReadStr(ini, L"NightStart", d.nightStart);
    s.nightEnd         = ReadStr(ini, L"NightEnd", d.nightEnd);
    s.show             = ReadBool(ini, L"Show", d.show);
    s.uv               = ReadBool(ini, L"UV", d.uv);
    s.uvThreshold      = Clamp(ReadNum(ini, L"UVThreshold", d.uvThreshold), 0, 20);
    s.airQuality       = ReadBool(ini, L"AirQuality", d.airQuality);
    s.nowPlaying       = ReadBool(ini, L"NowPlaying", d.nowPlaying);
    s.nowPlayingLinger = Clamp(ReadNum(ini, L"NowPlayingLinger", d.nowPlayingLinger), 0, 3600);
    s.nowPlayingPausedOpacity = Clamp(ReadNum(ini, L"NowPlayingPausedOpacity", d.nowPlayingPausedOpacity), 0, 100);
    s.secondaryOpacity = Clamp(ReadNum(ini, L"SecondaryOpacity", d.secondaryOpacity), 0, 100);
    s.weatherSize      = Clamp(ReadNum(ini, L"WeatherSize", d.weatherSize), 4, 400);
    s.weatherGap       = Clamp(ReadNum(ini, L"WeatherGap", d.weatherGap), -500, 1000);
    s.rainThreshold    = Clamp(ReadNum(ini, L"RainThreshold", d.rainThreshold), 0, 100);
    s.transitionMs     = Clamp(ReadNum(ini, L"TransitionMs", d.transitionMs), 0, 5000);
    s.hour24           = ReadBool(ini, L"Hour24", d.hour24);
    s.leadingZero      = ReadBool(ini, L"LeadingZero", d.leadingZero);
    s.showAmPm         = ReadBool(ini, L"ShowAmPm", d.showAmPm);
    s.dateFormat       = ReadStr(ini, L"DateFormat", d.dateFormat);
    s.dateLocale       = ReadStr(ini, L"DateLocale", d.dateLocale);
    s.timeFeatures     = ReadStr(ini, L"TimeFeatures", d.timeFeatures);
    s.opticalCentring  = Clamp(ReadNum(ini, L"OpticalCentring", d.opticalCentring), 0, 100);
    s.centreOnColon    = _wcsicmp(ReadStr(ini, L"CentreOn", d.centreOnColon ? L"colon" : L"ink").c_str(), L"colon") == 0;
    s.dateFeatures     = ReadStr(ini, L"DateFeatures", d.dateFeatures);
}

// Takes over what the app left under its old name (DesktopClock) in its own folder: the settings
// and weather cache are renamed once, and an old Start with Windows entry for this folder is
// removed (ApplySettings re-creates it under the new name when StartWithWindows=1). Another
// program's "DesktopClock" entry, pointing elsewhere, is left alone.
static void MigrateOldName() {
    const std::wstring dir = ExeDir() + L"\\";
    auto take = [&](const std::wstring& oldName, const std::wstring& now) {
        const std::wstring old = dir + oldName;
        if (_wcsicmp(old.c_str(), now.c_str()) != 0 && GetFileAttributesW(now.c_str()) == INVALID_FILE_ATTRIBUTES &&
            GetFileAttributesW(old.c_str()) != INVALID_FILE_ATTRIBUTES && MoveFileW(old.c_str(), now.c_str()))
            Log(L"settings: took over %s", oldName.c_str());
    };
    take(std::wstring(kOldName) + L".ini", IniPath());
    take(std::wstring(kOldName) + L".weather.ini", ExeBase() + L".weather.ini");
    wchar_t buf[MAX_PATH * 2]; DWORD cb = sizeof(buf);
    if (_wcsicmp(kOldName, kRunName) != 0 &&
        RegGetValueW(HKEY_CURRENT_USER, kRunKey, kOldName, RRF_RT_REG_SZ, nullptr, buf, &cb) == ERROR_SUCCESS) {
        const std::wstring cmd = buf, mine = L"\"" + dir;
        if (_wcsnicmp(cmd.c_str(), mine.c_str(), mine.size()) == 0) {
            HKEY k;
            if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_SET_VALUE, &k) == ERROR_SUCCESS) {
                RegDeleteValueW(k, kOldName);
                RegCloseKey(k);
            }
        }
    }
}

Settings LoadSettings() {
    MigrateOldName();
    const Src ini{ IniPath(), kSection };
    if (GetFileAttributesW(ini.path.c_str()) == INVALID_FILE_ATTRIBUTES) WriteDefaultIni(ini.path);

    Settings d, s;
    ReadClockKeys(ini, d, s);
    // App-wide keys: [Clock] only.
    s.monitor          = ReadStr(ini, L"Monitor", d.monitor);
    s.weatherCity      = ReadStr(ini, L"WeatherCity", d.weatherCity);
    if (s.weatherCity.size() >= 2 && s.weatherCity.front() == L'"' && s.weatherCity.back() == L'"')
        s.weatherCity = s.weatherCity.substr(1, s.weatherCity.size() - 2);
    {
        std::wstring u = ReadStr(ini, L"WeatherUnits", L"C");
        s.weatherFahrenheit = !u.empty() && towupper(u[0]) == L'F';
    }
    s.rainHours        = Clamp(ReadNum(ini, L"RainHours", d.rainHours), 1, 48);
    s.startWithWindows = ReadBool(ini, L"StartWithWindows", d.startWithWindows);
    s.trayIcon         = ReadBool(ini, L"TrayIcon", d.trayIcon);
    s.hideClocks       = ReadBool(ini, L"HideClocks", d.hideClocks);
    s.hiddenMonitors   = ReadStr(ini, L"HiddenMonitors", d.hiddenMonitors);
    s.nowPlayingApps   = ReadStr(ini, L"NowPlayingApps", d.nowPlayingApps);
    s.log              = ReadBool(ini, L"Log", d.log);
    return s;
}

std::vector<std::wstring> MonitorSections() {
    std::vector<std::wstring> out;
    std::vector<wchar_t> buf(8192);
    const DWORD n = GetPrivateProfileSectionNamesW(buf.data(), (DWORD)buf.size(), IniPath().c_str());
    const size_t prefix = wcslen(kMonitorPrefix);
    for (const wchar_t* p = buf.data(); p < buf.data() + n && *p; p += wcslen(p) + 1)
        if (wcslen(p) > prefix && _wcsnicmp(p, kMonitorPrefix, prefix) == 0) out.push_back(p + prefix);
    return out;
}

Settings MonitorSettings(const Settings& base, const std::wstring& spec) {
    Settings s = base;
    ReadClockKeys(Src{ IniPath(), kMonitorPrefix + spec }, base, s);
    return s;
}

void SaveBool(const wchar_t* key, bool v) {
    WritePrivateProfileStringW(kSection, key, v ? L"1" : L"0", IniPath().c_str());
}

void SaveString(const wchar_t* key, const std::wstring& v) {
    WritePrivateProfileStringW(kSection, key, v.c_str(), IniPath().c_str());
}

static std::wstring RunCommand() { return L"\"" + ExePath() + L"\""; }

bool IsAutostartEnabled() {
    wchar_t buf[MAX_PATH * 2]; DWORD cb = sizeof(buf);
    if (RegGetValueW(HKEY_CURRENT_USER, kRunKey, kRunName, RRF_RT_REG_SZ, nullptr, buf, &cb) != ERROR_SUCCESS) return false;
    return _wcsicmp(buf, RunCommand().c_str()) == 0;
}

void SetAutostart(bool on) {
    HKEY k;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kRunKey, 0, nullptr, 0, KEY_SET_VALUE | KEY_QUERY_VALUE, nullptr, &k, nullptr) != ERROR_SUCCESS) return;
    if (on) {
        std::wstring cmd = RunCommand();
        RegSetValueExW(k, kRunName, 0, REG_SZ, (const BYTE*)cmd.c_str(), (DWORD)((cmd.size() + 1) * sizeof(wchar_t)));
    } else {
        RegDeleteValueW(k, kRunName);
    }
    RegCloseKey(k);
}
