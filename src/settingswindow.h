#pragma once
#include <windows.h>
#include <string>
#include <vector>
#include "desktop.h"

// The settings window: the common settings, written straight into Overlook.ini (comments and
// the user's own values kept) and applied live through the clock's normal reload.

// One monitor of the Monitor= set, as the tray menu lists it.
struct SettingsMonitor {
    MonitorInfo mon;
    std::wstring id;          // the tray's id (name, or \\.\DISPLAYn when two share a name): HiddenMonitors entries
    bool trayHidden = false;  // listed in HiddenMonitors
    bool showOff = false;     // Show=0 in a [Monitor ...] section
};

struct SettingsHost {
    HWND ctl = nullptr;       // gets reloadMsg after each write
    UINT reloadMsg = 0;
    std::vector<SettingsMonitor> (*monitors)() = nullptr;
};

void OpenSettingsWindow(HINSTANCE inst, const SettingsHost& host);   // or brings the open one forward
void SettingsWindowReloaded();   // the clock (re)loaded the ini: refresh the shown values
void CloseSettingsWindow();
