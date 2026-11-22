#pragma once
#include <string>

// Overlook.exe --weather-test [outDir]
// Renders the weather line for synthetic readings (every symbol by day and night, negative
// temperatures, Fahrenheit, feels-like, wind, rain, sunrise/sunset, 12-hour time, polar day/
// night, stale and wrong-unit readings) and for a few real cities fetched live, into PNG
// galleries in outDir (default: the exe's folder). Doesn't touch a running clock or the
// weather cache. Returns the process exit code.
int RunWeatherTest(const std::wstring& outDir);
