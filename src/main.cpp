// Overlook — a macOS lock-screen style clock that lives on the Windows desktop layer.
#include <windows.h>
#include <shellapi.h>
#include <wtsapi32.h>
#include <dwmapi.h>
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
#include "settingswindow.h"
#include "inifile.h"
#include "weatherfx.h"
#include "../res/resource.h"
#include "update.h"

namespace {

const wchar_t kCtlClass[]   = L"OverlookController";
const wchar_t kClockClass[] = L"OverlookClock";
const wchar_t kMutexName[]  = L"Local\\Overlook-7f3c2a1e";

enum : UINT_PTR { TIMER_MINUTE = 1, TIMER_RELOAD, TIMER_REFRESH, TIMER_ATTACH, TIMER_HEALTH, TIMER_WALLPAPER,
                  TIMER_WEATHER, TIMER_MEDIA, TIMER_COVER, TIMER_UPDATE };
enum : UINT { WM_APP_TRAY = WM_APP + 1, WM_APP_RELOAD, WM_APP_REATTACH, WM_APP_WEATHER, WM_APP_MEDIA, WM_APP_SETTINGS,
              WM_APP_UPDATE };
const UINT kWeatherEveryMs = 30 * 60 * 1000;
// Update check: the first look a short while after start, then an hourly look at whether a day has passed.
const UINT kUpdateFirstMs = 2 * 60 * 1000, kUpdateEveryMs = 60 * 60 * 1000;
enum : UINT { ID_RELOAD = 100, ID_EDIT, ID_SETTINGS, ID_FOLDER, ID_AUTOSTART, ID_EXIT, ID_HIDE, ID_WEATHER_NOW, ID_UPDATE,
              ID_MONITOR = 200 };

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
// In a Remote Desktop session the console displays are not what the user sees (they are usually
// off while someone is connected), so their state is ignored there.
bool Paused() { return (g_displayOff && !GetSystemMetrics(SM_REMOTESESSION)) || g_locked; }
bool g_pausedNow;   // Paused() when SetPaused last acted on it
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
    if (WeatherFetchAsync(g_ctl, WM_APP_WEATHER, r)) Log(L"weather: fetching for '%s'", Private(r.city).c_str());
}

void Refresh(bool animate = false);

// ------------------------------------------------------------------ now playing

bool g_mediaOn;   // the media watcher runs (some shown clock has NowPlaying=1)

void UpdateMediaWatch() {
    MediaSetApps(g_s.nowPlayingApps);
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

// ------------------------------------------------------------------ update check

// The settings window's "Check for updates" (manual: runs even with CheckForUpdates=0).
void CheckUpdatesNow() {
    if (UpdateCheckAsync(g_ctl, WM_APP_UPDATE, g_s.updateBeta)) Log(L"update: checking");
}

void ScheduleUpdates(UINT firstMs) {
    if (g_s.checkForUpdates) SetTimer(g_ctl, TIMER_UPDATE, firstMs, nullptr);
    else KillTimer(g_ctl, TIMER_UPDATE);
}

void OpenUrl(const std::wstring& url) {
    ShellExecuteW(nullptr, L"open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
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
    if (const UpdateStatus u = UpdateCurrent(); u.newer) {
        const std::wstring tag = u.latest.tag[0] == L'v' || u.latest.tag[0] == L'V' ? L"v" + u.latest.tag.substr(1) : L"v" + u.latest.tag;
        AppendMenuW(m, MF_STRING, ID_UPDATE, (L"Update available: " + tag).c_str());
    }
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING | (g_s.hideClocks ? MF_CHECKED : 0), ID_HIDE, L"Hide clocks");
    AppendMonitorItems(m);
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, ID_SETTINGS, L"Settings…");
    AppendMenuW(m, MF_STRING, ID_EDIT, L"Edit settings file…");
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
    std::wstring fxKey;       // weatherfx::Describe of the effect shown ("" = none built yet)
    std::wstring fxBlocked;   // why its effect is paused ("" = it may run), for the log
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

// The settings window's Monitors page and scopes: the Monitor= set, hidden ones too.
std::vector<SettingsMonitor> SettingsMonitors() {
    std::vector<SettingsMonitor> out;
    for (auto& w : WantedClocks(true)) out.push_back({ w.mon, w.id, w.trayHidden, !w.s.show });
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
// --demo: StartTransition hands its layers here instead of to the compositor.
struct Capture {
    std::vector<ClockRenderer::AnimLayer> layers;
    std::vector<ComPtr<ID2D1Bitmap1>> keep;
    double seconds = 0, alpha = 1;
};
Capture* g_capture;

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
    if (g_capture) {   // --demo: hand the layers over instead of animating them
        g_capture->layers = layers;
        g_capture->keep = keep;
        g_capture->seconds = FadeMs(c.s) / 1000.0;
        g_capture->alpha = to.lay.opacity / 100.0;
        return true;
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

// AdaptiveLegibility. On a busy wallpaper the average behind the clock can look fine while its
// brightest patches (under light text) or darkest ones (under dark text) come close to the text's
// own lightness, and parts of the lines vanish into them. How close they come sets the boost,
// 0..1: from a gap of kLegibleGapNone L* (no help needed) down to kLegibleGapFull (all of it).
constexpr double kLegibleGapNone = 35, kLegibleGapFull = 15;   // a smooth mid-tone sky (gap ~33) reads fine as it is

// How far (L*) the clashing tenth of the wallpaper stays from the text.
double LegibilityGap(const WallpaperSample& ws, COLORREF text, bool darkText) {
    auto lin = [](int v) { const double c = v / 255.0; return c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4); };
    const double textL = LStar(0.2126 * lin(GetRValue(text)) + 0.7152 * lin(GetGValue(text)) + 0.0722 * lin(GetBValue(text)));
    return darkText ? ws.lightLo - textL : textL - ws.lightHi;
}

double LegibilityNeed(const WallpaperSample& ws, COLORREF text, bool darkText) {
    const double gap = LegibilityGap(ws, text, darkText);
    const double t = std::clamp((kLegibleGapNone - gap) / (kLegibleGapNone - kLegibleGapFull), 0.0, 1.0);
    return std::round(t * t * (3 - 2 * t) * 10) / 10;   // in steps, so a pixel shift doesn't redraw every line
}

// Moves the frame's shadow and weather/now-playing opacity toward kLegible* by `need`; never weakens them.
void ApplyLegibility(const Settings& s, double need, FrameLayout& f) {
    f.legibility = s.adaptiveLegibility ? need : 0;
    if (f.legibility <= 0) return;
    const double k = f.legibility;
    auto toward = [k](double from, double to) { return from + (std::max(from, to) - from) * k; };
    f.shadowOpacity = toward(s.shadow ? s.shadowOpacity : 0, kLegibleShadowOpacity);
    f.shadowBlur = toward(s.shadow ? s.shadowBlur : kLegibleShadowBlur, kLegibleShadowBlur);
    f.shadow = true;
    f.secondaryAlpha = (float)(toward(s.secondaryOpacity, kLegibleSecondary) / 100.0);
}

// The need for the chosen text colour over `ws` (the wallpaper behind all the lines). Auto-contrast
// goes by the average, which on a busy wallpaper can sit right at its threshold; when the text needs
// help, the other colour may clash less (light text with a dark shadow over bright gaps in dark
// foliage, say), and is used instead when it clearly needs less.
double LegibleChoice(const Settings& s, const WallpaperSample& ws, COLORREF& color, bool& dark) {
    if (!s.adaptiveLegibility) return 0;
    if (LegibilityNeed(ws, color, dark) > 0 && s.autoContrast) {
        // Light text counts 10 L* better: its dark shadow covers more than dark text's faint light halo.
        const COLORREF alt = TintColor(dark ? s.color : s.darkColor, ws, s.tint / 100.0);
        auto score = [&](COLORREF c, bool d) { return LegibilityGap(ws, c, d) + (d ? 0 : 10); };
        if (score(alt, !dark) > score(color, dark) + 3) { color = alt; dark = !dark; }
    }
    return LegibilityNeed(ws, color, dark);
}

// The same for the frame placed with its bitmap's top-left at `pos` (screen px) on `mon`.
double LegibilityAt(const Settings& s, const MonitorInfo& mon, const FrameLayout& lay, POINT pos, COLORREF& color, bool& dark) {
    if (!s.adaptiveLegibility) return 0;
    RECT rc = lay.g.lines;
    OffsetRect(&rc, pos.x, pos.y);
    WallpaperSample ws;
    if (!SampleWallpaper(mon.rc, rc, ws)) return 0;
    const bool wasDark = dark;
    const double need = LegibleChoice(s, ws, color, dark);
    Log(L"legibility %s: L* %.0f..%.0f (mean %.0f) -> %.1f%s", mon.device.c_str(), ws.lightLo, ws.lightHi, ws.lightness, need,
        dark != wasDark ? (dark ? L", dark text instead" : L", light text instead") : L"");
    return need;
}

// Debug mode (settings window, Ctrl+Shift+D) says so under every clock, so screenshots and
// feedback make clear the effects may be simulated.
void ShowDebugBadge(const Clock& c) {
    if (!c.hwnd) return;
    const RECT& rc = c.cur.rc;
    const int gap = (int)lround(6 * c.mon.dpi / 96.0);
    g_r.ShowBadge(c.hwnd, g_s.debugMode ? L"DEBUG MODE" : L"", (rc.left + rc.right) / 2 - c.winRc.left,
                  c.cur.rc.top + c.cur.lay.g.lines.bottom + gap - c.winRc.top, (float)c.mon.dpi);
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
    ApplyLegibility(s, LegibilityAt(s, mon, f.lay, pos, f.color, f.dark), f.lay);
    if (c.darkState >= 0) c.darkState = f.dark ? 1 : 0;   // (the hysteresis follows a switch made for legibility)

    f.rc = { pos.x, pos.y, pos.x + g.width, pos.y + g.height };
    f.win = mon.rc;
    f.hwnd = c.hwnd;
    wchar_t col[64]; swprintf_s(col, L"|%06X|%.0f|%d|%u|%.2f", (unsigned)f.color, s.opacity, f.dark ? 1 : 0, g_styleGen, f.lay.legibility);
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
        ShowDebugBadge(c);
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
    ShowDebugBadge(c);
    Log(L"render %s %s on %s (%d dpi) at %d,%d %dx%d%s%s [%s] [%s]", date.c_str(), time.c_str(), mon.device.c_str(),
        mon.dpi, pos.x, pos.y, g.width, g.height, night ? L" night" : L"", anim ? L" (transition)" : L"",
        Private(weather).c_str(), Private(media).c_str());
}

// ------------------------------------------------------------------ weather effects

// The effect, how much of it (density: 1 = normal) and its variants (lightning, icy), from
// WeatherEffectsForce or the WMO code. Lightning also needs WeatherEffectsLightning.
void FxKindFor(const Settings& s, const WeatherReading* wr, weatherfx::Params& p) {
    using K = weatherfx::Kind;
    struct Choice { const wchar_t* force; int code; K kind; float density; bool lightning, icy; };
    // Every effect, by its WeatherEffectsForce name and by WMO code. Drizzle (51-57) is light,
    // rain (61-67) and showers (80-82) go light/moderate/heavy by their codes, freezing drizzle
    // and rain (56/57/66/67) are icy, thunderstorms (95-99) are heavy with lightning; snow
    // (71/73/75), snow grains (77, fine and light), snow showers (85/86); 1 mainly clear,
    // 2 partly cloudy, 3 overcast; fog and rime fog (45/48).
    static const Choice kChoices[] = {
        { L"clouds-light", 1, K::Clouds, 0.5f }, { L"clouds", 2, K::Clouds, 1.0f }, { L"clouds-heavy", 3, K::Clouds, 1.6f },
        { L"fog", 45, K::Fog, 1.0f }, { nullptr, 48, K::Fog, 1.3f },
        { nullptr, 51, K::Rain, 0.35f }, { nullptr, 53, K::Rain, 0.5f }, { nullptr, 55, K::Rain, 0.65f },
        { nullptr, 56, K::Rain, 0.4f, false, true }, { nullptr, 57, K::Rain, 0.65f, false, true },
        { L"rain-light", 61, K::Rain, 0.6f }, { L"rain", 63, K::Rain, 1.0f }, { L"rain-heavy", 65, K::Rain, 1.5f },
        { nullptr, 66, K::Rain, 0.6f, false, true }, { L"sleet", 67, K::Rain, 1.2f, false, true },
        { nullptr, 80, K::Rain, 0.7f }, { nullptr, 81, K::Rain, 1.1f }, { nullptr, 82, K::Rain, 1.6f },
        { L"thunder", 95, K::Rain, 1.5f, true }, { nullptr, 96, K::Rain, 1.6f, true }, { nullptr, 99, K::Rain, 1.6f, true },
        { L"snow-light", 71, K::Snow, 0.5f }, { L"snow", 73, K::Snow, 1.0f }, { L"snow-heavy", 75, K::Snow, 1.6f },
        { nullptr, 77, K::Snow, 0.4f }, { nullptr, 85, K::Snow, 0.7f }, { nullptr, 86, K::Snow, 1.4f },
    };
    const Choice* pick = nullptr;
    for (auto& ch : kChoices)
        if (ch.force && _wcsicmp(s.weatherEffectsForce.c_str(), ch.force) == 0) pick = &ch;
    if (!pick && wr)
        for (auto& ch : kChoices) if (ch.code == wr->code) pick = &ch;
    if (!pick) { p.kind = K::None; return; }
    p.kind = pick->kind;
    p.density = pick->density;
    p.lightning = pick->lightning && s.weatherEffectsLightning;
    p.icy = pick->icy;
}

// Effects are DComp animations: while one runs DWM recomposes every vblank, so they stop
// whenever nobody benefits. The power and coverage state below is only kept up to date while
// some clock wants effects (UpdateFxWatch); otherwise it stays at "may run" and costs nothing.
bool g_saverLegacy, g_saverNew;   // GUID_POWER_SAVING_STATUS (battery saver), GUID_ENERGY_SAVER_STATUS (24H2+)
bool g_onBattery, g_hasBattery;   // running on DC power; the PC has a system battery at all
bool g_allCovered;                // a D3D exclusive full-screen app or presentation mode
std::vector<std::wstring> g_coveredMons;   // devices with a maximised or full-screen window over the desktop
// GUID_POWER_SAVING_STATUS, GUID_ENERGY_SAVER_STATUS, GUID_ACDC_POWER_SOURCE (defined here: the SDK
// only declares them, like GUID_CONSOLE_DISPLAY_STATE above)
const GUID kPowerSavingStatus = { 0xe00958c0, 0xc213, 0x4ace, { 0xac, 0x77, 0xfe, 0xcc, 0xed, 0x2e, 0xee, 0xa5 } };
const GUID kEnergySaverStatus = { 0x550e8400, 0xe29b, 0x41d4, { 0xa7, 0x16, 0x44, 0x66, 0x55, 0x44, 0x00, 0x00 } };
const GUID kAcDcPowerSource   = { 0x5d3e9a59, 0xe9d5, 0x4b00, { 0xa6, 0xbd, 0xff, 0x34, 0xff, 0x51, 0x65, 0x48 } };

// nullptr when c's effect may run, else a short reason for the log. The one place that decides.
const wchar_t* FxBlockedReason(const Clock& c) {
    if (Paused()) return L"display off/locked";
    if (g_s.debugMode && g_s.debugIgnorePauses) return nullptr;   // debug menu: watch them behind a maximised window
    if (GetSystemMetrics(SM_REMOTESESSION) && !g_s.weatherEffectsOverRemote)
        return L"remote session";   // every frame would cross the network
    BOOL anim = TRUE;
    if (SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &anim, 0) && !anim) return L"animations off";
    if (g_saverLegacy || g_saverNew) return L"energy saver";
    if (g_onBattery && g_hasBattery && !g_s.weatherEffectsOnBattery) return L"on battery";
    if (g_allCovered || std::find(g_coveredMons.begin(), g_coveredMons.end(), c.mon.device) != g_coveredMons.end())
        return L"covered";
    return nullptr;
}

weatherfx::Params FxParamsFor(const Clock& c) {
    weatherfx::Params p;
    const Settings& s = c.s;
    if (!s.weatherEffects || FxBlockedReason(c)) return p;
    WeatherReading wr;
    bool have = !g_s.weatherCity.empty() && WeatherLatest(g_s.weatherCity, wr);
    if (have) {   // a reading older than 2 hours no longer says what the sky is doing
        FILETIME now; GetSystemTimeAsFileTime(&now);
        const ULONGLONG n = ((ULONGLONG)now.dwHighDateTime << 32) | now.dwLowDateTime,
                        f = ((ULONGLONG)wr.fetchedUtc.dwHighDateTime << 32) | wr.fetchedUtc.dwLowDateTime;
        have = n - f < 2ull * 3600 * 10000000;
    }
    // Debug menu (DebugMode=1 only): a simulated WMO code replaces the reading's and counts as a
    // fresh reading, so it goes through the real mapping below (WeatherEffectsForce still wins).
    const bool dbg = g_s.debugMode;
    if (dbg && g_s.debugWeatherCode >= 0) {
        if (!have) { wr = WeatherReading(); wr.valid = true; }   // no city or a stale one: no real wind or night
        wr.code = g_s.debugWeatherCode;
        have = true;
    }
    FxKindFor(s, have ? &wr : nullptr, p);
    if (p.kind == weatherfx::Kind::None) return p;
    p.width = c.mon.rc.right - c.mon.rc.left;
    p.height = c.mon.rc.bottom - c.mon.rc.top;
    p.dpi = (float)c.mon.dpi;
    p.opacity = (float)(s.weatherEffectsOpacity / 100.0);
    p.isDay = have ? wr.isDay : true;
    // Light shapes on a dark wallpaper, mid-grey ones on a bright one (overcast, not a storm:
    // the darker tone is for rain and thunder), tinted like the text.
    WallpaperSample ws;
    const RECT top{ c.mon.rc.left, c.mon.rc.top, c.mon.rc.right, c.mon.rc.top + p.height / 2 };
    p.color = RGB(0xF2, 0xF5, 0xFA);
    if (SampleWallpaper(c.mon.rc, top, ws))
        p.color = TintColor(ws.lightness > 60 ? RGB(0x6B, 0x72, 0x80) : p.color, ws, 0.35);
    if (have && wr.hasWind) {   // the wind blows FROM windDir (0 = north): rain drifts the other way
        const double kmh = wr.fahrenheit ? wr.windSpeed * 1.609 : wr.windSpeed;
        p.windVx = (float)std::clamp(-std::sin(wr.windDir * 3.14159265 / 180) * kmh * 6.0, -400.0, 400.0);
        p.windVx = std::round(p.windVx / 20) * 20;   // in steps, so a small change doesn't rebuild the scene
    }
    if (dbg) {
        if (g_s.debugNight) p.isDay = false;
        if (g_s.debugWindOn) {   // km/h toward the right: the same conversion and steps as above
            p.windVx = (float)std::clamp(g_s.debugWind * 6.0, -400.0, 400.0);
            p.windVx = std::round(p.windVx / 20) * 20;
        }
        if (g_s.debugDensityOn) p.density *= (float)(g_s.debugDensity / 100.0);
    }
    return p;
}

}  // namespace

// The debug menu's "Copy report": what the effects are doing on this PC, as plain text for a
// GitHub issue. No city, wallpaper, track or path in it (settingsui.h declares it).
std::wstring EffectsReport() {
    std::wstring out;
    wchar_t line[512];
    swprintf_s(line, L"Overlook %hs, weather effects report\r\n", OVERLOOK_VERSION_STR);
    out += line;
    RTL_OSVERSIONINFOW v{ sizeof(v) };
    using RtlGetVersionFn = LONG(WINAPI*)(RTL_OSVERSIONINFOW*);
    if (auto get = (RtlGetVersionFn)(void*)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion")) get(&v);
    DWORD ubr = 0, cb = sizeof(ubr);
    RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", L"UBR", RRF_RT_REG_DWORD,
                 nullptr, &ubr, &cb);
    swprintf_s(line, L"Windows %lu.%lu.%lu.%lu\r\n", v.dwMajorVersion, v.dwMinorVersion, v.dwBuildNumber, ubr);
    out += line;
    out += L"\r\nMonitors\r\n";
    for (const MonitorInfo& m : EnumMonitors()) {
        DEVMODEW dm{}; dm.dmSize = sizeof(dm);
        const DWORD hz = EnumDisplaySettingsW(m.device.c_str(), ENUM_CURRENT_SETTINGS, &dm) ? dm.dmDisplayFrequency : 0;
        swprintf_s(line, L"  %s: %ldx%ld, %u %%, %lu Hz%s\r\n", m.device.c_str(), m.rc.right - m.rc.left,
                   m.rc.bottom - m.rc.top, m.dpi * 100 / 96, hz, m.primary ? L", primary" : L"");
        out += line;
    }
    out += L"\r\nClocks\r\n";
    if (g_clocks.empty()) out += L"  none shown\r\n";
    for (auto& c : g_clocks) {
        const std::wstring showing = c->fxKey.empty() ? L"none" : c->fxKey;
        swprintf_s(line, L"  %s: effects %s, showing %s%s%s\r\n", c->mon.device.c_str(),
                   c->s.weatherEffects ? L"on" : L"off", showing.c_str(), c->fxBlocked.empty() ? L"" : L", paused: ",
                   c->fxBlocked.c_str());
        out += line;
    }
    // The real reading: only its code and day/night (the place and temperatures stay out).
    WeatherReading wr;
    bool fresh = !g_s.weatherCity.empty() && WeatherLatest(g_s.weatherCity, wr);
    if (fresh) {
        FILETIME now; GetSystemTimeAsFileTime(&now);
        const ULONGLONG n = ((ULONGLONG)now.dwHighDateTime << 32) | now.dwLowDateTime,
                        f = ((ULONGLONG)wr.fetchedUtc.dwHighDateTime << 32) | wr.fetchedUtc.dwLowDateTime;
        fresh = n - f < 2ull * 3600 * 10000000;
    }
    if (g_s.weatherCity.empty()) swprintf_s(line, L"\r\nWeather: no city set\r\n");
    else if (fresh) swprintf_s(line, L"\r\nWeather: code %d, %s%s\r\n", wr.code, wr.isDay ? L"day" : L"night",
                               wr.hasWind ? L", wind reported" : L"");
    else swprintf_s(line, L"\r\nWeather: no reading from the last 2 hours\r\n");
    out += line;
    swprintf_s(line, L"Show (WeatherEffectsForce): %s\r\n", g_s.weatherEffectsForce.empty() ? L"the real weather"
                                                                                        : g_s.weatherEffectsForce.c_str());
    out += line;
    out += L"\r\nDebug settings\r\n";
    if (!g_s.debugMode) out += L"  debug mode off\r\n";
    else {
        wchar_t code[16] = L"real weather";
        if (g_s.debugWeatherCode >= 0) swprintf_s(code, L"%d", g_s.debugWeatherCode);
        wchar_t wind[32] = L"real", amount[32] = L"normal";
        if (g_s.debugWindOn) swprintf_s(wind, L"%+.0f km/h", g_s.debugWind);
        if (g_s.debugDensityOn) swprintf_s(amount, L"%.0f %%", g_s.debugDensity);
        swprintf_s(line, L"  effects on every monitor: %s\r\n  simulated weather: %s\r\n  wind: %s\r\n  amount: %s\r\n"
                         L"  night: %s\r\n  ignore pauses: %s\r\n",
                   g_s.debugEffectsEverywhere ? L"on" : L"off", code, wind, amount, g_s.debugNight ? L"on" : L"off",
                   g_s.debugIgnorePauses ? L"on" : L"off");
        out += line;
    }
    swprintf_s(line, L"Effects on battery: %s\r\n", g_s.weatherEffectsOnBattery ? L"on" : L"off");
    out += line;
    swprintf_s(line, L"Effects over Remote Desktop: %s\r\n", g_s.weatherEffectsOverRemote ? L"on" : L"off");
    out += line;
    return out;
}

namespace {

// Builds, swaps or removes each clock's effect when what it should show has changed.
void UpdateEffects() {
    bool changed = false;
    for (auto& c : g_clocks) {
        if (!c->hwnd) continue;
        const wchar_t* why = c->s.weatherEffects ? FxBlockedReason(*c) : nullptr;
        if (c->fxBlocked != (why ? why : L"")) {
            if (why) Log(L"effects %s: paused (%s)", c->mon.device.c_str(), why);
            else Log(L"effects %s: resumed (was %s)", c->mon.device.c_str(), c->fxBlocked.c_str());
            c->fxBlocked = why ? why : L"";
        }
        const weatherfx::Params p = FxParamsFor(*c);
        // (Describe leaves out day/night: added here so a change at dusk, or Pretend it's night, rebuilds)
        const std::wstring key = p.kind == weatherfx::Kind::None ? L"none" : weatherfx::Describe(p) + (p.isDay ? L"" : L" night");
        if (key == c->fxKey) continue;
        if (!why) Log(L"effects %s: %s", c->mon.device.c_str(), key.c_str());
        if (p.kind == weatherfx::Kind::None) weatherfx::Clear(g_r, c->hwnd);
        else if (!weatherfx::Show(g_r, c->hwnd, p)) Log(L"effects %s: failed", c->mon.device.c_str());
        c->fxKey = key;
        changed = true;
    }
    // Drawing the tiles takes pages the effect no longer needs (DWM holds the surfaces): hand
    // them back, as RenderAll does after a frame, also when a pause ends outside RenderAll.
    if (changed && !AnyAnimating()) TrimMemory();
}

void RenderAll(bool animate = false) {
    if (g_clocks.empty() || Paused()) return;   // drawn when the displays come back
    RefreshSlot();
    for (size_t i = 0; i < g_clocks.size(); ++i) Render(i, animate);
    UpdateEffects();
    if (!AnyAnimating()) TrimMemory();   // idle until the next minute: hand the pages back
}

// ------------------------------------------------------------------ when effects pause

// Coverage: a maximised or full-screen window of another app over a monitor hides its desktop.
// Watched with out-of-context window events (no DLL injection) only while some clock wants
// effects: foreground changes and minimise/restore system-wide, but moves and resizes only on the
// foreground window's thread, since location changes everywhere (the mouse cursor, carets, every
// other app's windows) would wake us all the time. A burst (a drag) is debounced.
bool g_fxWatching;
HWINEVENTHOOK g_fxFgHook, g_fxMinHook, g_fxLocHook;
DWORD g_fxLocTid;
HPOWERNOTIFY g_fxPower[3];
const UINT kCoverDebounceMs = 250;

// w is a visible, maximised or full-screen window of another app: device = the monitor it hides.
bool CoversMonitor(HWND w, std::wstring& device) {
    if (!IsWindowVisible(w) || IsIconic(w)) return false;
    DWORD pid = 0;
    GetWindowThreadProcessId(w, &pid);
    if (pid == GetCurrentProcessId()) return false;
    // Click-through overlays (game bars, GPU overlays) and tool windows don't hide anything.
    if (GetWindowLongPtrW(w, GWL_EXSTYLE) & (WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW)) return false;
    wchar_t cls[64] = L"";
    GetClassNameW(w, cls, 64);
    if (wcscmp(cls, L"Shell_TrayWnd") == 0 || wcscmp(cls, L"Shell_SecondaryTrayWnd") == 0) return false;
    DWORD cloaked = 0;   // UWP frames of suspended apps, windows on other virtual desktops
    if (SUCCEEDED(DwmGetWindowAttribute(w, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) && cloaked) return false;
    const HMONITOR m = MonitorFromWindow(w, MONITOR_DEFAULTTONULL);
    MONITORINFOEXW mi{};
    mi.cbSize = sizeof(mi);
    RECT r;
    if (!m || !GetMonitorInfoW(m, &mi) || !GetWindowRect(w, &r)) return false;
    auto contains = [&](const RECT& in) {
        return r.left <= in.left && r.top <= in.top && r.right >= in.right && r.bottom >= in.bottom;
    };
    if (!contains(mi.rcWork) && !contains(mi.rcMonitor)) return false;
    device = mi.szDevice;
    return true;
}

BOOL CALLBACK CoverEnum(HWND w, LPARAM lp) {
    // Top to bottom. The desktop (Progman, or the WorkerW holding the icons, which Win+D raises
    // above everything) ends the search: what is below it doesn't hide it.
    wchar_t cls[64] = L"";
    GetClassNameW(w, cls, 64);
    if (IsWindowVisible(w) && (wcscmp(cls, L"Progman") == 0 ||
        (wcscmp(cls, L"WorkerW") == 0 && FindWindowExW(w, nullptr, L"SHELLDLL_DefView", nullptr))))
        return FALSE;
    auto& out = *reinterpret_cast<std::vector<std::wstring>*>(lp);
    std::wstring dev;
    if (CoversMonitor(w, dev) && std::find(out.begin(), out.end(), dev) == out.end()) {
        if (g_logEnabled) {   // which window: a wrong "covered" is otherwise impossible to diagnose
            DWORD pid = 0;
            GetWindowThreadProcessId(w, &pid);
            wchar_t exe[MAX_PATH] = L"?";
            DWORD n = MAX_PATH;
            if (HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)) {
                if (!QueryFullProcessImageNameW(h, 0, exe, &n)) wcscpy_s(exe, L"?");
                CloseHandle(h);
            }
            const wchar_t* base = wcsrchr(exe, L'\\');
            RECT r{}; GetWindowRect(w, &r);
            Log(L"cover: %s by %s (%s) %ld,%ld %ldx%ld", dev.c_str(), cls, base ? base + 1 : exe,
                r.left, r.top, r.right - r.left, r.bottom - r.top);
        }
        out.push_back(std::move(dev));
    }
    return TRUE;
}

// Re-reads which monitors are covered; true if that changed.
bool UpdateCoverage() {
    QUERY_USER_NOTIFICATION_STATE q{};
    const bool all = SUCCEEDED(SHQueryUserNotificationState(&q)) &&
                     (q == QUNS_RUNNING_D3D_FULL_SCREEN || q == QUNS_PRESENTATION_MODE);
    std::vector<std::wstring> mons;
    if (!all) EnumWindows(CoverEnum, reinterpret_cast<LPARAM>(&mons));
    std::sort(mons.begin(), mons.end());
    if (all == g_allCovered && mons == g_coveredMons) return false;
    g_allCovered = all;
    g_coveredMons = std::move(mons);
    return true;
}

void CALLBACK FxWindowEvent(HWINEVENTHOOK, DWORD ev, HWND hwnd, LONG idObject, LONG idChild, DWORD, DWORD);

// Moves the location hook to the foreground window's thread (a maximise, F11, a drag).
void WatchForeground(HWND fg) {
    DWORD pid = 0;
    const DWORD tid = fg ? GetWindowThreadProcessId(fg, &pid) : 0;
    if (g_fxLocHook && tid == g_fxLocTid) return;
    if (g_fxLocHook) UnhookWinEvent(g_fxLocHook);
    g_fxLocHook = nullptr;
    g_fxLocTid = 0;
    if (!tid || pid == GetCurrentProcessId()) return;
    g_fxLocHook = SetWinEventHook(EVENT_OBJECT_LOCATIONCHANGE, EVENT_OBJECT_LOCATIONCHANGE, nullptr, FxWindowEvent,
                                  pid, tid, WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
    g_fxLocTid = tid;
}

void CALLBACK FxWindowEvent(HWINEVENTHOOK, DWORD ev, HWND hwnd, LONG idObject, LONG idChild, DWORD, DWORD) {
    if (idObject != OBJID_WINDOW || idChild != CHILDID_SELF || !hwnd || !g_ctl) return;
    if (ev == EVENT_OBJECT_LOCATIONCHANGE && GetAncestor(hwnd, GA_ROOT) != hwnd) return;   // top-level windows only
    if (ev == EVENT_SYSTEM_FOREGROUND) WatchForeground(hwnd);
    SetTimer(g_ctl, TIMER_COVER, kCoverDebounceMs, nullptr);   // each event of a burst pushes it back
}

void ReadPowerStatus() {
    SYSTEM_POWER_STATUS ps{};
    if (!GetSystemPowerStatus(&ps)) return;
    g_hasBattery = ps.BatteryFlag != 128 && ps.BatteryFlag != 255;   // 128 no system battery, 255 unknown
    g_onBattery = ps.ACLineStatus == 0;
}

// Starts or stops watching windows and power as some shown clock wants effects, or none does.
void StopFxWatch() {
    if (!g_fxWatching) return;
    g_fxWatching = false;
    for (HWINEVENTHOOK* h : { &g_fxFgHook, &g_fxMinHook, &g_fxLocHook })
        if (*h) { UnhookWinEvent(*h); *h = nullptr; }
    g_fxLocTid = 0;
    for (HPOWERNOTIFY& n : g_fxPower)
        if (n) { UnregisterPowerSettingNotification(n); n = nullptr; }
    if (g_ctl) KillTimer(g_ctl, TIMER_COVER);
    g_saverLegacy = g_saverNew = g_onBattery = g_allCovered = false;   // unknown again: nothing blocks
    g_coveredMons.clear();
    Log(L"effects: stopped watching");
}

void UpdateFxWatch() {
    const bool want = AnyClockWants(&Settings::weatherEffects);
    if (!want) { StopFxWatch(); return; }
    if (g_fxWatching) {
        UpdateCoverage();   // monitors may have come, gone or moved (RenderAll follows in Refresh)
        return;
    }
    g_fxWatching = true;
    const DWORD flags = WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS;
    g_fxFgHook = SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND, nullptr, FxWindowEvent,
                                 0, 0, flags);
    g_fxMinHook = SetWinEventHook(EVENT_SYSTEM_MINIMIZESTART, EVENT_SYSTEM_MINIMIZEEND, nullptr, FxWindowEvent,
                                  0, 0, flags);
    WatchForeground(GetForegroundWindow());
    // Read the state now, so effects don't start only to stop when the first notifications
    // (sent on registering) arrive. Before 24H2 the energy-saver registration has nothing to report.
    SYSTEM_POWER_STATUS ps{};
    g_saverLegacy = GetSystemPowerStatus(&ps) && ps.SystemStatusFlag == 1;   // battery saver on
    ReadPowerStatus();
    const GUID* guids[3] = { &kPowerSavingStatus, &kEnergySaverStatus, &kAcDcPowerSource };
    for (int i = 0; i < 3; ++i)
        g_fxPower[i] = RegisterPowerSettingNotification(g_ctl, guids[i], DEVICE_NOTIFY_WINDOW_HANDLE);
    UpdateCoverage();
    Log(L"effects: watching windows and power (hooks %s, battery %s)", g_fxFgHook && g_fxMinHook ? L"ok" : L"failed",
        g_hasBattery ? L"yes" : L"none");
}

// A power setting the effects depend on (WM_POWERBROADCAST); false if it isn't one.
bool FxPowerChanged(const POWERBROADCAST_SETTING& p) {
    if (p.DataLength < sizeof(DWORD)) return false;
    const DWORD v = *reinterpret_cast<const DWORD*>(p.Data);
    if (p.PowerSetting == kPowerSavingStatus) g_saverLegacy = v != 0;
    else if (p.PowerSetting == kEnergySaverStatus) g_saverNew = v != 0;   // ENERGY_SAVER_STANDARD or _HIGH_SAVINGS
    else if (p.PowerSetting == kAcDcPowerSource) { ReadPowerStatus(); g_onBattery = v != 0; }   // 1 battery, 2 UPS
    else return false;
    return true;
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

// The first lines of a log: what a bug report needs to know about this PC. The exe's folder is
// shown relative to the user's profile, so the account name stays out of the file.
void LogHeader() {
    if (!g_logEnabled) return;
    std::wstring exe = ExePath();
    wchar_t profile[MAX_PATH] = L"";
    const DWORD pl = GetEnvironmentVariableW(L"USERPROFILE", profile, MAX_PATH);
    if (pl && pl < MAX_PATH && _wcsnicmp(exe.c_str(), profile, pl) == 0) exe = L"%USERPROFILE%" + exe.substr(pl);
    Log(L"---- start Overlook %hs, %s", OVERLOOK_VERSION_STR, exe.c_str());
    RTL_OSVERSIONINFOW v{ sizeof(v) };
    using RtlGetVersionFn = LONG(WINAPI*)(RTL_OSVERSIONINFOW*);
    if (auto get = (RtlGetVersionFn)(void*)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion")) get(&v);
    DWORD ubr = 0, cb = sizeof(ubr);
    RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", L"UBR", RRF_RT_REG_DWORD,
                 nullptr, &ubr, &cb);
    Log(L"windows %lu.%lu.%lu.%lu", v.dwMajorVersion, v.dwMinorVersion, v.dwBuildNumber, ubr);
    for (const MonitorInfo& m : EnumMonitors()) {
        DEVMODEW dm{}; dm.dmSize = sizeof(dm);
        const DWORD hz = EnumDisplaySettingsW(m.device.c_str(), ENUM_CURRENT_SETTINGS, &dm) ? dm.dmDisplayFrequency : 0;
        Log(L"monitor %s \"%s\": %ldx%ld at %ld,%ld, %u dpi (%u%%), %lu Hz%s", m.device.c_str(), m.name.c_str(),
            m.rc.right - m.rc.left, m.rc.bottom - m.rc.top, m.rc.left, m.rc.top, m.dpi, m.dpi * 100 / 96, hz,
            m.primary ? L", primary" : L"");
    }
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
    g_displayOff = displayOff;
    g_locked = locked;
    if (g_pausedNow == Paused()) return;
    g_pausedNow = Paused();
    Log(L"%s (display %s, session %s%s)", Paused() ? L"paused" : L"resumed", displayOff ? L"off" : L"on",
        locked ? L"locked" : L"unlocked", GetSystemMetrics(SM_REMOTESESSION) ? L", remote" : L"");
    if (Paused()) {
        KillTimer(g_ctl, TIMER_MINUTE);
        UpdateEffects();   // nothing animates while nobody can see it: DWM can go idle
        return;
    }
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
    g_logPrivate = g_s.logPrivate;
    if (g_s.log && !old.log) LogHeader();   // logging just switched on: say what this is running on
    if (g_s.debugMode != old.debugMode) Log(L"debug mode %s", g_s.debugMode ? L"on" : L"off");
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
    if (g_s.checkForUpdates != old.checkForUpdates || g_s.updateBeta != old.updateBeta) {
        UpdateInit(g_s.checkForUpdates);
        ScheduleUpdates(10 * 1000);   // switched on, or a new channel: look soon
    }
    SettingsWindowReloaded();
}

// animate: fade if what the clock says or its colour changed (e.g. an edited Color/Tint).
void Refresh(bool animate) {
    UpdateMediaWatch();   // monitors may have come or gone, with their [Monitor ...] NowPlaying
    UpdateFxWatch();      // ... and their WeatherEffects
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
        case TIMER_UPDATE:   // hourly: check if a day has passed (not while nothing is visible)
            ScheduleUpdates(kUpdateEveryMs);
            if (g_s.checkForUpdates && !Paused() && UpdateDue(g_s.updateBeta)) CheckUpdatesNow();
            break;
        case TIMER_HEALTH:
            KillTimer(hwnd, TIMER_HEALTH);
            if (!Attached()) { Log(L"health check: re-attaching"); EnsureAttached(); }
            break;
        case TIMER_COVER:   // windows settled after a foreground change, minimise/restore or move
            KillTimer(hwnd, TIMER_COVER);
            if (UpdateCoverage()) UpdateEffects();
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
    case WM_APP_SETTINGS: {
        SettingsHost host;
        host.ctl = hwnd;
        host.reloadMsg = WM_APP_RELOAD;
        host.monitors = SettingsMonitors;
        host.checkUpdates = CheckUpdatesNow;
        OpenSettingsWindow(g_inst, host);
        return 0;
    }

    case WM_APP_UPDATE:   // an update check finished (posted from its worker thread)
        if (g_trayAdded && UpdateTakeNotify()) {   // once per newer version
            const UpdateStatus u = UpdateCurrent();
            NOTIFYICONDATAW nid{ sizeof(nid) };
            nid.hWnd = hwnd;
            nid.uID = 1;
            nid.uFlags = NIF_INFO;
            nid.dwInfoFlags = NIIF_USER | NIIF_LARGE_ICON | NIIF_RESPECT_QUIET_TIME;
            nid.hBalloonIcon = g_trayIcon;
            swprintf_s(nid.szInfoTitle, L"%s", UpdateStatusText(u).c_str());
            wcscpy_s(nid.szInfo, L"Get it from the Overlook icon's menu, or from Settings > About.");
            Shell_NotifyIconW(NIM_MODIFY, &nid);
        }
        SettingsWindowReloaded();
        return 0;
    case WM_APP_MEDIA:   // track, playback state or player changed (posted from the media watcher)
        RenderAll(true);
        ScheduleMediaLinger();
        return 0;
    case WM_APP_WEATHER: {   // a fetch finished (posted from the weather worker thread)
        if (wp) {
            WeatherReading w;
            if (WeatherLatest(g_s.weatherCity, w))
                Log(L"weather: %s  %.1f (H %.1f L %.1f) code %d %s, rain %d%%", Private(w.place).c_str(), w.temp, w.high,
                    w.low, w.code, w.isDay ? L"day" : L"night", w.rainChance);
            g_weatherFailures = 0;
            SetTimer(hwnd, TIMER_WEATHER, kWeatherEveryMs, nullptr);
        } else {
            // Offline or a server hiccup: retry after 1, 2, 5, 10, then every 15 minutes. The last
            // reading stays on screen until it is kWeatherStaleSeconds old.
            static const UINT kRetryMin[] = { 1, 2, 5, 10, 15 };
            const UINT delay = kRetryMin[std::min(g_weatherFailures, 4)];
            ++g_weatherFailures;
            Log(L"weather: failed (%s), retrying in %u min", PrivateIn(WeatherLastError(), g_s.weatherCity).c_str(), delay);
            SetTimer(hwnd, TIMER_WEATHER, delay * 60 * 1000, nullptr);
        }
        RenderAll(true);   // fades in the new reading (or hides a stale one)
        return 0;
    }

    case WM_SETTINGCHANGE:
        if (wp == SPI_SETDESKWALLPAPER) { SetTimer(hwnd, TIMER_WALLPAPER, 1500, nullptr); return 0; }
        if (lp && lstrcmpiW(reinterpret_cast<LPCWSTR>(lp), L"ImmersiveColorSet") == 0) UpdateTray();
        SetTimer(hwnd, TIMER_REFRESH, 500, nullptr);   // (also re-reads "Animation effects" for the weather effects)
        return 0;
    case WM_DISPLAYCHANGE:
    case WM_DPICHANGED:
    case WM_TIMECHANGE:
        SetTimer(hwnd, TIMER_REFRESH, 500, nullptr);
        return 0;
    case WM_WTSSESSION_CHANGE:
        if (wp == WTS_SESSION_LOCK) SetPaused(g_displayOff, true);
        else if (wp == WTS_SESSION_UNLOCK) SetPaused(g_displayOff, false);
        else if (wp >= WTS_CONSOLE_CONNECT && wp <= WTS_REMOTE_DISCONNECT) {   // remote or not now
            SetPaused(g_displayOff, g_locked);   // the console display state counts only when local
            UpdateEffects();
        }
        return 0;
    case WM_POWERBROADCAST:
        if (wp == PBT_POWERSETTINGCHANGE) {
            const auto* p = reinterpret_cast<const POWERBROADCAST_SETTING*>(lp);
            if (p && p->PowerSetting == kConsoleDisplayState && p->DataLength >= sizeof(DWORD))
                SetPaused(*reinterpret_cast<const DWORD*>(p->Data) == 0, g_locked);
            else if (p && FxPowerChanged(*p)) UpdateEffects();
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
        case NIN_BALLOONUSERCLICK:   // the update notification: show what's new
            OpenUrl(UpdatePageUrl(UpdateCurrent()));
            break;
        }
        return 0;
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case ID_RELOAD: PostMessageW(hwnd, WM_APP_RELOAD, 0, 0); break;
        case ID_SETTINGS: PostMessageW(hwnd, WM_APP_SETTINGS, 0, 0); break;
        case ID_EDIT:   ShellExecuteW(nullptr, L"open", IniPath().c_str(), nullptr, nullptr, SW_SHOWNORMAL); break;
        case ID_FOLDER: ShellExecuteW(nullptr, L"open", DataDir().c_str(), nullptr, nullptr, SW_SHOWNORMAL); break;
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
        case ID_UPDATE: OpenUrl(UpdateDownloadUrl(UpdateCurrent())); break;
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
        StopFxWatch();
        WTSUnRegisterSessionNotification(hwnd);
        CloseSettingsWindow();
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
            int darkState = -1;
            COLORREF color = s.color;
            bool dark = false;
            WallpaperSample spread{};
            if (wallpaperOverride.empty()) {
                OffsetRect(&ink, mon.rc.left + pos.x, mon.rc.top + pos.y);
                ChooseColour(s, mon, ink, darkState, color, dark);
                ApplyLegibility(s, LegibilityAt(s, mon, lay, { mon.rc.left + pos.x, mon.rc.top + pos.y }, color, dark), lay);
            } else {   // the image given, filling the monitor (monitor px)
                OffsetRect(&ink, pos.x, pos.y);
                WallpaperSample ws;
                if (r.SampleImage(wall, { monW, monH }, ink, ws)) {
                    dark = s.autoContrast && ws.lightness > s.brightThreshold;
                    color = TintColor(dark ? s.darkColor : s.color, ws, s.tint / 100.0);
                }
                RECT lines = g.lines;
                OffsetRect(&lines, pos.x, pos.y);
                if (s.adaptiveLegibility && r.SampleImage(wall, { monW, monH }, lines, spread))
                    ApplyLegibility(s, LegibleChoice(s, spread, color, dark), lay);
            }
            auto bmp = r.RenderFrame(lay, color, dark);
            wchar_t name[64], line[512];
            swprintf_s(name, L"preview-%d-%s.png", n, v ? L"proposed" : L"now");
            if (!bmp || !r.SaveScene(dir + name, { monW, monH }, { { wall, 1.f } }, position, bg,
                                       { { bmp.Get(), { (float)pos.x, (float)pos.y }, 1.f } },
                                       (float)std::min(1.0, 1920.0 / monW))) rc = 1;
            if (!wallpaperOverride.empty()) {
                swprintf_s(line, L"    behind the lines: L* %.0f..%.0f, mean %.0f\r\n", spread.lightLo, spread.lightHi, spread.lightness);
                report += line;
            }
            swprintf_s(line, L"%s  %s (%dx%d, %u dpi): TimeSize=%g DateSize=%g Gap=%g WeatherSize=%g WeatherGap=%g "
                             L"SecondaryOpacity=%g VerticalPosition=%.1f; date capitals at %.1f%% of the height; "
                             L"%s text, legibility boost %.1f\r\n",
                       name, mon.name.empty() ? mon.device.c_str() : mon.name.c_str(), monW, monH, mon.dpi, s.timeSize,
                       s.dateSize, s.gap, s.weatherSize, s.weatherGap, s.secondaryOpacity, s.verticalPos,
                       (pos.y + g.ink.top) * 100.0 / monH, dark ? L"dark" : L"light", lay.legibility);
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

// --demo <folder> <dark image> <light image>: PNG frames (30 fps, half size) for the README's
// animations, drawn with the clock's own renderer and transition, the compositor's curves
// reproduced here: a minute change (14:19 -> 14:20) and a wallpaper change from the dark image
// to the light one. Default settings, made-up weather and track. Also demo-still.png.
int RunDemo(std::wstring dir, const std::wstring& darkBg, const std::wstring& lightBg) {
    if (!dir.empty() && dir.back() != L'\\' && dir.back() != L'/') dir += L'\\';
    if (!g_r.Init()) return 1;
    const Settings s;   // what a new user gets
    MonitorInfo mon;
    mon.rc = { 0, 0, 2560, 1440 };
    mon.dpi = 120;
    const SIZE monSz{ 2560, 1440 };
    WeatherReading w;
    w.valid = true; w.code = 1; w.isDay = true; w.temp = 18; w.high = 21; w.low = 12;
    GetSystemTimeAsFileTime(&w.fetchedUtc);
    std::wstring demoTrack(1, icon::Music);
    demoTrack += L" Bonobo — Kerala";
    Clock c;
    c.mon = mon;
    c.s = s;
    c.winRc = mon.rc;

    // The demo day, Saturday 27 June 2026 (a waxing gibbous moon at night), at h:m local time, as UTC ticks (the weather line's "now").
    auto utcAt = [](int h, int m) {
        SYSTEMTIME local{}, utc{};
        local.wYear = 2026; local.wMonth = 6; local.wDay = 27; local.wDayOfWeek = 6; local.wHour = (WORD)h; local.wMinute = (WORD)m;
        TzSpecificLocalTimeToSystemTime(nullptr, &local, &utc);
        FILETIME ft{};
        SystemTimeToFileTime(&utc, &ft);
        return (ULONGLONG(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
    };
    auto make = [&](int h, int m, const std::wstring& bgPath, Shown& f, const WeatherReading* wx = nullptr,
                    const std::wstring* track = nullptr) {
        SYSTEMTIME t{};
        t.wYear = 2026; t.wMonth = 6; t.wDay = 27; t.wDayOfWeek = 6; t.wHour = (WORD)h; t.wMinute = (WORD)m;
        const std::wstring date = FormatDate(t, s), time = FormatTime(t, s);
        const std::wstring& media = track ? *track : demoTrack;
        WeatherReading rd = wx ? *wx : w;
        const ULONGLONG now = utcAt(h, m);
        rd.fetchedUtc = { (DWORD)now, (DWORD)(now >> 32) };
        std::wstring weather;
        if (!PrepareFitted(g_r, s, mon.dpi, date, time, &rd, now, false, f.lay, &weather, nullptr, media)) return false;
        const ClockGeometry& g = f.lay.g;
        const POINT pos{ monSz.cx / 2 - g.anchorX, (int)lround(monSz.cy * s.verticalPos / 100.0) - g.anchorY };
        RECT ink = g.ink;
        OffsetRect(&ink, pos.x, pos.y);
        WallpaperSample ws;
        f.color = s.color;
        f.dark = false;
        if (g_r.SampleImage(bgPath, monSz, ink, ws)) {
            f.dark = s.autoContrast && ws.lightness > s.brightThreshold;
            f.color = TintColor(f.dark ? s.darkColor : s.color, ws, s.tint / 100.0);
        }
        RECT lines = g.lines;
        OffsetRect(&lines, pos.x, pos.y);
        if (s.adaptiveLegibility && g_r.SampleImage(bgPath, monSz, lines, ws)) ApplyLegibility(s, LegibleChoice(s, ws, f.color, f.dark), f.lay);
        f.rc = { pos.x, pos.y, pos.x + g.width, pos.y + g.height };
        f.win = mon.rc;
        wchar_t col[64];
        swprintf_s(col, L"|%06X|%.0f|%d|%.2f", (unsigned)f.color, s.opacity, f.dark ? 1 : 0, f.lay.legibility);
        f.dateKey = date + col;
        f.weatherKey = weather + col;
        f.mediaKey = media + col;
        f.key = f.dateKey + L"|" + time + L"|" + weather + L"|" + f.mediaKey;
        f.bmp = g_r.RenderFrame(f.lay, f.color, f.dark);
        return f.bmp != nullptr;
    };
    using Walls = std::vector<ClockRenderer::SceneWall>;
    using Parts = std::vector<ClockRenderer::ScenePart>;
    constexpr int kFps = 30;
    constexpr float kScale = 0.5f;
    int rc = 0;
    auto save = [&](const wchar_t* prefix, int& n, const Walls& walls, const Parts& parts) {
        wchar_t name[64];
        swprintf_s(name, L"%s-%03d.png", prefix, n++);
        if (!g_r.SaveScene(dir + name, monSz, walls, 4, 0, parts, kScale)) rc = 1;
    };
    auto at = [](const Shown& f) { return D2D1_POINT_2F{ (float)f.rc.left, (float)f.rc.top }; };
    auto smooth = [](double u) { return u * u * (3 - 2 * u); };
    // The transition's layers at progress u (0..1) of the clock's own fade, as Animate runs them.
    auto layersAt = [&](const Capture& cap, double u) {
        Parts parts;
        const double e = smooth(std::clamp(u, 0.0, 1.0));
        for (const auto& l : cap.layers) {
            double o = 1;
            if (l.kind == ClockRenderer::AnimLayer::FadeIn) o = e;
            if (l.kind == ClockRenderer::AnimLayer::FadeOut) o = 1 - cap.alpha * e <= 1e-6 ? 0 : (1 - e) / (1 - cap.alpha * e);
            parts.push_back({ l.bmp, { (float)(l.x0 + (l.x1 - l.x0) * e), (float)(l.y0 + (l.y1 - l.y0) * e) }, (float)o });
        }
        return parts;
    };
    auto capture = [&](const Shown& from, Shown& to, double fadeMs, Capture& cap) {
        g_fadeMs = fadeMs;
        c.cur = std::move(to);
        g_capture = &cap;
        const bool ok = StartTransition(c, from);
        g_capture = nullptr;
        g_fadeMs = 0;
        to = std::move(c.cur);
        return ok;
    };

    {   // 1. The minute changes: 14:19 -> 14:20 (the "14:" glides, "19" fades to "20").
        Shown a, b;
        Capture cap;
        if (!make(14, 19, darkBg, a) || !make(14, 20, darkBg, b) || !capture(a, b, 0, cap)) return 1;
        const Walls walls{ { darkBg, 1.f } };
        int n = 0;
        for (int i = 0; i < kFps * 6 / 10; ++i) save(L"minute", n, walls, { { a.bmp.Get(), at(a), 1.f } });
        const int frames = (int)lround(cap.seconds * kFps);
        for (int i = 0; i <= frames; ++i) save(L"minute", n, walls, layersAt(cap, (double)i / frames));
        for (int i = 0; i < kFps; ++i) save(L"minute", n, walls, { { b.bmp.Get(), at(b), 1.f } });
        if (!g_r.SaveScene(dir + L"demo-still.png", monSz, walls, 4, 0, { { b.bmp.Get(), at(b), 1.f } }, 0.75f)) rc = 1;
    }
    {   // 2. The wallpaper changes from dark to light. Windows crossfades it over ~1.2 s; the clock
        // starts once Windows names the new image (~0.4 s in) and ends with it, as on the desktop.
        Shown a, b;
        Capture cap;
        constexpr double kWall = 1.2, kStart = 0.4;
        if (!make(14, 20, darkBg, a) || !make(14, 20, lightBg, b) || !capture(a, b, (kWall - kStart) * 1000, cap)) return 1;
        int n = 0;
        for (int i = 0; i < kFps * 6 / 10; ++i) save(L"wallpaper", n, { { darkBg, 1.f } }, { { a.bmp.Get(), at(a), 1.f } });
        const int frames = (int)lround(kWall * kFps);
        for (int i = 0; i <= frames; ++i) {
            const double t = (double)i / kFps;
            const Walls walls{ { darkBg, 1.f }, { lightBg, (float)smooth(std::min(1.0, t / kWall)) } };
            if (t < kStart) save(L"wallpaper", n, walls, { { a.bmp.Get(), at(a), 1.f } });
            else save(L"wallpaper", n, walls, layersAt(cap, (t - kStart) / (kWall - kStart)));
        }
        for (int i = 0; i < kFps; ++i) save(L"wallpaper", n, { { lightBg, 1.f } }, { { b.bmp.Get(), at(b), 1.f } });
    }
    {   // 3. The weather line through a day: each extra appears only when it matters, and each new
        // reading crossfades in (the time glides when it changes too).
        struct Step { int h, m; WeatherReading w; };
        std::vector<Step> steps;
        auto base = [](int code, double temp, double high, double low, bool day = true) {
            WeatherReading r;
            r.valid = true; r.code = code; r.isDay = day; r.temp = temp; r.high = high; r.low = low; r.latitude = 40;
            return r;
        };
        steps.push_back({ 10, 5, base(0, 19, 27, 15) });                                 // clear
        { auto r = base(0, 30, 31, 18); r.uvIndex = 9; r.euAqi = 64; steps.push_back({ 13, 40, r }); }   // hot: UV, air
        { auto r = base(3, 22, 24, 17); r.rainChance = 70; r.rainAtUtc = utcAt(16, 0); steps.push_back({ 14, 20, r }); }   // rain later
        { auto r = base(2, 16, 23, 15); r.hasApparent = true; r.apparent = 12; r.hasWind = true; r.windSpeed = 38;
          r.windDir = 315; steps.push_back({ 17, 35, r }); }                              // windy, feels colder
        { auto r = base(1, 19, 23, 15); r.sunUtc[1] = utcAt(21, 5); steps.push_back({ 19, 50, r }); }   // sunset soon
        steps.push_back({ 23, 15, base(0, 15, 23, 14, false) });                         // clear night: the moon
        const std::wstring none;
        std::vector<Shown> frames(steps.size());
        for (size_t i = 0; i < steps.size(); ++i)
            if (!make(steps[i].h, steps[i].m, darkBg, frames[i], &steps[i].w, &none)) return 1;
        const Walls walls{ { darkBg, 1.f } };
        int n = 0;
        for (size_t i = 0; i < frames.size(); ++i) {
            for (int k = 0; k < kFps * 16 / 10; ++k) save(L"weather", n, walls, { { frames[i].bmp.Get(), at(frames[i]), 1.f } });
            if (i + 1 == frames.size()) break;
            Shown next;
            if (!make(steps[i + 1].h, steps[i + 1].m, darkBg, next, &steps[i + 1].w, &none)) return 1;
            Capture cap;
            if (!capture(frames[i], next, 0, cap)) return 1;
            const int fr = (int)lround(cap.seconds * kFps);
            for (int k = 0; k <= fr; ++k) save(L"weather", n, walls, layersAt(cap, (double)k / fr));
        }
    }
    return rc;
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, LPWSTR cmdLine, int) {
    g_inst = inst;
    const bool exitCmd = wcsstr(cmdLine, L"--exit") != nullptr;
    const bool settingsCmd = wcsstr(cmdLine, L"--settings") != nullptr;
    if (const wchar_t* t = wcsstr(cmdLine, L"--ini-test")) {   // --ini-test <folder>: Overlook.ini round trips
        std::wstring dir = t + wcslen(L"--ini-test");
        while (!dir.empty() && (iswspace(dir.front()) || dir.front() == L'"')) dir.erase(dir.begin());
        while (!dir.empty() && (iswspace(dir.back()) || dir.back() == L'"')) dir.pop_back();
        return RunIniTest(dir.empty() ? DataDir() : dir);
    }
    if (const wchar_t* t = wcsstr(cmdLine, L"--update-test")) {   // --update-test <folder>: version and JSON checks
        std::wstring dir = t + wcslen(L"--update-test");
        while (!dir.empty() && (iswspace(dir.front()) || dir.front() == L'"')) dir.erase(dir.begin());
        while (!dir.empty() && (iswspace(dir.back()) || dir.back() == L'"')) dir.pop_back();
        return RunUpdateTest(dir.empty() ? DataDir() : dir);
    }    if (const wchar_t* t = wcsstr(cmdLine, L"--weather-test")) {
        std::wstring dir = t + wcslen(L"--weather-test");
        while (!dir.empty() && (iswspace(dir.front()) || dir.front() == L'"')) dir.erase(dir.begin());
        while (!dir.empty() && (iswspace(dir.back()) || dir.back() == L'"')) dir.pop_back();
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        const int rc = RunWeatherTest(dir);
        CoUninitialize();
        return rc;
    }
    if (wcsstr(cmdLine, L"--demo")) {   // --demo <folder> <dark image> <light image>
        int argc = 0;
        LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
        int rc = 1;
        for (int i = 1; argv && i + 3 < argc; ++i)
            if (wcscmp(argv[i], L"--demo") == 0) {
                CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
                rc = RunDemo(argv[i + 1], argv[i + 2], argv[i + 3]);   // (COM stays up: g_r is released at exit)
                break;
            }
        if (argv) LocalFree(argv);
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
        if (dir.empty()) dir = DataDir();
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        const int rc = RunPreview(dir, wall);
        CoUninitialize();
        return rc;
    }

    HANDLE mutex = CreateMutexW(nullptr, TRUE, kMutexName);
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        // Already running: --exit closes it, --settings opens its settings window, a plain launch
        // reloads its settings.
        if (HWND other = FindWindowW(kCtlClass, nullptr)) {
            if (settingsCmd) AllowSetForegroundWindow(ASFW_ANY);
            PostMessageW(other, exitCmd ? WM_CLOSE : settingsCmd ? WM_APP_SETTINGS : WM_APP_RELOAD, 0, 0);
        }
        return 0;
    }
    if (exitCmd) return 0;

    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    g_s = LoadSettings();
    g_logEnabled = g_s.log;
    g_logPrivate = g_s.logPrivate;
    g_iniTime = IniWriteTime();
    LogHeader();
    if (g_s.debugMode) Log(L"debug mode on");
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
    UpdateInit(g_s.checkForUpdates);   // a newer release seen earlier is in the tray menu at once
    ScheduleUpdates(kUpdateFirstMs);
    Refresh();   // (starts the media watcher if wanted)
    if (settingsCmd) PostMessageW(g_ctl, WM_APP_SETTINGS, 0, 0);
    if (!g_s.weatherCity.empty()) {   // a cached reading (if recent) is already shown; refresh it now
        SetTimer(g_ctl, TIMER_WEATHER, kWeatherEveryMs, nullptr);
        FetchWeather();
    }

    // Event-driven watches (no polling):
    //  - the settings folder, so saved .ini edits apply without restarting;
    //  - Explorer's Themes folder, rewritten whenever the wallpaper changes (slideshow
    //    changes are not always broadcast as WM_SETTINGCHANGE).
    wchar_t appData[MAX_PATH] = L"";
    GetEnvironmentVariableW(L"APPDATA", appData, MAX_PATH);
    const std::wstring themesDir = std::wstring(appData) + L"\\Microsoft\\Windows\\Themes";
    HANDLE watches[2] = {
        FindFirstChangeNotificationW(DataDir().c_str(), FALSE, FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_FILE_NAME),
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
