#pragma once
// HTTPS GET with WinHTTP, and a small JSON reader. Used from worker threads (weather, update check).
#include <windows.h>
#include <string>
#include <vector>

namespace net {

struct JVal {
    enum Type { Null, Bool, Number, String, Array, Object } type = Null;
    bool b = false;
    double num = 0;
    std::string str;                 // UTF-8
    std::vector<JVal> arr;
    std::vector<std::string> keys;   // object member names (parallel to vals)
    std::vector<JVal> vals;
};

bool ParseJson(const std::string& text, JVal& out);
const JVal* Get(const JVal* v, const char* key);   // object member, or nullptr
const JVal* At(const JVal* v, size_t i);           // array element, or nullptr
bool GetNum(const JVal* v, double& out);
bool GetStr(const JVal* v, std::string& out);
bool GetBool(const JVal* v, bool& out);

// HTTPS GET host + path(+query), with a generic User-Agent and nothing else identifying.
// headers: extra request headers ("Name: value\r\n..."), or nullptr. Returns true and the body on HTTP 200.
bool HttpGet(const wchar_t* host, const std::wstring& pathAndQuery, std::string& body, std::wstring& err,
             const wchar_t* headers = nullptr);
bool HttpGetJson(const wchar_t* host, const std::wstring& path, JVal& out, std::wstring& err);

}   // namespace net
