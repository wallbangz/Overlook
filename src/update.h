#pragma once
#include <windows.h>
#include <string>
#include <vector>

// The update check: about once a day, one anonymous GET to GitHub's releases API (a generic
// User-Agent, no identifiers), on a worker thread. It only tells: the tray menu and the settings
// window's About page offer the release's Overlook.zip in the browser. Nothing is downloaded or
// installed by the app itself.

// Semantic version "1.2.3" or "1.2.3-beta.2", with or without a leading "v". Build metadata
// ("+...") is ignored.
struct Version {
    long long major = 0, minor = 0, patch = 0;
    std::vector<std::string> pre;   // pre-release identifiers: "beta", "2"
};
bool ParseVersion(const std::wstring& tag, Version& out);   // false if malformed
int CompareVersions(const Version& a, const Version& b);    // < 0, 0, > 0 by semver precedence
const Version& CurrentVersion();                            // this build (OVERLOOK_VERSION_STR)

struct ReleaseInfo {
    std::wstring tag;        // "v0.4.0"; empty = no eligible release
    std::wstring pageUrl;    // its release page (html_url)
    std::wstring zipUrl;     // its Overlook.zip (browser_download_url), empty if missing
    std::wstring zipSha256;  // that zip's SHA-256 from GitHub (the asset's "digest"), 64 lowercase hex digits; empty if not given
};

// The newest eligible release in GitHub's JSON: one release object (/releases/latest) or an
// array of them (/releases). Drafts, malformed tags and URLs outside github.com/wallbangz/Overlook/ are skipped;
// pre-releases (flagged, or with a "-..." tag) count only when beta is set. False only when the
// JSON can't be read.
bool PickRelease(const std::string& json, bool beta, ReleaseInfo& out);

enum class UpdateState { NotChecked, Checking, UpToDate, Available, Failed };

struct UpdateStatus {
    UpdateState state = UpdateState::NotChecked;
    ReleaseInfo latest;      // the newest release seen (UpToDate, Available)
    bool newer = false;      // latest is newer than this build (kept when a later check fails)
};

// Loads the last result from Overlook.update.ini (beside the exe), when automatic checks are on.
void UpdateInit(bool enabled);
// A day since the last successful check, or never checked, or the channel changed.
bool UpdateDue(bool beta);
// Starts a check on a worker thread unless one is running; posts msg to notify (wParam 1 = the
// check succeeded) when it ends. Thread-safe state is in UpdateCurrent().
bool UpdateCheckAsync(HWND notify, UINT msg, bool beta);
UpdateStatus UpdateCurrent();
// What "Get the latest release" opens: the zip, or the release page when there is none.
std::wstring UpdateDownloadUrl(const UpdateStatus& s);
std::wstring UpdatePageUrl(const UpdateStatus& s);
// True once per newer version (remembered across restarts): show the tray notification.
bool UpdateTakeNotify();
// One line for the About page: "Up to date", "v0.4.0 is available"...
std::wstring UpdateStatusText(const UpdateStatus& s);

// Overlook.exe --update-test <dir>: version ordering and JSON parsing checks, then one live
// check (reported, not judged). Writes update-test.txt; returns 0 when everything passed.
int RunUpdateTest(const std::wstring& dir);
