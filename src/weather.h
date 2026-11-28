#pragma once
#include <windows.h>
#include <string>

// Weather from Open-Meteo (https://open-meteo.com, free, no API key), fetched with WinHTTP on a
// background thread so the UI thread (which shares input state with Explorer's desktop) never
// blocks on the network.

struct WeatherRequest {
    std::wstring city;        // free text from the .ini: "Lisbon", "Porto, PT", a postcode...
    bool fahrenheit = false;
    int rainHours = 6;        // look-ahead window for the rain chance
    bool airQuality = true;   // also fetch the air-quality index (a second, optional request)
};

struct WeatherReading {
    bool valid = false;
    std::wstring place;       // resolved place name, e.g. "Lisbon, Portugal" (for the log/tooltip)
    double temp = 0;          // current temperature, in the requested unit
    double high = 0, low = 0; // today's max / min
    int code = 0;             // WMO weather interpretation code (current)
    bool isDay = true;
    int rainChance = -1;      // highest precipitation probability (%) within the next rainHours, -1 if unknown
    ULONGLONG rainAtUtc = 0;  // start of the hour where that maximum first occurs (FILETIME ticks, UTC), 0 if unknown
    bool hasApparent = false;
    double apparent = 0;      // "feels like" temperature, in the requested unit
    bool hasWind = false;
    double windSpeed = 0;     // 10 m wind, km/h (mph when fahrenheit)
    int windDir = 0;          // degrees the wind comes FROM (0 = north)
    // Sunrise, sunset, sunrise, sunset (today and tomorrow at the location) as UTC FILETIME ticks,
    // so they compare correctly with the PC's clock whatever the two time zones; 0 when unknown.
    ULONGLONG sunUtc[4] = {};
    int utcOffset = 0;        // the location's UTC offset in seconds (for the log)
    double latitude = 0;      // of the location (the moon's lit side flips south of the equator)
    double uvIndex = -1;      // current UV index, -1 if unknown
    int euAqi = -1, usAqi = -1;   // current European / US air-quality index, -1 if unknown
    bool fahrenheit = false;  // units the reading is in
    ULONGLONG fetchedAt = 0;  // GetTickCount64() when this reading was received (0 = loaded from cache)
    FILETIME fetchedUtc{};    // wall-clock time of the fetch (survives restarts, for staleness)
};

// Starts a fetch on a worker thread unless one is already running (returns false then, or if
// city is empty). When it finishes, posts `msg` to `notify` with wParam = 1 on success, 0 on
// failure; the result is then available from WeatherTakeResult().
// City -> coordinates is resolved once with Open-Meteo's geocoding API and cached in
// "Overlook.weather.ini" in the settings folder, together with the last good reading.
bool WeatherFetchAsync(HWND notify, UINT msg, const WeatherRequest& req);

// The latest successful reading (from the last fetch, or from the cache file at startup).
// Thread-safe. Returns false if there is none for the current city.
bool WeatherLatest(const std::wstring& city, WeatherReading& out);

// Describes the most recent failure (for the log), empty if the last fetch succeeded.
std::wstring WeatherLastError();

// Synchronous fetch that neither reads nor writes the cache (for --weather-test).
bool WeatherFetchOnce(const WeatherRequest& req, WeatherReading& out, std::wstring& err);
