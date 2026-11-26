// The update check: GitHub's releases API via WinHTTP, on a worker thread. See update.h.
#include "update.h"
#include "net.h"
#include "settings.h"
#include "../res/resource.h"

#include <atomic>
#include <cstring>
#include <cwctype>
#include <thread>

namespace {

const wchar_t kApiHost[] = L"api.github.com";
const wchar_t kLatestPath[] = L"/repos/wallbangz/Overlook/releases/latest";
const wchar_t kListPath[] = L"/repos/wallbangz/Overlook/releases?per_page=30";
const wchar_t kHeaders[] = L"Accept: application/vnd.github+json\r\nX-GitHub-Api-Version: 2022-11-28\r\n";
const wchar_t kReleasesPage[] = L"https://github.com/wallbangz/Overlook/releases";
const ULONGLONG kDayTicks = 24ULL * 3600ULL * 10000000ULL;

std::wstring Widen(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    if (n <= 0) return {};
    std::wstring w((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
    return w;
}

std::string Narrow(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    if (n <= 0) return {};
    std::string s((size_t)n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}

bool AllDigits(const std::string& s) {
    if (s.empty()) return false;
    for (char c : s) if (c < '0' || c > '9') return false;
    return true;
}

// A version number part: digits, no leading zero, small enough to hold.
bool ParsePart(const std::string& s, long long& v) {
    if (!AllDigits(s) || s.size() > 9 || (s.size() > 1 && s[0] == '0')) return false;
    v = 0;
    for (char c : s) v = v * 10 + (c - '0');
    return true;
}

std::vector<std::string> Split(const std::string& s, char sep) {
    std::vector<std::string> out;
    size_t from = 0;
    for (;;) {
        const size_t at = s.find(sep, from);
        out.push_back(s.substr(from, at == std::string::npos ? std::string::npos : at - from));
        if (at == std::string::npos) return out;
        from = at + 1;
    }
}

// Only links into github.com are opened.
bool SafeUrl(const std::wstring& u) {
    if (u.size() > 2048 || u.compare(0, 19, L"https://github.com/") != 0) return false;
    for (wchar_t c : u) if (c <= L' ' || c == L'"' || c > 0x7E) return false;
    return true;
}

ULONGLONG NowTicks() {
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    return (ULONGLONG)ft.dwHighDateTime << 32 | ft.dwLowDateTime;
}

// Due a day after the last check; at once if never checked or the clock went back.
bool DueAt(ULONGLONG checked, ULONGLONG now) { return checked == 0 || now < checked || now - checked >= kDayTicks; }

bool Newer(const std::wstring& tag) {
    Version v;
    return !tag.empty() && ParseVersion(tag, v) && CompareVersions(v, CurrentVersion()) > 0;
}

std::wstring ShownTag(const std::wstring& tag) {
    return !tag.empty() && (tag[0] == L'v' || tag[0] == L'V') ? L"v" + tag.substr(1) : L"v" + tag;
}

// ---------------------------------------------------------------- state and cache

SRWLOCK g_lock = SRWLOCK_INIT;
UpdateStatus g_status;   // guarded by g_lock
bool g_newer = false;    // g_status.latest is newer than this build (kept when a later check fails)
std::atomic<bool> g_busy{ false };

struct Lock {
    Lock() { AcquireSRWLockExclusive(&g_lock); }
    ~Lock() { ReleaseSRWLockExclusive(&g_lock); }
};

std::wstring CachePath() { return ExeBase() + L".update.ini"; }   // Overlook.update.ini

std::wstring CacheGet(const wchar_t* key) {
    wchar_t buf[2100] = L"";
    GetPrivateProfileStringW(L"Update", key, L"", buf, ARRAYSIZE(buf), CachePath().c_str());
    return buf;
}

void CacheSet(const wchar_t* key, const std::wstring& v) {
    WritePrivateProfileStringW(L"Update", key, v.c_str(), CachePath().c_str());
}

ULONGLONG CacheTicks() {
    const std::wstring s = CacheGet(L"CheckedUtc");
    if (s.empty()) return 0;
    wchar_t* end = nullptr;
    const unsigned long long v = wcstoull(s.c_str(), &end, 10);
    return end && *end == 0 ? v : 0;
}

const wchar_t* Channel(bool beta) { return beta ? L"beta" : L"stable"; }

void RunCheck(HWND notify, UINT msg, bool beta) {
    bool ok = false;
    std::wstring err;
    ReleaseInfo info;
    try {
        std::string body;
        if (net::HttpGet(kApiHost, beta ? kListPath : kLatestPath, body, err, kHeaders)) {
            if (PickRelease(body, beta, info)) ok = true;
            else err = L"invalid JSON from GitHub";
        }
    } catch (...) {
        err = L"internal error";
    }
    if (ok) {
        const bool newer = Newer(info.tag);
        CacheSet(L"CheckedUtc", std::to_wstring(NowTicks()));
        CacheSet(L"Channel", Channel(beta));
        CacheSet(L"Tag", info.tag);
        CacheSet(L"Page", info.pageUrl);
        CacheSet(L"Zip", info.zipUrl);
        {
            Lock l;
            g_status.state = newer ? UpdateState::Available : UpdateState::UpToDate;
            g_status.latest = info;
            g_newer = newer;
        }
        Log(L"update: latest %s release is %s (this is %s)%s", Channel(beta), info.tag.empty() ? L"(none)" : info.tag.c_str(),
            Widen(OVERLOOK_VERSION_STR).c_str(), newer ? L": update available" : L"");
    } else {
        {
            Lock l;
            g_status.state = UpdateState::Failed;
        }
        Log(L"update: check failed: %s", err.c_str());
    }
    g_busy.store(false);
    if (notify) PostMessageW(notify, msg, ok ? 1 : 0, 0);
}

}   // namespace

// ---------------------------------------------------------------- versions

bool ParseVersion(const std::wstring& tagIn, Version& out) {
    std::string s;
    for (wchar_t c : tagIn) {
        if (c > 0x7E) return false;
        s.push_back((char)c);
    }
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.pop_back();
    size_t b = 0;
    while (b < s.size() && (s[b] == ' ' || s[b] == '\t')) ++b;
    s.erase(0, b);
    if (!s.empty() && (s[0] == 'v' || s[0] == 'V')) s.erase(0, 1);
    if (const size_t plus = s.find('+'); plus != std::string::npos) {   // build metadata: ignored, but not empty
        if (plus + 1 >= s.size()) return false;
        s.erase(plus);
    }
    std::string core = s, pre;
    const size_t dash = s.find('-');
    if (dash != std::string::npos) {
        core = s.substr(0, dash);
        pre = s.substr(dash + 1);
        if (pre.empty() || pre.size() > 32) return false;   // "beta.1", "rc.2"...; a longer one isn't a real tag
    }
    const auto parts = Split(core, '.');
    Version v;
    if (parts.size() != 3 || !ParsePart(parts[0], v.major) || !ParsePart(parts[1], v.minor) || !ParsePart(parts[2], v.patch))
        return false;
    if (dash != std::string::npos) {
        for (const std::string& id : Split(pre, '.')) {
            if (id.empty()) return false;
            for (char c : id)
                if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '-')) return false;
            if (AllDigits(id) && id.size() > 1 && id[0] == '0') return false;
            v.pre.push_back(id);
        }
    }
    out = std::move(v);
    return true;
}

int CompareVersions(const Version& a, const Version& b) {
    if (a.major != b.major) return a.major < b.major ? -1 : 1;
    if (a.minor != b.minor) return a.minor < b.minor ? -1 : 1;
    if (a.patch != b.patch) return a.patch < b.patch ? -1 : 1;
    if (a.pre.empty() || b.pre.empty()) return a.pre.empty() == b.pre.empty() ? 0 : a.pre.empty() ? 1 : -1;   // a release follows its pre-releases
    for (size_t i = 0; i < a.pre.size() && i < b.pre.size(); ++i) {
        const std::string &x = a.pre[i], &y = b.pre[i];
        const bool nx = AllDigits(x), ny = AllDigits(y);
        if (nx && ny) {
            if (x.size() != y.size()) return x.size() < y.size() ? -1 : 1;
            if (const int c = x.compare(y)) return c < 0 ? -1 : 1;
        } else if (nx != ny) {
            return nx ? -1 : 1;   // numeric identifiers sort before alphanumeric ones
        } else if (const int c = x.compare(y)) {
            return c < 0 ? -1 : 1;
        }
    }
    if (a.pre.size() != b.pre.size()) return a.pre.size() < b.pre.size() ? -1 : 1;
    return 0;
}

const Version& CurrentVersion() {
    static const Version v = [] {
        Version x;
        ParseVersion(Widen(OVERLOOK_VERSION_STR), x);
        return x;
    }();
    return v;
}

// ---------------------------------------------------------------- GitHub's JSON

bool PickRelease(const std::string& json, bool beta, ReleaseInfo& out) {
    using namespace net;
    out = ReleaseInfo();
    JVal root;
    if (!ParseJson(json, root)) return false;
    std::vector<const JVal*> list;
    if (root.type == JVal::Object) list.push_back(&root);
    else if (root.type == JVal::Array) for (const JVal& r : root.arr) list.push_back(&r);
    else return false;

    Version best;
    bool have = false;
    for (const JVal* r : list) {
        if (r->type != JVal::Object) continue;
        bool draft = false, pre = false;
        GetBool(Get(r, "draft"), draft);
        GetBool(Get(r, "prerelease"), pre);
        std::string tag;
        Version v;
        if (draft || !GetStr(Get(r, "tag_name"), tag) || !ParseVersion(Widen(tag), v)) continue;
        if (!beta && (pre || !v.pre.empty())) continue;
        if (have && CompareVersions(v, best) <= 0) continue;
        ReleaseInfo info;
        info.tag = Widen(tag);
        std::string page;
        if (GetStr(Get(r, "html_url"), page) && SafeUrl(Widen(page))) info.pageUrl = Widen(page);
        if (const JVal* assets = Get(r, "assets"); assets && assets->type == JVal::Array)
            for (const JVal& a : assets->arr) {
                std::string name, url;
                if (GetStr(Get(&a, "name"), name) && _stricmp(name.c_str(), "Overlook.zip") == 0 &&
                    GetStr(Get(&a, "browser_download_url"), url) && SafeUrl(Widen(url))) {
                    info.zipUrl = Widen(url);
                    break;
                }
            }
        best = v;
        have = true;
        out = std::move(info);
    }
    return true;
}

// ---------------------------------------------------------------- public API

void UpdateInit(bool enabled) {
    Lock l;
    if (g_status.state == UpdateState::Checking) return;
    g_status = UpdateStatus();
    g_newer = false;
    if (!enabled || CacheTicks() == 0) return;
    g_status.latest.tag = CacheGet(L"Tag");
    const std::wstring page = CacheGet(L"Page"), zip = CacheGet(L"Zip");
    if (SafeUrl(page)) g_status.latest.pageUrl = page;
    if (SafeUrl(zip)) g_status.latest.zipUrl = zip;
    g_newer = Newer(g_status.latest.tag);
    g_status.state = g_newer ? UpdateState::Available : UpdateState::UpToDate;
}

bool UpdateDue(bool beta) {
    if (_wcsicmp(CacheGet(L"Channel").c_str(), Channel(beta)) != 0) return true;
    return DueAt(CacheTicks(), NowTicks());
}

bool UpdateCheckAsync(HWND notify, UINT msg, bool beta) {
    bool expected = false;
    if (!g_busy.compare_exchange_strong(expected, true)) return false;
    {
        Lock l;
        g_status.state = UpdateState::Checking;
    }
    try {
        std::thread([notify, msg, beta]() {
            try { RunCheck(notify, msg, beta); }
            catch (...) { g_busy.store(false); }
        }).detach();
    } catch (...) {
        g_busy.store(false);
        Lock l;
        g_status.state = UpdateState::Failed;
        return false;
    }
    return true;
}

UpdateStatus UpdateCurrent() {
    Lock l;
    UpdateStatus s = g_status;
    s.newer = g_newer;
    return s;
}

std::wstring UpdatePageUrl(const UpdateStatus& s) {
    return s.latest.pageUrl.empty() ? std::wstring(kReleasesPage) : s.latest.pageUrl;
}

std::wstring UpdateDownloadUrl(const UpdateStatus& s) {
    return s.latest.zipUrl.empty() ? UpdatePageUrl(s) : s.latest.zipUrl;
}

bool UpdateTakeNotify() {
    std::wstring tag;
    {
        Lock l;
        if (!g_newer) return false;
        tag = g_status.latest.tag;
    }
    if (_wcsicmp(CacheGet(L"Notified").c_str(), tag.c_str()) == 0) return false;
    CacheSet(L"Notified", tag);
    return true;
}


std::wstring UpdateStatusText(const UpdateStatus& s) {
    switch (s.state) {
    case UpdateState::Checking: return L"Checking…";
    case UpdateState::UpToDate: return L"Up to date";
    case UpdateState::Available: return ShownTag(s.latest.tag) + L" is available";
    case UpdateState::Failed: return L"Couldn’t check";
    default: return L"Not checked yet";
    }
}

// ---------------------------------------------------------------- self test

namespace {

// A saved /releases/latest response (trimmed: fewer user fields; the body has escapes).
const char kLatestSample[] = R"json({
  "url": "https://api.github.com/repos/wallbangz/Overlook/releases/454850601",
  "assets_url": "https://api.github.com/repos/wallbangz/Overlook/releases/454850601/assets",
  "upload_url": "https://uploads.github.com/repos/wallbangz/Overlook/releases/454850601/assets{?name,label}",
  "html_url": "https://github.com/wallbangz/Overlook/releases/tag/v0.3.0",
  "id": 454850601,
  "author": {
    "login": "github-actions[bot]",
    "id": 41898282,
    "type": "Bot",
    "site_admin": false
  },
  "node_id": "RE_kwDOV3s69M4bHHgp",
  "tag_name": "v0.3.0",
  "target_commitish": "main",
  "name": "v0.3.0",
  "draft": false,
  "immutable": false,
  "prerelease": false,
  "created_at": "2026-11-23T00:10:47Z",
  "updated_at": "2026-11-23T00:11:51Z",
  "published_at": "2026-11-23T00:11:51Z",
  "assets": [
    {
      "url": "https://api.github.com/repos/wallbangz/Overlook/releases/assets/780915731",
      "id": 780915731,
      "node_id": "RA_kwDOV3s69M4ui9QT",
      "name": "Overlook.zip",
      "label": "",
      "uploader": { "login": "github-actions[bot]", "id": 41898282, "type": "Bot" },
      "content_type": "application/zip",
      "state": "uploaded",
      "size": 913397,
      "digest": "sha256:cd89236d93a0d2d767282bac19641f3103865244a3130ee6ad9b5574dbe88ad9",
      "download_count": 56,
      "created_at": "2026-11-23T00:11:50Z",
      "updated_at": "2026-11-23T00:11:51Z",
      "browser_download_url": "https://github.com/wallbangz/Overlook/releases/download/v0.3.0/Overlook.zip"
    }
  ],
  "tarball_url": "https://api.github.com/repos/wallbangz/Overlook/tarball/v0.3.0",
  "zipball_url": "https://api.github.com/repos/wallbangz/Overlook/zipball/v0.3.0",
  "body": "### Added\n- **Adapt to busy wallpapers** (`AdaptiveLegibility`, on by default; Settings › Look). \"Quoted\" \\ text.\r\n\n**Full Changelog**: https://github.com/wallbangz/Overlook/compare/v0.2.1...v0.3.0"
})json";

// A /releases list: a draft, betas, a stable release, a malformed tag, a foreign URL, no zip.
const char kListSample[] = R"json([
  { "tag_name": "v0.5.0", "draft": true, "prerelease": false, "html_url": "https://github.com/wallbangz/Overlook/releases/tag/v0.5.0", "assets": [] },
  { "tag_name": "nightly", "draft": false, "prerelease": true, "html_url": "https://github.com/wallbangz/Overlook/releases/tag/nightly", "assets": [] },
  { "tag_name": "v0.4.0-beta.2", "draft": false, "prerelease": true,
    "html_url": "https://github.com/wallbangz/Overlook/releases/tag/v0.4.0-beta.2",
    "assets": [ { "name": "notes.txt", "browser_download_url": "https://github.com/wallbangz/Overlook/releases/download/v0.4.0-beta.2/notes.txt" },
                { "name": "overlook.ZIP", "browser_download_url": "https://github.com/wallbangz/Overlook/releases/download/v0.4.0-beta.2/Overlook.zip" } ] },
  { "tag_name": "v0.4.0-beta.10", "draft": false, "prerelease": true,
    "html_url": "https://example.com/not-github", "assets": [] },
  { "tag_name": "v0.3.1", "draft": false, "prerelease": false, "html_url": "https://github.com/wallbangz/Overlook/releases/tag/v0.3.1",
    "assets": [ { "name": "Overlook.zip", "browser_download_url": "http://github.com/insecure/Overlook.zip" } ] },
  { "tag_name": "v0.3.5-rc.1", "draft": false, "prerelease": false, "html_url": "https://github.com/wallbangz/Overlook/releases/tag/v0.3.5-rc.1", "assets": [] },
  { "tag_name": "v0.3.0", "draft": false, "prerelease": false, "html_url": "https://github.com/wallbangz/Overlook/releases/tag/v0.3.0", "assets": [] }
])json";

struct Report {
    std::string log;
    bool ok = true;
    void Check(bool pass, const std::string& what) {
        log += (pass ? "PASS  " : "FAIL  ") + what + "\r\n";
        if (!pass) ok = false;
    }
    void Note(const std::string& what) { log += "      " + what + "\r\n"; }
};

int Cmp(const wchar_t* a, const wchar_t* b) {
    Version x, y;
    if (!ParseVersion(a, x) || !ParseVersion(b, y)) return 99;
    return CompareVersions(x, y);
}

}   // namespace

int RunUpdateTest(const std::wstring& dir) {
    Report r;
    r.Note("Overlook " OVERLOOK_VERSION_STR " update check self-test");

    // Parsing
    struct { const wchar_t* tag; bool ok; } kParse[] = {
        { L"v0.3.0", true }, { L"0.3.0", true }, { L"V1.2.3", true }, { L" v1.2.3 ", true }, { L"v10.20.30", true },
        { L"v0.4.0-beta.1", true }, { L"v1.0.0-alpha-1.x", true }, { L"v1.2.3+build.5", true }, { L"v1.2.3-rc.1+b", true },
        { L"", false }, { L"v", false }, { L"v1.2", false }, { L"v1.2.3.4", false }, { L"vx.y.z", false }, { L"v1..3", false },
        { L"v01.2.3", false }, { L"v1.2.3-", false }, { L"v1.2.3-beta..1", false }, { L"v1.2.3-beta.01", false },
        { L"v1.2.3+", false }, { L"v-1.2.3", false }, { L"v1.2.3-be ta", false }, { L"v9999999999.0.0", false },
        { L"nightly", false }, { L"v1.2.3-é", false }, { L"vv1.2.3", false },
        { L"v1.0.0-aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", true }, { L"v1.0.0-aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", false },
    };
    for (auto& p : kParse) {
        Version v;
        r.Check(ParseVersion(p.tag, v) == p.ok, "parse \"" + Narrow(p.tag) + "\" " + (p.ok ? "accepted" : "rejected"));
    }
    {
        Version v;
        r.Check(ParseVersion(L"v2.10.7-beta.3", v) && v.major == 2 && v.minor == 10 && v.patch == 7 && v.pre.size() == 2 &&
                v.pre[0] == "beta" && v.pre[1] == "3", "parse v2.10.7-beta.3 into 2, 10, 7, [beta, 3]");
    }

    // Ordering
    struct { const wchar_t *a, *b; int want; } kCmp[] = {
        { L"v0.3.0", L"0.3.0", 0 }, { L"v0.3.0", L"v0.3.0", 0 }, { L"v1.2.3+a", L"v1.2.3+b", 0 },
        { L"v0.3.1", L"v0.3.0", 1 }, { L"v0.4.0", L"v0.3.9", 1 }, { L"v1.0.0", L"v0.99.99", 1 }, { L"v0.10.0", L"v0.9.0", 1 },
        { L"v0.4.0-beta.1", L"v0.4.0", -1 }, { L"v0.4.0-beta.1", L"v0.3.0", 1 }, { L"v0.4.0-beta.2", L"v0.4.0-beta.1", 1 },
        { L"v0.4.0-beta.10", L"v0.4.0-beta.2", 1 }, { L"v0.4.0-alpha", L"v0.4.0-beta", -1 }, { L"v0.4.0-beta", L"v0.4.0-beta.1", -1 },
        { L"v1.0.0-1", L"v1.0.0-alpha", -1 }, { L"v1.0.0-rc.1", L"v1.0.0-beta.11", 1 }, { L"v0.3.0", L"v0.3.0-beta.9", 1 },
    };
    for (auto& c : kCmp) {
        const int got = Cmp(c.a, c.b), back = Cmp(c.b, c.a);
        r.Check(got == c.want && back == -c.want, Narrow(c.a) + (c.want < 0 ? " < " : c.want > 0 ? " > " : " == ") + Narrow(c.b));
    }
    {
        Version v;
        r.Check(ParseVersion(Widen(OVERLOOK_VERSION_STR), v) && CompareVersions(v, CurrentVersion()) == 0,
                "this build's version (" OVERLOOK_VERSION_STR ") parses and equals CurrentVersion()");
    }

    // The saved /releases/latest response
    {
        ReleaseInfo i;
        r.Check(PickRelease(kLatestSample, false, i), "latest sample: parses");
        r.Check(i.tag == L"v0.3.0", "latest sample: tag v0.3.0");
        r.Check(i.pageUrl == L"https://github.com/wallbangz/Overlook/releases/tag/v0.3.0", "latest sample: release page");
        r.Check(i.zipUrl == L"https://github.com/wallbangz/Overlook/releases/download/v0.3.0/Overlook.zip", "latest sample: Overlook.zip");
        Version v, older, same;
        ParseVersion(i.tag, v);
        ParseVersion(L"0.2.1", older);
        ParseVersion(L"0.3.0", same);
        r.Check(CompareVersions(v, older) > 0, "latest sample: newer than 0.2.1");
        r.Check(CompareVersions(v, same) == 0, "latest sample: not newer than 0.3.0");
        r.Check(PickRelease(kLatestSample, true, i) && i.tag == L"v0.3.0", "latest sample, beta channel: still v0.3.0");
    }
    {
        std::string pre = kLatestSample;
        const size_t at = pre.find("\"prerelease\": false");
        pre.replace(at, 19, "\"prerelease\": true");
        ReleaseInfo i;
        r.Check(PickRelease(pre, false, i) && i.tag.empty(), "latest sample flagged pre-release: ignored on the stable channel");
        r.Check(PickRelease(pre, true, i) && i.tag == L"v0.3.0", "latest sample flagged pre-release: counts on the beta channel");
        std::string draft = kLatestSample;
        draft.replace(draft.find("\"draft\": false"), 14, "\"draft\": true");
        r.Check(PickRelease(draft, true, i) && i.tag.empty(), "latest sample as a draft: ignored, even on the beta channel");
        std::string noZip = kLatestSample;
        noZip.replace(noZip.find("\"Overlook.zip\""), 14, "\"Other.zip\"");
        r.Check(PickRelease(noZip, false, i) && i.zipUrl.empty() && !i.pageUrl.empty(), "latest sample without Overlook.zip: no zip");
        UpdateStatus s;
        s.latest = i;
        r.Check(UpdateDownloadUrl(s) == i.pageUrl, "without the zip, Get the latest release opens the release page");
    }

    // A /releases list
    {
        ReleaseInfo i;
        r.Check(PickRelease(kListSample, false, i) && i.tag == L"v0.3.1",
                "list, stable: v0.3.1 (draft, pre-releases, -rc tag and malformed tag skipped)");
        r.Check(i.zipUrl.empty(), "list, stable: a non-https zip URL is dropped");
        r.Check(PickRelease(kListSample, true, i) && i.tag == L"v0.4.0-beta.10", "list, beta: v0.4.0-beta.10 (beta.10 > beta.2)");
        r.Check(i.pageUrl.empty(), "list, beta: a release page outside github.com is dropped");
        UpdateStatus s;
        s.latest = i;
        r.Check(UpdatePageUrl(s) == L"https://github.com/wallbangz/Overlook/releases", "no release page: the releases list instead");
    }

    // Malformed input
    {
        ReleaseInfo i;
        r.Check(!PickRelease("", false, i), "empty response: rejected");
        r.Check(!PickRelease("{\"tag_name\": \"v1.0.0\"", false, i), "truncated JSON: rejected");
        r.Check(!PickRelease("\"v1.0.0\"", false, i), "a bare string: rejected");
        r.Check(PickRelease("[]", true, i) && i.tag.empty(), "no releases: nothing newer");
        r.Check(PickRelease("{\"message\": \"Not Found\"}", false, i) && i.tag.empty(), "an error object: nothing newer");
        r.Check(PickRelease("[{\"tag_name\": 3}, 7, null]", true, i) && i.tag.empty(), "wrong types: skipped");
    }

    // Status lines and scheduling
    {
        UpdateStatus s;
        r.Check(UpdateStatusText(s) == L"Not checked yet", "status: not checked yet");
        s.state = UpdateState::Checking;
        r.Check(UpdateStatusText(s) == L"Checking…", "status: checking");
        s.state = UpdateState::UpToDate;
        r.Check(UpdateStatusText(s) == L"Up to date", "status: up to date");
        s.state = UpdateState::Failed;
        r.Check(UpdateStatusText(s) == L"Couldn’t check", "status: couldn't check");
        s.state = UpdateState::Available;
        s.latest.tag = L"0.4.0";
        r.Check(UpdateStatusText(s) == L"v0.4.0 is available", "status: available, with a v added");
        const ULONGLONG h = 3600ULL * 10000000ULL, t = 1000000ULL * h;
        r.Check(DueAt(0, t), "due: never checked");
        r.Check(!DueAt(t, t + 23 * h), "due: not after 23 hours");
        r.Check(DueAt(t, t + 24 * h), "due: after 24 hours");
        r.Check(DueAt(t, t - h), "due: the clock went back");
    }

    // One live check, reported only (offline or a firewall makes it fail; that's not a test failure)
    for (bool beta : { false, true }) {
        std::string body;
        std::wstring err;
        ReleaseInfo i;
        const std::string ch = beta ? "beta" : "stable";
        if (!net::HttpGet(kApiHost, beta ? kListPath : kLatestPath, body, err, kHeaders)) {
            r.Note("live, " + ch + ": couldn't check: " + Narrow(err));
        } else if (!PickRelease(body, beta, i)) {
            r.Note("live, " + ch + ": the response wasn't JSON");
        } else {
            r.Note("live, " + ch + ": latest " + (i.tag.empty() ? std::string("(none)") : Narrow(i.tag)) +
                   (Newer(i.tag) ? ", newer than this build" : ", not newer than this build") +
                   "\r\n        zip  " + Narrow(i.zipUrl) + "\r\n        page " + Narrow(i.pageUrl));
        }
    }

    r.log += r.ok ? "\r\nALL PASS\r\n" : "\r\nSOME FAILED\r\n";
    HANDLE f = CreateFileW((dir + L"\\update-test.txt").c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f != INVALID_HANDLE_VALUE) {
        DWORD w = 0;
        WriteFile(f, r.log.data(), (DWORD)r.log.size(), &w, nullptr);
        CloseHandle(f);
    }
    return r.ok ? 0 : 1;
}
