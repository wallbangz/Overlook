#include "desktop.h"
#include "settings.h"
#include <shellscalingapi.h>
#include <cwctype>
#include <algorithm>

// ---------------------------------------------------------------- monitors

static std::wstring FriendlyName(const std::wstring& gdiDevice) {
    UINT32 nPaths = 0, nModes = 0;
    if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &nPaths, &nModes) != ERROR_SUCCESS) return {};
    std::vector<DISPLAYCONFIG_PATH_INFO> paths(nPaths);
    std::vector<DISPLAYCONFIG_MODE_INFO> modes(nModes);
    if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &nPaths, paths.data(), &nModes, modes.data(), nullptr) != ERROR_SUCCESS) return {};
    for (UINT32 i = 0; i < nPaths; ++i) {
        DISPLAYCONFIG_SOURCE_DEVICE_NAME src{};
        src.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
        src.header.size = sizeof(src);
        src.header.adapterId = paths[i].sourceInfo.adapterId;
        src.header.id = paths[i].sourceInfo.id;
        if (DisplayConfigGetDeviceInfo(&src.header) != ERROR_SUCCESS) continue;
        if (_wcsicmp(src.viewGdiDeviceName, gdiDevice.c_str()) != 0) continue;
        DISPLAYCONFIG_TARGET_DEVICE_NAME tgt{};
        tgt.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME;
        tgt.header.size = sizeof(tgt);
        tgt.header.adapterId = paths[i].targetInfo.adapterId;
        tgt.header.id = paths[i].targetInfo.id;
        if (DisplayConfigGetDeviceInfo(&tgt.header) == ERROR_SUCCESS) return tgt.monitorFriendlyDeviceName;
    }
    return {};
}

static BOOL CALLBACK MonEnum(HMONITOR h, HDC, LPRECT, LPARAM lp) {
    auto* v = reinterpret_cast<std::vector<MonitorInfo>*>(lp);
    MONITORINFOEXW mi{}; mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoW(h, &mi)) return TRUE;
    MonitorInfo m;
    m.handle = h;
    m.rc = mi.rcMonitor;
    m.primary = (mi.dwFlags & MONITORINFOF_PRIMARY) != 0;
    m.device = mi.szDevice;
    UINT dx = 96, dy = 96;
    if (SUCCEEDED(GetDpiForMonitor(h, MDT_EFFECTIVE_DPI, &dx, &dy))) m.dpi = dx;
    m.name = FriendlyName(m.device);
    v->push_back(m);
    return TRUE;
}

std::vector<MonitorInfo> EnumMonitors() {
    std::vector<MonitorInfo> v;
    EnumDisplayMonitors(nullptr, nullptr, MonEnum, reinterpret_cast<LPARAM>(&v));
    return v;
}

static std::wstring Lower(std::wstring s) { for (auto& c : s) c = (wchar_t)towlower(c); return s; }

int FindMonitor(const std::vector<MonitorInfo>& mons, const std::wstring& specIn) {
    if (mons.empty()) return -1;
    std::wstring spec = Lower(specIn);
    while (!spec.empty() && iswspace(spec.back())) spec.pop_back();
    while (!spec.empty() && iswspace(spec.front())) spec.erase(spec.begin());
    auto pickBy = [&](auto better) {
        int best = 0;
        for (int i = 1; i < (int)mons.size(); ++i) if (better(mons[i], mons[best])) best = i;
        return best;
    };
    if (spec.empty()) return -1;
    if (spec == L"primary") {
        for (int i = 0; i < (int)mons.size(); ++i) if (mons[i].primary) return i;
        return 0;
    }
    if (spec == L"top")    return pickBy([](auto& a, auto& b) { return a.rc.top < b.rc.top; });
    if (spec == L"bottom") return pickBy([](auto& a, auto& b) { return a.rc.bottom > b.rc.bottom; });
    if (spec == L"left")   return pickBy([](auto& a, auto& b) { return a.rc.left < b.rc.left; });
    if (spec == L"right")  return pickBy([](auto& a, auto& b) { return a.rc.right > b.rc.right; });

    bool numeric = std::all_of(spec.begin(), spec.end(), [](wchar_t c) { return iswdigit(c); });
    if (numeric) {
        size_t i = (size_t)_wtoi(spec.c_str());
        return (i >= 1 && i <= mons.size()) ? (int)i - 1 : -1;
    }
    for (int i = 0; i < (int)mons.size(); ++i) if (Lower(mons[i].device) == spec) return i;
    for (int i = 0; i < (int)mons.size(); ++i)
        if (!mons[i].name.empty() && Lower(mons[i].name).find(spec) != std::wstring::npos) return i;
    return -1;
}

std::vector<MonitorInfo> PickMonitors(const std::wstring& spec, const std::vector<MonitorInfo>& mons) {
    std::vector<MonitorInfo> out;
    if (mons.empty()) return out;
    auto add = [&](int i) {
        if (i < 0) return;
        for (auto& m : out) if (m.handle == mons[i].handle) return;
        out.push_back(mons[i]);
    };
    std::wstring s = Lower(spec);
    s.erase(std::remove_if(s.begin(), s.end(), [](wchar_t c) { return iswspace(c); }), s.end());
    if (s == L"all" || s == L"*") {
        for (int i = 0; i < (int)mons.size(); ++i) add(i);
        return out;
    }
    size_t from = 0;
    for (;;) {
        const size_t comma = spec.find(L',', from);
        add(FindMonitor(mons, spec.substr(from, comma == std::wstring::npos ? std::wstring::npos : comma - from)));
        if (comma == std::wstring::npos) break;
        from = comma + 1;
    }
    if (out.empty()) {   // nothing matched: the primary monitor
        auto p = std::find_if(mons.begin(), mons.end(), [](auto& m) { return m.primary; });
        out.push_back(p != mons.end() ? *p : mons[0]);
    }
    return out;
}

// ---------------------------------------------------------------- desktop host

static BOOL CALLBACK FindDefViewHost(HWND top, LPARAM lp) {
    if (FindWindowExW(top, nullptr, L"SHELLDLL_DefView", nullptr)) {
        *reinterpret_cast<HWND*>(lp) = top;
        return FALSE;
    }
    return TRUE;
}

bool FindDesktopHost(DesktopHost& out) {
    out = {};
    HWND progman = FindWindowW(L"Progman", nullptr);
    if (!progman) return false;

    // Windows 11 24H2+: Progman (WS_EX_NOREDIRECTIONBITMAP) is the desktop; SHELLDLL_DefView (icons)
    // is a child. By default DefView paints the wallpaper opaquely. Message 0x052C (the one
    // wallpaper engines use) makes Progman create a WorkerW child that takes over the wallpaper,
    // leaving DefView transparent. We sit between the two: over the wallpaper, under the icons,
    // and shown together with the desktop on Win+D.
    HWND defview = FindWindowExW(progman, nullptr, L"SHELLDLL_DefView", nullptr);
    if (defview) {
        LONG_PTR ex = GetWindowLongPtrW(progman, GWL_EXSTYLE);
        if (ex & WS_EX_NOREDIRECTIONBITMAP) {
            if (!FindWindowExW(progman, nullptr, L"WorkerW", nullptr)) {
                DWORD_PTR r = 0;
                SendMessageTimeoutW(progman, 0x052C, 0xD, 0x1, SMTO_NORMAL, 1000, &r);
                SendMessageTimeoutW(progman, 0x052C, 0, 0, SMTO_NORMAL, 1000, &r);
                Log(L"sent 0x052C; WorkerW now %p", FindWindowExW(progman, nullptr, L"WorkerW", nullptr));
            }
            out.parent = progman;
            out.insertAfter = defview;
            out.modern = true;
            return true;
        }
    }

    // Older layout: ask Progman to split off a WorkerW behind the icons (the trick wallpaper
    // engines use), then become a child of the WorkerW that follows the one holding DefView.
    DWORD_PTR res = 0;
    SendMessageTimeoutW(progman, 0x052C, 0xD, 0x1, SMTO_NORMAL, 1000, &res);
    SendMessageTimeoutW(progman, 0x052C, 0, 0, SMTO_NORMAL, 1000, &res);
    HWND iconHost = nullptr;
    EnumWindows(FindDefViewHost, reinterpret_cast<LPARAM>(&iconHost));
    if (iconHost && iconHost != progman) {
        HWND worker = FindWindowExW(nullptr, iconHost, L"WorkerW", nullptr);
        if (worker) {
            out.parent = worker;
            out.insertAfter = HWND_TOP;
            return true;
        }
    }
    // Last resort: inside Progman, below DefView.
    defview = FindWindowExW(progman, nullptr, L"SHELLDLL_DefView", nullptr);
    if (!defview) return false;
    out.parent = progman;
    out.insertAfter = defview;
    return true;
}
