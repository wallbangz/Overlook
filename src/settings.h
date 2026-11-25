#pragma once
#include <windows.h>
#include <string>
#include <vector>

// All sizes are in DIPs (1 DIP = 1/96 inch; scaled by the target monitor's DPI).
struct Settings {
    // Placement
    std::wstring monitor = L"all";   // all, or a comma list of: primary | top | bottom | left | right | <index> | \\.\DISPLAYn | <name substring>
    double verticalPos   = 16.5;     // % of monitor height where the centre of the date+time block sits
    double horizontalOffset = 0.0;   // DIPs, + moves right

    // Sizes
    double timeSize      = 145.0;    // DIPs (font em size)
    double dateSize      = 32.0;     // DIPs
    double gap           = 25.0;     // DIPs between date baseline and top of the time digits
    double timeTracking  = -2.0;     // letter spacing, % of em (negative = tighter)
    double dateTracking  = 0.0;

    // Colour
    COLORREF color       = RGB(0xFF, 0xFF, 0xFF);
    COLORREF darkColor   = RGB(0x1A, 0x1A, 0x1A);
    double opacity       = 80.0;     // %
    bool   autoContrast  = true;
    double brightThreshold = 60.0;   // mean perceived lightness (L*, 0-100) above which darkColor is used
    double tint          = 35.0;     // % pull toward the wallpaper's hue behind the clock (0 = neutral)

    // Shadow
    bool   shadow        = true;
    double shadowOpacity = 30.0;     // %
    double shadowBlur    = 6.0;      // DIPs (gaussian std deviation)
    double shadowOffsetY = 1.0;      // DIPs
    bool   adaptiveLegibility = true;   // stronger shadow and secondary lines on busy wallpapers

    // Weather line under the time (Open-Meteo); off while weatherCity is empty
    std::wstring weatherCity;        // "Lisbon", "Porto, PT", a postcode...
    bool   weatherFahrenheit = false;
    bool   showWeather   = true;     // per-monitor switch for the line
    bool   feelsLike     = true;     // "feels 12°" when it differs from the temperature by >= feelsLikeDiff
    double feelsLikeDiff = 2.0;
    bool   wind          = true;     // wind speed/direction when >= windThreshold (km/h, or mph with F)
    double windThreshold = 20.0;
    bool   sunTimes      = true;     // next sunrise/sunset when it is within sunHours (0 = always)
    double sunHours      = 3.0;
    double weatherMaxWidth = 150.0;  // % of the time's width the line may take; extras give way beyond it
    bool   uv            = true;     // "UV 7" from UVThreshold
    double uvThreshold   = 6.0;
    bool   airQuality    = true;     // "Air poor" and worse (European index; US index with Fahrenheit)

    // Now-playing line under the weather (Windows' media controls: Spotify, browsers...)
    bool   nowPlaying    = true;
    // App-wide: the media apps the line listens to (see MediaSetApps); "all" = every app
    std::wstring nowPlayingApps = L"Spotify, Apple Music, iTunes, TIDAL, Deezer, Amazon Music, Qobuz, YouTube Music, "
                                  L"Pandora, foobar2000, MusicBee, AIMP, Winamp, Plexamp, Cider";
    double nowPlayingLinger = 60.0;  // seconds the line stays after playback pauses
    double nowPlayingPausedOpacity = 50.0;   // % of the text's opacity while paused
    double secondaryOpacity = 70.0;   // weather and now-playing lines, % of opacity
    double weatherSize   = 28.0;     // DIPs
    double weatherGap    = 35.0;     // DIPs between the time baseline and the top of the weather line's capitals
    double rainThreshold = 30.0;     // show the chance of rain when it reaches this % ...
    double rainHours     = 6.0;      // ... within this many hours

    // Weather effects: abstract shapes over the wallpaper, below the clock and icons (per monitor)
    bool   weatherEffects = false;
    double weatherEffectsOpacity = 40.0;   // % the most opaque shape gets
    bool   weatherEffectsLightning = true; // soft glows in thunderstorms
    std::wstring weatherEffectsForce;      // "" = from the weather; clouds-light | clouds | clouds-heavy | fog | rain-light | rain | rain-heavy | sleet | thunder | snow-light | snow | snow-heavy = always that
    bool   weatherEffectsOnBattery = false;   // app-wide: keep them running on battery (PCs without one never pause)

    // Debug menu (app-wide, hidden: Ctrl+Shift+D in the settings window). Every Debug* key is
    // ignored unless debugMode is on, so a normal install is never affected by leftovers.
    bool   debugMode     = false;
    bool   debugEffectsEverywhere = false;   // WeatherEffects treated as on for every clock (applied by LoadSettings/MonitorSettings)
    int    debugWeatherCode = -1;            // a WMO code replacing the reading's (counts as fresh, even without a city); -1 = real weather
    bool   debugWindOn   = false;
    double debugWind     = 0.0;              // km/h, + = blowing toward the right of the screen
    bool   debugDensityOn = false;
    double debugDensity  = 100.0;            // % of the effect's amount
    bool   debugNight    = false;            // effects drawn as at night
    bool   debugIgnorePauses = false;        // effects run when covered, on battery... (not with the display off or locked)

    // OLED care (per monitor)
    bool   pixelShift    = false;    // move the clock along a slow loop, 1 px at a time
    double pixelShiftRange = 4.0;    // physical pixels each way
    bool   nightDim      = false;    // lower the opacity between sunset and sunrise
    double nightOpacity  = 45.0;     // %
    std::wstring nightStart = L"20:00";   // used when there are no sunset/sunrise times (weather off)
    std::wstring nightEnd   = L"07:00";

    // Crossfade when the displayed text changes (0 = instant)
    double transitionMs  = 500.0;

    // Formats
    bool   hour24        = true;
    bool   leadingZero   = true;     // "07:05" vs "7:05"
    bool   showAmPm      = false;    // only in 12h mode
    std::wstring dateFormat = L"dddd, MMMM d";  // Windows date picture (GetDateFormatEx)
    std::wstring dateLocale = L"en-US";
    std::wstring timeFeatures;       // OpenType features for the time, e.g. "tnum,ss01"
    double opticalCentring = 0.0;    // 0 = time's ink box centred, 100 = its centre of mass; between = a blend
    bool   centreOnColon = false;    // CentreOn=colon: the time's colon on the centre line (OpticalCentring then unused)
    std::wstring dateFeatures;

    // App
    bool   startWithWindows = false;
    bool   trayIcon      = true;
    bool   checkForUpdates = true;   // app-wide: once a day, ask GitHub for the latest release
    bool   updateBeta    = false;    // UpdateChannel=beta: pre-releases count too
    bool   show          = true;     // per monitor: this monitor shows a clock (tray menu checkboxes)
    bool   hideClocks    = false;    // app-wide: hide every clock (tray menu)
    std::wstring hiddenMonitors;     // app-wide: "Dell U2720Q;\\.\DISPLAY3" - clocks switched off in the tray menu
    bool   log           = false;
    bool   logPrivate    = false;    // app-wide: wallpaper names, track titles and the weather place unmasked in the log
};

std::wstring ExeBase();   // the exe's path without ".exe": Overlook.ini, .log and .weather.ini sit beside it
std::wstring IniPath();
std::wstring ExeDir();
std::wstring ExePath();
void WriteDefaultIni(const std::wstring& path);
Settings LoadSettings();   // [Clock]
// Spec part of each [Monitor <spec>] section, in file order.
std::vector<std::wstring> MonitorSections();
// base with the keys of [Monitor <spec>] applied (app-wide keys stay as in base).
Settings MonitorSettings(const Settings& base, const std::wstring& spec);
void SaveBool(const wchar_t* key, bool v);
void SaveString(const wchar_t* key, const std::wstring& v);   // into [Clock]

// HKCU Run key
bool IsAutostartEnabled();
void SetAutostart(bool on);

void Log(const wchar_t* fmt, ...);
extern bool g_logEnabled;
extern bool g_logPrivate;   // LogPrivate: Private() and friends pass text through unmasked

// For the log: s itself with LogPrivate=1, otherwise "<12 chars #3F2A>" (its length and a short
// stable hash, so the same name can still be followed through a log without being readable).
std::wstring Private(const std::wstring& s);
// A file path the same way, keeping the extension: "<wallpaper 23 chars #1A2B .jpg>".
std::wstring PrivatePath(const std::wstring& path, const wchar_t* what);
// text with every occurrence of secret masked (e.g. the city inside an error message).
std::wstring PrivateIn(const std::wstring& text, const std::wstring& secret);
