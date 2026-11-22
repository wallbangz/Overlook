#pragma once
#include <windows.h>
#include <string>

// What Windows' media controls (GlobalSystemMediaTransportControls: Spotify, browsers, media
// players...) report as playing. Event-driven: no polling.

struct NowPlaying {
    bool active = false;       // there is a current media session with a title
    bool playing = false;      // its playback status is Playing
    std::wstring title, artist;
    std::wstring app;          // source app id (for the log)
    ULONGLONG statusSince = 0; // GetTickCount64() when `playing` last changed (or the track changed)
};

// Starts watching on a background thread (WinRT in its own multithreaded apartment). Posts `msg`
// to `notify` (wParam/lParam 0) whenever the current session, its track or its playback state
// changes. Returns at once (the worker connects in the background, retrying while the media
// service is slow to answer, e.g. right after logon); false only if the thread can't start.
// Start and Stop may be called again (NowPlaying switched off and on).
bool MediaWatchStart(HWND notify, UINT msg);
void MediaWatchStop();

// Latest snapshot (thread-safe).
bool MediaLatest(NowPlaying& out);
