#include "inifile.h"
#include "settings.h"
#include <windows.h>
#include <shlobj.h>
#include <algorithm>
#include <cwctype>
#include <string>
#include <vector>

namespace {

enum class Kind { Blank, Comment, Header, Key, Other };
struct Parsed { Kind kind = Kind::Other; std::wstring name; size_t eq = std::wstring::npos; };

bool IsWs(wchar_t c) { return iswspace(c) != 0; }

std::wstring Trim(const std::wstring& s) {
    size_t b = 0, e = s.size();
    while (b < e && IsWs(s[b])) ++b;
    while (e > b && IsWs(s[e - 1])) --e;
    return s.substr(b, e - b);
}

bool SameName(const std::wstring& a, const std::wstring& b) {
    return CompareStringOrdinal(a.c_str(), (int)a.size(), b.c_str(), (int)b.size(), TRUE) == CSTR_EQUAL;
}

Parsed Parse(const std::wstring& t) {
    Parsed p;
    size_t i = 0;
    while (i < t.size() && IsWs(t[i])) ++i;
    if (i == t.size()) { p.kind = Kind::Blank; return p; }
    if (t[i] == L';' || t[i] == L'#') { p.kind = Kind::Comment; return p; }
    if (t[i] == L'[') {
        const size_t close = t.find(L']', i);
        if (close == std::wstring::npos) return p;
        p.kind = Kind::Header;
        p.name = Trim(t.substr(i + 1, close - i - 1));
        return p;
    }
    const size_t eq = t.find(L'=');
    if (eq == std::wstring::npos) return p;
    p.name = Trim(t.substr(0, eq));
    if (p.name.empty()) return p;
    p.kind = Kind::Key;
    p.eq = eq;
    return p;
}

// Where the value text sits in a key line: [a, b) is the value without the whitespace around it and
// without an inline " ;comment" (the same cut ReadStr makes). Empty values give a == b.
void ValueSpan(const std::wstring& t, size_t eq, size_t& a, size_t& b) {
    size_t end = t.find(L" ;", eq + 1);
    if (end == std::wstring::npos) end = t.size();
    a = eq + 1;
    while (a < end && IsWs(t[a])) ++a;
    b = end;
    while (b > a && IsWs(t[b - 1])) --b;
}

bool IsBlankLine(const std::wstring& t) { return Parse(t).kind == Kind::Blank; }

std::wstring FromBytes(const char* p, size_t n, UINT cp, DWORD flags, bool& ok) {
    ok = true;
    if (n == 0) return std::wstring();
    const int len = MultiByteToWideChar(cp, flags, p, (int)n, nullptr, 0);
    if (len <= 0) { ok = false; return std::wstring(); }
    std::wstring w((size_t)len, L'\0');
    MultiByteToWideChar(cp, flags, p, (int)n, w.data(), len);
    return w;
}

std::vector<char> ToBytes(const std::wstring& w, UINT cp) {
    if (w.empty()) return {};
    const int len = WideCharToMultiByte(cp, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::vector<char> out((size_t)std::max(len, 0));
    if (len > 0) WideCharToMultiByte(cp, 0, w.c_str(), (int)w.size(), out.data(), len, nullptr, nullptr);
    return out;
}

}   // namespace

std::wstring IniDoc::NewEol() const { return crlf_ ? L"\r\n" : L"\n"; }

bool IniDoc::LoadBytes(const std::vector<char>& bytes) {
    lines_.clear();
    const unsigned char* u = reinterpret_cast<const unsigned char*>(bytes.data());
    const size_t n = bytes.size();
    std::wstring text;
    bom_ = false;
    if (n >= 2 && u[0] == 0xFF && u[1] == 0xFE) {
        enc_ = Enc::Utf16; bom_ = true;
        text.assign(reinterpret_cast<const wchar_t*>(bytes.data() + 2), (n - 2) / 2);   // an odd stray byte is dropped
    } else if (n >= 3 && u[0] == 0xEF && u[1] == 0xBB && u[2] == 0xBF) {
        bool ok;
        enc_ = Enc::Utf8; bom_ = true;
        text = FromBytes(bytes.data() + 3, n - 3, CP_UTF8, 0, ok);
    } else {
        bool ok;
        text = FromBytes(bytes.data(), n, CP_UTF8, MB_ERR_INVALID_CHARS, ok);
        if (ok) enc_ = Enc::Utf8;
        else { enc_ = Enc::Ansi; text = FromBytes(bytes.data(), n, CP_ACP, 0, ok); }
    }

    size_t crlf = 0, lf = 0, start = 0;
    while (start < text.size()) {
        const size_t nl = text.find(L'\n', start);
        Line ln;
        if (nl == std::wstring::npos) {
            ln.text = text.substr(start);
            start = text.size();
        } else if (nl > start && text[nl - 1] == L'\r') {
            ln.text = text.substr(start, nl - 1 - start); ln.eol = L"\r\n"; ++crlf;
            start = nl + 1;
        } else {
            ln.text = text.substr(start, nl - start); ln.eol = L"\n"; ++lf;
            start = nl + 1;
        }
        lines_.push_back(std::move(ln));
    }
    crlf_ = crlf >= lf;   // CRLF when unknown or tied
    return true;
}

bool IniDoc::Load(const std::wstring& path) {
    std::vector<char> bytes;
    if (!ReadFileBytes(path, bytes)) {
        lines_.clear(); enc_ = Enc::Utf16; bom_ = true; crlf_ = true;
        return false;
    }
    return LoadBytes(bytes);
}

std::vector<char> IniDoc::Bytes() const {
    std::wstring all;
    for (const Line& l : lines_) { all += l.text; all += l.eol; }
    std::vector<char> out;
    if (enc_ == Enc::Utf16) {
        if (bom_) out = { '\xFF', '\xFE' };
        const char* p = reinterpret_cast<const char*>(all.data());
        out.insert(out.end(), p, p + all.size() * sizeof(wchar_t));
        return out;
    }
    if (enc_ == Enc::Utf8 && bom_) out = { '\xEF', '\xBB', '\xBF' };
    const std::vector<char> body = ToBytes(all, enc_ == Enc::Utf8 ? CP_UTF8 : CP_ACP);
    out.insert(out.end(), body.begin(), body.end());
    return out;
}

bool IniDoc::Save(const std::wstring& path) const { return WriteFileReplace(path, Bytes()); }

// Mirrors GetPrivateProfileString: only the first section of a name counts, and within it the first key.
IniDoc::Loc IniDoc::Locate(const std::wstring& section, const std::wstring& key) const {
    const std::wstring sec = Trim(section), k = Trim(key);
    Loc loc{ std::wstring::npos, std::wstring::npos, std::wstring::npos };
    int state = sec.empty() ? 1 : 0;   // 0 before, 1 inside, 2 after the first matching section
    for (size_t i = 0; i < lines_.size() && state < 2; ++i) {
        const Parsed p = Parse(lines_[i].text);
        if (p.kind == Kind::Header) {
            if (state == 1) state = 2;
            else if (SameName(p.name, sec)) { state = 1; loc.header = loc.last = i; }
        } else if (p.kind == Kind::Key && state == 1) {
            loc.last = i;
            if (loc.key == std::wstring::npos && SameName(p.name, k)) loc.key = i;
        }
    }
    return loc;
}

bool IniDoc::Get(const std::wstring& section, const std::wstring& key, std::wstring& value) const {
    const Loc loc = Locate(section, key);
    if (loc.key == std::wstring::npos) return false;
    const std::wstring& t = lines_[loc.key].text;
    // Same steps as the app: GetPrivateProfileString trims the value and drops one pair of surrounding
    // quotes (" or '), then ReadStr cuts at the first " ;" and trims again.
    std::wstring s = Trim(t.substr(Parse(t).eq + 1));
    if (s.size() >= 2 && (s[0] == L'"' || s[0] == L'\'') && s.back() == s[0]) s = s.substr(1, s.size() - 2);
    const size_t sc = s.find(L" ;");
    if (sc != std::wstring::npos) s.resize(sc);
    value = Trim(s);
    return true;
}

bool IniDoc::Has(const std::wstring& section, const std::wstring& key) const {
    return Locate(section, key).key != std::wstring::npos;
}

void IniDoc::Set(const std::wstring& section, const std::wstring& key, const std::wstring& value) {
    const std::wstring sec = Trim(section), k = Trim(key);
    const Loc loc = Locate(sec, k);
    if (loc.key != std::wstring::npos) {
        std::wstring& t = lines_[loc.key].text;
        size_t a, b;
        ValueSpan(t, Parse(t).eq, a, b);
        t.replace(a, b - a, value);
        return;
    }
    if (loc.last != std::wstring::npos) {
        // After the section's last key, so the comments and blank lines before the next section stay below it.
        Line nl{ k + L"=" + value, NewEol() };
        if (loc.last + 1 == lines_.size() && lines_[loc.last].eol.empty()) {
            lines_[loc.last].eol = NewEol();   // the file had no final newline; keep it that way
            nl.eol.clear();
        }
        lines_.insert(lines_.begin() + (ptrdiff_t)loc.last + 1, std::move(nl));
        return;
    }
    if (!lines_.empty()) {
        if (lines_.back().eol.empty()) lines_.back().eol = NewEol();
        if (!IsBlankLine(lines_.back().text)) lines_.push_back({ L"", NewEol() });
    }
    lines_.push_back({ L"[" + sec + L"]", NewEol() });
    lines_.push_back({ k + L"=" + value, NewEol() });
}

bool IniDoc::Remove(const std::wstring& section, const std::wstring& key) {
    const Loc loc = Locate(section, key);
    if (loc.key == std::wstring::npos) return false;
    lines_.erase(lines_.begin() + (ptrdiff_t)loc.key);
    return true;
}

std::vector<std::wstring> IniDoc::Sections() const {
    std::vector<std::wstring> out;
    for (const Line& l : lines_) {
        const Parsed p = Parse(l.text);
        if (p.kind != Kind::Header) continue;
        bool seen = false;
        for (const std::wstring& s : out) if (SameName(s, p.name)) { seen = true; break; }
        if (!seen) out.push_back(p.name);
    }
    return out;
}

bool IniDoc::HasSection(const std::wstring& section) const {
    const std::wstring sec = Trim(section);
    for (const Line& l : lines_) {
        const Parsed p = Parse(l.text);
        if (p.kind == Kind::Header && SameName(p.name, sec)) return true;
    }
    return false;
}

// ---------------------------------------------------------------- files

bool ReadFileBytes(const std::wstring& path, std::vector<char>& out) {
    out.clear();
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER sz;
    bool ok = GetFileSizeEx(h, &sz) && sz.QuadPart <= (64 << 20);   // an ini of 64 MB is not an ini
    if (ok) {
        out.resize((size_t)sz.QuadPart);
        size_t got = 0;
        while (ok && got < out.size()) {
            DWORD n = 0;
            ok = ReadFile(h, out.data() + got, (DWORD)(out.size() - got), &n, nullptr) && n > 0;
            got += n;
        }
        if (got < out.size()) ok = false;
    }
    CloseHandle(h);
    if (!ok) out.clear();
    return ok;
}

bool WriteFileReplace(const std::wstring& path, const std::vector<char>& bytes) {
    const std::wstring tmp = path + L".tmp";
    HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    bool ok = true;
    size_t done = 0;
    while (ok && done < bytes.size()) {
        DWORD n = 0;
        ok = WriteFile(h, bytes.data() + done, (DWORD)(bytes.size() - done), &n, nullptr) && n > 0;
        done += n;
    }
    ok = ok && FlushFileBuffers(h);
    ok = CloseHandle(h) && ok;
    if (ok) {
        if (GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES)
            ok = ReplaceFileW(path.c_str(), tmp.c_str(), nullptr, 0, nullptr, nullptr) != FALSE;
        else
            ok = MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING) != FALSE;
    }
    if (!ok) DeleteFileW(tmp.c_str());
    return ok;
}

bool BackupIniOnce(const std::wstring& path) {
    static bool done = false;
    if (done) return true;
    done = true;
    return CopyFileW(path.c_str(), (path + L".bak").c_str(), FALSE) != FALSE;
}

// ---------------------------------------------------------------- self test

namespace {

struct Report {
    std::string log;
    bool ok = true;
    void Check(bool pass, const std::string& what) {
        log += (pass ? "PASS  " : "FAIL  ") + what + "\r\n";
        if (!pass) ok = false;
    }
    void Note(const std::string& what) { log += "      " + what + "\r\n"; }
};

std::string Narrow(const std::wstring& w) {
    std::vector<char> b = ToBytes(w, CP_UTF8);
    return std::string(b.begin(), b.end());
}

// What the app reads: GetPrivateProfileStringW plus ReadStr's inline-comment strip and trim.
bool AppRead(const std::wstring& path, const std::wstring& sec, const std::wstring& key, std::wstring& out) {
    wchar_t buf[512];
    GetPrivateProfileStringW(sec.c_str(), key.c_str(), L"\x01", buf, ARRAYSIZE(buf), path.c_str());
    std::wstring s = buf;
    if (s == L"\x01") return false;
    const size_t sc = s.find(L" ;");
    if (sc != std::wstring::npos) s.resize(sc);
    out = Trim(s);
    return true;
}

size_t LcsLen(const std::vector<std::wstring>& a, const std::vector<std::wstring>& b) {
    std::vector<size_t> prev(b.size() + 1), cur(b.size() + 1);
    for (size_t i = 1; i <= a.size(); ++i) {
        for (size_t j = 1; j <= b.size(); ++j)
            cur[j] = a[i - 1] == b[j - 1] ? prev[j - 1] + 1 : std::max(prev[j], cur[j - 1]);
        std::swap(prev, cur);
    }
    return prev[b.size()];
}

template <class V>
std::vector<std::wstring> Texts(const V& v) {
    std::vector<std::wstring> t;
    for (const auto& l : v) t.push_back(l.text);
    return t;
}

}   // namespace

struct IniTestAccess {
    static const auto& Lines(const IniDoc& d) { return d.lines_; }
};

namespace {

struct KeyEdit { const wchar_t* key; const wchar_t* value; };

void RunCase(Report& r, const std::string& label, const std::vector<char>& orig, const std::wstring& dir, const std::wstring& stem) {
    const std::wstring origPath = dir + L"\\" + stem + L".orig.ini";
    const std::wstring editPath = dir + L"\\" + stem + L".edit.ini";
    WriteFileReplace(origPath, orig);

    IniDoc d0, d;
    d0.LoadBytes(orig);
    d.LoadBytes(orig);
    const auto& O = IniTestAccess::Lines(d0);

    // 1. untouched round trip
    r.Check(d.Bytes() == orig, label + ": load + Bytes() is byte-identical");

    // Get agrees with GetPrivateProfileString for every key line of the original.
    {
        std::wstring sec;
        std::string bad;
        size_t checked = 0;
        for (size_t i = 0; i < O.size(); ++i) {
            const Parsed p = Parse(O[i].text);
            if (p.kind == Kind::Header) sec = p.name;
            if (p.kind != Kind::Key || sec.empty()) continue;
            std::wstring mine, theirs;
            const bool g = d.Get(sec, p.name, mine), a = AppRead(origPath, sec, p.name, theirs);
            ++checked;
            if (g != a || (g && mine != theirs)) bad += " line " + std::to_string(i + 1) + " key " + Narrow(p.name);
        }
        r.Check(bad.empty(), label + ": Get agrees with GetPrivateProfileString on " + std::to_string(checked) + " keys" + (bad.empty() ? "" : ", differs at:" + bad));
    }

    // 2. edits
    static const KeyEdit kExisting[] = { { L"Opacity", L"77" }, { L"Gap", L"31" }, { L"Color", L"#123456" } };
    struct Done { std::wstring key, value, orig; };
    std::vector<Done> done;
    for (const KeyEdit& e : kExisting) {
        std::wstring ov;
        if (!d.Get(L"Clock", e.key, ov)) { r.Note(label + ": SKIP [Clock] key " + Narrow(e.key) + " (not in this file)"); continue; }
        done.push_back({ e.key, e.value, ov });
    }
    for (const Done& e : done) d.Set(L"Clock", e.key, e.value);
    d.Set(L"Clock", L"TestNewKey", L"42");
    d.Set(L"Monitor test", L"Opacity", L"33");

    const bool saved = d.Save(editPath);
    r.Check(saved, label + ": Save");
    r.Check(GetFileAttributesW((editPath + L".tmp").c_str()) == INVALID_FILE_ATTRIBUTES, label + ": no .tmp left behind");

    struct Touch { std::wstring sec, key, value; };
    std::vector<Touch> touched;
    for (const Done& e : done) touched.push_back({ L"Clock", e.key, e.value });
    touched.push_back({ L"Clock", L"TestNewKey", L"42" });
    touched.push_back({ L"Monitor test", L"Opacity", L"33" });
    IniDoc d2;
    d2.Load(editPath);
    for (const Touch& t : touched) {
        std::wstring w, g, g2;
        const std::string name = label + ": [" + Narrow(t.sec) + "] " + Narrow(t.key);
        r.Check(AppRead(editPath, t.sec, t.key, w) && w == t.value, name + " reads back via GetPrivateProfileString");
        r.Check(d.Get(t.sec, t.key, g) && g == t.value && d2.Get(t.sec, t.key, g2) && g2 == t.value, name + " Get agrees");
    }

    const auto& N = IniTestAccess::Lines(d);
    const bool endsBlank = !O.empty() && IsBlankLine(O.back().text);
    const size_t inserted = 3 + (endsBlank ? 0 : 1);   // new key, header, its key, and a blank line when needed
    const size_t unmatched = N.size() - LcsLen(Texts(O), Texts(N));
    r.Check(N.size() == O.size() + inserted && unmatched == done.size() + inserted,
            label + ": diff touches only the edited lines (" + std::to_string(done.size()) + " changed, " + std::to_string(inserted) + " inserted)");

    // 3. restore
    for (const Done& e : done) d.Set(L"Clock", e.key, e.orig);
    const bool rm1 = d.Remove(L"Clock", L"TestNewKey"), rm2 = d.Remove(L"Monitor test", L"Opacity");
    r.Check(rm1 && rm2 && d.HasSection(L"Monitor test") && !d.Has(L"Clock", L"TestNewKey"), label + ": Remove deletes the key line and keeps the header");
    const auto& R = IniTestAccess::Lines(d);
    const size_t extra = endsBlank ? 1 : 2;
    bool same = R.size() == O.size() + extra;
    for (size_t i = 0; same && i < O.size(); ++i) {
        same = R[i].text == O[i].text && (R[i].eol == O[i].eol || (i + 1 == O.size() && O[i].eol.empty()));   // a missing final newline is added once
    }
    if (same) {
        if (!endsBlank) same = IsBlankLine(R[O.size()].text);
        same = same && R.back().text == L"[Monitor test]";
    }
    r.Check(same, label + ": after restore only the [Monitor test] header (and its blank line) remains");
    r.Check(d.Bytes().size() >= 2 && d.Bytes()[0] == orig[0] && d.Bytes()[1] == orig[1], label + ": encoding/BOM unchanged");
}

}   // namespace

int RunIniTest(const std::wstring& dir) {
    SHCreateDirectoryExW(nullptr, dir.c_str(), nullptr);
    Report r;

    // (a) the default ini
    const std::wstring defPath = dir + L"\\default.ini";
    DeleteFileW(defPath.c_str());
    WriteDefaultIni(defPath);
    std::vector<char> bytes;
    if (ReadFileBytes(defPath, bytes)) RunCase(r, "default", bytes, dir, L"a-default");
    else r.Check(false, "default: could not write default.ini");

    // (b) the user's own ini, if there is one (values are never printed)
    if (ReadFileBytes(IniPath(), bytes)) RunCase(r, "user", bytes, dir, L"b-user");
    else r.Note("user: no Overlook.ini in the settings folder, skipped");

    // (c) a hand-made tricky file: UTF-8 without BOM, LF, no final newline
    std::wstring tricky =
        L"; tricky test file °C é\n"
        L"Stray=before any section\n"
        L"[Clock]\n"
        L"Opacity=80 ; my value\n"
        L"opacity=55\n"
        L"  Gap = 25  \n"
        L"Color=#FFFFFF   ; spaced comment\n"
        L"Quoted=\"hello world\"\n"
        L"Single='x'\n"
        L"QuotedComment=\"a\" ; c\n"
        L"EmptyComment= ; c\n"
        L"Url=http://x/y;z\n"
        L"TimeSize=145\n"
        L"; trailing comment of clock\n"
        L"\n"
        L"[ Monitor primary ]\n"
        L"Opacity=60\n"
        L"[Clock]\n"
        L"OnlyInSecond=9\n"
        L"TimeSize=200\n"
        L"[Monitor primary]\n"
        L"Opacity=99";
    {
        std::vector<char> u8 = ToBytes(tricky, CP_UTF8);
        RunCase(r, "utf8-lf", u8, dir, L"c-utf8-lf");

        // (d) the same as UTF-16LE with BOM and alternating CRLF / LF
        std::wstring mixed;
        bool crlf = true;
        for (wchar_t c : tricky) {
            if (c == L'\n') { if (crlf) mixed += L'\r'; crlf = !crlf; }
            mixed += c;
        }
        std::vector<char> u16 = { '\xFF', '\xFE' };
        const char* p = reinterpret_cast<const char*>(mixed.data());
        u16.insert(u16.end(), p, p + mixed.size() * sizeof(wchar_t));
        RunCase(r, "utf16-mixed", u16, dir, L"d-utf16-mixed");

        // extras: ANSI fallback (invalid UTF-8) and UTF-8 with BOM must also round-trip
        std::vector<char> ansi = { '[', 'C', 'l', 'o', 'c', 'k', ']', '\r', '\n', 'W', '=', '\xE9', '\r', '\n' };
        IniDoc a; a.LoadBytes(ansi);
        r.Check(a.Bytes() == ansi, "ansi: invalid UTF-8 falls back to the ANSI code page and round-trips");
        std::vector<char> bom8 = { '\xEF', '\xBB', '\xBF' };
        bom8.insert(bom8.end(), u8.begin(), u8.end());
        IniDoc b; b.LoadBytes(bom8);
        r.Check(b.Bytes() == bom8, "utf8-bom: round-trips");
        IniDoc e;
        r.Check(!e.Load(dir + L"\\does-not-exist.ini") && e.Bytes() == std::vector<char>({ '\xFF', '\xFE' }), "missing file: Load fails, empty doc");
        e.Set(L"Clock", L"A", L"1");
        const std::vector<char> eb = e.Bytes();
        r.Check(eb.size() > 2 && (unsigned char)eb[0] == 0xFF && (unsigned char)eb[1] == 0xFE, "missing file: new doc is UTF-16LE with BOM");
    }

    r.log += r.ok ? "\r\nALL PASS\r\n" : "\r\nSOME FAILED\r\n";
    const std::string& s = r.log;
    WriteFileReplace(dir + L"\\ini-test.txt", std::vector<char>(s.begin(), s.end()));
    return r.ok ? 0 : 1;
}
