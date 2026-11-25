#pragma once
#include <windows.h>
#include <string>
#include <vector>
#include <wrl/client.h>
#include <d2d1_1.h>
#include <dwrite_3.h>
#include <wincodec.h>
#include <dcomp.h>
#include "settings.h"

using Microsoft::WRL::ComPtr;

// AdaptiveLegibility: the most a busy wallpaper gets of shadow (opacity %, blur DIPs) and of
// weather/now-playing opacity (% of Opacity).
constexpr double kLegibleShadowOpacity = 90, kLegibleShadowBlur = 12, kLegibleSecondary = 100;

// Result of laying out one frame, in physical pixels of the target monitor.
struct ClockGeometry {
    int width = 0, height = 0;   // bitmap/window size
    int anchorX = 0;             // horizontal centre of the text, inside the bitmap
    int anchorY = 0;             // vertical centre of the date+time block (date cap top .. time baseline)
    RECT ink{};                  // approximate text area inside the bitmap (for wallpaper sampling)
    RECT lines{};                // every line's box (date to now playing), inside the bitmap
};

// Everything needed to draw one frame; kept alive so the previous minute can be re-drawn
// piecewise during a transition.
struct FrameLayout {
    ComPtr<IDWriteTextLayout> time, date, weather, media;   // weather/media may be null (no line)
    D2D1_POINT_2F timeOrigin{}, dateOrigin{}, weatherOrigin{}, mediaOrigin{};   // DIPs inside the bitmap
    ClockGeometry g;
    float dpi = 96;
    std::wstring timeText;
    // Drawn icons in the weather line (sunrise/sunset): text position of their inline box.
    struct Icon { UINT32 pos; wchar_t kind; };   // kind: kSunriseIcon, kSunsetIcon or an icon:: placeholder
    std::vector<Icon> weatherIcons, mediaIcons;
    float mediaBaseline = 0;     // DIPs from the media layout's top to its baseline
    float mediaAlpha = 1;        // extra opacity of the now-playing line (dimmed while paused)
    float secondaryAlpha = 1;    // extra opacity of the weather and now-playing lines (SecondaryOpacity)
    float weatherBaseline = 0;   // DIPs from the weather layout's top to its baseline
    float weatherCap = 0;        // cap height of the weather line, DIPs
    float weatherWidth = 0;      // width of the weather line, DIPs (0 = none)
    float timeRefWidth = 0;      // width of the time with every digit as "0", DIPs: the line's width budget
    // Style the frame was laid out with (each monitor can have its own settings).
    double opacity = 80, shadowOpacity = 0, shadowBlur = 0, shadowOffsetY = 0;
    bool shadow = false;
    double legibility = 0;       // AdaptiveLegibility's boost for this frame, 0..1
    bool guide = false;          // self-test: draw the centre line
    double opticalCentring = 0;  // the setting the time was placed with
    int colonIndex = -1;         // CentreOn=colon: the time character on the centre line (-1 = the time's ink)
    std::wstring dateText, weatherText, mediaText;   // what the lines say (CentreLines' cache key)
    std::wstring styleKey;       // fonts, sizes, widths and DPI the lines were laid out with (same)
};

// Placeholders in the weather text for icons the renderer draws (private use area).
constexpr wchar_t kSunriseIcon = 0xE000, kSunsetIcon = 0xE001;

// One row of a self-test gallery: a label line, then (optionally) a rendered frame bitmap.
struct GalleryRow {
    std::wstring label;
    ComPtr<ID2D1Bitmap1> bmp;   // null = label-only row
    float srcTop = 0;           // px of the bitmap to skip from its top (crops to the weather line)
};

class ClockRenderer {
public:
    bool Init();   // factories + embedded fonts

    // Step 1: build text layouts and compute the bitmap geometry.
    // weather: the line under the time ("" = none), in the date's font. kSunriseIcon /
    // kSunsetIcon and the icon:: placeholders (icons.h) are drawn by the renderer itself; any
    // other symbol (U+2600 and up) falls back to Segoe UI Symbol, monochrome.
    // media: the now-playing line under the weather ("" = none), cut with an ellipsis at
    // WeatherMaxWidth % of the time's width.
    bool Prepare(const Settings& s, UINT dpi, const std::wstring& date, const std::wstring& time,
                 const std::wstring& weather, FrameLayout& f, const std::wstring& media = std::wstring());
    // Step 2: draw text + shadow into a finished, premultiplied frame bitmap.
    // timeMask: bit i set = time character i is drawn; the others keep their place but are invisible.
    ComPtr<ID2D1Bitmap1> RenderFrame(const FrameLayout& f, COLORREF color, bool darkText,
                                     bool drawDate = true, UINT32 timeMask = ~0u, bool drawWeather = true,
                                     bool drawMedia = true);
    // Pixels from the frame's left edge to the leading edge of time character `index`.
    float TimeCharX(const FrameLayout& f, UINT32 index);

    // Step 3: show it through DirectComposition (per-pixel alpha).
    // The 24H2+ desktop (Progman) has no GDI redirection surface, so layered/GDI content
    // in its children is never composited; DirectComposition content is.
    // pos: px of the frame's top-left inside the window.
    bool ShowFrame(HWND hwnd, ID2D1Bitmap1* bmp, POINT pos);

    // Transition run by the compositor (DWM): each layer becomes a visual whose offset and
    // opacity are DirectComposition animations, so it is sampled at the display's refresh rate
    // in sync with vblank (60 Hz, 240 Hz, ...) with no per-frame work in this process.
    // Positions are px inside the window. Motion and fades follow a smoothstep ease.
    struct AnimLayer {
        enum Kind { Solid, FadeIn, FadeOut } kind;
        ID2D1Bitmap1* bmp;
        float x0, y0, x1, y1;   // position at the start / end
    };
    // textAlpha: the text's own opacity; the fade-out curve is shaped with it so that a pixel
    // covered by both fading layers blends linearly (no dip mid-fade, as plain stacking would give).
    bool Animate(HWND hwnd, const AnimLayer* layers, int count, double seconds, double textAlpha);
    void ReleaseTarget(HWND hwnd);   // before a target window goes away (nullptr = all)

    // Weather effects (weatherfx.cpp) draw into a container visual that sits below the text in
    // the same window, with the same devices. keep: objects the effect must hold while it runs
    // (cleared with the layer). Changes show at the next Commit().
    IDCompositionVisual* FxLayer(HWND hwnd, std::vector<ComPtr<IUnknown>>** keep);
    IDCompositionDevice* Dcomp() { return dcomp_.Get(); }
    ID2D1DeviceContext* OffDc() { return offDc_.Get(); }
    bool Commit();

    // Self-test: stacks the rows (label, then the frame cropped from srcTop to its bottom) on an
    // opaque dark background and writes a PNG. dpi: the DPI the row bitmaps were rendered at.
    bool SaveGallery(const std::wstring& path, const std::vector<GalleryRow>& rows, float dpi);
    // Preview: a monitor of size `mon` (px) showing its wallpaper as the desktop does (position:
    // DESKTOP_WALLPAPER_POSITION; bg: the colour around it), with frames drawn on top at px
    // positions; written as a PNG scaled by `scale`.
    struct ScenePart { ID2D1Bitmap1* bmp; D2D1_POINT_2F pos; float opacity = 1; };   // pos: monitor px
    struct SceneWall { std::wstring path; float opacity = 1; };                       // drawn in order
    bool SaveScene(const std::wstring& path, SIZE mon, const std::vector<SceneWall>& walls, int position, COLORREF bg,
                   const std::vector<ScenePart>& parts, float scale);
    // The average colour and lightness of an image filling a monitor of size mon, over rc (px).
    bool SampleImage(const std::wstring& path, SIZE mon, const RECT& rc, struct WallpaperSample& out);

    // Where a text layout's ink lies, in DIPs from the layout's left edge (drawn at 4 px per DIP
    // on the CPU): the ink box's edges, and the centre of mass of its coverage (where the eye
    // puts the "middle" of the text).
    struct Ink { float left = 0, right = 0, mass = 0; };
    bool MeasureInk(IDWriteTextLayout* layout, Ink& ink);

    // How far one line's ink box, as drawn in the frame (icons included, at the frame's DPI),
    // sits from the centre line, in DIPs (+ = right). Edges are estimated to a fraction of a
    // pixel from the coverage of the outermost columns.
    enum Line { kDateLine, kTimeLine, kWeatherLine, kMediaLine };
    // timeMask (kTimeLine): which time characters count, e.g. only the colon.
    bool LineInkOffset(const FrameLayout& f, Line line, float& offsetDip, UINT32 timeMask = ~0u);
    // Step 1b, after Prepare: moves the date, weather and now-playing lines so their ink, not
    // their advance box, is centred (a leading icon or a trailing "°" otherwise pulls a line
    // aside). The time is trued up too: its ink, or with CentreOn=colon its colon.
    void CentreLines(FrameLayout& f);

    // Notification-area icon (Segoe Fluent Icons clock glyph).
    HICON MakeTrayIcon(int size, bool lightTaskbar);

private:
    ComPtr<IDWriteTextLayout> MakeLayout(const std::wstring& text, const std::wstring& family, DWRITE_FONT_WEIGHT w,
                                         float size, float trackingPct, const std::wstring& features);

    ComPtr<IDWriteTextLayout> MakeWeatherLayout(const std::wstring& text, const Settings& s, std::vector<FrameLayout::Icon>& icons);
    void DrawSunIcon(ID2D1SolidColorBrush* brush, float x, float baseline, float capH, bool sunrise, float dpi);
    void DrawWeatherIcon(ID2D1SolidColorBrush* brush, float x, float baseline, float capH, wchar_t kind);
    std::vector<std::pair<wchar_t, ComPtr<ID2D1Geometry>>> iconGeo_;   // built once per icon, in icon units
    ComPtr<ID2D1StrokeStyle> roundCaps_;
    bool CreateDevices();
    struct Image { std::wstring path; UINT w = 0, h = 0; std::vector<BYTE> px; };   // decoded, PBGRA
    std::vector<Image> images_;   // --preview / --demo backgrounds
    const Image* DecodeImage(const std::wstring& path);
    bool SavePng(ID2D1Bitmap1* target, UINT W, UINT H, float dpi, const std::wstring& path);   // opaque image
    // One composition target per clock window (one window per monitor).
    struct Target {
        HWND hwnd = nullptr;
        ComPtr<IDCompositionTarget> target;
        ComPtr<IDCompositionVisual> root;       // holds fx (bottom) and visual (top)
        ComPtr<IDCompositionVisual> fx;         // weather effects
        std::vector<ComPtr<IUnknown>> fxKeep;
        ComPtr<IDCompositionVisual> visual;     // the text: static frame or transition layers
        ComPtr<IDCompositionSurface> surface;   // the static frame
        int surfaceW = 0, surfaceH = 0;
        std::vector<ComPtr<IUnknown>> anim;     // visuals/surfaces/effects of a running transition
    };
    Target* EnsureTarget(HWND hwnd);
    bool CopyToSurface(IDCompositionSurface* surface, ID2D1Bitmap1* bmp);
    ComPtr<IDCompositionAnimation> Ease(double seconds, float from, float to);
    ComPtr<IDCompositionAnimation> FadeOutCurve(double seconds, double textAlpha);

    ComPtr<ID2D1Factory1> d2d_;
    ComPtr<ID2D1Device> d2dDevice_;
    ComPtr<ID2D1DeviceContext> offDc_;          // draws the text layer
    ComPtr<IDCompositionDevice> dcomp_;
    std::vector<Target> targets_;
    ComPtr<IDWriteFactory5> dw_;
    ComPtr<IWICImagingFactory> wic_;
    ComPtr<IDWriteInMemoryFontFileLoader> loader_;
    ComPtr<IDWriteFontCollection1> fonts_;
    std::wstring timeFamily_, dateFamily_;
    std::vector<std::pair<std::wstring, Ink>> inkCache_;   // the time's ink, by text + style (a few entries)
    // CentreLines: each line's ink centre relative to its (whole-pixel) origin, DIPs, by line + text +
    // style, so a frame that is redrawn unchanged doesn't render and read back its lines again.
    std::vector<std::pair<std::wstring, float>> lineCache_;
    DWRITE_FONT_METRICS timeMetrics_{}, dateMetrics_{};
};
