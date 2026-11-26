// HTTPS GET with WinHTTP and a small JSON reader, shared by the weather and the update check.
#include "net.h"

#include <winhttp.h>
#include <charconv>
#include <cstring>

#pragma comment(lib, "winhttp.lib")

namespace net {

namespace {

// ---------------------------------------------------------------- JSON

class JsonParser {
public:
    JsonParser(const char* p, size_t n) : p_(p), end_(p + n) {}

    bool Parse(JVal& out) {
        SkipWs();
        if (!Value(out, 0)) return false;
        SkipWs();
        return p_ == end_;
    }

private:
    // Each value is a JVal of over 100 bytes, so a body of "[0,0,0,...]" would take ~60x its size.
    // Real responses have a few thousand values; past this many, the body is rejected.
    static constexpr size_t kMaxNodes = 200000;
    const char* p_;
    const char* end_;
    size_t nodes_ = 0;

    void SkipWs() {
        while (p_ < end_ && (*p_ == ' ' || *p_ == '\t' || *p_ == '\n' || *p_ == '\r')) ++p_;
    }

    bool Lit(const char* s) {
        size_t n = strlen(s);
        if ((size_t)(end_ - p_) < n || memcmp(p_, s, n) != 0) return false;
        p_ += n;
        return true;
    }

    bool Value(JVal& v, int depth) {
        if (depth > 64 || p_ >= end_ || ++nodes_ > kMaxNodes) return false;
        switch (*p_) {
        case '{': return Object(v, depth);
        case '[': return ArrayV(v, depth);
        case '"': v.type = JVal::String; return Str(v.str);
        case 't': v.type = JVal::Bool; v.b = true; return Lit("true");
        case 'f': v.type = JVal::Bool; v.b = false; return Lit("false");
        case 'n': v.type = JVal::Null; return Lit("null");
        default: return Num(v);
        }
    }

    bool Num(JVal& v) {
        const char* s = p_;
        if (p_ < end_ && *p_ == '-') ++p_;
        while (p_ < end_ && ((*p_ >= '0' && *p_ <= '9') || *p_ == '.' || *p_ == 'e' || *p_ == 'E' ||
                             *p_ == '+' || *p_ == '-')) ++p_;
        if (p_ == s) return false;
        double d = 0;
        auto r = std::from_chars(s, p_, d);
        if (r.ec != std::errc() || r.ptr != p_) return false;
        v.type = JVal::Number;
        v.num = d;
        return true;
    }

    static void AppendUtf8(std::string& o, unsigned cp) {
        if (cp < 0x80) o.push_back((char)cp);
        else if (cp < 0x800) {
            o.push_back((char)(0xC0 | (cp >> 6)));
            o.push_back((char)(0x80 | (cp & 0x3F)));
        } else if (cp < 0x10000) {
            o.push_back((char)(0xE0 | (cp >> 12)));
            o.push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
            o.push_back((char)(0x80 | (cp & 0x3F)));
        } else {
            o.push_back((char)(0xF0 | (cp >> 18)));
            o.push_back((char)(0x80 | ((cp >> 12) & 0x3F)));
            o.push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
            o.push_back((char)(0x80 | (cp & 0x3F)));
        }
    }

    bool Hex4(unsigned& out) {
        if (end_ - p_ < 4) return false;
        unsigned v = 0;
        for (int i = 0; i < 4; ++i) {
            char c = *p_++;
            v <<= 4;
            if (c >= '0' && c <= '9') v |= (unsigned)(c - '0');
            else if (c >= 'a' && c <= 'f') v |= (unsigned)(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') v |= (unsigned)(c - 'A' + 10);
            else return false;
        }
        out = v;
        return true;
    }

    bool Str(std::string& out) {
        out.clear();
        if (p_ >= end_ || *p_ != '"') return false;
        ++p_;
        while (p_ < end_) {
            char c = *p_++;
            if (c == '"') return true;
            if (c != '\\') { out.push_back(c); continue; }
            if (p_ >= end_) return false;
            char e = *p_++;
            switch (e) {
            case '"': out.push_back('"'); break;
            case '\\': out.push_back('\\'); break;
            case '/': out.push_back('/'); break;
            case 'b': out.push_back('\b'); break;
            case 'f': out.push_back('\f'); break;
            case 'n': out.push_back('\n'); break;
            case 'r': out.push_back('\r'); break;
            case 't': out.push_back('\t'); break;
            case 'u': {
                unsigned cp = 0;
                if (!Hex4(cp)) return false;
                if (cp >= 0xD800 && cp <= 0xDBFF) {
                    unsigned lo = 0;
                    if (end_ - p_ >= 6 && p_[0] == '\\' && p_[1] == 'u') {
                        p_ += 2;
                        if (!Hex4(lo)) return false;
                        if (lo >= 0xDC00 && lo <= 0xDFFF) cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                        else cp = 0xFFFD;
                    } else cp = 0xFFFD;
                } else if (cp >= 0xDC00 && cp <= 0xDFFF) cp = 0xFFFD;
                AppendUtf8(out, cp);
                break;
            }
            default: return false;
            }
        }
        return false;
    }

    bool ArrayV(JVal& v, int depth) {
        v.type = JVal::Array;
        ++p_; // [
        SkipWs();
        if (p_ < end_ && *p_ == ']') { ++p_; return true; }
        for (;;) {
            SkipWs();
            v.arr.emplace_back();
            if (!Value(v.arr.back(), depth + 1)) return false;
            SkipWs();
            if (p_ >= end_) return false;
            if (*p_ == ',') { ++p_; continue; }
            if (*p_ == ']') { ++p_; return true; }
            return false;
        }
    }

    bool Object(JVal& v, int depth) {
        v.type = JVal::Object;
        ++p_; // {
        SkipWs();
        if (p_ < end_ && *p_ == '}') { ++p_; return true; }
        for (;;) {
            SkipWs();
            std::string k;
            if (!Str(k)) return false;
            SkipWs();
            if (p_ >= end_ || *p_ != ':') return false;
            ++p_;
            SkipWs();
            v.keys.push_back(std::move(k));
            v.vals.emplace_back();
            if (!Value(v.vals.back(), depth + 1)) return false;
            SkipWs();
            if (p_ >= end_) return false;
            if (*p_ == ',') { ++p_; continue; }
            if (*p_ == '}') { ++p_; return true; }
            return false;
        }
    }
};

}   // namespace

const JVal* Get(const JVal* v, const char* key) {
    if (!v || v->type != JVal::Object) return nullptr;
    for (size_t i = 0; i < v->keys.size(); ++i)
        if (v->keys[i] == key) return &v->vals[i];
    return nullptr;
}

const JVal* At(const JVal* v, size_t i) {
    if (!v || v->type != JVal::Array || i >= v->arr.size()) return nullptr;
    return &v->arr[i];
}

bool GetNum(const JVal* v, double& out) {
    if (!v || v->type != JVal::Number) return false;
    out = v->num;
    return true;
}

bool GetStr(const JVal* v, std::string& out) {
    if (!v || v->type != JVal::String) return false;
    out = v->str;
    return true;
}

// ---------------------------------------------------------------- WinHTTP

namespace {
struct WinHttpHandle {
    HINTERNET h = nullptr;
    WinHttpHandle() = default;
    explicit WinHttpHandle(HINTERNET x) : h(x) {}
    WinHttpHandle(const WinHttpHandle&) = delete;
    WinHttpHandle& operator=(const WinHttpHandle&) = delete;
    ~WinHttpHandle() { if (h) WinHttpCloseHandle(h); }
};
}   // namespace

bool HttpGet(const wchar_t* host, const std::wstring& pathAndQuery, std::string& body, std::wstring& err,
             const wchar_t* headers) {
    body.clear();
    WinHttpHandle ses(WinHttpOpen(L"Overlook/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                  WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
    if (!ses.h) { err = L"WinHttpOpen failed: " + std::to_wstring(GetLastError()); return false; }
    WinHttpSetTimeouts(ses.h, 10000, 10000, 10000, 10000);

    WinHttpHandle con(WinHttpConnect(ses.h, host, INTERNET_DEFAULT_HTTPS_PORT, 0));
    if (!con.h) { err = L"WinHttpConnect failed: " + std::to_wstring(GetLastError()); return false; }

    WinHttpHandle req(WinHttpOpenRequest(con.h, L"GET", pathAndQuery.c_str(), nullptr, WINHTTP_NO_REFERER,
                                         WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE));
    if (!req.h) { err = L"WinHttpOpenRequest failed: " + std::to_wstring(GetLastError()); return false; }

    if (!WinHttpSendRequest(req.h, headers ? headers : WINHTTP_NO_ADDITIONAL_HEADERS, headers ? (DWORD)-1L : 0,
                            WINHTTP_NO_REQUEST_DATA, 0, 0, 0)) {
        err = L"WinHttpSendRequest failed: " + std::to_wstring(GetLastError());
        return false;
    }
    if (!WinHttpReceiveResponse(req.h, nullptr)) {
        err = L"WinHttpReceiveResponse failed: " + std::to_wstring(GetLastError());
        return false;
    }
    DWORD status = 0, sz = sizeof status;
    if (!WinHttpQueryHeaders(req.h, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                             WINHTTP_HEADER_NAME_BY_INDEX, &status, &sz, WINHTTP_NO_HEADER_INDEX)) {
        err = L"WinHttpQueryHeaders failed: " + std::to_wstring(GetLastError());
        return false;
    }
    if (status != 200) {
        err = L"HTTP " + std::to_wstring(status) + L" from " + host;
        return false;
    }
    const size_t kMaxBody = 1u << 20;   // real responses are under 150 KB (GitHub's list of 30 releases)
    for (;;) {
        DWORD avail = 0;
        if (!WinHttpQueryDataAvailable(req.h, &avail)) {
            err = L"WinHttpQueryDataAvailable failed: " + std::to_wstring(GetLastError());
            return false;
        }
        if (avail == 0) break;
        if (body.size() + avail > kMaxBody) { err = L"response from " + std::wstring(host) + L" too large"; return false; }
        size_t old = body.size();
        body.resize(old + avail);
        DWORD got = 0;
        if (!WinHttpReadData(req.h, body.data() + old, avail, &got)) {
            err = L"WinHttpReadData failed: " + std::to_wstring(GetLastError());
            return false;
        }
        body.resize(old + got);
        if (got == 0) break;
    }
    return true;
}

bool HttpGetJson(const wchar_t* host, const std::wstring& path, JVal& out, std::wstring& err) {
    std::string body;
    if (!HttpGet(host, path, body, err)) return false;
    JsonParser jp(body.data(), body.size());
    if (!jp.Parse(out)) { err = L"invalid JSON from " + std::wstring(host); return false; }
    return true;
}

bool ParseJson(const std::string& text, JVal& out) {
    out = JVal();
    JsonParser jp(text.data(), text.size());
    return jp.Parse(out);
}

bool GetBool(const JVal* v, bool& out) {
    if (!v || v->type != JVal::Bool) return false;
    out = v->b;
    return true;
}

}   // namespace net
