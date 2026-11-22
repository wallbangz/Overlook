#include "media.h"
#include "settings.h"

#pragma warning(push)
#pragma warning(disable : 4458 4100 4189 4702 4505)
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Media.Control.h>
#pragma warning(pop)

#include <mutex>
#include <thread>

namespace wmc = winrt::Windows::Media::Control;

namespace {

std::mutex g_mu;
NowPlaying g_snap;

HWND g_notify = nullptr;
UINT g_msg = 0;
HANDLE g_stopEvt = nullptr;
HANDLE g_refreshEvt = nullptr; // set by WinRT event handlers; the worker does the actual work
std::thread g_thread;
bool g_started = false;

// Worker-thread-only state.
wmc::GlobalSystemMediaTransportControlsSessionManager g_mgr{ nullptr };
wmc::GlobalSystemMediaTransportControlsSession g_sess{ nullptr };
winrt::event_token g_tokCurrent{}, g_tokSessions{}, g_tokProps{}, g_tokPlayback{};

void Poke() {
    if (g_refreshEvt) SetEvent(g_refreshEvt);
}

void Unsubscribe() {
    if (g_sess) {
        try {
            g_sess.MediaPropertiesChanged(g_tokProps);
            g_sess.PlaybackInfoChanged(g_tokPlayback);
        } catch (...) {}
        g_sess = nullptr;
    }
    g_tokProps = {};
    g_tokPlayback = {};
}

void Refresh() {
    NowPlaying n;
    try {
        auto cur = g_mgr.GetCurrentSession();
        if (!(cur == g_sess)) {
            Unsubscribe();
            g_sess = cur;
            if (g_sess) {
                g_tokProps = g_sess.MediaPropertiesChanged([](auto&&, auto&&) { Poke(); });
                g_tokPlayback = g_sess.PlaybackInfoChanged([](auto&&, auto&&) { Poke(); });
            }
        }
        if (g_sess) {
            n.app = g_sess.SourceAppUserModelId().c_str();
            try {
                n.playing = g_sess.GetPlaybackInfo().PlaybackStatus() ==
                            wmc::GlobalSystemMediaTransportControlsSessionPlaybackStatus::Playing;
            } catch (...) {}
            auto props = g_sess.TryGetMediaPropertiesAsync().get(); // worker thread (MTA): safe
            if (props) {
                n.title = props.Title().c_str();
                n.artist = props.Artist().c_str();
                if (n.artist.empty()) n.artist = props.AlbumArtist().c_str();
            }
            n.active = !n.title.empty(); // empty title (track switch in flight) hides the line
        }
    } catch (winrt::hresult_error const&) {
        // Session vanished mid-query etc.: treat as inactive; the next event refreshes us.
        n = NowPlaying{};
    } catch (...) {
        n = NowPlaying{};
    }

    bool changed = false;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        if (g_snap.active != n.active || g_snap.playing != n.playing || g_snap.title != n.title ||
            g_snap.artist != n.artist) {
            bool statusChanged = g_snap.playing != n.playing || g_snap.title != n.title ||
                                 g_snap.artist != n.artist || g_snap.statusSince == 0;
            ULONGLONG since = statusChanged ? GetTickCount64() : g_snap.statusSince;
            g_snap = n;
            g_snap.statusSince = since;
            changed = true;
        } else {
            g_snap.app = n.app;
        }
    }
    if (changed) {
        if (n.active)
            Log(L"media: %s %s '%s - %s'", n.app.c_str(), n.playing ? L"playing" : L"paused",
                n.artist.c_str(), n.title.c_str());
        else
            Log(L"media: nothing active");
        if (g_notify) PostMessageW(g_notify, g_msg, 0, 0);
    }
}

// Waits up to ms for the stop event; true = stop asked for.
bool StopRequested(DWORD ms) { return WaitForSingleObject(g_stopEvt, ms) == WAIT_OBJECT_0; }

// Gets the session manager. Right after logon the media service can take a while to answer, so
// a slow or failed request is retried (2 s, 5 s, 15 s, then every minute, ten tries in all);
// a missing API (e.g. an N edition without the Media Feature Pack) is not.
bool Connect() {
    static const DWORD kRetryMs[] = { 2000, 5000, 15000 };
    for (int attempt = 0; attempt < 10; ++attempt) {
        try {
            auto op = wmc::GlobalSystemMediaTransportControlsSessionManager::RequestAsync();
            for (int i = 0; i < 20 && op.Status() == winrt::Windows::Foundation::AsyncStatus::Started; ++i)
                if (StopRequested(500)) { op.Cancel(); return false; }   // up to 10 s
            if (op.Status() == winrt::Windows::Foundation::AsyncStatus::Completed && (g_mgr = op.GetResults())) {
                g_tokCurrent = g_mgr.CurrentSessionChanged([](auto&&, auto&&) { Poke(); });
                g_tokSessions = g_mgr.SessionsChanged([](auto&&, auto&&) { Poke(); });
                if (attempt) Log(L"media: connected on try %d", attempt + 1);
                return true;
            }
            if (op.Status() == winrt::Windows::Foundation::AsyncStatus::Started) op.Cancel();
            Log(L"media: no answer from the media service (try %d)", attempt + 1);
        } catch (winrt::hresult_error const& e) {
            const HRESULT hr = e.code().value;
            Log(L"media: unavailable (0x%08X, try %d)", static_cast<unsigned>(hr), attempt + 1);
            if (hr == REGDB_E_CLASSNOTREG || hr == E_NOTIMPL || hr == E_NOINTERFACE) return false;
        } catch (...) {
            return false;
        }
        g_mgr = nullptr;
        if (StopRequested(attempt < 3 ? kRetryMs[attempt] : 60000)) return false;
    }
    return false;
}

void Worker() {
    try {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
    } catch (...) {
        Log(L"media: no WinRT apartment");
        return;
    }
    if (Connect()) {
        Refresh();
        HANDLE h[2] = { g_stopEvt, g_refreshEvt };
        for (;;) {
            DWORD r = WaitForMultipleObjects(2, h, FALSE, INFINITE);
            if (r != WAIT_OBJECT_0 + 1) break; // stop (or failure)
            Refresh();
        }
        try {
            Unsubscribe();
            g_mgr.CurrentSessionChanged(g_tokCurrent);
            g_mgr.SessionsChanged(g_tokSessions);
        } catch (...) {}
    }
    g_mgr = nullptr;
    g_sess = nullptr;
    try { winrt::uninit_apartment(); } catch (...) {}
}

} // namespace

bool MediaWatchStart(HWND notify, UINT msg) {
    if (g_started) return true;
    // The events live as long as the process: a WinRT callback still in flight on another thread
    // can then never signal a closed (or reused) handle.
    if (!g_stopEvt) g_stopEvt = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!g_refreshEvt) g_refreshEvt = CreateEventW(nullptr, FALSE, FALSE, nullptr); // auto-reset
    if (!g_stopEvt || !g_refreshEvt) return false;
    ResetEvent(g_stopEvt);
    ResetEvent(g_refreshEvt);
    g_notify = notify;
    g_msg = msg;
    try {
        g_thread = std::thread(Worker);   // connects in the background: never blocks the caller
    } catch (...) {
        return false;
    }
    g_started = true;
    return true;
}

void MediaWatchStop() {
    if (!g_started) return;
    g_started = false;
    SetEvent(g_stopEvt);
    if (g_thread.joinable()) g_thread.join();
    g_notify = nullptr;
    std::lock_guard<std::mutex> lk(g_mu);
    g_snap = NowPlaying{};
}

bool MediaLatest(NowPlaying& out) {
    std::lock_guard<std::mutex> lk(g_mu);
    out = g_snap;
    return true;
}
