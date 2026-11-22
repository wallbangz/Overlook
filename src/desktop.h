#pragma once
#include <windows.h>
#include <string>
#include <vector>

struct MonitorInfo {
    HMONITOR handle = nullptr;
    RECT rc{};              // full monitor rect, physical pixels
    UINT dpi = 96;          // effective DPI
    bool primary = false;
    std::wstring device;    // \\.\DISPLAYn
    std::wstring name;      // friendly (EDID) name, may be empty
};

std::vector<MonitorInfo> EnumMonitors();
// spec: primary | top | bottom | left | right | <1-based index> | \\.\DISPLAYn | <friendly-name substring>
// Index into mons, or -1 if nothing matches.
int FindMonitor(const std::vector<MonitorInfo>& mons, const std::wstring& spec);
// "all", or a comma separated list of specs; duplicates dropped, in list order.
// Falls back to the primary monitor when nothing matches.
std::vector<MonitorInfo> PickMonitors(const std::wstring& spec, const std::vector<MonitorInfo>& mons);

// Where the clock window lives inside Explorer's desktop.
struct DesktopHost {
    HWND parent = nullptr;       // window we become a child of
    HWND insertAfter = nullptr;  // z-order: we go directly below this sibling (HWND_TOP if none)
    bool modern = false;         // 24H2+ layout (DefView is a child of Progman)
};
bool FindDesktopHost(DesktopHost& out);
