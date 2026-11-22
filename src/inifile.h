#pragma once
#include <string>
#include <vector>

// Line-based editor for Overlook.ini: changes only the lines it is asked to.
class IniDoc {
public:
    bool Load(const std::wstring& path);   // false if missing/unreadable: then an empty doc (UTF-16LE + BOM, CRLF)
    bool LoadBytes(const std::vector<char>& bytes);
    std::vector<char> Bytes() const;       // encoded back in the file's own encoding
    bool Save(const std::wstring& path) const;   // WriteFileReplace(path, Bytes())
    // Value as the app reads it: first matching key in the first matching section (names compared
    // case-insensitively, trimmed), inline " ;comment" stripped, whitespace trimmed. false if absent.
    bool Get(const std::wstring& section, const std::wstring& key, std::wstring& value) const;
    bool Has(const std::wstring& section, const std::wstring& key) const;
    void Set(const std::wstring& section, const std::wstring& key, const std::wstring& value);
    bool Remove(const std::wstring& section, const std::wstring& key);   // deletes that one line
    std::vector<std::wstring> Sections() const;   // section names in file order, as written (trimmed); repeats listed once
    bool HasSection(const std::wstring& section) const;

private:
    struct Line { std::wstring text, eol; };   // eol is L"\r\n", L"\n" or empty (last line only)
    enum class Enc { Utf16, Utf8, Ansi };
    struct Loc { size_t key, header, last; };   // npos when absent; last = last key line of the section (or header)
    Loc Locate(const std::wstring& section, const std::wstring& key) const;
    std::wstring NewEol() const;

    std::vector<Line> lines_;
    Enc enc_ = Enc::Utf16;
    bool bom_ = true;
    bool crlf_ = true;   // dominant line ending, used for new lines

    friend struct IniTestAccess;   // the self test inspects the lines
};

bool ReadFileBytes(const std::wstring& path, std::vector<char>& out);
// Writes to "<path>.tmp" in the same folder, then ReplaceFileW (keeps ACLs/attributes); if the
// target doesn't exist, MoveFileExW. Leaves no temp file behind on failure.
bool WriteFileReplace(const std::wstring& path, const std::vector<char>& bytes);
// The first call in this process copies path to path + ".bak" (overwriting); later calls do nothing.
bool BackupIniOnce(const std::wstring& path);
// --ini-test <folder>: round-trip checks, report in <folder>\ini-test.txt; returns 0 when all pass.
int RunIniTest(const std::wstring& dir);
