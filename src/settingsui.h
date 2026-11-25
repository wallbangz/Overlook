#pragma once
// Internals of the settings window, shared by settingswindow.cpp (model, layout, drawing, input)
// and settingsuia.cpp (UI Automation, so Narrator and other screen readers can use it).
#include <windows.h>
#include <string>
#include <vector>
#include <memory>
#include <wrl/client.h>
#include <d2d1_1.h>
#include <dwrite_3.h>
#include "settingswindow.h"
#include "inifile.h"

// main.cpp: the debug menu's "Copy report" text (versions, monitors, each clock's effect, the
// debug settings in effect; no city, wallpaper, track or path).
std::wstring EffectsReport();

namespace sw {

using Microsoft::WRL::ComPtr;

enum class Kind {
    NavItem,    // a page in the left pane
    Combo,      // drop-down list (the "Settings for" scope selector too)
    EditCombo,  // text box with a list of suggestions
    Toggle,
    Slider,
    Edit,
    Color,      // colour swatch: opens the colour picker
    Button,
    Link,
    Reset,      // "Reset": removes a monitor's override
    Static,     // text only (the DEBUG MODE footer): never focused or clicked
};

enum Action { kNone, kUndo, kOpenIni, kAppsDefault, kScope, kUrl, kOpenFolder, kOpenLog, kCopyReport };

struct Option { std::wstring value, label; };

struct Ctl {
    int id = 0;                   // stable while the window is open (UI Automation runtime id)
    Kind kind = Kind::Toggle;
    int page = -1;                // -1: always shown (left pane)
    std::wstring key;             // ini key, "" = none
    std::wstring label, desc;     // row title and the line under it
    std::wstring defValue;        // what the app uses when the key is missing
    bool appWide = false;         // read from [Clock] only
    std::wstring dependsOn;       // greyed while this toggle is off ("!Key": while on)
    // Slider
    double lo = 0, hi = 100, step = 1, big = 10;
    int decimals = 0;
    std::wstring unit, zeroText;  // " %", " ms"...; text shown for 0 (e.g. "Off")
    // Combo, EditCombo
    std::vector<Option> options;
    double boxWidth = 0;          // DIPs, 0 = the kind's default
    // Button, Link
    Action action = kNone;
    std::wstring url;             // kUrl: what it opens
    int owner = -1;               // Reset: index of the setting it resets
    std::wstring monitorId;       // Monitors page: a clock's on/off (HiddenMonitors)

    // State
    std::wstring value;           // effective value, as ini text
    bool overridden = false;      // set in the scope's own [Monitor ...] section
    std::wstring elsewhere;       // All monitors: "LG OLED keeps its own: 16.2 %" (monitors that override it)
    std::vector<std::wstring> elsewhereSections;   // ... and the sections that do
    bool enabled = true;
    bool visible = true;
    D2D1_RECT_F box{};            // control, DIPs; page controls in content coordinates (before scrolling)
    D2D1_RECT_F card{};           // its row (hit area for toggles)
};

// One line of a page: a section header or a row of controls (left to right; the first one owns the label).
enum Note { kStatic, kCityEmpty, kMonitorSpec, kAbout, kEffectsCity };

struct Item {
    std::wstring header, note;    // header text, or a note line
    Note noteKind = kStatic;      // notes whose text depends on the current values
    std::vector<int> ctls;
    D2D1_RECT_F rc{};             // content coordinates; empty = not shown
    float extraH = 0;             // a row's bottom line ("... keeps its own"), DIPs
};

struct Page {
    std::wstring name, desc;
    wchar_t icon = 0;
    std::vector<Item> items;
    float height = 0;             // content height, DIPs
};

struct Scope {
    std::wstring label;
    int mon = -1;                 // index into monitors_, -1 = all monitors / a section without a connected monitor
    std::wstring section;         // "Clock" or the [Monitor ...] section written to
    bool exists = true;           // the section is already in the file
};

struct Theme {
    D2D1_COLOR_F bg, pane, card, cardHover, cardStroke, text, text2, disabled, accent, accentHover, onAccent, link,
        ctlFill, ctlHover, ctlStroke, ctlStrokeBottom, toggleOff, focus, popup, selFill, track;
    bool dark = false, contrast = false;
};

class UiaLink;   // settingsuia.cpp

class Window {
public:
    // ---- for settingsuia.cpp
    HWND hwnd = nullptr;
    std::vector<Ctl> ctls;
    std::vector<int> UiaOrder() const;                 // shown controls, in focus order
    int CtlIndex(int id) const;                        // -1 if gone
    RECT ScreenRect(int i) const;                      // physical px
    bool Offscreen(int i) const;
    int FocusIndex() const { return focus_; }
    bool PopupOpenFor(int i) const { return popup_.ctl == i; }
    std::wstring ValueText(int i) const;               // what the control shows ("80 %", "On", "Celsius")
    std::wstring UiaName(int i) const;
    std::wstring ShownDesc(int i) const;               // the line under the label, as shown
    double SliderValue(int i) const;
    bool ToggleOn(int i) const;
    int CurrentPage() const { return page_; }
    void UiaFocus(int i);
    void UiaToggle(int i);
    void UiaInvoke(int i);
    void UiaSetRange(int i, double v);
    bool UiaSetValue(int i, const std::wstring& v);
    void UiaExpand(int i, bool open);
    int CtlAtScreen(POINT pt) const;
    UiaLink* uia = nullptr;

    // ---- the window
    Window(HINSTANCE inst, const SettingsHost& host);
    ~Window();
    bool Create();
    void Reloaded();
    static LRESULT CALLBACK Proc(HWND, UINT, WPARAM, LPARAM);
    static LRESULT CALLBACK EditProc(HWND, UINT, WPARAM, LPARAM);

private:
    LRESULT Handle(UINT msg, WPARAM wp, LPARAM lp);
    // model
    void BuildControls();
    void BuildMonitorRows();
    void BuildScopes();
    void LoadValues();
    void UpdateEnabled();
    std::wstring Effective(const Ctl& c, bool* overridden) const;
    std::wstring Display(const Ctl& c, const std::wstring& v) const;   // a value as the control would show it
    void FindOverrides(Ctl& c) const;
    void Write(int i, const std::wstring& v);
    void ResetOverride(int i);
    void WriteShowMonitor(int i, bool on);
    void Undo();
    void SetScope(int s);
    // layout and drawing
    void Layout();
    void LayoutPage(Page& p);
    void Paint();
    void DrawPane();
    void DrawContent();
    void DrawCtl(int i, bool inContent);
    void DrawPopup();
    void DrawText(const std::wstring& s, IDWriteTextFormat* f, const D2D1_RECT_F& rc, const D2D1_COLOR_F& c,
                  DWRITE_TEXT_ALIGNMENT a = DWRITE_TEXT_ALIGNMENT_LEADING, bool center = true);
    float TextWidth(const std::wstring& s, IDWriteTextFormat* f);
    void FillRound(const D2D1_RECT_F& r, float radius, const D2D1_COLOR_F& c);
    void StrokeRound(const D2D1_RECT_F& r, float radius, const D2D1_COLOR_F& c, float px = 1);
    void FocusRing(const D2D1_RECT_F& r, float radius);
    D2D1_RECT_F ViewRect(int i) const;                 // the control on screen (DIPs, client)
    D2D1_RECT_F ContentRect() const;
    float Snap(float v) const;
    bool CreateTarget();
    void CreateFonts();
    void LoadTheme();
    // input
    void OnKey(UINT vk);
    void OnChar(wchar_t ch);
    void OnMouseDown(float x, float y, bool dbl);
    void OnMouseMove(float x, float y);
    void OnMouseUp(float x, float y);
    void OnWheel(int delta);
    int HitTest(float x, float y) const;
    void SetFocusCtl(int i, bool keyboard);
    void MoveFocus(int dir);
    void Activate(int i);                              // Space/Enter/click
    void StepSlider(int i, double steps, bool big);
    void SetSlider(int i, double v, bool commitNow);
    void SliderFromX(int i, float x);
    void CommitSlider();
    void ComboStep(int i, int dir);
    void OpenPopup(int i);
    void ClosePopup(bool accept);
    void SelectOption(int i, int opt);
    void StartEdit(int i, bool selectAll = true);
    void EndEdit(bool commit);
    void SetPage(int p);
    void ScrollTo(float y);
    void EnsureVisible(int i);
    void PickColor(int i);
    void Invalidate();
    std::vector<int> FocusOrder() const;
    void NotifyFocus();
    void NotifyValue(int i);
    // debug menu
    int PageCount() const;                             // the Debug page counts only in debug mode
    void ToggleDebug();                                // Ctrl+Shift+D
    void CopyReport(int i);

    HINSTANCE inst_;
    SettingsHost host_;
    std::wstring iniPath_;
    IniDoc doc_;                   // the file as last read
    std::vector<char> original_;   // the file as it was when the window opened (Undo changes)
    bool changed_ = false;
    std::vector<SettingsMonitor> monitors_;
    std::vector<MonitorInfo> allMons_;
    std::vector<Scope> scopes_;
    int scope_ = 0;
    std::vector<Page> pages_;
    int page_ = 0;
    int focus_ = -1;
    bool kbdCues_ = false;         // focus rings only after the keyboard was used
    int hot_ = -1, pressed_ = -1;
    int drag_ = -1;                // slider being dragged
    bool sliderPending_ = false;
    int sliderCtl_ = -1;
    float scroll_ = 0, scrollMax_ = 0;
    bool thumbDrag_ = false; float thumbGrab_ = 0;
    // first/shown: a list taller than the window scrolls (shown items from first)
    struct Popup { int ctl = -1; int hot = -1; int sel = -1; D2D1_RECT_F rc{}; float itemH = 0; int first = 0, shown = 0; } popup_;
    bool debug_ = false;           // DebugMode=1: the Debug page and the DEBUG MODE footer
    int debugNav_ = -1, debugLabel_ = -1, copyLink_ = -1;
    HWND edit_ = nullptr;
    WNDPROC editBase_ = nullptr;
    int editing_ = -1;
    HFONT editFont_ = nullptr;
    HBRUSH editBrush_ = nullptr;
    std::wstring editLive_;        // DateFormat being typed: the sample under it follows
    float dpi_ = 96, scale_ = 1;
    float w_ = 0, h_ = 0;          // client, DIPs
    Theme t_{};
    ComPtr<ID2D1Factory1> d2d_;
    ComPtr<ID2D1HwndRenderTarget> rt_;
    ComPtr<ID2D1SolidColorBrush> brush_;
    ComPtr<IDWriteFactory> dw_;
    ComPtr<IDWriteTextFormat> body_, caption_, strong_, title_, icon_, iconSmall_, bodyRight_, badge_;
    std::wstring family_, iconFamily_;
};

void UiaRaiseFocus(Window& w, int i);
void UiaRaiseToggle(Window& w, int i, bool on);
void UiaRaiseValue(Window& w, int i);
void UiaRaiseName(Window& w, int i, const std::wstring& was);
LRESULT UiaGetObject(Window& w, WPARAM wp, LPARAM lp);
void UiaDetach(Window& w);

}   // namespace sw
