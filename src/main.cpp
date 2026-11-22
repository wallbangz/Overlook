// Overlook — a macOS lock-screen style clock that lives on the Windows desktop layer.
#include <windows.h>
#include <shellapi.h>
#include <wtsapi32.h>
#include <string>
#include <cmath>
#include <algorithm>
#include <vector>
#include <memory>
#include "settings.h"
#include "desktop.h"
#include "render.h"
#include "wallpaper.h"
#include "color.h"
#include "weather.h"
#include "weatherline.h"
#include "selftest.h"
#include "media.h"
#include "icons.h"

namespace {

const wchar_t kCtlClass[]   = L"OverlookController";
const wchar_t kClockClass[] = L"OverlookClock";
const wchar_t kMutexName[]  = L"Local\\Overlook-7f3c2a1e";

enum : UINT_PTR { TIMER_MINUTE = 1, TIMER_RELOAD, TIMER_REFRESH, TIMER_ATTACH, TIMER_HEALTH, TIMER_WALLPAPER,
                  TIMER_WEATHER, TIMER_MEDIA };
enum : UINT { WM_APP_TRAY = WM_APP + 1, WM_APP_RELOAD, WM_APP_REATTACH, WM_APP_WEATHER, WM_APP_MEDIA };
const UINT kWeatherEveryMs = 30 * 60 * 1000;
enum : UINT { ID_RELOAD = 100, ID_EDIT, ID_FOLDER, ID_AUTOSTART, ID_EXIT, ID_HIDE, ID_WEATHER_NOW, ID_MONITOR = 200 };

HINSTANCE g_inst;
HWND g_ctl;
HWND g_clockParent;     // the desktop window the clocks are children of
HWND g_insertAfter;     // sibling the first clock sits directly below (DefView), or HWND_TOP
Settings g_s;           // [Clock]: app-wide keys and the defaults for every monitor
ClockRenderer g_r;
UINT g_msgTaskbarCreated;
bool g_trayAdded;
HICON g_trayIcon;
FILETIME g_iniTime;
int g_attachFailures;
UINT g_styleGen;          // bumped on every settings load: part of each frame's key

// Paused while every display is off or the session is locked: nothing is visible, so the
// minute timer stops and anything that changed is drawn once on the way back.
bool g_displayOff, g_locked, g_weatherDue;

// Wallpaper changes: Windows crossfades a new wallpaper in over about 1.2 s from the moment its
// new wallpaper layer (WorkerW) appears. The clock starts its colour fade as soon as Windows
// reports the new image and times it to end with Windows' fade.
const ULONGLONG kWallpaperFadeMs = 1200;
ULONGLONG g_wallChangeAt;               // GetTickCount64() of the change being followed, 0 = none
std::vector<std::wstring> g_wallSeen;   // per clock: the image the colour was last checked against
double g_fadeMs;                        // > 0: the duration of the transitions being started
double FadeMs(const Settings& s) { return g_fadeMs > 0 ? g_fadeMs : s.transitionMs; }
HPOWERNOTIFY g_powerNotify;
bool Paused() { return g_displayOff || g_locked; }
// GUID_CONSOLE_DISPLAY_STATE (all console displays: 0 off, 1 on, 2 dimmed)
const GUID kConsoleDisplayState = { 0x6fe69556, 0x704a, 0x47a0, { 0x8f, 0x24, 0xc2, 0x8d, 0x93, 0x6f, 0xda, 0x47 } };

// ------------------------------------------------------------------ helpers

FILETIME IniWriteTime() {
    WIN32_FILE_ATTRIBUTE_DATA a{};
    GetFileAttributesExW(IniPath().c_str(), GetFileExInfoStandard, &a);
    return a.ftLastWriteTime;
}

bool LightTaskbar() {
    DWORD v = 0, cb = sizeof(v);
    RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                 L"SystemUsesLightTheme", RRF_RT_REG_DWORD, nullptr, &v, &cb);
    return v != 0;
}

std::wstring FormatTime(const SYSTEMTIME& t, const Settings& s) {
    int h = t.wHour;
    const wchar_t* suffix = L"";
    if (!s.hour24) {
        suffix = h < 12 ? L" AM" : L" PM";
        h %= 12;
        if (h == 0) h = 12;
    }
    wchar_t buf[32];
    swprintf_s(buf, s.leadingZero ? L"%02d:%02u%s" : L"%d:%02u%s", h, t.wMinute,
               (!s.hour24 && s.showAmPm) ? suffix : L"");
    return buf;
}

std::wstring FormatDate(const SYSTEMTIME& t, const Settings& s) {
    wchar_t buf[256];
    if (!GetDateFormatEx(s.dateLocale.c_str(), 0, &t, s.dateFormat.c_str(), buf, ARRAYSIZE(buf), nullptr) &&
        !GetDateFormatEx(L"en-US", 0, &t, L"dddd, MMMM d", buf, ARRAYSIZE(buf), nullptr))
        return L"";
    return buf;
}

// ------------------------------------------------------------------ weather

int g_weatherFailures;

// "HH:MM" -> minutes after midnight, or -1.
int ParseHm(const std::wstring& v) {
    int h = 0, m = 0;
    if (swscanf_s(v.c_str(), L"%d:%d", &h, &m) != 2 || h < 0 || h > 23 || m < 0 || m > 59) return -1;
    return h * 60 + m;
}

// Between sunset and sunrise at the weather location (or NightStart..NightEnd without sun times).
bool IsNight(const SYSTEMTIME& now, const Settings& s) {
    WeatherReading w;
    bool night = false;
    if (!g_s.weatherCity.empty() && WeatherLatest(g_s.weatherCity, w) && SunNight(w, NowUtc(), night)) return night;
    const int m = now.wHour * 60 + now.wMinute;
    const int a = ParseHm(s.nightStart), b = ParseHm(s.nightEnd);
    if (a < 0 || b < 0 || a == b) return false;
    return a < b ? (m >= a && m < b) : (m >= a || m < b);
}

// Per-monitor keys that need app-wide work (the media watcher, the air-quality request): wanted
// if any shown clock wants them, whether set in [Clock] or in a [Monitor ...] section.
bool AnyClockWants(bool Settings::* key);
double MaxClockSetting(double Settings::* key);
bool g_airWanted;   // the last weather request asked for air quality

void FetchWeather() {
    if (g_s.weatherCity.empty()) { KillTimer(g_ctl, TIMER_WEATHER); return; }
    WeatherRequest r;
    r.city = g_s.weatherCity;
    r.fahrenheit = g_s.weatherFahrenheit;
    r.rainHours = (int)g_s.rainHours;
    r.airQuality = g_airWanted = AnyClockWants(&Settings::airQuality);
    if (WeatherFetchAsync(g_ctl, WM_APP_WEATHER, r)) Log(L"weather: fetching for '%s'", r.city.c_str());
}

void Refresh(bool animate = false);

// ------------------------------------------------------------------ now playing

bool g_mediaOn;   // the media watcher runs (some shown clock has NowPlaying=1)

void UpdateMediaWatch() {
    const bool want = AnyClockWants(&Settings::nowPlaying);
    if (want && !g_mediaOn) {
        g_mediaOn = MediaWatchStart(g_ctl, WM_APP_MEDIA);
        Log(L"media: watcher %s", g_mediaOn ? L"started" : L"could not start");
    } else if (!want && g_mediaOn) {
        MediaWatchStop();
        g_mediaOn = false;
        KillTimer(g_ctl, TIMER_MEDIA);
    }
}

// "<note> Artist — Title" while something plays, and for NowPlayingLinger seconds after it pauses
// (*paused is then set: the line is dimmed).
std::wstring MediaText(const Settings& s, bool* paused) {
    NowPlaying np;
    *paused = false;
    if (!g_mediaOn || !s.nowPlaying || !MediaLatest(np) || !np.active) return L"";
    if (!np.playing && GetTickCount64() - np.statusSince >= (ULONGLONG)(s.nowPlayingLinger * 1000.0)) return L"";
    *paused = !np.playing;
    // Private-use characters would be taken for the renderer's own icon placeholders.
    auto clean = [](std::wstring v) { std::erase_if(v, [](wchar_t ch) { return ch >= 0xE000 && ch <= 0xF8FF; }); return v; };
    const std::wstring title = clean(CleanTrackTitle(np.title)), artist = clean(np.artist);   // title without "(feat. ...)" etc.
    if (title.empty()) return L"";
    std::wstring t(1, icon::Music);
    t += L"\u2002";
    t += artist.empty() ? title : artist + L" \u2014 " + title;
    return t;
}

// While a track is paused, wake up when its linger time is over (to fade the line out).
void ScheduleMediaLinger() {
    NowPlaying np;
    if (g_mediaOn && MediaLatest(np) && np.active && !np.playing) {
        const ULONGLONG linger = (ULONGLONG)(MaxClockSetting(&Settings::nowPlayingLinger) * 1000.0), gone = GetTickCount64() - np.statusSince;
        if (gone < linger) { SetTimer(g_ctl, TIMER_MEDIA, (UINT)(linger - gone + 50), nullptr); return; }
    }
    KillTimer(g_ctl, TIMER_MEDIA);
}

// ------------------------------------------------------------------ tray

void UpdateTray(bool remove = false) {
    NOTIFYICONDATAW nid{ sizeof(nid) };
    nid.hWnd = g_ctl;
    nid.uID = 1;
    if (remove || !g_s.trayIcon) {
        if (g_trayAdded) Shell_NotifyIconW(NIM_DELETE, &nid);
        g_trayAdded = false;
        return;
    }
    if (g_trayIcon) DestroyIcon(g_trayIcon);
    UINT dpi = GetDpiForWindow(g_ctl);
    g_trayIcon = g_r.MakeTrayIcon(GetSystemMetricsForDpi(SM_CXSMICON, dpi), LightTaskbar());
    nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP;
    nid.uCallbackMessage = WM_APP_TRAY;
    nid.hIcon = g_trayIcon;
    wcscpy_s(nid.szTip, L"Overlook");
    if (g_trayAdded && Shell_NotifyIconW(NIM_MODIFY, &nid)) return;
    if (Shell_NotifyIconW(NIM_ADD, &nid)) {
        nid.uVersion = NOTIFYICON_VERSION_4;
        Shell_NotifyIconW(NIM_SETVERSION, &nid);
        g_trayAdded = true;
    }
    Log(L"tray icon %s", g_trayAdded ? L"added" : L"failed");
}

// Monitors listed in the tray menu (Monitor= set), by menu id - ID_MONITOR.
struct TrayMonitor { std::wstring id; bool hidden; bool hiddenInSettings; };
std::vector<TrayMonitor> g_trayMonitors;
struct Wanted;
std::vector<Wanted> WantedClocks(bool includeHidden);
void AppendMonitorItems(HMENU m);

void ShowTrayMenu() {
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING | MF_GRAYED, 0, L"Overlook");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING | (g_s.hideClocks ? MF_CHECKED : 0), ID_HIDE, L"Hide clocks");
    AppendMonitorItems(m);
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, ID_EDIT, L"Edit settings…");
    AppendMenuW(m, MF_STRING, ID_RELOAD, L"Reload settings");
    AppendMenuW(m, MF_STRING, ID_FOLDER, L"Open folder");
    if (!g_s.weatherCity.empty()) AppendMenuW(m, MF_STRING, ID_WEATHER_NOW, L"Refresh weather");
    AppendMenuW(m, MF_STRING | (IsAutostartEnabled() ? MF_CHECKED : 0), ID_AUTOSTART, L"Start with Windows");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, ID_EXIT, L"Exit");
    POINT pt; GetCursorPos(&pt);
    SetForegroundWindow(g_ctl);
    TrackPopupMenuEx(m, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN, pt.x, pt.y, g_ctl, nullptr);
    PostMessageW(g_ctl, WM_NULL, 0, 0);
    DestroyMenu(m);
}

// ------------------------------------------------------------------ clock windows

void ScheduleMinute() {
    if (Paused()) return;
    SYSTEMTIME t; GetLocalTime(&t);
    UINT ms = (59 - t.wSecond) * 1000 + (1000 - t.wMilliseconds) + 40;   // just past the next minute
    SetCoalescableTimer(g_ctl, TIMER_MINUTE, ms, nullptr, TIMERV_NO_COALESCING);
}

// What is on screen (or, during a transition, what we are moving to).
struct Shown {
    FrameLayout lay;          // kept so this frame can be re-drawn piecewise for the next transition
    ComPtr<ID2D1Bitmap1> bmp;
    RECT rc{};                // screen px of the frame
    RECT win{};               // window rect it was shown in (its monitor)
    HWND hwnd = nullptr;
    COLORREF color = 0;
    bool dark = false;
    std::wstring key;         // date|colour|time: what the frame says
    std::wstring dateKey;     // date|colour: if equal in both frames, the date pixels are identical on screen
    std::wstring weatherKey;  // weather line|colour: same idea for the weather line
    std::wstring mediaKey;    // now-playing line|colour
};

// One clock per monitor: its own window (a child of the desktop), settings and frame.
struct Clock {
    MonitorInfo mon;
    Settings s;               // [Clock] plus the [Monitor ...] sections that name this monitor
    HWND hwnd = nullptr;
    RECT winRc{};             // the window's rect, screen px (its whole monitor)
    Shown cur;
    bool animating = false;
    int darkState = -1;       // auto-contrast choice so far (-1 none, 0 light, 1 dark text), for hysteresis
};
const double kContrastHysteresis = 4.0;   // L*: switch light <-> dark only this far past BrightThreshold
std::vector<std::unique_ptr<Clock>> g_clocks;   // in z-order: the first sits directly below the icons
const UINT_PTR kAnimTimer = 1;                  // set on the clock's own window

Clock* ClockFor(HWND hwnd) {
    if (!hwnd) return nullptr;
    for (auto& c : g_clocks) if (c->hwnd == hwnd) return c.get();
    return nullptr;
}

// The monitors that should show a clock, each with its settings.
// How the tray's HiddenMonitors list names a monitor: its friendly name (stable across
// reconnects, unlike \\.\DISPLAYn), or the device name when the name is missing or shared.
std::wstring MonitorId(const MonitorInfo& m, const std::vector<MonitorInfo>& mons) {
    if (m.name.empty()) return m.device;
    for (auto& o : mons)
        if (o.handle != m.handle && _wcsicmp(o.name.c_str(), m.name.c_str()) == 0) return m.device;
    return m.name;
}

std::vector<std::wstring> SplitList(const std::wstring& v) {   // "a;b" -> {a, b}
    std::vector<std::wstring> out;
    size_t from = 0;
    while (from <= v.size()) {
        const size_t semi = v.find(L';', from);
        std::wstring item = v.substr(from, semi == std::wstring::npos ? std::wstring::npos : semi - from);
        while (!item.empty() && iswspace(item.back())) item.pop_back();
        while (!item.empty() && iswspace(item.front())) item.erase(item.begin());
        if (!item.empty()) out.push_back(item);
        if (semi == std::wstring::npos) break;
        from = semi + 1;
    }
    return out;
}

bool TrayHidden(const std::wstring& id) {
    for (auto& h : SplitList(g_s.hiddenMonitors)) if (_wcsicmp(h.c_str(), id.c_str()) == 0) return true;
    return false;
}

struct Wanted { MonitorInfo mon; Settings s; std::wstring id; bool trayHidden = false; };
// includeHidden: also the monitors whose clock is switched off (for the tray menu).
std::vector<Wanted> WantedClocks(bool includeHidden = false) {
    const auto mons = EnumMonitors();
    const auto sections = MonitorSections();
    std::vector<Wanted> out;
    for (auto& m : PickMonitors(g_s.monitor, mons)) {
        Wanted w{ m, g_s, MonitorId(m, mons) };
        w.trayHidden = TrayHidden(w.id);
        for (auto& spec : sections) {   // later sections win
            const int i = FindMonitor(mons, spec);
            if (i >= 0 && mons[i].handle == m.handle) w.s = MonitorSettings(w.s, spec);
        }
        if (!includeHidden && (g_s.hideClocks || !w.s.show || w.trayHidden)) continue;
        out.push_back(std::move(w));
    }
    return out;
}

bool AnyClockWants(bool Settings::* key) {
    for (const Wanted& w : WantedClocks()) if (w.s.*key) return true;
    return false;
}

double MaxClockSetting(double Settings::* key) {   // over the clocks on screen ([Clock] if none)
    if (g_clocks.empty()) return g_s.*key;
    double v = g_clocks.front()->s.*key;
    for (auto& c : g_clocks) v = std::max(v, c->s.*key);
    return v;
}

bool Attached() {
    if (g_clocks.empty()) return false;
    DesktopHost h;
    if (!FindDesktopHost(h)) return false;
    for (auto& c : g_clocks)
        if (!c->hwnd || !IsWindow(c->hwnd) || GetAncestor(c->hwnd, GA_PARENT) != h.parent) return false;
    return true;
}

void TrimMemory() { SetProcessWorkingSetSize(GetCurrentProcess(), (SIZE_T)-1, (SIZE_T)-1); }

// The sibling clock i sits directly below: the icon layer for the first, then the previous clock.
HWND SlotAbove(size_t i) { return i == 0 ? g_insertAfter : g_clocks[i - 1]->hwnd; }

void RefreshSlot() {
    DesktopHost h;
    g_insertAfter = (FindDesktopHost(h) && h.parent == g_clockParent) ? h.insertAfter : HWND_TOP;
}

// A clock window covers its whole monitor: it has no redirection bitmap, so the size costs
// nothing, and it never has to move while a transition runs (frames are visuals inside it).
// Also re-asserts its slot below the icon layer.
void PlaceWindow(size_t i) {
    Clock& c = *g_clocks[i];
    c.winRc = c.mon.rc;
    POINT p{ c.winRc.left, c.winRc.top };
    MapWindowPoints(HWND_DESKTOP, g_clockParent, &p, 1);
    SetWindowPos(c.hwnd, SlotAbove(i), p.x, p.y, c.winRc.right - c.winRc.left, c.winRc.bottom - c.winRc.top,
                 SWP_NOACTIVATE | SWP_SHOWWINDOW);
}

POINT InWindow(const Clock& c, const RECT& rc) { return { rc.left - c.winRc.left, rc.top - c.winRc.top }; }

bool ShowSingle(const Clock& c) { return g_r.ShowFrame(c.hwnd, c.cur.bmp.Get(), InWindow(c, c.cur.rc)); }

void StopAnimation(Clock& c) {
    if (!c.animating) return;
    if (c.hwnd) KillTimer(c.hwnd, kAnimTimer);
    c.animating = false;
}

// True if any clock's monitor now shows a different wallpaper image than when last asked.
bool WallpaperChanged() {
    bool changed = false;
    g_wallSeen.resize(g_clocks.size());
    for (size_t i = 0; i < g_clocks.size(); ++i) {
        std::wstring path;
        int position = 0;
        COLORREF bg = 0;
        if (!WallpaperFile(g_clocks[i]->mon.rc, path, position, bg)) continue;
        if (path != g_wallSeen[i]) { g_wallSeen[i] = path; changed = true; }
    }
    return changed;
}

bool AnyAnimating() {
    for (auto& c : g_clocks) if (c->animating) return true;
    return false;
}

// DWM has finished the transition: swap in the exact final frame (one atomic commit).
void FinishAnimation(Clock& c) {
    StopAnimation(c);
    if (c.hwnd && c.cur.hwnd == c.hwnd) ShowSingle(c);
    if (!AnyAnimating()) TrimMemory();
}

// Transition from `from` (on screen now) to c.cur, run by the compositor. Layers (each a
// pre-rendered bitmap with its shadow):
//   - runs of unchanged time characters (e.g. "03:4" in 03:42 -> 03:43) slide from their old
//     to their new centred position at full opacity;
//   - the changed characters of the old and new time crossfade;
//   - the date stays put, unless it (or the colour) changed, in which case it crossfades too.
bool StartTransition(Clock& c, const Shown& from) {
    using AL = ClockRenderer::AnimLayer;
    const Shown& to = c.cur;
    const POINT o = InWindow(c, from.rc), n = InWindow(c, to.rc);

    // Characters at the same index that didn't change glide; the rest crossfade.
    // (Different lengths, e.g. 9:59 -> 10:00 in 12-hour mode, or a colour change: all crossfade.)
    const std::wstring& t0 = from.lay.timeText;
    const std::wstring& t1 = to.lay.timeText;
    UINT32 stable = 0;
    if (t0.size() == t1.size() && t0.size() <= 32 && from.color == to.color && from.dark == to.dark &&
        from.lay.opacity == to.lay.opacity)
        for (UINT32 i = 0; i < t0.size(); ++i)
            if (t0[i] == t1[i]) stable |= 1u << i;
    const bool dateSame = from.dateKey == to.dateKey;
    const bool weatherSame = from.weatherKey == to.weatherKey;
    const bool mediaSame = from.mediaKey == to.mediaKey;

    std::vector<ComPtr<ID2D1Bitmap1>> keep;   // alive until Animate has copied them
    std::vector<AL> layers;
    auto add = [&](AL::Kind k, ComPtr<ID2D1Bitmap1> b, POINT p0, POINT p1) {
        if (!b) return false;
        keep.push_back(b);
        layers.push_back({ k, b.Get(), (float)p0.x, (float)p0.y, (float)p1.x, (float)p1.y });
        return true;
    };

    if (!stable && !dateSame) {
        if (!add(AL::FadeOut, from.bmp, o, o) || !add(AL::FadeIn, to.bmp, n, n)) return false;
    } else {
        const UINT32 changing = ~stable;
        if (!add(AL::FadeOut, g_r.RenderFrame(from.lay, from.color, from.dark, !dateSame, changing, !weatherSame, !mediaSame), o, o) ||
            !add(AL::FadeIn, g_r.RenderFrame(to.lay, to.color, to.dark, !dateSame, changing, !weatherSame, !mediaSame), n, n))
            return false;
        // Whatever didn't change among the date and weather lines keeps its pixels and slides
        // from where it was to where it goes (usually nowhere; one pixel when the clock shifts).
        const float k = to.lay.dpi / 96.f;
        auto moved = [&](D2D1_POINT_2F a, D2D1_POINT_2F b) -> POINT {   // screen px the unchanged line moves by
            return { lroundf((to.rc.left + b.x * k) - (from.rc.left + a.x * k)), lroundf((to.rc.top + b.y * k) - (from.rc.top + a.y * k)) };
        };
        // Lines that move by the same amount share one layer.
        struct Still { bool date = false, weather = false, media = false; POINT d{}; };
        std::vector<Still> stills;
        auto keepStill = [&](int which, POINT d) {
            auto it = std::find_if(stills.begin(), stills.end(), [&](const Still& x) { return x.d.x == d.x && x.d.y == d.y; });
            if (it == stills.end()) { stills.push_back({}); it = stills.end() - 1; it->d = d; }
            (which == 0 ? it->date : which == 1 ? it->weather : it->media) = true;
        };
        if (dateSame) keepStill(0, moved(from.lay.dateOrigin, to.lay.dateOrigin));
        if (weatherSame && to.lay.weather && from.lay.weather) keepStill(1, moved(from.lay.weatherOrigin, to.lay.weatherOrigin));
        if (mediaSame && to.lay.media && from.lay.media) keepStill(2, moved(from.lay.mediaOrigin, to.lay.mediaOrigin));
        for (const Still& st : stills)
            if (!add(AL::Solid, g_r.RenderFrame(to.lay, to.color, to.dark, st.date, 0, st.weather, st.media),
                     { n.x - st.d.x, n.y - st.d.y }, n))
                return false;
        for (UINT32 i = 0; i < t0.size();) {
            if (!(stable & (1u << i))) { ++i; continue; }
            UINT32 run = 0, start = i;
            while (i < t0.size() && (stable & (1u << i))) run |= 1u << i++;
            // The run is drawn as the new frame has it and slides into that exact place, so the
            // swap to the static frame at the end is pixel-identical. Lines start on whole pixels,
            // so for a leading run (the usual case) dx is a whole number too and the start matches
            // the old frame exactly; otherwise the sub-pixel difference is at the start of the motion.
            const float dx = (to.rc.left + g_r.TimeCharX(to.lay, start)) - (from.rc.left + g_r.TimeCharX(from.lay, start));
            auto bmp = g_r.RenderFrame(to.lay, to.color, to.dark, false, run, false, false);
            if (!bmp) return false;
            keep.push_back(bmp);
            const float sx = std::fabs(dx - std::round(dx)) < 0.01f ? (float)n.x - std::round(dx) : (float)n.x - dx;
            layers.push_back({ AL::Solid, bmp.Get(), sx, (float)o.y, (float)n.x, (float)n.y });
        }
    }
    return g_r.Animate(c.hwnd, layers.data(), (int)layers.size(), FadeMs(c.s) / 1000.0, to.lay.opacity / 100.0);
}

// animate: transition from the frame on screen if the text or colour changed.
// Burn-in care: a slow loop within +-range px, one step every 10 minutes (steps of at most 1 px).
POINT PixelShift(const Settings& s) {
    if (!s.pixelShift) return { 0, 0 };
    FILETIME ft; GetSystemTimeAsFileTime(&ft);
    const ULONGLONG step = ((ULONGLONG(ft.dwHighDateTime) << 32) | ft.dwLowDateTime) / (10ULL * 60 * 10000000);
    const double t = double(step % (37ULL * 29)), R = s.pixelShiftRange, tau = 6.283185307179586;
    return { (LONG)lround(R * std::sin(tau * t / 37)), (LONG)lround(R * std::sin(tau * t / 29 + 1.0)) };   // periods ~6 h and ~5 h
}

// The text colour for a frame whose date+time block covers `ink` (screen px): auto-contrast
// against the wallpaper behind it (with hysteresis: darkState is the choice so far), then the
// tint toward the wallpaper's colour.
void ChooseColour(const Settings& s, const MonitorInfo& mon, const RECT& ink, int& darkState, COLORREF& color, bool& dark) {
    color = s.color;
    dark = false;
    if (!s.autoContrast && s.tint <= 0) return;
    WallpaperSample ws;
    if (!SampleWallpaper(mon.rc, ink, ws)) return;
    if (s.autoContrast) {
        // Hysteresis: a wallpaper right at the threshold doesn't flip the colour back and forth.
        const double thr = darkState < 0 ? s.brightThreshold
                         : darkState ? s.brightThreshold - kContrastHysteresis
                                     : s.brightThreshold + kContrastHysteresis;
        dark = ws.lightness > thr;
        darkState = dark ? 1 : 0;
        if (dark) color = s.darkColor;
    }
    const COLORREF base = color;
    color = TintColor(base, ws, s.tint / 100.0);
    Log(L"wallpaper %s L*=%.1f rgb(lin)=%.3f,%.3f,%.3f -> %s text #%02X%02X%02X tinted #%02X%02X%02X",
        mon.device.c_str(), ws.lightness, ws.r, ws.g, ws.b, dark ? L"dark" : L"light",
        GetRValue(base), GetGValue(base), GetBValue(base), GetRValue(color), GetGValue(color), GetBValue(color));
}

void Render(size_t i, bool animate) {
    Clock& c = *g_clocks[i];
    if (!c.hwnd) return;
    Settings s = c.s;   // copy: the night opacity applies to this frame only
    const MonitorInfo& mon = c.mon;
    SYSTEMTIME t; GetLocalTime(&t);

    const bool night = s.nightDim && IsNight(t, s);
    if (night) s.opacity = s.nightOpacity;
    const std::wstring date = FormatDate(t, s), time = FormatTime(t, s);
    WeatherReading wr;
    const bool haveWeather = !g_s.weatherCity.empty() && WeatherLatest(g_s.weatherCity, wr);
    std::wstring weather;
    bool mediaPaused = false;
    const std::wstring media = MediaText(s, &mediaPaused);
    Shown f;
    if (!PrepareFitted(g_r, s, mon.dpi, date, time, haveWeather ? &wr : nullptr, NowUtc(), g_s.weatherFahrenheit,
                       f.lay, &weather, nullptr, media)) {
        Log(L"layout failed");
        return;
    }
    if (mediaPaused) f.lay.mediaAlpha = (float)(s.nowPlayingPausedOpacity / 100.0);
    const ClockGeometry& g = f.lay.g;

    const double k = mon.dpi / 96.0;
    const int monW = mon.rc.right - mon.rc.left, monH = mon.rc.bottom - mon.rc.top;
    const int cx = mon.rc.left + (int)lround(monW / 2.0 + s.horizontalOffset * k);
    const int cy = mon.rc.top + (int)lround(monH * s.verticalPos / 100.0);
    const POINT shift = PixelShift(s);
    POINT pos{ cx - g.anchorX + shift.x, cy - g.anchorY + shift.y };   // screen px

    RECT ink = g.ink;
    OffsetRect(&ink, pos.x, pos.y);
    ChooseColour(s, mon, ink, c.darkState, f.color, f.dark);

    f.rc = { pos.x, pos.y, pos.x + g.width, pos.y + g.height };
    f.win = mon.rc;
    f.hwnd = c.hwnd;
    wchar_t col[48]; swprintf_s(col, L"|%06X|%.0f|%d|%u", (unsigned)f.color, s.opacity, f.dark ? 1 : 0, g_styleGen);
    f.dateKey = date + col;
    f.weatherKey = weather + col;
    f.mediaKey = media + col + (mediaPaused ? L"|paused" : L"");
    f.key = f.dateKey + L"|" + time + L"|" + weather + L"|" + f.mediaKey;   // (mediaKey carries the paused state)
    // Same text, colour, style and place as what is on screen, or as a running transition is
    // heading to (e.g. a settings broadcast on unlock, or a second wallpaper notification during
    // the colour fade): nothing to draw, just keep the slot. Redrawing would cut the fade short.
    if (c.cur.bmp && c.cur.hwnd == c.hwnd && c.cur.key == f.key && EqualRect(&c.cur.rc, &f.rc) &&
        EqualRect(&c.cur.win, &f.win) && c.cur.lay.dpi == f.lay.dpi) {
        PlaceWindow(i);
        Log(L"render %s: unchanged", mon.device.c_str());
        return;
    }
    f.bmp = g_r.RenderFrame(f.lay, f.color, f.dark);
    if (!f.bmp) { Log(L"render failed"); return; }

    const bool wantAnim = animate && s.transitionMs > 0 && c.cur.bmp && c.cur.hwnd == c.hwnd &&
                          c.cur.lay.dpi == f.lay.dpi && EqualRect(&c.cur.win, &f.win) && c.cur.key != f.key;
    StopAnimation(c);   // a running transition just jumps ahead to the new frame
    PlaceWindow(i);
    Shown from = std::move(c.cur);
    c.cur = std::move(f);
    const bool anim = wantAnim && StartTransition(c, from);
    if (anim) {
        c.animating = true;
        // DWM runs the animation; we only come back to swap in the final static frame.
        SetTimer(c.hwnd, kAnimTimer, (UINT)FadeMs(c.s) + 60, nullptr);
    } else {
        ShowSingle(c);
    }
    Log(L"render %s %s on %s (%d dpi) at %d,%d %dx%d%s%s [%s] [%s]", date.c_str(), time.c_str(), mon.device.c_str(),
        mon.dpi, pos.x, pos.y, g.width, g.height, night ? L" night" : L"", anim ? L" (transition)" : L"", weather.c_str(),
        media.c_str());
}

void RenderAll(bool animate = false) {
    if (g_clocks.empty() || Paused()) return;   // drawn when the displays come back
    RefreshSlot();
    for (size_t i = 0; i < g_clocks.size(); ++i) Render(i, animate);
    if (!AnyAnimating()) TrimMemory();   // idle until the next minute: hand the pages back
}

// ------------------------------------------------------------------ keeping our slot on the desktop

// Explorer recreates the wallpaper WorkerW when the wallpaper changes, and a new child window
// starts at the top of its siblings, i.e. above the clocks. We watch Explorer's window events
// (out of context: no DLL injection, nothing runs while the desktop is idle) and put the clocks
// back directly below the icon layer the moment anything lands above them.
HWINEVENTHOOK g_deskHook;
DWORD g_deskHookTid;

std::wstring DescribeDesktop() {   // "DefView > CLOCK1 > CLOCK2 > WorkerW" (for the log)
    std::wstring s;
    for (HWND w = GetWindow(g_clockParent, GW_CHILD); w; w = GetWindow(w, GW_HWNDNEXT)) {
        wchar_t cls[64] = L"";
        GetClassNameW(w, cls, 64);
        if (!s.empty()) s += L" > ";
        int idx = 0;
        for (size_t i = 0; i < g_clocks.size(); ++i) if (g_clocks[i]->hwnd == w) idx = (int)i + 1;
        s += idx ? L"CLOCK" + std::to_wstring(idx) : wcscmp(cls, L"SHELLDLL_DefView") == 0 ? L"DefView" : cls;
        if (!IsWindowVisible(w)) s += L"(hidden)";
    }
    return s;
}

bool InPlace() {
    for (size_t i = 0; i < g_clocks.size(); ++i) {
        const HWND above = SlotAbove(i), me = g_clocks[i]->hwnd;
        if (!me) continue;
        if (above == HWND_TOP ? GetWindow(me, GW_HWNDPREV) != nullptr : GetWindow(above, GW_HWNDNEXT) != me) return false;
    }
    return true;
}

void KeepInPlace(const wchar_t* why) {
    if (g_clocks.empty() || !g_insertAfter || InPlace()) return;
    if (g_logEnabled) Log(L"z-order: %s (%s) -> restoring", DescribeDesktop().c_str(), why);
    for (size_t i = 0; i < g_clocks.size(); ++i)
        if (g_clocks[i]->hwnd)
            SetWindowPos(g_clocks[i]->hwnd, SlotAbove(i), 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}

void CALLBACK DesktopEvent(HWINEVENTHOOK, DWORD ev, HWND hwnd, LONG idObject, LONG, DWORD, DWORD) {
    if (idObject != OBJID_WINDOW || !hwnd || g_clocks.empty() || !g_clockParent) return;
    const bool sibling = !ClockFor(hwnd) && GetAncestor(hwnd, GA_PARENT) == g_clockParent;
    if (hwnd != g_clockParent && !sibling) return;
    if (sibling && (ev == EVENT_OBJECT_CREATE || ev == EVENT_OBJECT_SHOW)) {
        wchar_t cls[64] = L"";
        GetClassNameW(hwnd, cls, 64);
        if (wcscmp(cls, L"WorkerW") == 0) {   // the wallpaper layer is being replaced: re-sample soon
            Log(L"desktop: WorkerW %p %s", hwnd, ev == EVENT_OBJECT_CREATE ? L"created" : L"shown");
            if (!g_wallChangeAt) g_wallChangeAt = GetTickCount64();
            SetTimer(g_ctl, TIMER_WALLPAPER, 30, nullptr);
        }
    }
    static const wchar_t* names[] = { L"create", L"destroy", L"show", L"hide", L"reorder" };
    KeepInPlace(ev >= EVENT_OBJECT_CREATE && ev <= EVENT_OBJECT_REORDER ? names[ev - EVENT_OBJECT_CREATE] : L"?");
}

// Only the desktop window's own thread is watched: it is the one that creates the wallpaper
// WorkerW, while File Explorer windows and the taskbar (same process, other threads) generate
// a stream of unrelated events that would otherwise wake us.
void HookDesktop() {
    DWORD pid = 0;
    const DWORD tid = GetWindowThreadProcessId(g_clockParent, &pid);
    if (g_deskHook && tid == g_deskHookTid) return;
    if (g_deskHook) UnhookWinEvent(g_deskHook);
    g_deskHook = SetWinEventHook(EVENT_OBJECT_CREATE, EVENT_OBJECT_REORDER, nullptr, DesktopEvent, pid, tid,
                                 WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
    g_deskHookTid = tid;
    Log(L"desktop hook on explorer pid %lu thread %lu: %s", pid, tid, g_deskHook ? L"ok" : L"failed");
}

void DestroyClocks() {
    auto old = std::move(g_clocks);   // emptied first, so their WM_DESTROY doesn't ask to re-attach
    g_clocks.clear();
    for (auto& c : old) {
        if (!c->hwnd) continue;
        g_r.ReleaseTarget(c->hwnd);
        if (IsWindow(c->hwnd)) DestroyWindow(c->hwnd);
    }
}

bool Attach(const DesktopHost& h, std::vector<Wanted>& want) {
    DestroyClocks();
    g_clockParent = h.parent;
    g_insertAfter = h.insertAfter;
    std::wstring where;
    for (auto& w : want) {
        auto c = std::make_unique<Clock>();
        c->mon = w.mon;
        c->s = std::move(w.s);
        c->hwnd = CreateWindowExW(WS_EX_NOREDIRECTIONBITMAP | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW,
                                  kClockClass, L"Overlook", WS_CHILD | WS_DISABLED | WS_CLIPSIBLINGS,
                                  0, 0, 1, 1, h.parent, nullptr, g_inst, nullptr);
        if (!c->hwnd) { Log(L"CreateWindow failed %lu", GetLastError()); DestroyClocks(); return false; }
        where += (where.empty() ? L"" : L", ") + c->mon.device;
        g_clocks.push_back(std::move(c));
    }
    Log(L"attached %zu clock(s) (%s) to %p (%s layout), below %p", g_clocks.size(), where.c_str(), h.parent,
        h.modern ? L"24H2+" : L"legacy", h.insertAfter);
    HookDesktop();
    RenderAll();
    return true;
}

// Called whenever the desktop, the monitors or the settings may have changed: (re)creates the
// clock windows if needed and hands each clock its monitor and settings.
// Returns true if the windows were (re)created (and drawn).
bool EnsureAttached() {
    auto want = WantedClocks();
    DesktopHost h;
    const bool host = FindDesktopHost(h);
    bool same = host && h.parent == g_clockParent && want.size() == g_clocks.size();
    for (size_t i = 0; same && i < want.size(); ++i) {
        const Clock& c = *g_clocks[i];
        same = c.hwnd && IsWindow(c.hwnd) && GetAncestor(c.hwnd, GA_PARENT) == h.parent &&
               c.mon.device == want[i].mon.device && EqualRect(&c.mon.rc, &want[i].mon.rc);
    }
    if (same) {
        for (size_t i = 0; i < want.size(); ++i) {
            g_clocks[i]->mon = want[i].mon;   // e.g. a new DPI
            g_clocks[i]->s = std::move(want[i].s);
        }
        g_attachFailures = 0;
        return false;
    }
    if (host && Attach(h, want)) {
        g_attachFailures = 0;
        // Explorer sometimes rebuilds the desktop right after starting; double-check shortly after.
        SetTimer(g_ctl, TIMER_HEALTH, 5000, nullptr);
        return true;
    }
    ++g_attachFailures;
    SetTimer(g_ctl, TIMER_ATTACH, g_attachFailures < 30 ? 1000 : 10000, nullptr);
    return false;
}

// One checkbox per monitor of the Monitor= set, when there is more than one or one is hidden
// (so a hidden clock can always be brought back from here).
void AppendMonitorItems(HMENU m) {
    g_trayMonitors.clear();
    const auto all = WantedClocks(true);
    bool anyHidden = false;
    for (auto& w : all) anyHidden |= w.trayHidden || !w.s.show;
    if (all.size() < 2 && !anyHidden) return;
    for (size_t i = 0; i < all.size(); ++i) {
        const MonitorInfo& mon = all[i].mon;
        const bool inSettings = !all[i].s.show;   // Show=0 in a [Monitor ...] section: edited there, not here
        wchar_t label[200];
        swprintf_s(label, L"%s  (%ld\u00D7%ld)%s", mon.name.empty() ? mon.device.c_str() : mon.name.c_str(),
                   mon.rc.right - mon.rc.left, mon.rc.bottom - mon.rc.top, inSettings ? L"  \u2013 Show=0 in settings" : L"");
        g_trayMonitors.push_back({ all[i].id, all[i].trayHidden, inSettings });
        AppendMenuW(m, MF_STRING | (!all[i].trayHidden && !inSettings ? MF_CHECKED : 0) |
                    (g_s.hideClocks || inSettings ? MF_GRAYED : 0), ID_MONITOR + (UINT)i, label);
    }
}

void SetPaused(bool displayOff, bool locked) {
    const bool was = Paused();
    g_displayOff = displayOff;
    g_locked = locked;
    if (was == Paused()) return;
    Log(L"%s (display %s, session %s)", Paused() ? L"paused" : L"resumed", displayOff ? L"off" : L"on",
        locked ? L"locked" : L"unlocked");
    if (Paused()) { KillTimer(g_ctl, TIMER_MINUTE); return; }
    Refresh();   // catch up at once, no transition: the screen has just come back
    if (g_weatherDue) {
        g_weatherDue = false;
        if (!g_s.weatherCity.empty()) SetTimer(g_ctl, TIMER_WEATHER, kWeatherEveryMs, nullptr);
        FetchWeather();
    }
}

LRESULT CALLBACK ClockProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_NCHITTEST:    return HTTRANSPARENT;
    case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
    case WM_ERASEBKGND:   return 1;
    case WM_PAINT: { PAINTSTRUCT ps; BeginPaint(hwnd, &ps); EndPaint(hwnd, &ps); return 0; }   // content is a DComp visual
    case WM_TIMER:
        if (wp == kAnimTimer) {
            if (Clock* c = ClockFor(hwnd)) FinishAnimation(*c);
            else KillTimer(hwnd, kAnimTimer);
        }
        return 0;
    case WM_DESTROY:
        // Our parent belongs to Explorer; if Explorer exits, we're destroyed with it.
        if (Clock* c = ClockFor(hwnd)) {
            c->hwnd = nullptr;
            c->animating = false;
            g_r.ReleaseTarget(hwnd);
            if (g_ctl) PostMessageW(g_ctl, WM_APP_REATTACH, 0, 0);
        }
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// ------------------------------------------------------------------ settings

void ApplySettings() {
    const Settings old = g_s;
    g_s = LoadSettings();
    ++g_styleGen;   // fonts, sizes, shadow... may have changed: redraw even if the text didn't
    g_logEnabled = g_s.log;
    g_iniTime = IniWriteTime();
    if (g_s.startWithWindows != IsAutostartEnabled()) SetAutostart(g_s.startWithWindows);
    UpdateTray();
    UpdateMediaWatch();
    InvalidateWallpaperCache();
    for (auto& c : g_clocks) c->darkState = -1;   // BrightThreshold etc. may have changed: decide afresh
    // AirQuality newly on (anywhere): fetch now rather than at the next half hour.
    const bool airNow = !g_airWanted && AnyClockWants(&Settings::airQuality);
    if (g_s.weatherCity != old.weatherCity || g_s.weatherFahrenheit != old.weatherFahrenheit ||
        g_s.rainHours != old.rainHours || airNow) {
        g_weatherFailures = 0;
        if (!g_s.weatherCity.empty()) SetTimer(g_ctl, TIMER_WEATHER, kWeatherEveryMs, nullptr);
        FetchWeather();
    }
}

// animate: fade if what the clock says or its colour changed (e.g. an edited Color/Tint).
void Refresh(bool animate) {
    UpdateMediaWatch();   // monitors may have come or gone, with their [Monitor ...] NowPlaying
    if (!EnsureAttached()) RenderAll(animate);
    ScheduleMinute();
}

LRESULT CALLBACK CtlProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == g_msgTaskbarCreated && g_msgTaskbarCreated) {   // Explorer (re)started
        Log(L"TaskbarCreated");
        g_trayAdded = false;
        UpdateTray();
        EnsureAttached();
        return 0;
    }
    switch (msg) {
    case WM_TIMER:
        switch (wp) {
        case TIMER_MINUTE:
            if (!Attached()) EnsureAttached();
            RenderAll(true);
            ScheduleMinute();
            break;
        case TIMER_MEDIA:   // a paused track's linger time is up: fade the line out
            KillTimer(hwnd, TIMER_MEDIA);
            RenderAll(true);
            break;
        case TIMER_WEATHER:
            if (Paused()) {   // fetched when the displays come back; the timer is re-armed then
                g_weatherDue = true;
                KillTimer(hwnd, TIMER_WEATHER);
                break;
            }
            FetchWeather();
            break;
        case TIMER_WALLPAPER: {   // wallpaper changed: re-sample and fade to the new colour
            KillTimer(hwnd, TIMER_WALLPAPER);
            const ULONGLONG since = g_wallChangeAt ? GetTickCount64() - g_wallChangeAt : kWallpaperFadeMs;
            // Windows names the new image a moment after its new layer appears: look again shortly.
            if (g_wallChangeAt && since < 800 && !WallpaperChanged()) { SetTimer(hwnd, TIMER_WALLPAPER, 50, nullptr); break; }
            if (!g_wallChangeAt) WallpaperChanged();   // (just note what is shown now)
            g_wallChangeAt = 0;
            InvalidateWallpaperCache();
            for (auto& c : g_clocks) c->darkState = -1;   // a new wallpaper decides afresh
            if (since < kWallpaperFadeMs) g_fadeMs = std::max(250.0, (double)(kWallpaperFadeMs - since));
            RenderAll(true);
            g_fadeMs = 0;
            break;
        }
        case TIMER_RELOAD: {
            KillTimer(hwnd, TIMER_RELOAD);
            FILETIME ft = IniWriteTime();
            if (CompareFileTime(&ft, &g_iniTime) != 0) { ApplySettings(); Log(L"settings reloaded"); Refresh(true); }
            break;
        }
        case TIMER_REFRESH:
            KillTimer(hwnd, TIMER_REFRESH);
            Refresh();
            break;
        case TIMER_ATTACH:
            KillTimer(hwnd, TIMER_ATTACH);
            EnsureAttached();
            break;
        case TIMER_HEALTH:
            KillTimer(hwnd, TIMER_HEALTH);
            if (!Attached()) { Log(L"health check: re-attaching"); EnsureAttached(); }
            break;
        }
        return 0;

    case WM_APP_REATTACH:
        SetTimer(hwnd, TIMER_ATTACH, 500, nullptr);
        return 0;
    case WM_APP_RELOAD:
        ApplySettings();
        Refresh(true);
        return 0;

    case WM_APP_MEDIA:   // track, playback state or player changed (posted from the media watcher)
        RenderAll(true);
        ScheduleMediaLinger();
        return 0;
    case WM_APP_WEATHER: {   // a fetch finished (posted from the weather worker thread)
        if (wp) {
            WeatherReading w;
            if (WeatherLatest(g_s.weatherCity, w))
                Log(L"weather: %s  %.1f (H %.1f L %.1f) code %d %s, rain %d%%", w.place.c_str(), w.temp, w.high,
                    w.low, w.code, w.isDay ? L"day" : L"night", w.rainChance);
            g_weatherFailures = 0;
            SetTimer(hwnd, TIMER_WEATHER, kWeatherEveryMs, nullptr);
        } else {
            // Offline or a server hiccup: retry after 1, 2, 5, 10, then every 15 minutes. The last
            // reading stays on screen until it is kWeatherStaleSeconds old.
            static const UINT kRetryMin[] = { 1, 2, 5, 10, 15 };
            const UINT delay = kRetryMin[std::min(g_weatherFailures, 4)];
            ++g_weatherFailures;
            Log(L"weather: failed (%s), retrying in %u min", WeatherLastError().c_str(), delay);
            SetTimer(hwnd, TIMER_WEATHER, delay * 60 * 1000, nullptr);
        }
        RenderAll(true);   // fades in the new reading (or hides a stale one)
        return 0;
    }

    case WM_SETTINGCHANGE:
        if (wp == SPI_SETDESKWALLPAPER) { SetTimer(hwnd, TIMER_WALLPAPER, 1500, nullptr); return 0; }
        if (lp && lstrcmpiW(reinterpret_cast<LPCWSTR>(lp), L"ImmersiveColorSet") == 0) UpdateTray();
        SetTimer(hwnd, TIMER_REFRESH, 500, nullptr);
        return 0;
    case WM_DISPLAYCHANGE:
    case WM_DPICHANGED:
    case WM_TIMECHANGE:
        SetTimer(hwnd, TIMER_REFRESH, 500, nullptr);
        return 0;
    case WM_WTSSESSION_CHANGE:
        if (wp == WTS_SESSION_LOCK) SetPaused(g_displayOff, true);
        else if (wp == WTS_SESSION_UNLOCK) SetPaused(g_displayOff, false);
        return 0;
    case WM_POWERBROADCAST:
        if (wp == PBT_POWERSETTINGCHANGE) {
            const auto* p = reinterpret_cast<const POWERBROADCAST_SETTING*>(lp);
            if (p && p->PowerSetting == kConsoleDisplayState && p->DataLength >= sizeof(DWORD))
                SetPaused(*reinterpret_cast<const DWORD*>(p->Data) == 0, g_locked);
            return TRUE;
        }
        if (wp == PBT_APMRESUMEAUTOMATIC || wp == PBT_APMRESUMESUSPEND) {
            SetTimer(hwnd, TIMER_REFRESH, 1000, nullptr);
            if (!g_s.weatherCity.empty()) SetTimer(hwnd, TIMER_WEATHER, 5000, nullptr);   // let the network come back first
        }
        return TRUE;

    case WM_APP_TRAY:
        switch (LOWORD(lp)) {
        case WM_CONTEXTMENU:
        case NIN_SELECT:
        case NIN_KEYSELECT:
            ShowTrayMenu();
            break;
        }
        return 0;
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case ID_RELOAD: PostMessageW(hwnd, WM_APP_RELOAD, 0, 0); break;
        case ID_EDIT:   ShellExecuteW(nullptr, L"open", IniPath().c_str(), nullptr, nullptr, SW_SHOWNORMAL); break;
        case ID_FOLDER: ShellExecuteW(nullptr, L"open", ExeDir().c_str(), nullptr, nullptr, SW_SHOWNORMAL); break;
        case ID_AUTOSTART: {
            bool on = !IsAutostartEnabled();
            SetAutostart(on);
            SaveBool(L"StartWithWindows", on);
            g_s.startWithWindows = on;
            g_iniTime = IniWriteTime();
            break;
        }
        case ID_WEATHER_NOW:
            g_weatherFailures = 0;
            if (!g_s.weatherCity.empty()) SetTimer(hwnd, TIMER_WEATHER, kWeatherEveryMs, nullptr);
            FetchWeather();
            break;
        case ID_HIDE:
            SaveBool(L"HideClocks", !g_s.hideClocks);
            PostMessageW(hwnd, WM_APP_RELOAD, 0, 0);
            break;
        case ID_EXIT: DestroyWindow(hwnd); break;
        default:
            if (LOWORD(wp) >= ID_MONITOR && LOWORD(wp) < ID_MONITOR + g_trayMonitors.size()) {
                const TrayMonitor& tm = g_trayMonitors[LOWORD(wp) - ID_MONITOR];
                std::wstring list;
                for (auto& h : SplitList(g_s.hiddenMonitors))
                    if (_wcsicmp(h.c_str(), tm.id.c_str()) != 0) list += (list.empty() ? L"" : L";") + h;
                if (!tm.hidden) list += (list.empty() ? L"" : L";") + tm.id;
                SaveString(L"HiddenMonitors", list);
                PostMessageW(hwnd, WM_APP_RELOAD, 0, 0);
            }
            break;
        }
        return 0;

    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        if (g_deskHook) { UnhookWinEvent(g_deskHook); g_deskHook = nullptr; }
        if (g_powerNotify) { UnregisterPowerSettingNotification(g_powerNotify); g_powerNotify = nullptr; }
        WTSUnRegisterSessionNotification(hwnd);
        UpdateTray(true);
        MediaWatchStop();
        DestroyClocks();
        g_ctl = nullptr;
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

}  // namespace

// --preview <folder>: each monitor's clock drawn over its wallpaper, as set now and with
// macOS-like proportions (everything 15% larger, the date's capitals at 9.5% of the screen
// height, the weather and now-playing lines at SecondaryOpacity 70), as PNGs plus preview.txt
// with the settings each one used. A running clock is left alone.
// wallpaperOverride: an image to draw (filling each monitor) instead of the desktop's own (the
// text colour is still chosen from the desktop's wallpaper).
int RunPreview(std::wstring dir, const std::wstring& wallpaperOverride) {
    if (!dir.empty() && dir.back() != L'\\' && dir.back() != L'/') dir += L'\\';
    ClockRenderer r;   // local: released before CoUninitialize
    if (!r.Init()) return 1;
    g_s = LoadSettings();
    SYSTEMTIME t;
    GetLocalTime(&t);
    WeatherReading wr;
    const bool haveWeather = !g_s.weatherCity.empty() && WeatherLatest(g_s.weatherCity, wr);
    std::wstring media(1, icon::Music);
    media += L" Bonobo — Kerala";   // a sample track
    std::wstring report;
    int n = 0, rc = 0;
    for (const Wanted& w : WantedClocks()) {
        ++n;
        const MonitorInfo& mon = w.mon;
        const int monW = mon.rc.right - mon.rc.left, monH = mon.rc.bottom - mon.rc.top;
        std::wstring wall;
        int position = 4;
        COLORREF bg = 0;
        WallpaperFile(mon.rc, wall, position, bg);
        if (!wallpaperOverride.empty()) { wall = wallpaperOverride; position = 4; }
        for (int v = 0; v < 2; ++v) {
            Settings s = w.s;
            if (v == 1) {
                const double k = 1.15;
                s.timeSize *= k; s.dateSize *= k; s.gap *= k; s.weatherSize *= k; s.weatherGap *= k;
                s.secondaryOpacity = 70;
            }
            FrameLayout lay;
            if (!PrepareFitted(r, s, mon.dpi, FormatDate(t, s), FormatTime(t, s), haveWeather ? &wr : nullptr, NowUtc(),
                               g_s.weatherFahrenheit, lay, nullptr, nullptr, s.nowPlaying ? media : std::wstring())) {
                rc = 1;
                continue;
            }
            const ClockGeometry& g = lay.g;
            const double k = mon.dpi / 96.0;
            if (v == 1) s.verticalPos = (0.095 * monH + g.anchorY - g.ink.top) * 100.0 / monH;   // date capitals at 9.5%
            const int cx = (int)lround(monW / 2.0 + s.horizontalOffset * k), cy = (int)lround(monH * s.verticalPos / 100.0);
            const POINT pos{ cx - g.anchorX, cy - g.anchorY };   // px on the monitor
            RECT ink = g.ink;
            OffsetRect(&ink, mon.rc.left + pos.x, mon.rc.top + pos.y);
            int darkState = -1;
            COLORREF color = 0;
            bool dark = false;
            ChooseColour(s, mon, ink, darkState, color, dark);
            auto bmp = r.RenderFrame(lay, color, dark);
            wchar_t name[64], line[512];
            swprintf_s(name, L"preview-%d-%s.png", n, v ? L"proposed" : L"now");
            if (!bmp || !r.SaveScene(dir + name, { monW, monH }, wall, position, bg, { { bmp.Get(), pos } },
                                       (float)std::min(1.0, 1920.0 / monW))) rc = 1;
            swprintf_s(line, L"%s  %s (%dx%d, %u dpi): TimeSize=%g DateSize=%g Gap=%g WeatherSize=%g WeatherGap=%g "
                             L"SecondaryOpacity=%g VerticalPosition=%.1f; date capitals at %.1f%% of the height\r\n",
                       name, mon.name.empty() ? mon.device.c_str() : mon.name.c_str(), monW, monH, mon.dpi, s.timeSize,
                       s.dateSize, s.gap, s.weatherSize, s.weatherGap, s.secondaryOpacity, s.verticalPos,
                       (pos.y + g.ink.top) * 100.0 / monH);
            report += line;
        }
    }
    const std::string u8 = "\xEF\xBB\xBF" + [&] {
        const int len = WideCharToMultiByte(CP_UTF8, 0, report.c_str(), (int)report.size(), nullptr, 0, nullptr, nullptr);
        std::string out(len, '\0');
        WideCharToMultiByte(CP_UTF8, 0, report.c_str(), (int)report.size(), out.data(), len, nullptr, nullptr);
        return out;
    }();
    HANDLE h = CreateFileW((dir + L"preview.txt").c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h != INVALID_HANDLE_VALUE) { DWORD wr2 = 0; WriteFile(h, u8.data(), (DWORD)u8.size(), &wr2, nullptr); CloseHandle(h); }
    return n ? rc : 1;
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, LPWSTR cmdLine, int) {
    g_inst = inst;
    const bool exitCmd = wcsstr(cmdLine, L"--exit") != nullptr;
    if (const wchar_t* t = wcsstr(cmdLine, L"--weather-test")) {
        std::wstring dir = t + wcslen(L"--weather-test");
        while (!dir.empty() && (iswspace(dir.front()) || dir.front() == L'"')) dir.erase(dir.begin());
        while (!dir.empty() && (iswspace(dir.back()) || dir.back() == L'"')) dir.pop_back();
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        const int rc = RunWeatherTest(dir);
        CoUninitialize();
        return rc;
    }
    if (const wchar_t* t = wcsstr(cmdLine, L"--preview")) {
        std::wstring dir = t + wcslen(L"--preview"), wall;
        auto trim = [](std::wstring& v) {
            while (!v.empty() && (iswspace(v.front()) || v.front() == L'"')) v.erase(v.begin());
            while (!v.empty() && (iswspace(v.back()) || v.back() == L'"')) v.pop_back();
        };
        if (const size_t w = dir.find(L"--wallpaper"); w != std::wstring::npos) {   // draw over this image instead
            wall = dir.substr(w + wcslen(L"--wallpaper"));
            dir.erase(w);
            trim(wall);
        }
        trim(dir);
        if (dir.empty()) dir = ExeDir();
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        const int rc = RunPreview(dir, wall);
        CoUninitialize();
        return rc;
    }

    HANDLE mutex = CreateMutexW(nullptr, TRUE, kMutexName);
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        // Already running: --exit closes it, a plain launch reloads its settings.
        if (HWND other = FindWindowW(kCtlClass, nullptr))
            PostMessageW(other, exitCmd ? WM_CLOSE : WM_APP_RELOAD, 0, 0);
        return 0;
    }
    if (exitCmd) return 0;

    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    g_s = LoadSettings();
    g_logEnabled = g_s.log;
    g_iniTime = IniWriteTime();
    Log(L"---- start %s", ExePath().c_str());
    if (!g_r.Init()) {
        Log(L"renderer init failed");
        MessageBoxW(nullptr, L"Overlook could not initialise Direct2D/DirectWrite or load its fonts.", L"Overlook", MB_ICONERROR);
        return 1;
    }
    if (g_s.startWithWindows != IsAutostartEnabled()) SetAutostart(g_s.startWithWindows);

    WNDCLASSEXW wc{ sizeof(wc) };
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpfnWndProc = CtlProc;
    wc.lpszClassName = kCtlClass;
    RegisterClassExW(&wc);
    wc.lpfnWndProc = ClockProc;
    wc.lpszClassName = kClockClass;
    RegisterClassExW(&wc);

    // Hidden top-level window: receives TaskbarCreated, setting/display/power broadcasts and tray messages.
    g_ctl = CreateWindowExW(WS_EX_TOOLWINDOW, kCtlClass, L"Overlook", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, inst, nullptr);
    g_msgTaskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
    ChangeWindowMessageFilterEx(g_ctl, g_msgTaskbarCreated, MSGFLT_ALLOW, nullptr);
    // Display on/off (sends the current state right away) and lock/unlock: pause while unseen.
    g_powerNotify = RegisterPowerSettingNotification(g_ctl, &kConsoleDisplayState, DEVICE_NOTIFY_WINDOW_HANDLE);
    WTSRegisterSessionNotification(g_ctl, NOTIFY_FOR_THIS_SESSION);
    UpdateTray();
    Refresh();   // (starts the media watcher if wanted)
    if (!g_s.weatherCity.empty()) {   // a cached reading (if recent) is already shown; refresh it now
        SetTimer(g_ctl, TIMER_WEATHER, kWeatherEveryMs, nullptr);
        FetchWeather();
    }

    // Event-driven watches (no polling):
    //  - the exe folder, so saved .ini edits apply without restarting;
    //  - Explorer's Themes folder, rewritten whenever the wallpaper changes (slideshow
    //    changes are not always broadcast as WM_SETTINGCHANGE).
    wchar_t appData[MAX_PATH] = L"";
    GetEnvironmentVariableW(L"APPDATA", appData, MAX_PATH);
    const std::wstring themesDir = std::wstring(appData) + L"\\Microsoft\\Windows\\Themes";
    HANDLE watches[2] = {
        FindFirstChangeNotificationW(ExeDir().c_str(), FALSE, FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_FILE_NAME),
        FindFirstChangeNotificationW(themesDir.c_str(), FALSE, FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_FILE_NAME),
    };
    const UINT_PTR watchTimers[2] = { TIMER_RELOAD, TIMER_WALLPAPER };
    const UINT watchDelays[2] = { 300, 1500 };   // debounce: a wallpaper change writes several files
    HANDLE active[2]; int activeIdx[2]; DWORD nActive = 0;
    for (int i = 0; i < 2; ++i)
        if (watches[i] != INVALID_HANDLE_VALUE) { active[nActive] = watches[i]; activeIdx[nActive++] = i; }
    MSG m;
    for (;;) {
        DWORD r = MsgWaitForMultipleObjectsEx(nActive, active, INFINITE, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        if (r < WAIT_OBJECT_0 + nActive) {
            const int i = activeIdx[r - WAIT_OBJECT_0];
            FindNextChangeNotification(watches[i]);
            if (g_ctl) SetTimer(g_ctl, watchTimers[i], watchDelays[i], nullptr);
        }
        while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) {
            if (m.message == WM_QUIT) {
                for (HANDLE h : watches) if (h != INVALID_HANDLE_VALUE) FindCloseChangeNotification(h);
                CoUninitialize();
                ReleaseMutex(mutex);
                return (int)m.wParam;
            }
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
    }
}
