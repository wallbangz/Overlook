// The settings window: a Windows 11 style page set drawn with Direct2D/DirectWrite (sharp at any
// DPI, light/dark/high contrast), one native EDIT control reused for text entry, and UI
// Automation (settingsuia.cpp). Every change is written straight into Overlook.ini with IniDoc,
// which keeps comments and the user's own values, and applied through the clock's usual reload.
#include "settingsui.h"
#include "settings.h"
#include <windowsx.h>
#include <dwmapi.h>
#include <commdlg.h>
#include <shellapi.h>
#include <shellscalingapi.h>
#include <initguid.h>   // CLSID_AccPropServices and the PROPID_ACC_ GUIDs, defined here
#include <oleacc.h>
#include <cmath>
#include <cwchar>
#include <cwctype>
#include <algorithm>

namespace sw {

namespace {

const wchar_t kClass[] = L"OverlookSettings";
enum : UINT_PTR { TIMER_SLIDER = 1 };
enum : UINT { WM_APP_MOVEFOCUS = WM_APP + 40 };

// Left pane and content geometry, DIPs.
const float kPaneW = 280, kNavTop = 100, kNavH = 36, kNavGap = 4, kTitleH = 76, kCardGap = 4, kPad = 16;
const float kMaxCardW = 1000, kCtlH = 32;

std::unique_ptr<Window> g_win;

D2D1_COLOR_F Rgb(int r, int g, int b, float a = 1) { return D2D1::ColorF(r / 255.f, g / 255.f, b / 255.f, a); }
D2D1_COLOR_F FromRef(COLORREF c) { return Rgb(GetRValue(c), GetGValue(c), GetBValue(c)); }
COLORREF ToRef(const D2D1_COLOR_F& c) { return RGB((int)(c.r * 255 + 0.5f), (int)(c.g * 255 + 0.5f), (int)(c.b * 255 + 0.5f)); }
D2D1_RECT_F Inflate(D2D1_RECT_F r, float dx, float dy) { return { r.left - dx, r.top - dy, r.right + dx, r.bottom + dy }; }
bool Inside(const D2D1_RECT_F& r, float x, float y) { return x >= r.left && x < r.right && y >= r.top && y < r.bottom; }
bool Empty(const D2D1_RECT_F& r) { return r.right <= r.left || r.bottom <= r.top; }

std::wstring Trim(std::wstring s) {
    while (!s.empty() && iswspace(s.back())) s.pop_back();
    size_t b = 0; while (b < s.size() && iswspace(s[b])) ++b;
    return s.substr(b);
}

// The same rules as settings.cpp's ReadBool / ReadNum.
bool ParseBool(const std::wstring& s, bool def) {
    if (s.empty()) return def;
    const wchar_t c = (wchar_t)towlower(s[0]);
    if (c == L'1' || c == L't' || c == L'y') return true;
    if (c == L'0' || c == L'f' || c == L'n') return false;
    if (_wcsicmp(s.c_str(), L"on") == 0) return true;
    if (_wcsicmp(s.c_str(), L"off") == 0) return false;
    return def;
}

double ParseNum(std::wstring s, double def) {
    if (!s.empty() && s.back() == L'%') s.pop_back();
    if (s.empty()) return def;
    wchar_t* end = nullptr;
    const double v = wcstod(s.c_str(), &end);
    return end == s.c_str() ? def : v;
}

std::wstring Fmt(double v, int decimals) {
    wchar_t buf[64];
    swprintf_s(buf, L"%.*f", decimals, v);
    std::wstring s = buf;
    if (s.find(L'.') != std::wstring::npos) {
        while (!s.empty() && s.back() == L'0') s.pop_back();
        if (!s.empty() && s.back() == L'.') s.pop_back();
    }
    if (s == L"-0") s = L"0";
    return s;
}

// "#abc", "abc", "#AABBCC" -> "#AABBCC"; "" if it isn't a colour.
std::wstring NormalColor(std::wstring s) {
    s = Trim(s);
    if (!s.empty() && s[0] == L'#') s.erase(0, 1);
    if (s.size() == 3) s = { s[0], s[0], s[1], s[1], s[2], s[2] };
    if (s.size() != 6) return L"";
    for (auto& c : s) { if (!iswxdigit(c)) return L""; c = (wchar_t)towupper(c); }
    return L"#" + s;
}

COLORREF ParseColor(const std::wstring& v, COLORREF def) {
    const std::wstring n = NormalColor(v);
    if (n.empty()) return def;
    const unsigned long x = wcstoul(n.c_str() + 1, nullptr, 16);
    return RGB((x >> 16) & 0xFF, (x >> 8) & 0xFF, x & 0xFF);
}

std::vector<std::wstring> SplitSemi(const std::wstring& v) {
    std::vector<std::wstring> out;
    size_t from = 0;
    while (from <= v.size()) {
        const size_t semi = v.find(L';', from);
        std::wstring item = Trim(v.substr(from, semi == std::wstring::npos ? std::wstring::npos : semi - from));
        if (!item.empty()) out.push_back(item);
        if (semi == std::wstring::npos) break;
        from = semi + 1;
    }
    return out;
}

std::wstring DateSample(const std::wstring& format, const std::wstring& locale) {
    SYSTEMTIME st; GetLocalTime(&st);
    wchar_t buf[256];
    if (format.empty() || !GetDateFormatEx(locale.empty() ? L"en-US" : locale.c_str(), 0, &st, format.c_str(), buf,
                                           ARRAYSIZE(buf), nullptr))
        return L"";
    return buf;
}

std::wstring LocaleName(const std::wstring& locale) {
    wchar_t buf[200];
    if (locale.empty() || !GetLocaleInfoEx(locale.c_str(), LOCALE_SLOCALIZEDDISPLAYNAME, buf, ARRAYSIZE(buf))) return L"";
    return buf;
}

const wchar_t kDefaultApps[] = L"Spotify, Apple Music, iTunes, TIDAL, Deezer, Amazon Music, Qobuz, YouTube Music, "
                               L"Pandora, foobar2000, MusicBee, AIMP, Winamp, Plexamp, Cider";

bool AppsLight() {
    DWORD v = 1, cb = sizeof(v);
    RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                 L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &v, &cb);
    return v != 0;
}

}   // namespace

// ================================================================== model

Window::Window(HINSTANCE inst, const SettingsHost& host) : inst_(inst), host_(host) {}

Window::~Window() {
    if (editFont_) DeleteObject(editFont_);
    if (editBrush_) DeleteObject(editBrush_);
}

void Window::BuildControls() {
    ctls.clear();
    pages_.clear();
    auto add = [&](Kind k, int page, const wchar_t* key, const wchar_t* label, const wchar_t* desc,
                   const wchar_t* def) -> int {
        Ctl c;
        c.id = (int)ctls.size() + 1;
        c.kind = k; c.page = page; c.key = key; c.label = label; c.desc = desc; c.defValue = def;
        ctls.push_back(c);
        return (int)ctls.size() - 1;
    };
    auto slider = [&](int page, const wchar_t* key, const wchar_t* label, const wchar_t* desc, const wchar_t* def,
                      double lo, double hi, double step, double big, const wchar_t* unit, int decimals = 0) {
        const int i = add(Kind::Slider, page, key, label, desc, def);
        Ctl& c = ctls[i];
        c.lo = lo; c.hi = hi; c.step = step; c.big = big; c.unit = unit; c.decimals = decimals;
        return i;
    };
    auto reset = [&](int owner) {
        const int i = add(Kind::Reset, ctls[owner].page, L"", L"Reset", L"", L"");
        ctls[i].owner = owner;
        return i;
    };
    auto header = [&](int page, const wchar_t* text) { Item it; it.header = text; pages_[page].items.push_back(it); };
    auto note = [&](int page, const wchar_t* text, Note kind) {
        Item it; it.note = text; it.noteKind = kind; pages_[page].items.push_back(it);
    };
    // A row: its controls, plus a Reset link in front of per-monitor settings.
    auto row = [&](std::initializer_list<int> ids) {
        Item it;
        const Ctl& first = ctls[*ids.begin()];
        if (!first.appWide && !first.key.empty() && first.monitorId.empty()) it.ctls.push_back(reset(*ids.begin()));
        for (int i : ids) it.ctls.push_back(i);
        pages_[first.page].items.push_back(it);
    };

    // Left pane
    {
        const int s = add(Kind::Combo, -1, L"", L"Settings for", L"", L"0");
        ctls[s].action = kScope;
        static const struct { const wchar_t* name; wchar_t icon; } kPages[] = {
            { L"Clock", 0xE121 }, { L"Look", 0xE790 }, { L"Weather", 0xE753 }, { L"Now playing", 0xEC4F }, { L"Monitors", 0xE7F4 },
        };
        for (int p = 0; p < (int)ARRAYSIZE(kPages); ++p) {
            Page pg; pg.name = kPages[p].name; pg.icon = kPages[p].icon;
            pages_.push_back(pg);
            const int n = add(Kind::NavItem, -1, L"", kPages[p].name, L"", L"");
            ctls[n].owner = p;
        }
        const int u = add(Kind::Button, -1, L"", L"Undo changes", L"Puts Overlook.ini back as it was when this window opened", L"");
        ctls[u].action = kUndo;
        const int o = add(Kind::Link, -1, L"", L"More settings in Overlook.ini", L"Opens the file in your text editor", L"");
        ctls[o].action = kOpenIni;
    }

    // Clock: what it says, then startup
    header(0, L"Time");
    row({ add(Kind::Toggle, 0, L"Hour24", L"24-hour clock", L"", L"1") });
    {
        const int i = add(Kind::Toggle, 0, L"ShowAmPm", L"Show AM/PM", L"In 12-hour mode", L"0");
        ctls[i].dependsOn = L"!Hour24";
        row({ i });
    }
    row({ add(Kind::Toggle, 0, L"LeadingZero", L"Leading zero", L"07:05 instead of 7:05", L"1") });
    header(0, L"Date");
    {
        const int i = add(Kind::EditCombo, 0, L"DateFormat", L"Date format", L"", L"dddd, MMMM d");
        for (const wchar_t* f : { L"dddd, MMMM d", L"dddd, d MMMM", L"dddd d MMMM", L"ddd, MMM d", L"ddd d MMM",
                                  L"dddd, MMMM d, yyyy", L"dddd, d MMMM yyyy", L"d MMMM yyyy", L"yyyy-MM-dd" })
            ctls[i].options.push_back({ f, L"" });   // labels: samples, filled in by LoadValues
        ctls[i].boxWidth = 260;
        row({ i });
        const int l = add(Kind::EditCombo, 0, L"DateLocale", L"Date language", L"", L"en-US");
        for (const wchar_t* loc : { L"en-US", L"en-GB", L"pt-PT", L"pt-BR", L"es-ES", L"fr-FR", L"de-DE", L"it-IT",
                                    L"nl-NL", L"sv-SE", L"pl-PL", L"ja-JP" })
            ctls[l].options.push_back({ loc, LocaleName(loc) });
        ctls[l].boxWidth = 260;
        row({ l });
    }
    header(0, L"Startup");
    {
        const int s = add(Kind::Toggle, 0, L"StartWithWindows", L"Start with Windows", L"", L"0");
        ctls[s].appWide = true;
        row({ s });
    }

    // Look: where and how big, colour, opacity, motion
    header(1, L"Position and size");
    row({ slider(1, L"VerticalPosition", L"Vertical position", L"% of the screen height, from the top",
                 L"16.5", 0, 100, 0.1, 1, L" %", 1) });
    row({ slider(1, L"TimeSize", L"Time size", L"", L"145", 40, 400, 1, 10, L"") });
    row({ slider(1, L"DateSize", L"Date size", L"", L"32", 8, 120, 1, 4, L"") });
    header(1, L"Colour");
    {
        const int sw = add(Kind::Color, 1, L"Color", L"Text colour", L"Used on dark wallpapers", L"#FFFFFF");
        const int hex = add(Kind::Edit, 1, L"Color", L"Text colour, hex", L"", L"#FFFFFF");
        ctls[hex].boxWidth = 104;
        row({ sw, hex });
    }
    row({ add(Kind::Toggle, 1, L"AutoContrast", L"Auto-contrast", L"Dark text on bright wallpapers", L"1") });
    row({ slider(1, L"Tint", L"Tint", L"Takes on the colour of the wallpaper behind it", L"35", 0, 100, 1, 10, L" %") });
    header(1, L"Opacity and shadow");
    row({ slider(1, L"Opacity", L"Opacity", L"", L"80", 0, 100, 1, 10, L" %") });
    row({ slider(1, L"SecondaryOpacity", L"Weather and now-playing opacity", L"% of the clock's opacity, so the time stands out",
                 L"70", 0, 100, 1, 10, L" %") });
    row({ add(Kind::Toggle, 1, L"Shadow", L"Shadow", L"A soft shadow for legibility", L"1") });
    header(1, L"Motion");
    {
        const int i = slider(1, L"TransitionMs", L"Minute transition", L"Unchanged digits slide, changed ones crossfade",
                             L"500", 0, 2000, 50, 250, L" ms");
        ctls[i].zeroText = L"Off";
        row({ i });
    }

    // Weather: the on/off switch first, then where, then what it shows
    note(2, L"", kCityEmpty);
    row({ add(Kind::Toggle, 2, L"ShowWeather", L"Show the weather line", L"", L"1") });
    {
        const int c = add(Kind::Edit, 2, L"WeatherCity", L"City",
                          L"A city, \"Porto, PT\" or a postcode; empty turns the weather off. Only the city (once) and its "
                          L"coordinates are sent to open-meteo.com.", L"");
        ctls[c].appWide = true;
        ctls[c].boxWidth = 260;
        ctls[c].dependsOn = L"ShowWeather";
        row({ c });
        const int u = add(Kind::Combo, 2, L"WeatherUnits", L"Units", L"", L"C");
        ctls[u].appWide = true;
        ctls[u].options = { { L"C", L"Celsius, km/h" }, { L"F", L"Fahrenheit, mph" } };
        ctls[u].dependsOn = L"ShowWeather";
        row({ u });
    }
    header(2, L"Extras, each shown only when it matters");
    {
        const int r = slider(2, L"RainThreshold", L"Chance of rain", L"Shown when it reaches this within the next hours",
                             L"30", 0, 100, 5, 10, L" %");
        ctls[r].dependsOn = L"ShowWeather";
        row({ r });
        const struct { const wchar_t *key, *label, *desc; } kExtras[] = {
            { L"FeelsLike", L"Feels like", L"When the apparent temperature is a few degrees off" },
            { L"Wind", L"Wind", L"Strong wind, with the direction it blows from" },
            { L"UV", L"UV index", L"When it is high" },
            { L"AirQuality", L"Air quality", L"From \u201Cpoor\u201D" },
            { L"SunTimes", L"Sunrise and sunset", L"When the next one is a few hours away" },
        };
        for (auto& e : kExtras) {
            const int i = add(Kind::Toggle, 2, e.key, e.label, e.desc, L"1");
            ctls[i].dependsOn = L"ShowWeather";
            row({ i });
        }
    }

    // Now playing
    row({ add(Kind::Toggle, 3, L"NowPlaying", L"Show what's playing", L"From Windows' media controls", L"1") });
    {
        const int a = add(Kind::Edit, 3, L"NowPlayingApps", L"Apps",
                          L"Part of each app's name, comma separated. Add chrome, msedge or firefox for browsers, or "
                          L"\u201Call\u201D for every app.", kDefaultApps);
        ctls[a].appWide = true;
        ctls[a].boxWidth = 300;
        ctls[a].dependsOn = L"NowPlaying";
        const int d = add(Kind::Button, 3, L"", L"Default", L"The usual music apps", L"");
        ctls[d].action = kAppsDefault;
        ctls[d].appWide = true;
        ctls[d].dependsOn = L"NowPlaying";
        row({ a, d });
        const int l = slider(3, L"NowPlayingLinger", L"Stays after pausing", L"Dimmed while paused, then it fades out",
                             L"60", 0, 600, 5, 60, L" s");
        ctls[l].dependsOn = L"NowPlaying";
        row({ l });
    }

    // Monitors: the clock rows come from BuildMonitorRows
    header(4, L"Clocks");
    note(4, L"", kMonitorSpec);
    header(4, L"OLED care");
    row({ add(Kind::Toggle, 4, L"PixelShift", L"Pixel shift", L"Moves the clock 1 px every 10 minutes, within a few pixels", L"0") });
    row({ add(Kind::Toggle, 4, L"NightDim", L"Night dimming", L"Lower opacity between sunset and sunrise", L"0") });
    {
        const int n = slider(4, L"NightOpacity", L"Night opacity", L"", L"45", 0, 100, 1, 10, L" %");
        ctls[n].dependsOn = L"NightDim";
        row({ n });
    }
}

// One on/off row per monitor of the Monitor= set, after the "Clocks" header and its note. Rows
// of monitors that went away stay in ctls (ids must stay unique) but are hidden for good.
void Window::BuildMonitorRows() {
    Page& p = pages_[4];
    for (auto& c : ctls) if (!c.monitorId.empty()) { c.visible = false; c.page = -2; }
    p.items.erase(std::remove_if(p.items.begin(), p.items.end(), [&](const Item& it) {
        return !it.ctls.empty() && ctls[it.ctls[0]].page == -2; }), p.items.end());
    size_t at = 2;   // after the header and the note
    int nextId = 0;
    for (auto& c : ctls) nextId = std::max(nextId, c.id);
    for (auto& m : monitors_) {
        Ctl c;
        c.id = ++nextId;
        c.kind = Kind::Toggle;
        c.page = 4;
        c.appWide = true;
        c.monitorId = m.id;
        wchar_t label[300];
        swprintf_s(label, L"%s  (%ld×%ld)", m.mon.name.empty() ? m.mon.device.c_str() : m.mon.name.c_str(),
                   m.mon.rc.right - m.mon.rc.left, m.mon.rc.bottom - m.mon.rc.top);
        c.label = label;
        c.defValue = L"1";
        ctls.push_back(c);
        Item it;
        it.ctls.push_back((int)ctls.size() - 1);
        p.items.insert(p.items.begin() + (ptrdiff_t)at++, it);
    }
}

// "All monitors", then each monitor of the Monitor= set, then [Monitor ...] sections that match none.
void Window::BuildScopes() {
    const std::wstring was = scope_ < (int)scopes_.size() ? scopes_[scope_].section : L"Clock";
    scopes_.clear();
    scopes_.push_back({ L"All monitors", -1, L"Clock", true });
    std::vector<bool> used;
    std::vector<std::wstring> specs;
    for (auto& s : doc_.Sections())
        if (s.size() > 8 && _wcsnicmp(s.c_str(), L"Monitor ", 8) == 0) specs.push_back(s);
    used.assign(specs.size(), false);
    for (size_t k = 0; k < monitors_.size(); ++k) {
        const SettingsMonitor& m = monitors_[k];
        Scope sc;
        sc.mon = (int)k;
        sc.label = (m.mon.name.empty() ? m.mon.device : m.mon.name) + (m.mon.primary ? L" (primary)" : L"");
        for (size_t j = 0; j < specs.size(); ++j) {
            const int f = FindMonitor(allMons_, specs[j].substr(8));
            if (f >= 0 && allMons_[f].handle == m.mon.handle) { sc.section = specs[j]; used[j] = true; }   // later ones win
        }
        sc.exists = !sc.section.empty();
        if (!sc.exists) sc.section = L"Monitor " + (m.mon.primary ? std::wstring(L"primary") : m.id);
        scopes_.push_back(sc);
    }
    for (size_t j = 0; j < specs.size(); ++j)
        if (!used[j]) scopes_.push_back({ specs[j].substr(8) + L" (not connected)", -1, specs[j], true });
    scope_ = 0;
    for (size_t s = 0; s < scopes_.size(); ++s)
        if (_wcsicmp(scopes_[s].section.c_str(), was.c_str()) == 0) scope_ = (int)s;
    Ctl& c = ctls[0];
    c.options.clear();
    for (size_t s = 0; s < scopes_.size(); ++s) c.options.push_back({ std::to_wstring(s), scopes_[s].label });
    c.value = std::to_wstring(scope_);
}

// The value a control shows: [Clock], then (in a monitor scope) every [Monitor ...] section that
// matches the monitor, later ones winning, as the clock itself reads them.
std::wstring Window::Effective(const Ctl& c, bool* overridden) const {
    std::wstring v;
    if (!doc_.Get(L"Clock", c.key, v)) v = c.defValue;
    if (overridden) *overridden = false;
    if (c.appWide || scope_ == 0) return v;
    const Scope& s = scopes_[scope_];
    for (auto& sec : doc_.Sections()) {
        if (sec.size() <= 8 || _wcsnicmp(sec.c_str(), L"Monitor ", 8) != 0) continue;
        bool match = _wcsicmp(sec.c_str(), s.section.c_str()) == 0;
        if (!match && s.mon >= 0) {
            const int f = FindMonitor(allMons_, sec.substr(8));
            match = f >= 0 && allMons_[f].handle == monitors_[s.mon].mon.handle;
        }
        std::wstring x;
        if (match && doc_.Get(sec, c.key, x)) v = x;
    }
    if (overridden) *overridden = doc_.Has(s.section, c.key);
    return v;
}

void Window::LoadValues() {
    doc_.Load(iniPath_);
    allMons_ = EnumMonitors();
    auto mons = host_.monitors ? host_.monitors() : std::vector<SettingsMonitor>();
    bool same = mons.size() == monitors_.size();
    for (size_t k = 0; same && k < mons.size(); ++k)
        same = mons[k].id == monitors_[k].id && mons[k].mon.handle == monitors_[k].mon.handle;
    monitors_ = std::move(mons);
    if (!same) BuildMonitorRows();
    BuildScopes();

    std::wstring hidden;
    doc_.Get(L"Clock", L"HiddenMonitors", hidden);
    const auto hiddenList = SplitSemi(hidden);
    std::wstring locale;
    for (size_t i = 0; i < ctls.size(); ++i) {
        Ctl& c = ctls[i];
        if ((int)i == drag_ || (sliderPending_ && (int)i == sliderCtl_) || (int)i == editing_) continue;
        if (!c.monitorId.empty()) {
            bool off = false;
            for (auto& h : hiddenList) off |= _wcsicmp(h.c_str(), c.monitorId.c_str()) == 0;
            c.value = off ? L"0" : L"1";
            c.desc.clear();
            for (auto& m : monitors_)
                if (m.id == c.monitorId) {
                    if (m.showOff) c.desc = L"Turned off with Show=0 in Overlook.ini";
                    else if (m.mon.primary) c.desc = L"Primary monitor";
                }
            continue;
        }
        if (c.key.empty()) continue;
        c.value = Effective(c, &c.overridden);
        FindOverrides(c);
        if (c.key == L"WeatherUnits") c.value = !c.value.empty() && towupper(c.value[0]) == L'F' ? L"F" : L"C";
        if (c.key == L"Color") c.value = NormalColor(c.value).empty() ? c.defValue : NormalColor(c.value);
        if (c.key == L"DateLocale") locale = c.value;
    }
    for (auto& c : ctls) {
        if (c.key == L"DateFormat") {
            for (auto& o : c.options) o.label = DateSample(o.value, locale);
            const std::wstring sample = DateSample(editing_ >= 0 && &ctls[editing_] == &c ? editLive_ : c.value, locale);
            c.desc = sample.empty() ? L"Windows date picture: dddd weekday, MMMM month, d day, yyyy year" : L"Today: " + sample;
        }
        if (c.key == L"DateLocale") {
            const std::wstring name = LocaleName(editing_ >= 0 && &ctls[editing_] == &c ? editLive_ : c.value);
            c.desc = name.empty() ? L"Day and month names, e.g. en-GB or pt-PT" : name;
        }
    }
    UpdateEnabled();
    Layout();
    Invalidate();
}

void Window::UpdateEnabled() {
    for (auto& c : ctls) {
        if (c.page == -2) continue;
        c.enabled = true;
        if (c.kind == Kind::Reset) {
            const Ctl& o = ctls[c.owner];
            c.visible = scope_ > 0 ? o.overridden : !o.elsewhere.empty();
            c.label = scope_ > 0 ? L"Reset" : L"Use for all";
            continue;
        }
        if (c.appWide && scope_ > 0) c.enabled = false;
        if (!c.dependsOn.empty()) {
            const bool invert = c.dependsOn[0] == L'!';
            const std::wstring key = invert ? c.dependsOn.substr(1) : c.dependsOn;
            for (size_t j = 0; j < ctls.size(); ++j)
                if (ctls[j].key == key && ctls[j].kind == Kind::Toggle) { if (ToggleOn((int)j) == invert) c.enabled = false; break; }
        }
        if (!c.monitorId.empty())
            for (auto& m : monitors_) if (m.id == c.monitorId && m.showOff) c.enabled = false;
        if (c.action == kUndo) c.enabled = changed_;
    }
}

void Window::Write(int i, const std::wstring& v) {
    Ctl& c = ctls[i];
    if (c.key.empty()) return;
    bool overridden = false;
    const std::wstring now = Effective(c, &overridden);
    const bool global = c.appWide || scope_ == 0;
    if (v == now && (global || overridden)) { c.value = v; Invalidate(); return; }
    IniDoc d;
    d.Load(iniPath_);   // fresh: the tray or an editor may have changed it since
    BackupIniOnce(iniPath_);
    d.Set(global ? L"Clock" : scopes_[scope_].section, c.key, v);
    if (!d.Save(iniPath_)) { MessageBeep(MB_ICONWARNING); LoadValues(); return; }
    changed_ = true;
    c.value = v;
    if (host_.ctl) PostMessageW(host_.ctl, host_.reloadMsg, 0, 0);
    LoadValues();
    NotifyValue(i);
}

void Window::ResetOverride(int i) {
    const Ctl& o = ctls[ctls[i].owner];
    IniDoc d;
    d.Load(iniPath_);
    BackupIniOnce(iniPath_);
    bool removed = false;
    if (scope_ > 0) removed = d.Remove(scopes_[scope_].section, o.key);
    else for (auto& sec : o.elsewhereSections) removed |= d.Remove(sec, o.key);   // every monitor back on [Clock]
    if (removed && d.Save(iniPath_)) {
        changed_ = true;
        if (host_.ctl) PostMessageW(host_.ctl, host_.reloadMsg, 0, 0);
    }
    const int owner = ctls[i].owner;
    LoadValues();
    if (focus_ == i) SetFocusCtl(owner, kbdCues_);
    NotifyValue(owner);
}

void Window::WriteShowMonitor(int i, bool on) {
    IniDoc d;
    d.Load(iniPath_);
    std::wstring hidden;
    d.Get(L"Clock", L"HiddenMonitors", hidden);
    std::wstring list;
    for (auto& h : SplitSemi(hidden))
        if (_wcsicmp(h.c_str(), ctls[i].monitorId.c_str()) != 0) list += (list.empty() ? L"" : L";") + h;
    if (!on) list += (list.empty() ? L"" : L";") + ctls[i].monitorId;
    BackupIniOnce(iniPath_);
    d.Set(L"Clock", L"HiddenMonitors", list);
    if (!d.Save(iniPath_)) { MessageBeep(MB_ICONWARNING); return; }
    changed_ = true;
    if (host_.ctl) PostMessageW(host_.ctl, host_.reloadMsg, 0, 0);
    LoadValues();
    UiaRaiseToggle(*this, i, on);
}

void Window::Undo() {
    if (!changed_) return;
    EndEdit(false);
    if (original_.empty() || !WriteFileReplace(iniPath_, original_)) { MessageBeep(MB_ICONWARNING); return; }
    changed_ = false;
    if (host_.ctl) PostMessageW(host_.ctl, host_.reloadMsg, 0, 0);
    LoadValues();
    for (size_t i = 0; i < ctls.size(); ++i) if (ctls[i].action == kUndo && focus_ == (int)i) focus_ = -1;
    if (focus_ < 0) SetFocusCtl(0, kbdCues_);
}

void Window::SetScope(int s) {
    if (s < 0 || s >= (int)scopes_.size() || s == scope_) return;
    EndEdit(true);
    scope_ = s;
    ctls[0].value = std::to_wstring(s);
    LoadValues();
    NotifyValue(0);
}

// ================================================================== values for display and UIA

std::wstring Window::Display(const Ctl& c, const std::wstring& v) const {
    switch (c.kind) {
    case Kind::Toggle: return ParseBool(v, ParseBool(c.defValue, false)) ? L"On" : L"Off";
    case Kind::Slider: {
        const double x = std::clamp(ParseNum(v, ParseNum(c.defValue, c.lo)), c.lo, c.hi);
        return x == 0 && !c.zeroText.empty() ? c.zeroText : Fmt(x, c.decimals) + c.unit;
    }
    case Kind::Combo:
        for (auto& o : c.options) if (_wcsicmp(o.value.c_str(), v.c_str()) == 0) return o.label;
        return v;
    case Kind::Color: case Kind::Edit: return c.key == L"Color" && !NormalColor(v).empty() ? NormalColor(v) : v;
    default: return v;
    }
}

// All monitors: the shown monitors whose own [Monitor ...] section sets this key, so a change
// here doesn't reach them.
void Window::FindOverrides(Ctl& c) const {
    c.elsewhere.clear();
    c.elsewhereSections.clear();
    if (scope_ != 0 || c.appWide || c.key.empty() || !c.monitorId.empty() || c.kind == Kind::Edit && c.key == L"Color") return;
    std::wstring names;
    int count = 0;
    std::wstring single;
    for (auto& m : monitors_) {
        std::wstring v;
        bool has = false;
        for (auto& sec : doc_.Sections()) {
            if (sec.size() <= 8 || _wcsnicmp(sec.c_str(), L"Monitor ", 8) != 0) continue;
            const int f = FindMonitor(allMons_, sec.substr(8));
            if (f < 0 || allMons_[f].handle != m.mon.handle) continue;
            std::wstring x;
            if (doc_.Get(sec, c.key, x)) {
                v = x;
                has = true;
                if (std::find(c.elsewhereSections.begin(), c.elsewhereSections.end(), sec) == c.elsewhereSections.end())
                    c.elsewhereSections.push_back(sec);
            }
        }
        if (!has) continue;
        const std::wstring name = m.mon.name.empty() ? m.mon.device : m.mon.name;
        names += (names.empty() ? L"" : L", ") + name;
        single = Display(c, v);
        ++count;
    }
    if (count == 1) c.elsewhere = names + L" keeps its own: " + single;
    else if (count > 1) c.elsewhere = names + L" keep their own values";
}

std::wstring Window::ShownDesc(int i) const {
    const Ctl& c = ctls[i];
    if (c.appWide && scope_ > 0) return L"The same on every monitor";
    return c.desc;
}

bool Window::ToggleOn(int i) const {
    const Ctl& c = ctls[i];
    return ParseBool(c.value, ParseBool(c.defValue, false));
}

double Window::SliderValue(int i) const {
    const Ctl& c = ctls[i];
    return std::clamp(ParseNum(c.value, ParseNum(c.defValue, c.lo)), c.lo, c.hi);
}

std::wstring Window::ValueText(int i) const {
    const Ctl& c = ctls[i];
    switch (c.kind) {
    case Kind::Toggle: return ToggleOn(i) ? L"On" : L"Off";
    case Kind::Slider: {
        const double v = SliderValue(i);
        if (v == 0 && !c.zeroText.empty()) return c.zeroText;
        return Fmt(v, c.decimals) + c.unit;
    }
    case Kind::Combo:
        for (auto& o : c.options) if (_wcsicmp(o.value.c_str(), c.value.c_str()) == 0) return o.label;
        return c.value;
    case Kind::NavItem: return L"";
    default: return c.value;
    }
}

std::wstring Window::UiaName(int i) const {
    const Ctl& c = ctls[i];
    if (c.kind == Kind::Reset)
        return scope_ > 0 ? L"Reset " + ctls[c.owner].label + L" to the value for all monitors"
                          : L"Use the value for all monitors for " + ctls[c.owner].label;
    if (c.kind == Kind::Color) return c.label + L", " + c.value;
    return c.label;
}

int Window::CtlIndex(int id) const {
    for (size_t i = 0; i < ctls.size(); ++i) if (ctls[i].id == id && ctls[i].page != -2) return (int)i;
    return -1;
}

std::vector<int> Window::UiaOrder() const {
    std::vector<int> out;
    for (size_t i = 0; i < ctls.size(); ++i)
        if (ctls[i].page == -1 && ctls[i].kind != Kind::Button && ctls[i].kind != Kind::Link) out.push_back((int)i);
    for (auto& it : pages_[page_].items)
        for (int i : it.ctls) if (ctls[i].visible) out.push_back(i);
    for (size_t i = 0; i < ctls.size(); ++i)
        if (ctls[i].page == -1 && (ctls[i].kind == Kind::Button || ctls[i].kind == Kind::Link)) out.push_back((int)i);
    return out;
}

std::vector<int> Window::FocusOrder() const {
    std::vector<int> out;
    for (int i : UiaOrder()) {
        const Ctl& c = ctls[i];
        if (!c.enabled || !c.visible) continue;
        if (c.kind == Kind::NavItem && c.owner != page_) continue;   // the page list is one tab stop
        out.push_back(i);
    }
    return out;
}

RECT Window::ScreenRect(int i) const {
    const D2D1_RECT_F r = ViewRect(i);
    RECT rc{ (LONG)std::floor(r.left * scale_), (LONG)std::floor(r.top * scale_),
             (LONG)std::ceil(r.right * scale_), (LONG)std::ceil(r.bottom * scale_) };
    MapWindowPoints(hwnd, nullptr, (POINT*)&rc, 2);
    return rc;
}

bool Window::Offscreen(int i) const {
    if (ctls[i].page < 0) return false;
    const D2D1_RECT_F r = ViewRect(i), c = ContentRect();
    return r.bottom <= c.top || r.top >= c.bottom;
}

int Window::CtlAtScreen(POINT pt) const {
    ScreenToClient(hwnd, &pt);
    return HitTest(pt.x / scale_, pt.y / scale_);
}

void Window::UiaFocus(int i) {
    if (i < 0 || !ctls[i].visible) return;
    if (ctls[i].kind == Kind::NavItem && ctls[i].owner != page_) SetPage(ctls[i].owner);
    SetFocusCtl(i, true);
}
void Window::UiaToggle(int i) { if (ctls[i].enabled) Activate(i); }
void Window::UiaInvoke(int i) { if (ctls[i].enabled) Activate(i); }
void Window::UiaSetRange(int i, double v) { if (ctls[i].enabled && ctls[i].kind == Kind::Slider) SetSlider(i, v, true); }
void Window::UiaExpand(int i, bool open) {
    if (!ctls[i].enabled) return;
    if (open && popup_.ctl != i) OpenPopup(i);
    if (!open && popup_.ctl == i) ClosePopup(false);
}

bool Window::UiaSetValue(int i, const std::wstring& v) {
    Ctl& c = ctls[i];
    if (!c.enabled) return false;
    switch (c.kind) {
    case Kind::Combo:
        for (size_t o = 0; o < c.options.size(); ++o)
            if (_wcsicmp(c.options[o].label.c_str(), v.c_str()) == 0 || _wcsicmp(c.options[o].value.c_str(), v.c_str()) == 0) {
                SelectOption(i, (int)o);
                return true;
            }
        return false;
    case Kind::Edit: case Kind::EditCombo: case Kind::Color: {
        if (editing_ == i) EndEdit(false);
        std::wstring s = Trim(v);
        if (c.key == L"Color") { s = NormalColor(s); if (s.empty()) return false; }
        Write(i, s);
        return true;
    }
    case Kind::Slider: {
        wchar_t* end = nullptr;
        const double d = wcstod(v.c_str(), &end);
        if (end == v.c_str()) return false;
        SetSlider(i, d, true);
        return true;
    }
    default: return false;
    }
}

// ================================================================== theme, fonts, target

void Window::LoadTheme() {
    HIGHCONTRASTW hc{ sizeof(hc) };
    SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(hc), &hc, 0);
    Theme& t = t_;
    if (hc.dwFlags & HCF_HIGHCONTRASTON) {
        const auto sys = [](int i) { return FromRef(GetSysColor(i)); };
        t.contrast = true;
        t.bg = t.pane = t.card = t.cardHover = t.ctlFill = t.ctlHover = t.popup = sys(COLOR_WINDOW);
        t.text = t.text2 = t.cardStroke = t.ctlStroke = t.ctlStrokeBottom = t.toggleOff = t.focus = t.track = sys(COLOR_WINDOWTEXT);
        t.disabled = sys(COLOR_GRAYTEXT);
        t.accent = t.accentHover = t.selFill = sys(COLOR_HIGHLIGHT);
        t.onAccent = sys(COLOR_HIGHLIGHTTEXT);
        t.link = sys(COLOR_HOTLIGHT);
        t.dark = GetRValue(GetSysColor(COLOR_WINDOW)) < 128;
        return;
    }
    t.contrast = false;
    t.dark = !AppsLight();
    if (t.dark) {
        t.bg = t.pane = Rgb(32, 32, 32);
        t.card = Rgb(43, 43, 43); t.cardHover = Rgb(50, 50, 50); t.cardStroke = Rgb(28, 28, 28);
        t.text = Rgb(255, 255, 255); t.text2 = Rgb(200, 200, 200); t.disabled = Rgb(118, 118, 118);
        t.ctlFill = Rgb(55, 55, 55); t.ctlHover = Rgb(61, 61, 61); t.ctlStroke = Rgb(66, 66, 66);
        t.ctlStrokeBottom = Rgb(154, 154, 154);
        t.toggleOff = Rgb(200, 200, 200); t.focus = Rgb(255, 255, 255);
        t.popup = Rgb(44, 44, 44); t.selFill = Rgb(58, 58, 58); t.track = Rgb(140, 140, 140);
    } else {
        t.bg = t.pane = Rgb(243, 243, 243);
        t.card = Rgb(251, 251, 251); t.cardHover = Rgb(246, 246, 246); t.cardStroke = Rgb(229, 229, 229);
        t.text = Rgb(27, 27, 27); t.text2 = Rgb(96, 96, 96); t.disabled = Rgb(160, 160, 160);
        t.ctlFill = Rgb(254, 254, 254); t.ctlHover = Rgb(249, 249, 249); t.ctlStroke = Rgb(229, 229, 229);
        t.ctlStrokeBottom = Rgb(134, 134, 134);
        t.toggleOff = Rgb(96, 96, 96); t.focus = Rgb(27, 27, 27);
        t.popup = Rgb(249, 249, 249); t.selFill = Rgb(237, 237, 237); t.track = Rgb(139, 139, 139);
    }
    // A vivid green accent, like macOS (Apple's system green; a darker one for link text on light cards).
    t.accent = t.dark ? Rgb(48, 209, 88) : Rgb(52, 199, 89);
    t.link = t.dark ? Rgb(48, 209, 88) : Rgb(36, 138, 61);
    t.onAccent = Rgb(255, 255, 255);
    t.accentHover = t.accent;
    t.accentHover.a = 0.9f;
}

void Window::CreateFonts() {
    if (!dw_) return;
    ComPtr<IDWriteFontCollection> sys;
    dw_->GetSystemFontCollection(&sys);
    auto has = [&](const wchar_t* name) { UINT32 i; BOOL e = FALSE; return sys && SUCCEEDED(sys->FindFamilyName(name, &i, &e)) && e; };
    family_ = has(L"Segoe UI Variable Text") ? L"Segoe UI Variable Text" : L"Segoe UI";
    const std::wstring display = has(L"Segoe UI Variable Display") ? L"Segoe UI Variable Display" : family_;
    iconFamily_ = has(L"Segoe Fluent Icons") ? L"Segoe Fluent Icons" : L"Segoe MDL2 Assets";
    auto make = [&](const std::wstring& fam, float size, DWRITE_FONT_WEIGHT w, bool wrap, ComPtr<IDWriteTextFormat>& out) {
        out.Reset();
        dw_->CreateTextFormat(fam.c_str(), nullptr, w, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, size, L"en-us", &out);
        if (!out) return;
        out->SetWordWrapping(wrap ? DWRITE_WORD_WRAPPING_WRAP : DWRITE_WORD_WRAPPING_NO_WRAP);
        if (!wrap) {
            ComPtr<IDWriteInlineObject> sign;
            dw_->CreateEllipsisTrimmingSign(out.Get(), &sign);
            DWRITE_TRIMMING tr{ DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0 };
            out->SetTrimming(&tr, sign.Get());
        }
    };
    make(family_, 14, DWRITE_FONT_WEIGHT_NORMAL, false, body_);
    make(family_, 14, DWRITE_FONT_WEIGHT_NORMAL, false, bodyRight_);
    make(family_, 12, DWRITE_FONT_WEIGHT_NORMAL, true, caption_);
    make(family_, 14, DWRITE_FONT_WEIGHT_SEMI_BOLD, false, strong_);
    make(display, 28, DWRITE_FONT_WEIGHT_SEMI_BOLD, false, title_);
    make(iconFamily_, 16, DWRITE_FONT_WEIGHT_NORMAL, false, icon_);
    make(iconFamily_, 12, DWRITE_FONT_WEIGHT_NORMAL, false, iconSmall_);

    // The EDIT control's font, at the window's DPI.
    if (editFont_) DeleteObject(editFont_);
    editFont_ = CreateFontW(-(int)std::lround(14 * scale_), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                            OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, family_.c_str());
    if (edit_) SendMessageW(edit_, WM_SETFONT, (WPARAM)editFont_, TRUE);
}

bool Window::CreateTarget() {
    if (rt_) return true;
    RECT rc; GetClientRect(hwnd, &rc);
    const D2D1_RENDER_TARGET_PROPERTIES rp = D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_DEFAULT,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE), dpi_, dpi_);
    const D2D1_HWND_RENDER_TARGET_PROPERTIES hp = D2D1::HwndRenderTargetProperties(hwnd,
        D2D1::SizeU((UINT32)std::max<LONG>(1, rc.right), (UINT32)std::max<LONG>(1, rc.bottom)));
    if (FAILED(d2d_->CreateHwndRenderTarget(rp, hp, &rt_))) return false;
    rt_->CreateSolidColorBrush(t_.text, &brush_);
    return true;
}

// ================================================================== layout

float Window::Snap(float v) const { return std::round(v * scale_) / scale_; }

D2D1_RECT_F Window::ContentRect() const { return { kPaneW, kTitleH, w_, h_ }; }

D2D1_RECT_F Window::ViewRect(int i) const {
    D2D1_RECT_F r = ctls[i].box;
    if (ctls[i].page >= 0) {
        const float dy = ContentRect().top - scroll_;
        r.top += dy; r.bottom += dy;
    }
    return r;
}

float Window::TextWidth(const std::wstring& s, IDWriteTextFormat* f) {
    ComPtr<IDWriteTextLayout> l;
    if (!f || FAILED(dw_->CreateTextLayout(s.c_str(), (UINT32)s.size(), f, 4000, 100, &l))) return 0;
    l->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    DWRITE_TEXT_METRICS m{};
    l->GetMetrics(&m);
    return m.widthIncludingTrailingWhitespace;
}

static float TextHeight(IDWriteFactory* dw, const std::wstring& s, IDWriteTextFormat* f, float width) {
    ComPtr<IDWriteTextLayout> l;
    if (s.empty() || !f || FAILED(dw->CreateTextLayout(s.c_str(), (UINT32)s.size(), f, std::max(10.f, width), 1000, &l))) return 0;
    DWRITE_TEXT_METRICS m{};
    l->GetMetrics(&m);
    return m.height;
}

void Window::Layout() {
    if (!dw_ || pages_.empty()) return;
    // Left pane
    float y = kNavTop;
    for (auto& c : ctls) {
        if (c.page != -1) continue;
        switch (c.kind) {
        case Kind::Combo: c.box = { kPad, 42, kPaneW - kPad, 42 + kCtlH }; break;
        case Kind::NavItem: c.box = { 8, y, kPaneW - 8, y + kNavH }; y += kNavH + kNavGap; break;
        case Kind::Button: c.box = { kPad, h_ - 96, kPad + TextWidth(c.label, body_.Get()) + 52, h_ - 96 + kCtlH }; break;
        case Kind::Link: c.box = { kPad, h_ - 50, kPad + TextWidth(c.label, body_.Get()) + 26, h_ - 22 }; break;
        default: break;
        }
    }
    for (auto& p : pages_) LayoutPage(p);
    const D2D1_RECT_F cr = ContentRect();
    scrollMax_ = std::max(0.f, pages_[page_].height - (cr.bottom - cr.top));
    scroll_ = std::clamp(scroll_, 0.f, scrollMax_);
}

void Window::LayoutPage(Page& p) {
    const float x0 = kPaneW + 4, x1 = std::min(w_ - 24, x0 + kMaxCardW);
    float y = 0;
    bool first = true;
    for (auto& it : p.items) {
        it.rc = {};
        if (!it.header.empty()) {
            y += first ? 0 : 22;
            it.rc = { x0 + 2, y, x1, y + 24 };
            y += 30;
            first = false;
            continue;
        }
        if (it.ctls.empty()) {   // note
            std::wstring text = it.note;
            if (it.noteKind == kCityEmpty) {
                std::wstring city;
                for (auto& c : ctls) if (c.key == L"WeatherCity") city = Trim(c.value);
                text = city.empty() ? L"The weather line is off until you set a city." : L"";
            } else if (it.noteKind == kMonitorSpec) {
                std::wstring spec;
                doc_.Get(L"Clock", L"Monitor", spec);
                text = L"The same switches as in the tray menu.";
                if (!spec.empty() && _wcsicmp(spec.c_str(), L"all") != 0)
                    text += L" Monitor=" + spec + L" in Overlook.ini picks which monitors are listed.";
            }
            it.note = it.noteKind == kStatic ? it.note : text;
            if (text.empty()) continue;
            const float th = TextHeight(dw_.Get(), text, caption_.Get(), x1 - x0 - 4);
            it.rc = { x0 + 2, y, x1, y + th };
            y += th + 10;
            first = false;
            continue;
        }
        // A row: controls right-aligned, label and description on the left.
        float xr = x1 - kPad;
        float ctlsW = 0;
        std::vector<float> widths(it.ctls.size(), 0);
        for (size_t k = 0; k < it.ctls.size(); ++k) {
            Ctl& c = ctls[it.ctls[k]];
            float w = 0;
            if (c.visible) {
                switch (c.kind) {
                case Kind::Toggle: w = 40; break;
                case Kind::Slider: w = 200; break;
                case Kind::Edit: case Kind::EditCombo: w = c.boxWidth > 0 ? (float)c.boxWidth : 240; break;
                case Kind::Combo: w = c.boxWidth > 0 ? (float)c.boxWidth : 200; break;
                case Kind::Color: w = kCtlH; break;
                case Kind::Button: w = TextWidth(c.label, body_.Get()) + 24; break;
                case Kind::Reset: w = TextWidth(c.label, body_.Get()) + 16; break;
                default: break;
                }
            }
            widths[k] = w;
            if (w > 0) ctlsW += w + 8;
        }
        // On/Off and slider value text sit to the left of their control.
        const Ctl& main = ctls[it.ctls.back()];
        if (main.kind == Kind::Toggle) ctlsW += 40;
        if (main.kind == Kind::Slider) ctlsW += 80;
        int labelOwner = it.ctls[0];
        for (int i : it.ctls) if (ctls[i].kind != Kind::Reset) { labelOwner = i; break; }
        const float textW = (x1 - x0) - 2 * kPad - ctlsW - 16;
        const std::wstring shown = ShownDesc(labelOwner);
        const float descH = TextHeight(dw_.Get(), shown, caption_.Get(), textW);
        const std::wstring& ew = ctls[labelOwner].elsewhere;
        it.extraH = ew.empty() ? 0 : TextHeight(dw_.Get(), ew, caption_.Get(), x1 - x0 - 2 * kPad) + 14;
        const float topH = std::max(shown.empty() ? 48.f : 62.f, 22 + 20 + descH);
        const float cardH = topH + it.extraH;
        const D2D1_RECT_F card{ x0, y, x1, y + cardH };
        const float cy = y + topH / 2;
        for (size_t k = it.ctls.size(); k-- > 0;) {
            Ctl& c = ctls[it.ctls[k]];
            c.card = card;
            if (widths[k] <= 0) { c.box = {}; continue; }
            float hh = kCtlH / 2;
            if (c.kind == Kind::Toggle) hh = 10;
            if (c.kind == Kind::Slider) hh = 10;
            if (c.kind == Kind::Reset) hh = 14;
            c.box = { xr - widths[k], cy - hh, xr, cy + hh };
            xr -= widths[k] + 8;
            if (k + 1 == it.ctls.size() && c.kind == Kind::Toggle) xr -= 40;
            if (k + 1 == it.ctls.size() && c.kind == Kind::Slider) xr -= 80;
        }
        it.rc = card;
        y += cardH + kCardGap;
        first = false;
    }
    p.height = y + 24;
}

// ================================================================== drawing

void Window::Invalidate() { if (hwnd) InvalidateRect(hwnd, nullptr, FALSE); }

void Window::DrawText(const std::wstring& s, IDWriteTextFormat* f, const D2D1_RECT_F& rc, const D2D1_COLOR_F& c,
                      DWRITE_TEXT_ALIGNMENT a, bool center) {
    if (!f || s.empty()) return;
    f->SetTextAlignment(a);
    f->SetParagraphAlignment(center ? DWRITE_PARAGRAPH_ALIGNMENT_CENTER : DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
    brush_->SetColor(c);
    rt_->DrawTextW(s.c_str(), (UINT32)s.size(), f, rc, brush_.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
}

void Window::FillRound(const D2D1_RECT_F& r, float radius, const D2D1_COLOR_F& c) {
    brush_->SetColor(c);
    const D2D1_RECT_F s{ Snap(r.left), Snap(r.top), Snap(r.right), Snap(r.bottom) };
    rt_->FillRoundedRectangle(D2D1::RoundedRect(s, radius, radius), brush_.Get());
}

// A border of `px` physical pixels inside r, on whole pixels.
void Window::StrokeRound(const D2D1_RECT_F& r, float radius, const D2D1_COLOR_F& c, float px) {
    brush_->SetColor(c);
    const float w = px / scale_, h = w / 2;
    const D2D1_RECT_F s{ Snap(r.left) + h, Snap(r.top) + h, Snap(r.right) - h, Snap(r.bottom) - h };
    rt_->DrawRoundedRectangle(D2D1::RoundedRect(s, std::max(0.f, radius - h), std::max(0.f, radius - h)), brush_.Get(), w);
}

void Window::FocusRing(const D2D1_RECT_F& r, float radius) {
    StrokeRound(Inflate(r, 3, 3), radius + 3, t_.focus, std::max(2.f, std::round(2 * scale_)));
}

void Window::Paint() {
    if (!CreateTarget()) return;
    rt_->BeginDraw();
    rt_->SetDpi(dpi_, dpi_);
    rt_->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_CLEARTYPE);
    rt_->Clear(t_.bg);
    DrawContent();
    DrawPane();
    DrawPopup();
    if (rt_->EndDraw() == D2DERR_RECREATE_TARGET) { rt_.Reset(); brush_.Reset(); Invalidate(); }
    if (edit_ && editing_ >= 0) RedrawWindow(edit_, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW);
}

void Window::DrawPane() {
    DrawText(L"Settings for", caption_.Get(), { kPad + 2, 16, kPaneW - kPad, 38 }, t_.text2);
    for (size_t i = 0; i < ctls.size(); ++i) if (ctls[i].page == -1) DrawCtl((int)i, false);
}

void Window::DrawContent() {
    const Page& p = pages_[page_];
    const D2D1_RECT_F cr = ContentRect();
    DrawText(p.name, title_.Get(), { kPaneW + 6, 14, w_ - 24, kTitleH - 14 }, t_.text);
    rt_->PushAxisAlignedClip(cr, D2D1_ANTIALIAS_MODE_ALIASED);
    const float dy = cr.top - scroll_;
    for (auto& it : p.items) {
        if (Empty(it.rc)) continue;
        D2D1_RECT_F r = it.rc; r.top += dy; r.bottom += dy;
        if (r.bottom < cr.top || r.top > cr.bottom) continue;
        if (!it.header.empty()) { DrawText(it.header, strong_.Get(), r, t_.text); continue; }
        if (it.ctls.empty()) { DrawText(it.note, caption_.Get(), r, t_.text2, DWRITE_TEXT_ALIGNMENT_LEADING, false); continue; }
        int lc = it.ctls[0];
        for (int i : it.ctls) if (ctls[i].kind != Kind::Reset) { lc = i; break; }
        const Ctl& c = ctls[lc];
        const bool rowHot = c.kind == Kind::Toggle && hot_ == lc && c.enabled;
        FillRound(r, 4, rowHot ? t_.cardHover : t_.card);
        StrokeRound(r, 4, t_.cardStroke);
        const float textR = (ctls[it.ctls[0]].visible && !Empty(ctls[it.ctls[0]].box) ? ctls[it.ctls[0]].box.left : c.box.left) -
                            (c.kind == Kind::Toggle ? 48 : c.kind == Kind::Slider ? 88 : 16);
        const D2D1_COLOR_F tc = c.enabled ? t_.text : t_.disabled, dc = c.enabled ? t_.text2 : t_.disabled;
        const std::wstring desc = ShownDesc(lc);
        if (it.extraH > 0) {   // monitors that keep their own value, on a line under the row
            const float ly = Snap(r.bottom - it.extraH);
            brush_->SetColor(t_.cardStroke);
            rt_->DrawLine({ r.left + kPad, ly + 0.5f / scale_ }, { r.right - kPad, ly + 0.5f / scale_ }, brush_.Get(), 1 / scale_);
            DrawText(c.elsewhere, caption_.Get(), { r.left + kPad, ly + 2, r.right - kPad, r.bottom - 2 }, dc,
                     DWRITE_TEXT_ALIGNMENT_LEADING, true);
            r.bottom -= it.extraH;
        }
        // Overridden in this monitor's section: an accent dot before the title.
        float tx = r.left + kPad;
        if (c.overridden && scope_ > 0 && !c.appWide) {
            brush_->SetColor(t_.accent);
            rt_->FillEllipse(D2D1::Ellipse({ tx + 3, r.top + (desc.empty() ? (r.bottom - r.top) / 2 : 21.f) }, 3, 3), brush_.Get());
            tx += 12;
        }
        if (desc.empty()) {
            DrawText(c.label, body_.Get(), { tx, r.top, textR, r.bottom }, tc);
        } else {
            const float descH = TextHeight(dw_.Get(), desc, caption_.Get(), textR - r.left - kPad);
            const float top = r.top + ((r.bottom - r.top) - (20 + descH)) / 2;
            DrawText(c.label, body_.Get(), { tx, top, textR, top + 20 }, tc);
            DrawText(desc, caption_.Get(), { r.left + kPad, top + 20, textR, top + 20 + descH + 2 }, dc,
                     DWRITE_TEXT_ALIGNMENT_LEADING, false);
        }
        for (int i : it.ctls) if (ctls[i].visible && !Empty(ctls[i].box)) DrawCtl(i, true);
    }
    rt_->PopAxisAlignedClip();
    // Scroll bar: a thin overlay thumb, wider while the mouse is near it.
    if (scrollMax_ > 0) {
        const float trackH = cr.bottom - cr.top - 8, viewH = cr.bottom - cr.top;
        const float thumbH = std::max(32.f, trackH * viewH / (viewH + scrollMax_));
        const float top = cr.top + 4 + (trackH - thumbH) * (scroll_ / scrollMax_);
        const bool wide = thumbDrag_ || hot_ == -100;
        const float wdt = wide ? 6.f : 3.f;
        FillRound({ w_ - 6 - wdt, top, w_ - 6, top + thumbH }, wdt / 2, t_.text2);
    }
}

void Window::DrawCtl(int i, bool inContent) {
    const Ctl& c = ctls[i];
    if (!c.visible) return;
    const D2D1_RECT_F r = ViewRect(i);
    const bool hot = hot_ == i && c.enabled, focused = focus_ == i && kbdCues_ && GetFocus() == hwnd;
    const D2D1_COLOR_F tc = c.enabled ? t_.text : t_.disabled;
    const float cy = (r.top + r.bottom) / 2;
    (void)inContent;
    switch (c.kind) {
    case Kind::NavItem: {
        const bool sel = c.owner == page_;
        if (sel || hot) FillRound(r, 4, sel ? t_.selFill : t_.ctlHover);
        if (sel) FillRound({ r.left, cy - 8, r.left + 3, cy + 8 }, 1.5f, t_.accent);
        if (t_.contrast && sel) StrokeRound(r, 4, t_.text);
        wchar_t ic[2] = { pages_[c.owner].icon, 0 };
        DrawText(ic, icon_.Get(), { r.left + 14, r.top, r.left + 34, r.bottom }, t_.text);
        DrawText(c.label, body_.Get(), { r.left + 46, r.top, r.right - 8, r.bottom }, t_.text);
        if (focused) FocusRing(r, 4);
        break;
    }
    case Kind::Combo: case Kind::EditCombo: case Kind::Edit: {
        const bool editing = editing_ == i;
        const D2D1_COLOR_F fill = editing ? (t_.dark ? Rgb(31, 31, 31) : Rgb(255, 255, 255)) : hot ? t_.ctlHover : t_.ctlFill;
        FillRound(r, 4, t_.contrast ? t_.bg : fill);
        StrokeRound(r, 4, c.enabled ? t_.ctlStroke : t_.cardStroke);
        if (t_.contrast) StrokeRound(r, 4, tc);
        if (c.kind != Kind::Combo && c.enabled) {   // text boxes: a bottom line, accent and thicker while editing
            const float th = (editing ? std::round(2 * scale_) : 1) / scale_;
            brush_->SetColor(editing ? t_.accent : t_.ctlStrokeBottom);
            rt_->PushAxisAlignedClip({ Snap(r.left), Snap(r.bottom) - th, Snap(r.right), Snap(r.bottom) }, D2D1_ANTIALIAS_MODE_ALIASED);
            rt_->FillRoundedRectangle(D2D1::RoundedRect({ Snap(r.left), Snap(r.top), Snap(r.right), Snap(r.bottom) }, 4, 4), brush_.Get());
            rt_->PopAxisAlignedClip();
        }
        const float textR = r.right - (c.kind == Kind::Edit ? 10 : 34);
        if (!editing) {
            std::wstring v = ValueText(i);
            if (c.kind == Kind::Edit && c.key == L"WeatherCity" && v.empty()) {
                DrawText(L"Not set", body_.Get(), { r.left + 11, r.top, textR, r.bottom }, t_.disabled);
            } else {
                DrawText(v, body_.Get(), { r.left + 11, r.top, textR, r.bottom }, tc);
            }
        }
        if (c.kind != Kind::Edit) {
            const wchar_t chev[2] = { 0xE70D, 0 };
            DrawText(chev, iconSmall_.Get(), { r.right - 30, r.top, r.right - 10, r.bottom }, c.enabled ? t_.text2 : t_.disabled,
                     DWRITE_TEXT_ALIGNMENT_CENTER);
        }
        if (focused && !editing) FocusRing(r, 4);
        break;
    }
    case Kind::Toggle: {
        const bool on = ToggleOn(i);
        const D2D1_RECT_F sw{ r.right - 40, cy - 10, r.right, cy + 10 };
        DrawText(on ? L"On" : L"Off", bodyRight_.Get(), { sw.left - 48, r.top - 4, sw.left - 12, r.bottom + 4 }, tc,
                 DWRITE_TEXT_ALIGNMENT_TRAILING);
        const bool rowHot = hot_ == i && c.enabled;
        if (on) {
            FillRound(sw, 10, c.enabled ? t_.accent : t_.disabled);
            if (rowHot && !t_.contrast) FillRound(sw, 10, t_.dark ? Rgb(255, 255, 255, 0.08f) : Rgb(0, 0, 0, 0.08f));
            brush_->SetColor(t_.contrast ? t_.onAccent : t_.onAccent);
            const float k = rowHot ? 7.f : 6.f;
            rt_->FillEllipse(D2D1::Ellipse({ sw.right - 10, cy }, k, k), brush_.Get());
        } else {
            if (rowHot) FillRound(sw, 10, t_.dark ? Rgb(255, 255, 255, 0.06f) : Rgb(0, 0, 0, 0.04f));
            StrokeRound(sw, 10, c.enabled ? t_.toggleOff : t_.disabled);
            brush_->SetColor(c.enabled ? t_.toggleOff : t_.disabled);
            const float k = rowHot ? 6.f : 5.f;
            rt_->FillEllipse(D2D1::Ellipse({ sw.left + 10, cy }, k, k), brush_.Get());
        }
        if (focused) FocusRing(sw, 10);
        break;
    }
    case Kind::Slider: {
        const double v = SliderValue(i);
        const float f = (float)((v - c.lo) / (c.hi - c.lo));
        const float x = r.left + 10 + f * (r.right - r.left - 20);
        DrawText(ValueText(i), bodyRight_.Get(), { r.left - 90, r.top - 6, r.left - 10, r.bottom + 6 }, tc, DWRITE_TEXT_ALIGNMENT_TRAILING);
        FillRound({ r.left + 10, cy - 2, r.right - 10, cy + 2 }, 2, c.enabled ? t_.track : t_.disabled);
        FillRound({ r.left + 10, cy - 2, x, cy + 2 }, 2, c.enabled ? t_.accent : t_.disabled);
        brush_->SetColor(t_.contrast ? t_.bg : t_.dark ? Rgb(69, 69, 69) : Rgb(255, 255, 255));
        rt_->FillEllipse(D2D1::Ellipse({ x, cy }, 10, 10), brush_.Get());
        brush_->SetColor(t_.contrast ? t_.text : t_.ctlStroke);
        rt_->DrawEllipse(D2D1::Ellipse({ x, cy }, 9.5f, 9.5f), brush_.Get(), 1 / scale_);
        brush_->SetColor(c.enabled ? t_.accent : t_.disabled);
        const float k = drag_ == i ? 5.f : hot ? 7.f : 6.f;
        rt_->FillEllipse(D2D1::Ellipse({ x, cy }, k, k), brush_.Get());
        if (focused) FocusRing({ r.left, r.top - 2, r.right, r.bottom + 2 }, 6);
        break;
    }
    case Kind::Color: {
        FillRound(r, 4, FromRef(ParseColor(c.value, RGB(255, 255, 255))));
        StrokeRound(r, 4, hot ? t_.text2 : t_.dark ? Rgb(120, 120, 120) : Rgb(160, 160, 160));
        if (focused) FocusRing(r, 4);
        break;
    }
    case Kind::Button: {
        const bool undo = c.action == kUndo;
        FillRound(r, 4, t_.contrast ? t_.bg : hot ? t_.ctlHover : t_.ctlFill);
        StrokeRound(r, 4, t_.contrast ? tc : t_.ctlStroke);
        if (undo) {
            const wchar_t ic[2] = { 0xE7A7, 0 };
            DrawText(ic, iconSmall_.Get(), { r.left + 12, r.top, r.left + 28, r.bottom }, tc);
            DrawText(c.label, body_.Get(), { r.left + 36, r.top, r.right - 8, r.bottom }, tc);
        } else {
            DrawText(c.label, body_.Get(), r, tc, DWRITE_TEXT_ALIGNMENT_CENTER);
        }
        if (focused) FocusRing(r, 4);
        break;
    }
    case Kind::Link: case Kind::Reset: {
        D2D1_COLOR_F col = c.enabled ? t_.link : t_.disabled;
        if (hot && !t_.contrast) col.a = 0.8f;
        if (c.kind == Kind::Link) {
            const wchar_t ic[2] = { 0xE8A7, 0 };
            DrawText(c.label, body_.Get(), { r.left + 2, r.top, r.right - 24, r.bottom }, col);
            DrawText(ic, iconSmall_.Get(), { r.right - 20, r.top, r.right - 4, r.bottom }, col);
        } else {
            DrawText(c.label, body_.Get(), r, col, DWRITE_TEXT_ALIGNMENT_CENTER);
        }
        if (hot) {
            brush_->SetColor(col);
            const float uy = Snap(cy + 9) + 0.5f / scale_;
            const float l = c.kind == Kind::Link ? r.left + 2 : r.left + 8;
            const float rr = c.kind == Kind::Link ? r.left + 2 + TextWidth(c.label, body_.Get()) : r.right - 8;
            rt_->DrawLine({ l, uy }, { rr, uy }, brush_.Get(), 1 / scale_);
        }
        if (focused) FocusRing(r, 4);
        break;
    }
    }
}

void Window::DrawPopup() {
    if (popup_.ctl < 0) return;
    const Ctl& c = ctls[popup_.ctl];
    const D2D1_RECT_F r = popup_.rc;
    // A soft shadow under the list.
    for (int k = 1; k <= 4; ++k)
        FillRound(Inflate({ r.left, r.top + 2, r.right, r.bottom + 2 }, (float)k * 1.5f, (float)k * 1.5f), 8 + k * 1.5f,
                  Rgb(0, 0, 0, t_.dark ? 0.09f : 0.035f));
    FillRound(r, 8, t_.popup);
    StrokeRound(r, 8, t_.contrast ? t_.text : t_.dark ? Rgb(20, 20, 20) : Rgb(220, 220, 220));
    for (size_t o = 0; o < c.options.size(); ++o) {
        const D2D1_RECT_F it{ r.left + 4, r.top + 4 + o * popup_.itemH, r.right - 4, r.top + 4 + (o + 1) * popup_.itemH };
        const bool sel = (int)o == popup_.sel, hot = (int)o == popup_.hot;
        if (hot || sel) FillRound(Inflate(it, 0, -1), 4, hot && t_.contrast ? t_.accent : t_.selFill);
        if (sel) FillRound({ it.left, (it.top + it.bottom) / 2 - 8, it.left + 3, (it.top + it.bottom) / 2 + 8 }, 1.5f, t_.accent);
        const D2D1_COLOR_F tc = hot && t_.contrast ? t_.onAccent : t_.text;
        const std::wstring& label = c.options[o].label.empty() ? c.options[o].value : c.options[o].label;
        if (c.kind == Kind::EditCombo) {
            DrawText(c.options[o].value, caption_.Get(), { it.left + 12, it.top, it.right - 12, it.bottom }, hot && t_.contrast ? tc : t_.text2,
                     DWRITE_TEXT_ALIGNMENT_TRAILING);
            DrawText(label, body_.Get(), { it.left + 12, it.top, it.right - 120, it.bottom }, tc);
        } else {
            DrawText(label, body_.Get(), { it.left + 12, it.top, it.right - 12, it.bottom }, tc);
        }
    }
}

// ================================================================== input

int Window::HitTest(float x, float y) const {
    if (popup_.ctl >= 0) return -1;
    const D2D1_RECT_F cr = ContentRect();
    int card = -1;
    for (size_t i = 0; i < ctls.size(); ++i) {
        const Ctl& c = ctls[i];
        if (!c.visible || c.page == -2 || (c.page >= 0 && c.page != page_)) continue;
        if (c.page >= 0 && !Inside(cr, x, y)) continue;
        D2D1_RECT_F r = ViewRect((int)i);
        if (c.kind == Kind::Slider) r = Inflate(r, 4, 6);
        if (c.kind == Kind::Toggle) r = { r.left - 48, r.top - 6, r.right, r.bottom + 6 };
        if (Inside(r, x, y)) return (int)i;
        if (c.kind == Kind::Toggle && c.page >= 0) {
            D2D1_RECT_F k = c.card;
            const float dy = cr.top - scroll_;
            k.top += dy; k.bottom += dy;
            if (Inside(k, x, y)) card = (int)i;
        }
    }
    return card;
}

void Window::SetFocusCtl(int i, bool keyboard) {
    if (i < 0 || i >= (int)ctls.size()) return;
    if (editing_ >= 0 && editing_ != i) EndEdit(true);
    if (GetFocus() != hwnd && editing_ < 0) SetFocus(hwnd);
    focus_ = i;
    EnsureVisible(i);
    if (keyboard && (ctls[i].kind == Kind::Edit || ctls[i].kind == Kind::EditCombo) && ctls[i].enabled) StartEdit(i);
    else NotifyFocus();
    Invalidate();
}

void Window::MoveFocus(int dir) {
    const auto order = FocusOrder();
    if (order.empty()) return;
    auto at = std::find(order.begin(), order.end(), focus_);
    int pos = at == order.end() ? (dir > 0 ? -1 : 0) : (int)(at - order.begin());
    pos = (pos + dir + (int)order.size()) % (int)order.size();
    SetFocusCtl(order[pos], true);
}

void Window::Activate(int i) {
    Ctl& c = ctls[i];
    if (!c.enabled || !c.visible) return;
    switch (c.kind) {
    case Kind::NavItem: SetPage(c.owner); break;
    case Kind::Toggle: {
        const bool on = !ToggleOn(i);
        if (!c.monitorId.empty()) WriteShowMonitor(i, on);
        else { Write(i, on ? L"1" : L"0"); UiaRaiseToggle(*this, i, on); }
        break;
    }
    case Kind::Combo: if (popup_.ctl == i) ClosePopup(false); else OpenPopup(i); break;
    case Kind::Edit: case Kind::EditCombo: StartEdit(i); break;
    case Kind::Color: PickColor(i); break;
    case Kind::Reset: ResetOverride(i); break;
    case Kind::Button: case Kind::Link:
        if (c.action == kUndo) Undo();
        else if (c.action == kOpenIni) ShellExecuteW(hwnd, L"open", iniPath_.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        else if (c.action == kAppsDefault)
            for (size_t j = 0; j < ctls.size(); ++j) if (ctls[j].key == L"NowPlayingApps") Write((int)j, kDefaultApps);
        break;
    case Kind::Slider: break;
    }
}

void Window::SetSlider(int i, double v, bool commitNow) {
    Ctl& c = ctls[i];
    v = std::clamp(v, c.lo, c.hi);
    v = c.lo + std::round((v - c.lo) / c.step) * c.step;
    v = std::clamp(v, c.lo, c.hi);
    const std::wstring s = Fmt(v, c.decimals);
    if (sliderPending_ && sliderCtl_ != i) CommitSlider();
    if (s != c.value) {
        c.value = s;
        sliderCtl_ = i;
        sliderPending_ = true;
        NotifyValue(i);
        UpdateEnabled();
        Invalidate();
    }
    if (commitNow) CommitSlider();
    else if (sliderPending_) SetTimer(hwnd, TIMER_SLIDER, 150, nullptr);
}

void Window::CommitSlider() {
    KillTimer(hwnd, TIMER_SLIDER);
    if (!sliderPending_) return;
    sliderPending_ = false;
    const int i = sliderCtl_;
    const std::wstring v = ctls[i].value;
    ctls[i].value = Effective(ctls[i], nullptr);   // so Write sees the change
    Write(i, v);
}

void Window::StepSlider(int i, double steps, bool big) {
    const Ctl& c = ctls[i];
    SetSlider(i, SliderValue(i) + steps * (big ? c.big : c.step), false);
}

void Window::SliderFromX(int i, float x) {
    const D2D1_RECT_F r = ViewRect(i);
    const Ctl& c = ctls[i];
    const double f = std::clamp((x - (r.left + 10)) / (r.right - r.left - 20), 0.f, 1.f);
    SetSlider(i, c.lo + f * (c.hi - c.lo), false);
}

void Window::ComboStep(int i, int dir) {
    const Ctl& c = ctls[i];
    int cur = -1;
    for (size_t o = 0; o < c.options.size(); ++o) if (_wcsicmp(c.options[o].value.c_str(), c.value.c_str()) == 0) cur = (int)o;
    const int next = std::clamp(cur + dir, 0, (int)c.options.size() - 1);
    if (next != cur) SelectOption(i, next);
}

void Window::SelectOption(int i, int opt) {
    Ctl& c = ctls[i];
    if (opt < 0 || opt >= (int)c.options.size()) return;
    if (c.action == kScope) { SetScope(opt); return; }
    if (editing_ == i) {
        SetWindowTextW(edit_, c.options[opt].value.c_str());
        EndEdit(true);
        return;
    }
    Write(i, c.options[opt].value);
}

void Window::OpenPopup(int i) {
    Ctl& c = ctls[i];
    if (c.options.empty()) return;
    if (editing_ >= 0 && editing_ != i) EndEdit(true);
    EnsureVisible(i);
    popup_.ctl = i;
    popup_.sel = -1;
    for (size_t o = 0; o < c.options.size(); ++o) if (_wcsicmp(c.options[o].value.c_str(), c.value.c_str()) == 0) popup_.sel = (int)o;
    popup_.hot = popup_.sel >= 0 ? popup_.sel : 0;
    popup_.itemH = 36;
    const D2D1_RECT_F b = ViewRect(i);
    float wdt = b.right - b.left;
    for (auto& o : c.options) {
        float need = TextWidth(o.label.empty() ? o.value : o.label, body_.Get()) + 40;
        if (c.kind == Kind::EditCombo) need += TextWidth(o.value, caption_.Get()) + 24;
        wdt = std::max(wdt, need);
    }
    const float hgt = c.options.size() * popup_.itemH + 8;
    D2D1_RECT_F r{ b.right - wdt, b.bottom + 4, b.right, b.bottom + 4 + hgt };
    if (r.left < 8) { r.right += 8 - r.left; r.left = 8; }
    if (r.bottom > h_ - 8) { r.bottom = b.top - 4; r.top = r.bottom - hgt; }
    if (r.top < 8) { r.top = 8; r.bottom = r.top + hgt; }
    popup_.rc = r;
    hot_ = -1;
    Invalidate();
    NotifyValue(i);
}

void Window::ClosePopup(bool accept) {
    if (popup_.ctl < 0) return;
    const int i = popup_.ctl, opt = popup_.hot;
    popup_.ctl = -1;
    Invalidate();
    if (accept) SelectOption(i, opt);
    NotifyValue(i);
}

void Window::StartEdit(int i, bool selectAll) {
    Ctl& c = ctls[i];
    if (editing_ == i) return;
    if (editing_ >= 0) EndEdit(true);
    EnsureVisible(i);
    editing_ = i;
    focus_ = i;
    editLive_ = c.value;
    const D2D1_RECT_F r = ViewRect(i);
    const float right = r.right - (c.kind == Kind::EditCombo ? 34 : 8);
    TEXTMETRICW tm{};
    HDC dc = GetDC(edit_);
    HGDIOBJ old = SelectObject(dc, editFont_);
    GetTextMetricsW(dc, &tm);
    SelectObject(dc, old);
    ReleaseDC(edit_, dc);
    const int hpx = tm.tmHeight + 2;
    const int x = (int)std::lround((r.left + 10) * scale_), y = (int)std::lround((r.top + r.bottom) / 2 * scale_) - hpx / 2;
    SetWindowTextW(edit_, c.value.c_str());
    SetWindowPos(edit_, HWND_TOP, x, y, (int)std::lround(right * scale_) - x, hpx, SWP_SHOWWINDOW);
    SendMessageW(edit_, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELPARAM(1, 1));
    // Narrator: the edit box is named after its setting.
    ComPtr<IAccPropServices> props;
    if (SUCCEEDED(CoCreateInstance(CLSID_AccPropServices, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&props)))) {
        const std::wstring name = UiaName(i);
        props->SetHwndPropStr(edit_, (DWORD)OBJID_CLIENT, CHILDID_SELF, PROPID_ACC_NAME, name.c_str());
        if (!c.desc.empty()) props->SetHwndPropStr(edit_, (DWORD)OBJID_CLIENT, CHILDID_SELF, PROPID_ACC_DESCRIPTION, c.desc.c_str());
    }
    SetFocus(edit_);
    SendMessageW(edit_, EM_SETSEL, selectAll ? 0 : (WPARAM)-1, selectAll ? -1 : 0);
    Invalidate();
}

void Window::EndEdit(bool commit) {
    if (editing_ < 0) return;
    const int i = editing_;
    editing_ = -1;
    if (popup_.ctl == i) ClosePopup(false);
    const int n = GetWindowTextLengthW(edit_);
    std::wstring text(n, L'\0');
    if (n) GetWindowTextW(edit_, text.data(), n + 1);
    ShowWindow(edit_, SW_HIDE);
    if (GetFocus() == edit_) SetFocus(hwnd);
    editLive_.clear();
    if (commit) {
        std::wstring v = Trim(text);
        if (ctls[i].key == L"Color") {
            v = NormalColor(v);
            if (v.empty()) MessageBeep(MB_ICONWARNING);
        }
        if (!(ctls[i].key == L"Color" && v.empty()) && v != ctls[i].value) Write(i, v);
    }
    LoadValues();
    NotifyFocus();
}

void Window::SetPage(int p) {
    p = (p + (int)pages_.size()) % (int)pages_.size();
    if (p == page_) return;
    EndEdit(true);
    ClosePopup(false);
    if (sliderPending_) CommitSlider();
    const bool navFocused = focus_ >= 0 && ctls[focus_].kind == Kind::NavItem;
    const bool pageFocused = focus_ >= 0 && ctls[focus_].page >= 0;
    page_ = p;
    scroll_ = 0;
    Layout();
    if (navFocused || pageFocused)
        for (size_t i = 0; i < ctls.size(); ++i)
            if (ctls[i].kind == Kind::NavItem && ctls[i].owner == p) focus_ = (int)i;
    NotifyFocus();
    Invalidate();
}

void Window::ScrollTo(float y) {
    y = std::clamp(y, 0.f, scrollMax_);
    if (y == scroll_) return;
    if (editing_ >= 0) EndEdit(true);
    if (popup_.ctl >= 0) ClosePopup(false);
    scroll_ = y;
    Invalidate();
}

void Window::EnsureVisible(int i) {
    if (i < 0 || ctls[i].page < 0) return;
    const D2D1_RECT_F cr = ContentRect();
    const float viewH = cr.bottom - cr.top;
    const D2D1_RECT_F& card = ctls[i].card;
    if (card.top - 8 < scroll_) ScrollTo(card.top - 8);
    else if (card.bottom + 8 > scroll_ + viewH) ScrollTo(card.bottom + 8 - viewH);
}

void Window::PickColor(int i) {
    static COLORREF custom[16] = {};
    CHOOSECOLORW cc{ sizeof(cc) };
    cc.hwndOwner = hwnd;
    cc.rgbResult = ParseColor(ctls[i].value, RGB(255, 255, 255));
    cc.lpCustColors = custom;
    cc.Flags = CC_RGBINIT | CC_FULLOPEN | CC_ANYCOLOR;
    if (!ChooseColorW(&cc)) return;
    wchar_t hex[8];
    swprintf_s(hex, L"#%02X%02X%02X", GetRValue(cc.rgbResult), GetGValue(cc.rgbResult), GetBValue(cc.rgbResult));
    Write(i, hex);
}

void Window::OnKey(UINT vk) {
    kbdCues_ = true;
    const bool ctrl = GetKeyState(VK_CONTROL) < 0, shift = GetKeyState(VK_SHIFT) < 0, alt = GetKeyState(VK_MENU) < 0;
    if (popup_.ctl >= 0) {
        const int n = (int)ctls[popup_.ctl].options.size();
        switch (vk) {
        case VK_UP: popup_.hot = std::max(0, popup_.hot - 1); Invalidate(); break;
        case VK_DOWN: if (alt) { ClosePopup(true); break; } popup_.hot = std::min(n - 1, popup_.hot + 1); Invalidate(); break;
        case VK_HOME: popup_.hot = 0; Invalidate(); break;
        case VK_END: popup_.hot = n - 1; Invalidate(); break;
        case VK_RETURN: case VK_SPACE: case VK_TAB: case VK_F4: ClosePopup(true); break;
        case VK_ESCAPE: ClosePopup(false); break;
        }
        return;
    }
    if (vk == VK_TAB) {
        if (ctrl) SetPage(page_ + (shift ? -1 : 1));
        else MoveFocus(shift ? -1 : 1);
        return;
    }
    if (ctrl && (vk == VK_PRIOR || vk == VK_NEXT)) { SetPage(page_ + (vk == VK_PRIOR ? -1 : 1)); return; }
    if (vk == VK_ESCAPE) { DestroyWindow(hwnd); return; }
    const int f = focus_;
    const float viewH = ContentRect().bottom - ContentRect().top;
    if (f < 0) {
        if (vk == VK_PRIOR || vk == VK_NEXT) ScrollTo(scroll_ + (vk == VK_PRIOR ? -1 : 1) * (viewH - 48));
        else if (vk == VK_DOWN || vk == VK_UP) MoveFocus(vk == VK_DOWN ? 1 : -1);
        return;
    }
    const Ctl& c = ctls[f];
    switch (c.kind) {
    case Kind::NavItem:
        if (vk == VK_UP || vk == VK_LEFT) SetPage(page_ - 1);
        else if (vk == VK_DOWN || vk == VK_RIGHT) SetPage(page_ + 1);
        else if (vk == VK_HOME) SetPage(0);
        else if (vk == VK_END) SetPage((int)pages_.size() - 1);
        break;
    case Kind::Slider:
        if (!c.enabled) break;
        if (vk == VK_LEFT || vk == VK_DOWN) StepSlider(f, -1, false);
        else if (vk == VK_RIGHT || vk == VK_UP) StepSlider(f, 1, false);
        else if (vk == VK_PRIOR) StepSlider(f, 1, true);
        else if (vk == VK_NEXT) StepSlider(f, -1, true);
        else if (vk == VK_HOME) SetSlider(f, c.lo, false);
        else if (vk == VK_END) SetSlider(f, c.hi, false);
        break;
    case Kind::Combo:
        if ((alt && vk == VK_DOWN) || vk == VK_F4 || vk == VK_RETURN || vk == VK_SPACE) OpenPopup(f);
        else if (vk == VK_UP || vk == VK_DOWN) ComboStep(f, vk == VK_UP ? -1 : 1);
        break;
    case Kind::EditCombo:
        if ((alt && vk == VK_DOWN) || vk == VK_F4) OpenPopup(f);
        else if (vk == VK_RETURN || vk == VK_SPACE) StartEdit(f);
        else if (vk == VK_UP || vk == VK_DOWN) MoveFocus(vk == VK_DOWN ? 1 : -1);
        break;
    default:
        if (vk == VK_SPACE || vk == VK_RETURN) Activate(f);
        else if (vk == VK_UP || vk == VK_DOWN) MoveFocus(vk == VK_DOWN ? 1 : -1);
        else if (vk == VK_PRIOR || vk == VK_NEXT) ScrollTo(scroll_ + (vk == VK_PRIOR ? -1 : 1) * (viewH - 48));
        break;
    }
}

void Window::OnMouseDown(float x, float y, bool dbl) {
    (void)dbl;
    kbdCues_ = false;
    if (popup_.ctl >= 0) {
        if (Inside(popup_.rc, x, y)) {
            const int o = (int)((y - popup_.rc.top - 4) / popup_.itemH);
            if (o >= 0 && o < (int)ctls[popup_.ctl].options.size()) { popup_.hot = o; ClosePopup(true); }
        } else {
            ClosePopup(false);
        }
        return;
    }
    // The scroll bar
    const D2D1_RECT_F cr = ContentRect();
    if (scrollMax_ > 0 && x >= w_ - 16 && y >= cr.top) {
        const float trackH = cr.bottom - cr.top - 8, viewH = cr.bottom - cr.top;
        const float thumbH = std::max(32.f, trackH * viewH / (viewH + scrollMax_));
        const float top = cr.top + 4 + (trackH - thumbH) * (scroll_ / scrollMax_);
        if (y >= top && y < top + thumbH) { thumbDrag_ = true; thumbGrab_ = y - top; SetCapture(hwnd); }
        else ScrollTo(scroll_ + (y < top ? -1 : 1) * (viewH - 48));
        return;
    }
    const int i = HitTest(x, y);
    if (i < 0) { if (editing_ >= 0) EndEdit(true); return; }
    Ctl& c = ctls[i];
    if (!c.enabled) return;
    if (c.kind == Kind::Edit || (c.kind == Kind::EditCombo && x < ViewRect(i).right - 34)) {
        if (editing_ != i) StartEdit(i, false);
        // place the caret where the click was
        POINT pt{ (LONG)std::lround(x * scale_), (LONG)std::lround(y * scale_) };
        MapWindowPoints(hwnd, edit_, &pt, 1);
        const LRESULT ch = SendMessageW(edit_, EM_CHARFROMPOS, 0, MAKELPARAM(pt.x, pt.y));
        const int pos = LOWORD(ch);
        SendMessageW(edit_, EM_SETSEL, pos, pos);
        return;
    }
    if (editing_ >= 0 && editing_ != i) EndEdit(true);
    SetFocusCtl(i, false);
    pressed_ = i;
    SetCapture(hwnd);
    if (c.kind == Kind::Slider) { drag_ = i; SliderFromX(i, x); }
    if (c.kind == Kind::Combo || c.kind == Kind::EditCombo) { pressed_ = -1; ReleaseCapture(); OpenPopup(i); }
}

void Window::OnMouseMove(float x, float y) {
    if (thumbDrag_) {
        const D2D1_RECT_F cr = ContentRect();
        const float trackH = cr.bottom - cr.top - 8, viewH = cr.bottom - cr.top;
        const float thumbH = std::max(32.f, trackH * viewH / (viewH + scrollMax_));
        ScrollTo((y - thumbGrab_ - cr.top - 4) / std::max(1.f, trackH - thumbH) * scrollMax_);
        return;
    }
    if (drag_ >= 0) { SliderFromX(drag_, x); return; }
    int h;
    if (popup_.ctl >= 0) {
        if (Inside(popup_.rc, x, y)) {
            const int o = (int)((y - popup_.rc.top - 4) / popup_.itemH);
            if (o >= 0 && o < (int)ctls[popup_.ctl].options.size() && o != popup_.hot) { popup_.hot = o; Invalidate(); }
        }
        return;
    }
    h = (scrollMax_ > 0 && x >= w_ - 16 && y >= ContentRect().top) ? -100 : HitTest(x, y);
    if (h != hot_) { hot_ = h; Invalidate(); }
    TRACKMOUSEEVENT tme{ sizeof(tme), TME_LEAVE, hwnd, 0 };
    TrackMouseEvent(&tme);
}

void Window::OnMouseUp(float x, float y) {
    if (GetCapture() == hwnd) ReleaseCapture();
    if (thumbDrag_) { thumbDrag_ = false; Invalidate(); return; }
    if (drag_ >= 0) { drag_ = -1; CommitSlider(); pressed_ = -1; Invalidate(); return; }
    const int i = pressed_;
    pressed_ = -1;
    if (i >= 0 && HitTest(x, y) == i) Activate(i);
}

void Window::OnWheel(int delta) {
    ScrollTo(scroll_ - delta / (float)WHEEL_DELTA * 48 * 3 / 2);
    POINT pt; GetCursorPos(&pt); ScreenToClient(hwnd, &pt);
    OnMouseMove(pt.x / scale_, pt.y / scale_);
}

void Window::NotifyFocus() {
    if (focus_ >= 0 && editing_ < 0 && GetFocus() == hwnd) UiaRaiseFocus(*this, focus_);
}

void Window::NotifyValue(int i) { UiaRaiseValue(*this, i); }

// ================================================================== window

LRESULT CALLBACK Window::EditProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    Window* w = (Window*)GetPropW(h, L"OverlookSettings");
    if (!w) return DefWindowProcW(h, msg, wp, lp);
    if (msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN) {
        const bool alt = GetKeyState(VK_MENU) < 0, ctrl = GetKeyState(VK_CONTROL) < 0, shift = GetKeyState(VK_SHIFT) < 0;
        const int i = w->editing_;
        if (w->popup_.ctl >= 0 && (wp == VK_UP || wp == VK_DOWN || wp == VK_RETURN || wp == VK_ESCAPE || wp == VK_F4 || wp == VK_TAB)) {
            w->OnKey((UINT)wp);
            return 0;
        }
        switch (wp) {
        case VK_TAB:
            w->EndEdit(true);
            if (ctrl) w->SetPage(w->page_ + (shift ? -1 : 1));
            else PostMessageW(w->hwnd, WM_APP_MOVEFOCUS, shift ? (WPARAM)-1 : 1, 0);
            return 0;
        case VK_RETURN: w->kbdCues_ = true; w->EndEdit(true); return 0;
        case VK_ESCAPE: w->kbdCues_ = true; w->EndEdit(false); return 0;
        case VK_F4:
            if (i >= 0 && w->ctls[i].kind == Kind::EditCombo) { w->OpenPopup(i); return 0; }
            break;
        case VK_DOWN:
            if (alt && i >= 0 && w->ctls[i].kind == Kind::EditCombo) { w->OpenPopup(i); return 0; }
            break;
        }
    }
    if (msg == WM_CHAR && (wp == L'\t' || wp == L'\r' || wp == 0x1B)) return 0;   // no beep
    const WNDPROC base = w->editBase_;
    if (msg == WM_NCDESTROY) { SetWindowLongPtrW(h, GWLP_WNDPROC, (LONG_PTR)base); RemovePropW(h, L"OverlookSettings"); }
    return CallWindowProcW(base, h, msg, wp, lp);
}

LRESULT CALLBACK Window::Proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    Window* w = (Window*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    if (msg == WM_NCCREATE) {
        w = (Window*)((CREATESTRUCTW*)lp)->lpCreateParams;
        w->hwnd = hwnd;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)w);
    }
    if (!w) return DefWindowProcW(hwnd, msg, wp, lp);
    if (msg == WM_NCDESTROY) {
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        if (g_win.get() == w) g_win.reset();
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
    return w->Handle(msg, wp, lp);
}

static void ApplyTitleBar(HWND hwnd, const Theme& t) {
    const BOOL dark = t.dark;
    DwmSetWindowAttribute(hwnd, 20 /*DWMWA_USE_IMMERSIVE_DARK_MODE*/, &dark, sizeof(dark));
    const COLORREF cap = t.contrast ? 0xFFFFFFFF /*DWMWA_COLOR_DEFAULT*/ : ToRef(t.bg);
    DwmSetWindowAttribute(hwnd, 35 /*DWMWA_CAPTION_COLOR*/, &cap, sizeof(cap));
}

LRESULT Window::Handle(UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE: {
        dpi_ = (float)GetDpiForWindow(hwnd);
        scale_ = dpi_ / 96.f;
        D2D1_FACTORY_OPTIONS fo{};
        D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory1), &fo, (void**)d2d_.GetAddressOf());
        DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), (IUnknown**)dw_.GetAddressOf());
        if (!d2d_ || !dw_) return -1;
        LoadTheme();
        ApplyTitleBar(hwnd, t_);
        edit_ = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | ES_AUTOHSCROLL | ES_LEFT, 0, 0, 10, 10, hwnd, (HMENU)1, inst_, nullptr);
        SetPropW(edit_, L"OverlookSettings", (HANDLE)this);
        editBase_ = (WNDPROC)SetWindowLongPtrW(edit_, GWLP_WNDPROC, (LONG_PTR)EditProc);
        SendMessageW(edit_, EM_LIMITTEXT, 500, 0);   // the clock reads values up to 511 characters
        CreateFonts();
        RECT rc; GetClientRect(hwnd, &rc);
        w_ = rc.right / scale_; h_ = rc.bottom / scale_;
        BuildControls();
        LoadValues();
        focus_ = -1;
        return 0;
    }
    case WM_SIZE: {
        w_ = LOWORD(lp) / scale_; h_ = HIWORD(lp) / scale_;
        if (rt_) rt_->Resize(D2D1::SizeU(LOWORD(lp), HIWORD(lp)));
        ClosePopup(false);
        if (editing_ >= 0) EndEdit(true);
        Layout();
        Invalidate();
        return 0;
    }
    case WM_GETMINMAXINFO: {
        auto* mm = (MINMAXINFO*)lp;
        const UINT dpi = GetDpiForWindow(hwnd);
        RECT r{ 0, 0, MulDiv(720, dpi, 96), MulDiv(480, dpi, 96) };
        AdjustWindowRectExForDpi(&r, WS_OVERLAPPEDWINDOW, FALSE, 0, dpi);
        mm->ptMinTrackSize = { r.right - r.left, r.bottom - r.top };
        return 0;
    }
    case WM_DPICHANGED: {
        dpi_ = HIWORD(wp);
        scale_ = dpi_ / 96.f;
        EndEdit(true);
        ClosePopup(false);
        rt_.Reset(); brush_.Reset();
        CreateFonts();
        const RECT* r = (const RECT*)lp;
        SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
        Layout();
        Invalidate();
        return 0;
    }
    case WM_SETTINGCHANGE:
        if ((lp && wcscmp((const wchar_t*)lp, L"ImmersiveColorSet") == 0) || wp == SPI_SETHIGHCONTRAST) {
            LoadTheme();
            ApplyTitleBar(hwnd, t_);
            if (editBrush_) { DeleteObject(editBrush_); editBrush_ = nullptr; }
            Invalidate();
        }
        return 0;
    case WM_SYSCOLORCHANGE: case WM_THEMECHANGED:
        LoadTheme();
        ApplyTitleBar(hwnd, t_);
        if (editBrush_) { DeleteObject(editBrush_); editBrush_ = nullptr; }
        Invalidate();
        return 0;
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        BeginPaint(hwnd, &ps);
        Paint();
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_CTLCOLOREDIT: {
        const D2D1_COLOR_F bg = t_.contrast ? t_.bg : t_.dark ? Rgb(31, 31, 31) : Rgb(255, 255, 255);
        if (!editBrush_) editBrush_ = CreateSolidBrush(ToRef(bg));
        SetTextColor((HDC)wp, ToRef(t_.text));
        SetBkColor((HDC)wp, ToRef(bg));
        return (LRESULT)editBrush_;
    }
    case WM_COMMAND:
        if ((HWND)lp == edit_ && HIWORD(wp) == EN_KILLFOCUS && editing_ >= 0) EndEdit(true);
        else if ((HWND)lp == edit_ && HIWORD(wp) == EN_CHANGE && editing_ >= 0) {
            const std::wstring& key = ctls[editing_].key;
            if (key == L"DateFormat" || key == L"DateLocale") {
                const int n = GetWindowTextLengthW(edit_);
                editLive_.assign(n, L'\0');
                if (n) GetWindowTextW(edit_, editLive_.data(), n + 1);
                std::wstring locale, format;
                for (auto& c : ctls) { if (c.key == L"DateLocale") locale = c.value; if (c.key == L"DateFormat") format = c.value; }
                Ctl& c = ctls[editing_];
                if (key == L"DateFormat") {
                    const std::wstring s = DateSample(editLive_, locale);
                    c.desc = s.empty() ? L"Not a date picture Windows understands" : L"Today: " + s;
                } else {
                    const std::wstring name = LocaleName(editLive_);
                    c.desc = name.empty() ? L"Not a language Windows knows" : name;
                    for (auto& d : ctls) if (d.key == L"DateFormat") {
                        const std::wstring s = DateSample(format, editLive_);
                        if (!s.empty()) d.desc = L"Today: " + s;
                    }
                }
                Invalidate();
            }
        }
        return 0;
    case WM_APP_MOVEFOCUS + 1: {   // UI Automation Invoke, run outside the UIA call
        const int i = CtlIndex((int)wp);
        if (i >= 0) UiaInvoke(i);
        return 0;
    }
    case WM_APP_MOVEFOCUS:
        kbdCues_ = true;
        MoveFocus((int)(INT_PTR)wp);
        return 0;
    case WM_TIMER:
        if (wp == TIMER_SLIDER && drag_ < 0) CommitSlider();
        return 0;
    case WM_KEYDOWN: case WM_SYSKEYDOWN:
        if (msg == WM_SYSKEYDOWN && wp != VK_DOWN) break;   // Alt+F4 etc.
        OnKey((UINT)wp);
        return 0;
    case WM_SYSCHAR:
        break;
    case WM_LBUTTONDOWN: case WM_LBUTTONDBLCLK:
        SetFocus(hwnd);
        OnMouseDown(GET_X_LPARAM(lp) / scale_, GET_Y_LPARAM(lp) / scale_, msg == WM_LBUTTONDBLCLK);
        Invalidate();
        return 0;
    case WM_MOUSEMOVE: OnMouseMove(GET_X_LPARAM(lp) / scale_, GET_Y_LPARAM(lp) / scale_); return 0;
    case WM_LBUTTONUP: OnMouseUp(GET_X_LPARAM(lp) / scale_, GET_Y_LPARAM(lp) / scale_); return 0;
    case WM_MOUSELEAVE: if (hot_ != -1 && !thumbDrag_) { hot_ = -1; Invalidate(); } return 0;
    case WM_MOUSEWHEEL: OnWheel(GET_WHEEL_DELTA_WPARAM(wp)); return 0;
    case WM_CAPTURECHANGED:
        if (drag_ >= 0) { drag_ = -1; CommitSlider(); }
        thumbDrag_ = false;
        return 0;
    case WM_SETFOCUS: Invalidate(); NotifyFocus(); return 0;
    case WM_KILLFOCUS: Invalidate(); return 0;
    case WM_ACTIVATE:
        if (LOWORD(wp) == WA_INACTIVE) ClosePopup(false);
        else if (editing_ >= 0) { SetFocus(edit_); return 0; }
        break;
    case WM_GETOBJECT:
        return UiaGetObject(*this, wp, lp);
    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        if (sliderPending_) CommitSlider();
        if (editing_ >= 0) EndEdit(true);
        KillTimer(hwnd, TIMER_SLIDER);
        UiaDetach(*this);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

bool Window::Create() {
    iniPath_ = IniPath();
    ReadFileBytes(iniPath_, original_);
    WNDCLASSEXW wc{ sizeof(wc) };
    if (!GetClassInfoExW(inst_, kClass, &wc)) {
        wc = { sizeof(wc) };
        wc.style = CS_DBLCLKS;
        wc.lpfnWndProc = Proc;
        wc.hInstance = inst_;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hIcon = LoadIconW(inst_, MAKEINTRESOURCEW(1));
        wc.lpszClassName = kClass;
        RegisterClassExW(&wc);
    }
    // On the monitor with the mouse (the tray icon's), below where the clock sits.
    POINT pt; GetCursorPos(&pt);
    HMONITOR mon = MonitorFromPoint(pt, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi{ sizeof(mi) };
    GetMonitorInfoW(mon, &mi);
    UINT dx = 96, dy = 96;
    GetDpiForMonitor(mon, MDT_EFFECTIVE_DPI, &dx, &dy);
    const RECT& wa = mi.rcWork;
    RECT r{ 0, 0, MulDiv(1000, dx, 96), MulDiv(720, dx, 96) };
    AdjustWindowRectExForDpi(&r, WS_OVERLAPPEDWINDOW, FALSE, 0, dx);
    int w = r.right - r.left, h = r.bottom - r.top;
    const int screenH = mi.rcMonitor.bottom - mi.rcMonitor.top;
    int y = mi.rcMonitor.top + screenH * 34 / 100;   // under the clock's date, time and weather
    if (y + h > wa.bottom - 8) {
        const int minH = MulDiv(520, dx, 96);
        h = std::max(minH, (int)(wa.bottom - 8 - y));
        y = std::max((int)wa.top + 8, (int)(wa.bottom - 8 - h));
    }
    w = std::min(w, (int)(wa.right - wa.left - 16));
    const int x = wa.left + ((wa.right - wa.left) - w) / 2;
    hwnd = CreateWindowExW(0, kClass, L"Overlook settings", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, x, y, w, h,
                           nullptr, nullptr, inst_, this);
    if (!hwnd) return false;
    ShowWindow(hwnd, SW_SHOWNORMAL);
    SetForegroundWindow(hwnd);
    return true;
}

void Window::Reloaded() {
    if (sliderPending_ || drag_ >= 0) return;   // our own write is on its way back
    LoadValues();
}

}   // namespace sw

void OpenSettingsWindow(HINSTANCE inst, const SettingsHost& host) {
    using sw::g_win;
    if (g_win && g_win->hwnd) {
        HWND h = g_win->hwnd;
        if (IsIconic(h)) ShowWindow(h, SW_RESTORE);
        SetForegroundWindow(h);
        return;
    }
    g_win = std::make_unique<sw::Window>(inst, host);
    if (!g_win->Create()) g_win.reset();
}

void SettingsWindowReloaded() {
    if (sw::g_win && sw::g_win->hwnd) sw::g_win->Reloaded();
}

void CloseSettingsWindow() {
    if (sw::g_win && sw::g_win->hwnd) DestroyWindow(sw::g_win->hwnd);
}
