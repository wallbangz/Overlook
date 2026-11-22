#include "render.h"
#include "icons.h"
#include "wallpaper.h"
#include "../res/resource.h"
#include <d2d1_1helper.h>
#include <d2d1effects.h>
#include <d3d11.h>
#include <cmath>
#include <algorithm>
#include <vector>

// The embedded font data lives in the image's resource section for the life of the process, so
// the in-memory loader can reference it without copying. DirectWrite requires a non-null owner
// object for that; this one has no state.
namespace {
struct StaticOwner : IUnknown {
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (riid == __uuidof(IUnknown)) { *ppv = this; return S_OK; }
        *ppv = nullptr; return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return 1; }
    ULONG STDMETHODCALLTYPE Release() override { return 1; }
} g_fontOwner;

bool ResourceBytes(int id, const void*& data, UINT32& size) {
    HRSRC r = FindResourceW(nullptr, MAKEINTRESOURCEW(id), RT_RCDATA);
    if (!r) return false;
    HGLOBAL g = LoadResource(nullptr, r);
    if (!g) return false;
    data = LockResource(g);
    size = SizeofResource(nullptr, r);
    return data && size;
}

D2D1_COLOR_F ToColor(COLORREF c, float a) {
    return D2D1::ColorF(GetRValue(c) / 255.f, GetGValue(c) / 255.f, GetBValue(c) / 255.f, a);
}
}  // namespace

bool ClockRenderer::Init() {
    D2D1_FACTORY_OPTIONS opt{};
    if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory1), &opt,
                                 reinterpret_cast<void**>(d2d_.GetAddressOf())))) return false;
    if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory5),
                                   reinterpret_cast<IUnknown**>(dw_.GetAddressOf())))) return false;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic_)))) return false;

    // Private font collection from the RCDATA resources (never installed, invisible to other apps).
    if (FAILED(dw_->CreateInMemoryFontFileLoader(&loader_))) return false;
    if (FAILED(dw_->RegisterFontFileLoader(loader_.Get()))) return false;
    ComPtr<IDWriteFontSetBuilder1> builder;
    if (FAILED(dw_->CreateFontSetBuilder(&builder))) return false;
    for (int id : { IDR_FONT_TIME, IDR_FONT_DATE }) {
        const void* data; UINT32 size;
        if (!ResourceBytes(id, data, size)) { Log(L"font resource %d missing", id); return false; }
        ComPtr<IDWriteFontFile> file;
        if (FAILED(loader_->CreateInMemoryFontFileReference(dw_.Get(), data, size, &g_fontOwner, &file))) return false;
        if (FAILED(builder->AddFontFile(file.Get()))) return false;
    }
    ComPtr<IDWriteFontSet> set;
    if (FAILED(builder->CreateFontSet(&set))) return false;
    if (FAILED(dw_->CreateFontCollectionFromFontSet(set.Get(), &fonts_))) return false;

    // Find the family name DirectWrite assigned to each weight (Bold = time, Medium = date).
    for (UINT32 f = 0; f < fonts_->GetFontFamilyCount(); ++f) {
        ComPtr<IDWriteFontFamily> fam; fonts_->GetFontFamily(f, &fam);
        ComPtr<IDWriteLocalizedStrings> names; fam->GetFamilyNames(&names);
        wchar_t name[128] = L""; names->GetString(0, name, ARRAYSIZE(name));
        for (UINT32 i = 0; i < fam->GetFontCount(); ++i) {
            ComPtr<IDWriteFont> font; fam->GetFont(i, &font);
            Log(L"font: family='%s' weight=%d", name, (int)font->GetWeight());
            if (font->GetWeight() == DWRITE_FONT_WEIGHT_BOLD)   { timeFamily_ = name; font->GetMetrics(&timeMetrics_); }
            if (font->GetWeight() == DWRITE_FONT_WEIGHT_MEDIUM) { dateFamily_ = name; font->GetMetrics(&dateMetrics_); }
        }
    }
    return !timeFamily_.empty() && !dateFamily_.empty();
}

ComPtr<IDWriteTextLayout> ClockRenderer::MakeLayout(const std::wstring& text, const std::wstring& family, DWRITE_FONT_WEIGHT w,
                                                    float size, float trackingPct, const std::wstring& features) {
    ComPtr<IDWriteTextFormat> fmt;
    if (FAILED(dw_->CreateTextFormat(family.c_str(), fonts_.Get(), w, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                     size, L"en-us", &fmt))) return nullptr;
    fmt->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    ComPtr<IDWriteTextLayout> layout;
    if (FAILED(dw_->CreateTextLayout(text.c_str(), (UINT32)text.size(), fmt.Get(), 100000.f, 10000.f, &layout))) return nullptr;

    // Tracking on every character except the last, so the line stays optically centred.
    ComPtr<IDWriteTextLayout1> l1;
    if (trackingPct != 0 && text.size() > 1 && SUCCEEDED(layout.As(&l1)))
        l1->SetCharacterSpacing(0, size * trackingPct / 100.f, 0, { 0, (UINT32)text.size() - 1 });

    if (!features.empty()) {
        ComPtr<IDWriteTypography> typo;
        if (SUCCEEDED(dw_->CreateTypography(&typo))) {
            size_t i = 0;
            while (i < features.size()) {
                size_t j = features.find(L',', i);
                std::wstring tag = features.substr(i, j == std::wstring::npos ? std::wstring::npos : j - i);
                tag.erase(std::remove_if(tag.begin(), tag.end(), iswspace), tag.end());
                if (tag.size() == 4)
                    typo->AddFontFeature({ (DWRITE_FONT_FEATURE_TAG)DWRITE_MAKE_OPENTYPE_TAG(tag[0], tag[1], tag[2], tag[3]), 1 });
                if (j == std::wstring::npos) break;
                i = j + 1;
            }
            layout->SetTypography(typo.Get(), { 0, (UINT32)text.size() });
        }
    }
    return layout;
}

namespace {
// Room on each side of every inline icon, as a fraction of the cap height (on top of the space
// characters around it), so icons and text keep the same gaps whatever the icon.
constexpr float kIconPad = 0.06f;   // just keeps ink off its neighbours: the en/em spaces do the spacing
// Room the frame keeps on each side so CentreLines can move a line by its ink (DIPs).
constexpr float kLineSlackDip = 6.f;

// A Segoe UI Symbol glyph placed inline in an Inter line, with its *ink* centred on Inter's cap
// height. Segoe UI Symbol sits its pictographs on a different vertical grid, so with plain font
// fallback a cloud or umbrella floats above the digits. The symbol is a nested text layout drawn
// through the same renderer (so it gets the line's brush), shifted vertically; the inline box
// takes Inter's ascent/descent so the line's metrics don't change.
class CenteredSymbol final : public IDWriteInlineObject {
public:
    CenteredSymbol(ComPtr<IDWriteTextLayout> glyph, float ascent, float descent, float capHeight)
        : glyph_(std::move(glyph)), ascent_(ascent), descent_(descent) {
        DWRITE_TEXT_METRICS m{};
        glyph_->GetMetrics(&m);
        width_ = m.widthIncludingTrailingWhitespace;
        // Overhangs are measured against the layout's max box, so shrink it to the text first.
        glyph_->SetMaxWidth(m.widthIncludingTrailingWhitespace);
        glyph_->SetMaxHeight(m.height);
        DWRITE_OVERHANG_METRICS o{};
        glyph_->GetOverhangMetrics(&o);
        // Ink extent relative to the nested layout's top: [-o.top, m.height + o.bottom].
        const float inkCentre = (-o.top + (m.height + o.bottom)) / 2.f;
        dy_ = (ascent_ - capHeight / 2.f) - inkCentre;   // so the ink centre lands at cap-height/2 above the baseline
        // Horizontally the box is the ink alone: Segoe UI Symbol's side bearings differ a lot from
        // glyph to glyph (a cloud nearly touched the digits while the sun stood apart), so the
        // space that follows is the same after every symbol.
        const float inkLeft = -o.left, inkRight = m.widthIncludingTrailingWhitespace + o.right;
        if (inkRight > inkLeft) {
            width_ = inkRight - inkLeft + 2.f * capHeight * kIconPad;
            dx_ = capHeight * kIconPad - inkLeft;
        }
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IDWriteInlineObject)) { *ppv = this; AddRef(); return S_OK; }
        *ppv = nullptr; return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
    ULONG STDMETHODCALLTYPE Release() override { ULONG r = --refs_; if (!r) delete this; return r; }

    HRESULT STDMETHODCALLTYPE Draw(void* ctx, IDWriteTextRenderer* renderer, FLOAT x, FLOAT y, BOOL, BOOL, IUnknown*) override {
        return glyph_->Draw(ctx, renderer, x + dx_, y + dy_);
    }
    HRESULT STDMETHODCALLTYPE GetMetrics(DWRITE_INLINE_OBJECT_METRICS* m) override {
        *m = { width_, ascent_ + descent_, ascent_, FALSE };
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetOverhangMetrics(DWRITE_OVERHANG_METRICS* o) override { *o = {}; return S_OK; }
    HRESULT STDMETHODCALLTYPE GetBreakConditions(DWRITE_BREAK_CONDITION* before, DWRITE_BREAK_CONDITION* after) override {
        *before = *after = DWRITE_BREAK_CONDITION_NEUTRAL;
        return S_OK;
    }
private:
    ComPtr<IDWriteTextLayout> glyph_;
    float ascent_, descent_, width_ = 0, dx_ = 0, dy_ = 0;
    ULONG refs_ = 1;
};
// An empty inline box that reserves room for an icon the renderer draws itself afterwards.
class Spacer final : public IDWriteInlineObject {
public:
    Spacer(float width, float ascent, float descent) : width_(width), ascent_(ascent), descent_(descent) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IDWriteInlineObject)) { *ppv = this; AddRef(); return S_OK; }
        *ppv = nullptr; return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
    ULONG STDMETHODCALLTYPE Release() override { ULONG r = --refs_; if (!r) delete this; return r; }
    HRESULT STDMETHODCALLTYPE Draw(void*, IDWriteTextRenderer*, FLOAT, FLOAT, BOOL, BOOL, IUnknown*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE GetMetrics(DWRITE_INLINE_OBJECT_METRICS* m) override {
        *m = { width_, ascent_ + descent_, ascent_, FALSE };
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetOverhangMetrics(DWRITE_OVERHANG_METRICS* o) override { *o = {}; return S_OK; }
    HRESULT STDMETHODCALLTYPE GetBreakConditions(DWRITE_BREAK_CONDITION* before, DWRITE_BREAK_CONDITION* after) override {
        *before = *after = DWRITE_BREAK_CONDITION_NEUTRAL;
        return S_OK;
    }
private:
    float width_, ascent_, descent_;
    ULONG refs_ = 1;
};

// Sunrise/sunset icon proportions, relative to s = 1.3 x the line's cap height. The horizon,
// its widest part, spans 1.2 s (plus round caps).
constexpr float kSunIconScale = 1.3f, kSunIconWidth = 1.2f;
}  // namespace

// The weather line mixes Inter (text, digits, arrows) with monochrome symbols from Segoe UI
// Symbol (U+2600 and up: sun, clouds, rain, umbrella...), which Inter doesn't have.
ComPtr<IDWriteTextLayout> ClockRenderer::MakeWeatherLayout(const std::wstring& text, const Settings& s,
                                                            std::vector<FrameLayout::Icon>& icons) {
    const float size = (float)s.weatherSize;
    auto layout = MakeLayout(text, dateFamily_, DWRITE_FONT_WEIGHT_MEDIUM, size, (float)s.dateTracking, s.dateFeatures);
    if (!layout) return nullptr;
    const float em = (float)dateMetrics_.designUnitsPerEm;
    const float ascent = size * dateMetrics_.ascent / em, descent = size * dateMetrics_.descent / em;
    const float capHeight = size * dateMetrics_.capHeight / em;
    ComPtr<IDWriteFontCollection> sys;
    dw_->GetSystemFontCollection(&sys);
    for (UINT32 i = 0; i < text.size();) {
        const UINT32 len = IS_HIGH_SURROGATE(text[i]) && i + 1 < text.size() ? 2 : 1;
        const UINT32 cp = len == 2 ? 0x10000 + ((text[i] - 0xD800) << 10) + (text[i + 1] - 0xDC00) : text[i];
        if (cp == kSunriseIcon || cp == kSunsetIcon) {
            ComPtr<IDWriteInlineObject> obj;
            obj.Attach(new Spacer(capHeight * (kSunIconScale * kSunIconWidth + 2.f * kIconPad), ascent, descent));
            layout->SetInlineObject(obj.Get(), { i, len });
            icons.push_back({ i, (wchar_t)cp });
        } else if (cp <= 0xFFFF && icon::IsDrawn((wchar_t)cp)) {
            ComPtr<IDWriteInlineObject> obj;
            obj.Attach(new Spacer(capHeight * (kSunIconScale * icon::Width((wchar_t)cp) + 2.f * kIconPad), ascent, descent));
            layout->SetInlineObject(obj.Get(), { i, len });
            icons.push_back({ i, (wchar_t)cp });
        } else if (cp >= 0x2600) {
            ComPtr<IDWriteTextFormat> fmt;
            ComPtr<IDWriteTextLayout> glyph;
            // Symbols run a little small next to Inter's digits.
            if (SUCCEEDED(dw_->CreateTextFormat(L"Segoe UI Symbol", sys.Get(), DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
                                                DWRITE_FONT_STRETCH_NORMAL, size * 1.12f, L"en-us", &fmt)) &&
                SUCCEEDED(dw_->CreateTextLayout(text.c_str() + i, len, fmt.Get(), 1000.f, 1000.f, &glyph))) {
                ComPtr<IDWriteInlineObject> obj;
                obj.Attach(new CenteredSymbol(glyph, ascent, descent, capHeight));
                layout->SetInlineObject(obj.Get(), { i, len });
            }
        }
        i += len;
    }
    return layout;
}

bool ClockRenderer::Prepare(const Settings& s, UINT dpi, const std::wstring& date, const std::wstring& time,
                            const std::wstring& weather, FrameLayout& f, const std::wstring& media) {
    f = {};
    f.dpi = (float)dpi;
    f.opacity = s.opacity;
    f.shadow = s.shadow;
    f.shadowOpacity = s.shadowOpacity;
    f.shadowBlur = s.shadowBlur;
    f.shadowOffsetY = s.shadowOffsetY;
    f.opticalCentring = s.opticalCentring;
    f.secondaryAlpha = (float)(s.secondaryOpacity / 100.0);
    f.timeText = time;
    f.dateText = date;
    f.weatherText = weather;
    f.mediaText = media;
    ClockGeometry& g = f.g;
    const float k = f.dpi / 96.f;   // DIP -> px

    f.time = MakeLayout(time, timeFamily_, DWRITE_FONT_WEIGHT_BOLD, (float)s.timeSize, (float)s.timeTracking, s.timeFeatures);
    f.date = MakeLayout(date, dateFamily_, DWRITE_FONT_WEIGHT_MEDIUM, (float)s.dateSize, (float)s.dateTracking, s.dateFeatures);
    if (!f.time || !f.date) return false;
    {
        std::wstring zeros = time;   // same characters, every digit as Inter's widest
        for (auto& ch : zeros) if (ch >= L'0' && ch <= L'9') ch = L'0';
        DWRITE_TEXT_METRICS zm{};
        auto ref = MakeLayout(zeros, timeFamily_, DWRITE_FONT_WEIGHT_BOLD, (float)s.timeSize, (float)s.timeTracking, s.timeFeatures);
        if (ref && SUCCEEDED(ref->GetMetrics(&zm))) f.timeRefWidth = zm.width;
    }
    if (!weather.empty()) f.weather = MakeWeatherLayout(weather, s, f.weatherIcons);
    if (!media.empty() && (f.media = MakeWeatherLayout(media, s, f.mediaIcons))) {
        // Long titles end in "..." at the same width budget as the weather line.
        const float budget = (f.timeRefWidth > 0 ? f.timeRefWidth : 300.f) * (float)(s.weatherMaxWidth / 100.0);
        f.media->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        f.media->SetMaxWidth(budget);
        ComPtr<IDWriteInlineObject> ellipsis;
        if (SUCCEEDED(dw_->CreateEllipsisTrimmingSign(f.media.Get(), &ellipsis))) {
            const DWRITE_TRIMMING trim{ DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0 };
            f.media->SetTrimming(&trim, ellipsis.Get());
        }
    }

    DWRITE_TEXT_METRICS tm{}, dm{}, wm{}, mm{};
    f.time->GetMetrics(&tm);
    f.date->GetMetrics(&dm);
    DWRITE_LINE_METRICS tl{}, dl{}, wl{}, ml{}; UINT32 n;
    f.time->GetLineMetrics(&tl, 1, &n);
    f.date->GetLineMetrics(&dl, 1, &n);
    if (f.weather) {
        f.weather->GetMetrics(&wm);
        f.weather->GetLineMetrics(&wl, 1, &n);
        f.weatherBaseline = wl.baseline;
        f.weatherWidth = wm.width;
        f.weatherCap = (float)s.weatherSize * dateMetrics_.capHeight / dateMetrics_.designUnitsPerEm;
    }
    if (f.media) {
        f.media->GetMetrics(&mm);
        f.media->GetLineMetrics(&ml, 1, &n);
        f.mediaBaseline = ml.baseline;
        f.weatherCap = (float)s.weatherSize * dateMetrics_.capHeight / dateMetrics_.designUnitsPerEm;
    }

    // Block geometry in DIPs, y = 0 at the top of the date's capitals. The weather line hangs
    // below the time and doesn't count toward the block's centre, so turning it on or off
    // doesn't move the date and time.
    const float capRatio = (float)dateMetrics_.capHeight / dateMetrics_.designUnitsPerEm;
    const float dateCap = (float)s.dateSize * capRatio;
    const float timeCap = (float)s.timeSize * timeMetrics_.capHeight / timeMetrics_.designUnitsPerEm;
    const float dateBase = dateCap;
    const float timeBase = dateCap + (float)s.gap + timeCap;
    const float weatherBase = timeBase + (float)s.weatherGap + (float)s.weatherSize * capRatio;
    const float dateTop = dateBase - dl.baseline, dateBottom = dateTop + dm.height;
    const float timeTop = timeBase - tl.baseline, timeBottom = timeTop + tm.height;
    const float weatherBottom = f.weather ? weatherBase - wl.baseline + wm.height : timeBottom;
    // The now-playing line hangs under the weather line (or takes its place when there is none).
    const float mediaBase = f.weather ? weatherBase + (float)s.weatherSize * 1.55f : weatherBase;
    const float mediaBottom = f.media ? mediaBase - ml.baseline + mm.height : timeBottom;
    const float contentTop = std::min(dateTop, timeTop);
    const float contentBottom = std::max({ dateBottom, timeBottom, weatherBottom, mediaBottom });
    // The time is centred by its ink, not its advance box: side bearings differ per digit (a
    // trailing "1" left the time up to 2.7 DIP off). OpticalCentring moves the point that is
    // centred from the ink box toward the ink's centre of mass, since a thin "1" or "7" leaves
    // a time looking heavy on one side. CentreOn=colon puts the colon on the centre line instead,
    // so it never moves; a lopsided time (e.g. "13:11", ~17 DIP) then sits off-centre. (Tried and
    // dropped: equal-width digit slots, which loosened the time.)
    float timeShift = 0;   // DIPs the time sits right of advance-box centring
    float boxShift = 0;    // the same for its ink box alone
    {
        const std::wstring key = time + L"|" + std::to_wstring(s.timeSize) + L"|" + std::to_wstring(s.timeTracking) + L"|" + s.timeFeatures;
        auto it = std::find_if(inkCache_.begin(), inkCache_.end(), [&](const auto& e) { return e.first == key; });
        Ink ink;
        bool ok = it != inkCache_.end();
        if (ok) ink = it->second;
        else if ((ok = MeasureInk(f.time.Get(), ink))) {
            if (inkCache_.size() >= 8) inkCache_.erase(inkCache_.begin());
            inkCache_.emplace_back(key, ink);
        }
        if (ok) {
            const float box = (ink.left + ink.right) / 2.f;
            const float centre = box + (ink.mass - box) * (float)(s.opticalCentring / 100.0);
            timeShift = tm.width / 2.f - centre;
            boxShift = tm.width / 2.f - box;
        }
    }
    const size_t colon = time.find(L':');
    if (s.centreOnColon && colon != std::wstring::npos && colon < 32) {
        DWRITE_HIT_TEST_METRICS hm{};
        FLOAT hx = 0, hy = 0;
        if (SUCCEEDED(f.time->HitTestTextPosition((UINT32)colon, FALSE, &hx, &hy, &hm))) {
            // The colon's cell includes the tracking after it (MakeLayout); the glyph sits in the
            // rest. CentreLines trues it up from the drawn colon.
            const float trail = colon + 1 < time.size() ? (float)(s.timeSize * s.timeTracking / 100.0) : 0.f;
            timeShift = tm.width / 2.f - (hm.left + (hm.width - trail) / 2.f);
            f.colonIndex = (int)colon;
        }
    }
    const float contentW = std::max({ dm.width, tm.width + 2.f * std::fabs(timeShift),
                                      f.weather ? wm.width : 0.f, f.media ? mm.width : 0.f });

    const float padDip = 4.f + (s.shadow ? (float)(s.shadowBlur * 3 + std::fabs(s.shadowOffsetY)) : 0.f);
    const int pad = (int)std::ceil(padDip * k);

    g.width = (int)std::ceil((contentW + 2.f * kLineSlackDip) * k) + 2 * pad;
    g.width += g.width & 1;
    g.height = (int)std::ceil((contentBottom - contentTop) * k) + 2 * pad;
    g.anchorX = g.width / 2;

    // Baselines on whole pixels for consistent stems.
    const float dateBasePx = pad + std::round((dateBase - contentTop) * k);
    const float timeBasePx = pad + std::round((timeBase - contentTop) * k);
    g.anchorY = (int)std::lround(((dateBasePx - dateCap * k) + timeBasePx) / 2.f);
    // The wallpaper is sampled over the date+time block at a width that doesn't depend on the
    // digits (the time with every digit as "0"), so "6:38" vs "06:38" or a narrow "11:11" can't
    // tip a wallpaper sitting near BrightThreshold from light to dark text.
    // With CentreOn=colon (or OpticalCentring) the time's ink sits off the centre line: the band
    // follows it, so the text colour is chosen from the wallpaper actually behind the digits.
    const float timeOff = timeShift - boxShift, timeW = f.timeRefWidth > 0 ? f.timeRefWidth : tm.width;
    const float inkL = std::min(-dm.width / 2.f, timeOff - timeW / 2.f), inkR = std::max(dm.width / 2.f, timeOff + timeW / 2.f);
    g.ink = { (LONG)(g.anchorX + inkL * k), (LONG)(dateBasePx - dateCap * k),
              (LONG)(g.anchorX + inkR * k), (LONG)timeBasePx };
    {
        wchar_t sk[256];
        swprintf_s(sk, L"%u|%g|%g|%g|%g|%g|%g|%g|", dpi, s.timeSize, s.timeTracking, s.dateSize, s.dateTracking,
                   s.weatherSize, s.weatherMaxWidth, f.timeRefWidth);
        f.styleKey = sk + s.timeFeatures + L"|" + s.dateFeatures;
    }

    // Each line starts on a whole pixel. Glyphs are positioned with sub-pixel precision, so this
    // keeps an unchanged run of characters (e.g. "04:1" in 04:16 -> 04:17) at the same sub-pixel
    // phase from one minute to the next: a transition slides it by whole pixels and it lands
    // identical to the next static frame.
    const float cx = g.anchorX / k;
    auto snap = [k](float dip) { return std::round(dip * k) / k; };
    f.dateOrigin = { snap(cx - dm.width / 2.f), dateBasePx / k - dl.baseline };
    f.timeOrigin = { snap(cx - tm.width / 2.f + timeShift), timeBasePx / k - tl.baseline };
    if (f.weather) {
        const float weatherBasePx = pad + std::round((weatherBase - contentTop) * k);
        f.weatherOrigin = { snap(cx - wm.width / 2.f), weatherBasePx / k - wl.baseline };
    }
    if (f.media) {
        const float mediaBasePx = pad + std::round((mediaBase - contentTop) * k);
        f.mediaOrigin = { snap(cx - mm.width / 2.f), mediaBasePx / k - ml.baseline };
    }
    return true;
}

float ClockRenderer::TimeCharX(const FrameLayout& f, UINT32 index) {
    FLOAT x = 0, y = 0;
    DWRITE_HIT_TEST_METRICS m{};
    f.time->HitTestTextPosition(index, FALSE, &x, &y, &m);
    return (f.timeOrigin.x + x) * f.dpi / 96.f;
}

bool ClockRenderer::CreateDevices() {
    // WARP (software) D3D device: the clock redraws once a minute, so there is no GPU memory
    // to hold and no device-lost handling (driver updates, GPU resets) to worry about.
    ComPtr<ID3D11Device> d3d;
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                                   nullptr, 0, D3D11_SDK_VERSION, &d3d, nullptr, nullptr);
    if (FAILED(hr)) { Log(L"D3D11CreateDevice(WARP) failed 0x%08X", hr); return false; }
    ComPtr<IDXGIDevice> dxgi;
    if (FAILED(d3d.As(&dxgi))) return false;
    if (FAILED(d2d_->CreateDevice(dxgi.Get(), &d2dDevice_))) return false;
    if (FAILED(d2dDevice_->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &offDc_))) return false;
    hr = DCompositionCreateDevice2(d2dDevice_.Get(), IID_PPV_ARGS(&dcomp_));
    if (FAILED(hr)) { Log(L"DCompositionCreateDevice2 failed 0x%08X", hr); return false; }
    return true;
}

void ClockRenderer::ReleaseTarget(HWND hwnd) {
    const size_t n = targets_.size();
    std::erase_if(targets_, [hwnd](const Target& t) { return !hwnd || t.hwnd == hwnd; });
    if (dcomp_ && targets_.size() != n) dcomp_->Commit();
}

ComPtr<ID2D1Bitmap1> ClockRenderer::RenderFrame(const FrameLayout& f, COLORREF color, bool darkText,
                                               bool drawDate, UINT32 timeMask, bool drawWeather, bool drawMedia) {
    const int W = f.g.width, H = f.g.height;
    if (!dcomp_ && !CreateDevices()) return nullptr;

    const D2D1_BITMAP_PROPERTIES1 bp = D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_TARGET, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), f.dpi, f.dpi);
    ComPtr<ID2D1Bitmap1> textBmp, frame;
    if (FAILED(offDc_->CreateBitmap(D2D1::SizeU(W, H), nullptr, 0, bp, &textBmp))) return nullptr;
    if (FAILED(offDc_->CreateBitmap(D2D1::SizeU(W, H), nullptr, 0, bp, &frame))) return nullptr;

    offDc_->SetDpi(f.dpi, f.dpi);
    offDc_->SetTransform(D2D1::Matrix3x2F::Identity());
    offDc_->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);

    // Hidden time characters keep their layout position but draw with a transparent brush.
    const UINT32 len = (UINT32)f.timeText.size();
    const UINT32 all = len >= 32 ? ~0u : (1u << len) - 1;
    const bool masked = (timeMask & all) != all;
    ComPtr<ID2D1SolidColorBrush> brush, clear;
    offDc_->CreateSolidColorBrush(ToColor(color, (float)(f.opacity / 100.0)), &brush);
    if (masked) {
        offDc_->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0, 0), &clear);
        for (UINT32 i = 0; i < len && i < 32; ++i)
            if (!(timeMask & (1u << i))) f.time->SetDrawingEffect(clear.Get(), { i, 1 });
    }

    // 1) Text on its own layer (the shadow effect's input).
    offDc_->SetTarget(textBmp.Get());
    offDc_->BeginDraw();
    offDc_->Clear(D2D1::ColorF(0, 0, 0, 0));
    if (drawDate) offDc_->DrawTextLayout(f.dateOrigin, f.date.Get(), brush.Get(), D2D1_DRAW_TEXT_OPTIONS_NONE);
    if (timeMask) offDc_->DrawTextLayout(f.timeOrigin, f.time.Get(), brush.Get(), D2D1_DRAW_TEXT_OPTIONS_NONE);
    // A line in the date's font with drawn icons (the weather and now-playing lines).
    auto drawLine = [&](IDWriteTextLayout* layout, D2D1_POINT_2F origin, float baseline,
                        const std::vector<FrameLayout::Icon>& icons, float alpha) {
        ComPtr<ID2D1SolidColorBrush> lineBrush = brush;
        if (alpha < 1.f) offDc_->CreateSolidColorBrush(ToColor(color, (float)(f.opacity / 100.0) * alpha), &lineBrush);
        offDc_->DrawTextLayout(origin, layout, lineBrush.Get(), D2D1_DRAW_TEXT_OPTIONS_NONE);
        if (icons.empty()) return;
        // Strokes overlap where they join, so they are drawn opaque into a layer that carries
        // the text's opacity (a translucent brush would darken every joint).
        ComPtr<ID2D1SolidColorBrush> solid;
        offDc_->CreateSolidColorBrush(ToColor(color, 1.f), &solid);
        offDc_->PushLayer(D2D1::LayerParameters1(D2D1::InfiniteRect(), nullptr, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
                                                 D2D1::IdentityMatrix(), (float)(f.opacity / 100.0) * alpha), nullptr);
        for (const auto& icon : icons) {
            FLOAT x = 0, y = 0;
            DWRITE_HIT_TEST_METRICS m{};
            layout->HitTestTextPosition(icon.pos, FALSE, &x, &y, &m);
            if (m.width <= 0) continue;   // trimmed away by the ellipsis
            const float ix = origin.x + m.left, base = origin.y + baseline;
            if (icon.kind == kSunriseIcon || icon.kind == kSunsetIcon)
                DrawSunIcon(solid.Get(), ix, base, f.weatherCap, icon.kind == kSunriseIcon, f.dpi);
            else
                DrawWeatherIcon(solid.Get(), ix, base, f.weatherCap, icon.kind);
        }
        offDc_->PopLayer();
    };
    if (drawWeather && f.weather) drawLine(f.weather.Get(), f.weatherOrigin, f.weatherBaseline, f.weatherIcons, f.secondaryAlpha);
    if (drawMedia && f.media)
        drawLine(f.media.Get(), f.mediaOrigin, f.mediaBaseline, f.mediaIcons, f.mediaAlpha * f.secondaryAlpha);
    if (f.guide) {   // self-test: the centre line
        ComPtr<ID2D1SolidColorBrush> red;
        offDc_->CreateSolidColorBrush(D2D1::ColorF(1.f, 0.25f, 0.25f, 0.9f), &red);
        const float x = f.g.anchorX * 96.f / f.dpi;   // the boundary between the two middle pixel columns
        offDc_->DrawLine({ x, 0 }, { x, H * 96.f / f.dpi }, red.Get(), 2.f * 96.f / f.dpi);
    }
    HRESULT hr = offDc_->EndDraw();
    if (masked) f.time->SetDrawingEffect(nullptr, { 0, len });

    // 2) Shadow + text = the finished frame.
    if (SUCCEEDED(hr)) {
        offDc_->SetTarget(frame.Get());
        offDc_->BeginDraw();
        offDc_->Clear(D2D1::ColorF(0, 0, 0, 0));
        if (f.shadow && f.shadowOpacity > 0) {
            ComPtr<ID2D1Effect> fx;
            if (SUCCEEDED(offDc_->CreateEffect(CLSID_D2D1Shadow, &fx))) {
                // Dark shadow under light text; a faint light halo under dark text.
                const float a = (float)(f.shadowOpacity / 100.0);
                const D2D1_VECTOR_4F c = darkText ? D2D1::Vector4F(1, 1, 1, a * 0.6f) : D2D1::Vector4F(0, 0, 0, a);
                fx->SetInput(0, textBmp.Get());
                fx->SetValue(D2D1_SHADOW_PROP_BLUR_STANDARD_DEVIATION, (float)f.shadowBlur);
                fx->SetValue(D2D1_SHADOW_PROP_COLOR, c);
                offDc_->DrawImage(fx.Get(), D2D1::Point2F(0, (float)f.shadowOffsetY));
            }
        }
        offDc_->DrawImage(textBmp.Get());
        hr = offDc_->EndDraw();
    }
    offDc_->SetTarget(nullptr);
    if (FAILED(hr)) { Log(L"frame draw failed 0x%08X", hr); return nullptr; }
    return frame;
}

bool ClockRenderer::SaveGallery(const std::wstring& path, const std::vector<GalleryRow>& rows, float dpi) {
    if (rows.empty() || (!dcomp_ && !CreateDevices())) return false;
    const float k = dpi / 96.f;   // px -> DIP is 1/k; everything below is laid out in pixels
    constexpr int kLabelH = 20, kPad = 4, kMargin = 12, kMaxSide = 16384;

    ComPtr<IDWriteTextFormat> fmt;
    if (FAILED(dw_->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
                                     DWRITE_FONT_STRETCH_NORMAL, 13.f / k, L"en-us", &fmt))) return false;
    fmt->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);

    // Measure.
    struct Item { ComPtr<IDWriteTextLayout> label; int y = 0, h = 0, crop = 0; };
    std::vector<Item> items(rows.size());
    int W = 480, H = 0;
    for (size_t i = 0; i < rows.size(); ++i) {
        Item& it = items[i];
        if (FAILED(dw_->CreateTextLayout(rows[i].label.c_str(), (UINT32)rows[i].label.size(), fmt.Get(), 100000.f, 100.f, &it.label)))
            return false;
        DWRITE_TEXT_METRICS tm{};
        it.label->GetMetrics(&tm);
        W = std::max(W, (int)std::ceil(tm.widthIncludingTrailingWhitespace * k) + 2 * kMargin);
        if (rows[i].bmp) {
            const D2D1_SIZE_U sz = rows[i].bmp->GetPixelSize();
            it.crop = std::max(0, (int)sz.height - (int)rows[i].srcTop);
            W = std::max(W, (int)sz.width);
        }
        it.y = H;
        it.h = kPad + kLabelH + it.crop + kPad;
        H += it.h;
    }
    if (W > kMaxSide || H > kMaxSide) { Log(L"gallery too large: %dx%d", W, H); return false; }

    const D2D1_BITMAP_PROPERTIES1 bp = D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_TARGET, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), dpi, dpi);
    ComPtr<ID2D1Bitmap1> target;
    if (FAILED(offDc_->CreateBitmap(D2D1::SizeU(W, H), nullptr, 0, bp, &target))) return false;

    offDc_->SetDpi(dpi, dpi);
    offDc_->SetTransform(D2D1::Matrix3x2F::Identity());
    offDc_->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    ComPtr<ID2D1SolidColorBrush> bgA, bgB, text;
    offDc_->CreateSolidColorBrush(D2D1::ColorF(0x2B3A55), &bgA);
    offDc_->CreateSolidColorBrush(D2D1::ColorF(0x34466A), &bgB);
    offDc_->CreateSolidColorBrush(D2D1::ColorF(0xD0D5DE), &text);

    offDc_->SetTarget(target.Get());
    offDc_->BeginDraw();
    offDc_->Clear(D2D1::ColorF(0x2B3A55));
    for (size_t i = 0; i < rows.size(); ++i) {
        const Item& it = items[i];
        offDc_->FillRectangle(D2D1::RectF(0, it.y / k, W / k, (it.y + it.h) / k), (i & 1) ? bgB.Get() : bgA.Get());
        offDc_->DrawTextLayout(D2D1::Point2F(kMargin / k, (it.y + kPad) / k), it.label.Get(), text.Get(), D2D1_DRAW_TEXT_OPTIONS_NONE);
        if (rows[i].bmp && it.crop > 0) {
            const D2D1_SIZE_U sz = rows[i].bmp->GetPixelSize();
            const int top = (int)rows[i].srcTop, y = it.y + kPad + kLabelH;
            const D2D1_RECT_F src = D2D1::RectF(0, top / k, sz.width / k, sz.height / k);
            const D2D1_RECT_F dst = D2D1::RectF(0, y / k, sz.width / k, (y + it.crop) / k);
            offDc_->DrawBitmap(rows[i].bmp.Get(), &dst, 1.f, D2D1_INTERPOLATION_MODE_NEAREST_NEIGHBOR, &src, nullptr);
        }
    }
    HRESULT hr = offDc_->EndDraw();
    offDc_->SetTarget(nullptr);
    if (FAILED(hr)) { Log(L"gallery draw failed 0x%08X", hr); return false; }
    return SavePng(target.Get(), W, H, dpi, path);
}

// Decodes an image once (kept for the next call) as premultiplied BGRA pixels.
const ClockRenderer::Image* ClockRenderer::DecodeImage(const std::wstring& path) {
    for (const auto& im : images_) if (im.path == path) return &im;
    ComPtr<IWICBitmapDecoder> dec;
    ComPtr<IWICBitmapFrameDecode> fr;
    ComPtr<IWICFormatConverter> conv;
    Image im;
    im.path = path;
    if (FAILED(wic_->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &dec)) ||
        FAILED(dec->GetFrame(0, &fr)) || FAILED(wic_->CreateFormatConverter(&conv)) ||
        FAILED(conv->Initialize(fr.Get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0,
                                WICBitmapPaletteTypeCustom)) ||
        FAILED(conv->GetSize(&im.w, &im.h))) {
        Log(L"preview: could not load %s", path.c_str());
        return nullptr;
    }
    im.px.resize((size_t)im.w * im.h * 4);
    if (FAILED(conv->CopyPixels(nullptr, im.w * 4, (UINT)im.px.size(), im.px.data()))) return nullptr;
    images_.push_back(std::move(im));
    return &images_.back();
}

bool ClockRenderer::SampleImage(const std::wstring& path, SIZE mon, const RECT& rc, WallpaperSample& out) {
    const Image* im = DecodeImage(path);
    if (!im || rc.right <= rc.left || rc.bottom <= rc.top) return false;
    // The image fills the monitor (centred, cropped), as the desktop's "fill" mode shows it.
    const double s = std::max((double)mon.cx / im->w, (double)mon.cy / im->h);
    const double ox = (mon.cx - im->w * s) / 2, oy = (mon.cy - im->h * s) / 2;
    auto lin = [](int v) { const double c = v / 255.0; return c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4); };
    double r = 0, g = 0, b = 0;
    const int kCols = 48, kRows = 16;
    for (int y = 0; y < kRows; ++y)
        for (int x = 0; x < kCols; ++x) {
            const double sx = rc.left + (x + 0.5) * (rc.right - rc.left) / kCols, sy = rc.top + (y + 0.5) * (rc.bottom - rc.top) / kRows;
            const UINT ix = (UINT)std::clamp((sx - ox) / s, 0.0, im->w - 1.0), iy = (UINT)std::clamp((sy - oy) / s, 0.0, im->h - 1.0);
            const BYTE* p = &im->px[((size_t)iy * im->w + ix) * 4];
            b += lin(p[0]); g += lin(p[1]); r += lin(p[2]);
        }
    const double n = kCols * kRows;
    out.r = r / n; out.g = g / n; out.b = b / n;
    const double Y = 0.2126 * out.r + 0.7152 * out.g + 0.0722 * out.b;
    out.lightness = Y > 0.008856 ? 116.0 * std::cbrt(Y) - 16.0 : 903.3 * Y;
    return true;
}

bool ClockRenderer::SaveScene(const std::wstring& path, SIZE mon, const std::vector<SceneWall>& walls, int position,
                              COLORREF bg, const std::vector<ScenePart>& parts, float scale) {
    if (!dcomp_ && !CreateDevices()) return false;
    const UINT W = (UINT)std::lround(mon.cx * scale), H = (UINT)std::lround(mon.cy * scale);
    const D2D1_BITMAP_PROPERTIES1 bp = D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_TARGET, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96.f, 96.f);
    ComPtr<ID2D1Bitmap1> target;
    if (FAILED(offDc_->CreateBitmap(D2D1::SizeU(W, H), nullptr, 0, bp, &target))) return false;
    offDc_->SetTarget(target.Get());
    offDc_->SetDpi(96.f, 96.f);
    offDc_->SetTransform(D2D1::Matrix3x2F::Scale(scale, scale));   // monitor px -> output px
    offDc_->BeginDraw();
    offDc_->Clear(D2D1::ColorF(GetRValue(bg) / 255.f, GetGValue(bg) / 255.f, GetBValue(bg) / 255.f));
    for (const SceneWall& w : walls) {
        const Image* im = w.path.empty() ? nullptr : DecodeImage(w.path);
        if (!im || w.opacity <= 0) continue;
        ComPtr<ID2D1Bitmap1> wall;   // 1 DIP per image pixel
        const D2D1_BITMAP_PROPERTIES1 wp = D2D1::BitmapProperties1(
            D2D1_BITMAP_OPTIONS_NONE, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96.f, 96.f);
        if (FAILED(offDc_->CreateBitmap(D2D1::SizeU(im->w, im->h), im->px.data(), im->w * 4, wp, &wall))) continue;
        // DESKTOP_WALLPAPER_POSITION: 0 centre, 2 stretch, 3 fit; fill (4) for the rest (span and
        // tile roughly).
        const float iw = (float)im->w, ih = (float)im->h, mw = (float)mon.cx, mh = (float)mon.cy;
        float dw = iw * std::max(mw / iw, mh / ih), dh = ih * std::max(mw / iw, mh / ih);
        if (position == 0) { dw = iw; dh = ih; }
        else if (position == 2) { dw = mw; dh = mh; }
        else if (position == 3) { dw = iw * std::min(mw / iw, mh / ih); dh = ih * std::min(mw / iw, mh / ih); }
        const D2D1_RECT_F dst = D2D1::RectF((mw - dw) / 2, (mh - dh) / 2, (mw + dw) / 2, (mh + dh) / 2);
        offDc_->DrawBitmap(wall.Get(), &dst, w.opacity, D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC, nullptr, nullptr);
    }
    for (const ScenePart& p : parts) {
        if (!p.bmp || p.opacity <= 0) continue;
        const D2D1_SIZE_U s = p.bmp->GetPixelSize();
        const D2D1_RECT_F dst = D2D1::RectF(p.pos.x, p.pos.y, p.pos.x + (float)s.width, p.pos.y + (float)s.height);
        offDc_->DrawBitmap(p.bmp, &dst, p.opacity, D2D1_INTERPOLATION_MODE_LINEAR, nullptr, nullptr);
    }
    const HRESULT hr = offDc_->EndDraw();
    offDc_->SetTarget(nullptr);
    offDc_->SetTransform(D2D1::Matrix3x2F::Identity());
    if (FAILED(hr)) { Log(L"preview draw failed 0x%08X", hr); return false; }
    return SavePng(target.Get(), W, H, 96.f, path);
}

bool ClockRenderer::SavePng(ID2D1Bitmap1* target, UINT W, UINT H, float dpi, const std::wstring& path) {
    // Read the pixels back through a CPU-readable copy.
    const D2D1_BITMAP_PROPERTIES1 rp = D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_CPU_READ | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), dpi, dpi);
    ComPtr<ID2D1Bitmap1> cpu;
    if (FAILED(offDc_->CreateBitmap(D2D1::SizeU(W, H), nullptr, 0, rp, &cpu))) return false;
    if (FAILED(cpu->CopyFromBitmap(nullptr, target, nullptr))) return false;
    D2D1_MAPPED_RECT mr{};
    if (FAILED(cpu->Map(D2D1_MAP_OPTIONS_READ, &mr))) return false;

    // The images are opaque, so premultiplied and straight BGRA are the same bytes.
    bool ok = false;
    ComPtr<IWICBitmap> wb;
    ComPtr<IWICStream> stream;
    ComPtr<IWICBitmapEncoder> enc;
    ComPtr<IWICBitmapFrameEncode> frame;
    ComPtr<IPropertyBag2> props;
    WICPixelFormatGUID pf = GUID_WICPixelFormat32bppBGRA;
    if (SUCCEEDED(wic_->CreateBitmapFromMemory(W, H, GUID_WICPixelFormat32bppBGRA, mr.pitch, mr.pitch * H, mr.bits, &wb)) &&
        SUCCEEDED(wic_->CreateStream(&stream)) &&
        SUCCEEDED(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE)) &&
        SUCCEEDED(wic_->CreateEncoder(GUID_ContainerFormatPng, nullptr, &enc)) &&
        SUCCEEDED(enc->Initialize(stream.Get(), WICBitmapEncoderNoCache)) &&
        SUCCEEDED(enc->CreateNewFrame(&frame, &props)) &&
        SUCCEEDED(frame->Initialize(props.Get())) &&
        SUCCEEDED(frame->SetSize(W, H)) &&
        SUCCEEDED(frame->SetPixelFormat(&pf)) &&
        SUCCEEDED(frame->WriteSource(wb.Get(), nullptr)) &&
        SUCCEEDED(frame->Commit()) &&
        SUCCEEDED(enc->Commit()))
        ok = true;
    cpu->Unmap();
    if (!ok) Log(L"PNG encode failed: %s", path.c_str());
    return ok;
}

// Half sun on the horizon with rays, and an arrow below it: up for sunrise, down for sunset.
// x: left edge of the icon's box; baseline: the line's baseline (DIPs). Centred on cap height
// like the Segoe UI Symbol glyphs; the horizon and the arrow's stem sit on whole pixels.
void ClockRenderer::DrawSunIcon(ID2D1SolidColorBrush* brush, float x, float baseline, float capH, bool sunrise, float dpi) {
    if (!roundCaps_) {
        const D2D1_STROKE_STYLE_PROPERTIES p = D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND,
                                                                           D2D1_CAP_STYLE_ROUND, D2D1_LINE_JOIN_ROUND);
        d2d_->CreateStrokeStyle(p, nullptr, 0, &roundCaps_);
    }
    const float k = dpi / 96.f;
    const float s = capH * kSunIconScale;
    const float r = s * 0.25f;
    const float wpx = std::max(1.f, std::round(s * 0.10f * k));   // stroke, whole pixels (= the weather icons')
    const float w = wpx / k;
    auto snapLine = [&](float v) { return (std::round(v * k - wpx / 2.f) + wpx / 2.f) / k; };   // stroke centre on the pixel grid
    const float top = -1.9f * r, bot = 0.62f * s;
    const float hy = (baseline - capH / 2.f) - (top + bot) / 2.f;
    const float cx = snapLine(x + capH * kIconPad + s * kSunIconWidth / 2.f);
    const float sunY = hy - w * 0.6f;

    // Half disc
    ComPtr<ID2D1PathGeometry> disc;
    ComPtr<ID2D1GeometrySink> sink;
    if (SUCCEEDED(d2d_->CreatePathGeometry(&disc)) && SUCCEEDED(disc->Open(&sink))) {
        sink->BeginFigure(D2D1::Point2F(cx - r, sunY), D2D1_FIGURE_BEGIN_FILLED);
        sink->AddArc(D2D1::ArcSegment(D2D1::Point2F(cx + r, sunY), D2D1::SizeF(r, r), 0.f,
                                      D2D1_SWEEP_DIRECTION_CLOCKWISE, D2D1_ARC_SIZE_SMALL));
        sink->EndFigure(D2D1_FIGURE_END_CLOSED);
        sink->Close();
        offDc_->FillGeometry(disc.Get(), brush);
    }
    // Rays
    for (float deg : { 200.f, 245.f, 270.f, 295.f, 340.f }) {
        const float a = deg * 3.14159265f / 180.f;
        const float r1 = r * 1.45f, r2 = r * 1.9f;
        offDc_->DrawLine(D2D1::Point2F(cx + r1 * std::cos(a), sunY + r1 * std::sin(a)),
                         D2D1::Point2F(cx + r2 * std::cos(a), sunY + r2 * std::sin(a)), brush, w, roundCaps_.Get());
    }
    // Horizon
    const float hl = snapLine(hy + s * 0.08f);
    offDc_->DrawLine(D2D1::Point2F(cx - s * 0.6f, hl), D2D1::Point2F(cx + s * 0.6f, hl), brush, w, roundCaps_.Get());
    // Arrow
    const float a0 = hl + s * 0.2f, a1 = hy + bot;
    const float tip = sunrise ? a0 : a1, tail = sunrise ? a1 : a0;
    const float dir = sunrise ? 1.f : -1.f, aw = s * 0.21f;
    offDc_->DrawLine(D2D1::Point2F(cx, tail), D2D1::Point2F(cx, tip), brush, w, roundCaps_.Get());
    offDc_->DrawLine(D2D1::Point2F(cx - aw, tip + dir * aw), D2D1::Point2F(cx, tip), brush, w, roundCaps_.Get());
    offDc_->DrawLine(D2D1::Point2F(cx + aw, tip + dir * aw), D2D1::Point2F(cx, tip), brush, w, roundCaps_.Get());
}

// One of the weather icons (icons.cpp), scaled to the line: icon units -> DIPs, centred on the
// cap height like the sunrise/sunset icon. x: left edge of its inline box.
void ClockRenderer::DrawWeatherIcon(ID2D1SolidColorBrush* brush, float x, float baseline, float capH, wchar_t kind) {
    ComPtr<ID2D1Geometry> g;
    for (auto& e : iconGeo_) if (e.first == kind) g = e.second;
    if (!g) {
        g = icon::Build(d2d_.Get(), kind);
        if (!g) return;
        iconGeo_.push_back({ kind, g });
    }
    const float s = capH * kSunIconScale;
    D2D1_MATRIX_3X2_F old;
    offDc_->GetTransform(&old);
    offDc_->SetTransform(D2D1::Matrix3x2F::Scale(s, s) *
                         D2D1::Matrix3x2F::Translation(x + capH * kIconPad, baseline - capH / 2.f - s / 2.f) * old);
    offDc_->FillGeometry(g.Get(), brush);
    offDc_->SetTransform(old);
}

ClockRenderer::Target* ClockRenderer::EnsureTarget(HWND hwnd) {
    if (!dcomp_ && !CreateDevices()) return nullptr;
    for (auto& t : targets_) if (t.hwnd == hwnd) return &t;
    // Targets of windows that no longer exist (Explorer took them down) are dropped here.
    std::erase_if(targets_, [](const Target& t) { return !IsWindow(t.hwnd); });
    Target t;
    t.hwnd = hwnd;
    HRESULT hr = dcomp_->CreateTargetForHwnd(hwnd, TRUE, &t.target);
    if (FAILED(hr)) { Log(L"CreateTargetForHwnd failed 0x%08X", hr); return nullptr; }
    if (FAILED(dcomp_->CreateVisual(&t.visual))) return nullptr;
    t.target->SetRoot(t.visual.Get());
    targets_.push_back(std::move(t));
    return &targets_.back();
}

// Copies a frame bitmap 1:1 into a composition surface of the same size.
bool ClockRenderer::CopyToSurface(IDCompositionSurface* surface, ID2D1Bitmap1* bmp) {
    ComPtr<ID2D1DeviceContext> dc;
    POINT off{};
    HRESULT hr = surface->BeginDraw(nullptr, IID_PPV_ARGS(&dc), &off);
    if (FAILED(hr)) { Log(L"surface BeginDraw failed 0x%08X", hr); return false; }
    const D2D1_SIZE_U sz = bmp->GetPixelSize();
    dc->SetDpi(96, 96);   // pixels
    dc->SetTransform(D2D1::Matrix3x2F::Translation((float)off.x, (float)off.y));
    dc->Clear(D2D1::ColorF(0, 0, 0, 0));
    dc->DrawBitmap(bmp, D2D1::RectF(0, 0, (float)sz.width, (float)sz.height), 1.f,
                   D2D1_INTERPOLATION_MODE_NEAREST_NEIGHBOR, nullptr, nullptr);
    return SUCCEEDED(surface->EndDraw());
}

bool ClockRenderer::ShowFrame(HWND hwnd, ID2D1Bitmap1* bmp, POINT pos) {
    Target* t = EnsureTarget(hwnd);
    if (!t) return false;
    const D2D1_SIZE_U sz = bmp->GetPixelSize();
    if (!t->surface || (int)sz.width != t->surfaceW || (int)sz.height != t->surfaceH) {
        t->surface.Reset();
        HRESULT hr = dcomp_->CreateSurface(sz.width, sz.height, DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_ALPHA_MODE_PREMULTIPLIED, &t->surface);
        if (FAILED(hr)) { Log(L"CreateSurface failed 0x%08X", hr); return false; }
        t->surfaceW = sz.width; t->surfaceH = sz.height;
    }
    if (!CopyToSurface(t->surface.Get(), bmp)) return false;
    // Drop any transition visuals and show the static frame, in one atomic commit.
    t->visual->RemoveAllVisuals();
    t->anim.clear();
    t->visual->SetContent(t->surface.Get());
    t->visual->SetOffsetX((float)pos.x);
    t->visual->SetOffsetY((float)pos.y);
    HRESULT hr = dcomp_->Commit();
    if (FAILED(hr)) { Log(L"Commit failed 0x%08X", hr); return false; }
    return true;
}

// from -> to along smoothstep (3u^2 - 2u^3): one cubic segment, evaluated by DWM per frame.
ComPtr<IDCompositionAnimation> ClockRenderer::Ease(double seconds, float from, float to) {
    ComPtr<IDCompositionAnimation> a;
    if (FAILED(dcomp_->CreateAnimation(&a))) return nullptr;
    const double d = seconds, delta = to - from;
    a->AddCubic(0.0, from, 0.f, (float)(3 * delta / (d * d)), (float)(-2 * delta / (d * d * d)));
    a->End(d, to);
    return a;
}

// Opacity for the layer being faded out underneath the one fading in. With the incoming layer
// at opacity s (smoothstep), a pixel of alpha a covered by both composites to
//     a*s + a*o*(1 - a*s)
// which stays exactly a (and its colour lerps linearly) when o = (1 - s) / (1 - a*s).
// That curve isn't a polynomial, so it is approximated by short linear segments.
ComPtr<IDCompositionAnimation> ClockRenderer::FadeOutCurve(double seconds, double textAlpha) {
    ComPtr<IDCompositionAnimation> a;
    if (FAILED(dcomp_->CreateAnimation(&a))) return nullptr;
    auto smooth = [](double u) { return u * u * (3 - 2 * u); };
    auto o = [&](double u) {
        const double s = smooth(u), denom = 1 - textAlpha * s;
        return denom <= 1e-6 ? 0.0 : (1 - s) / denom;
    };
    const int kSegments = 32;
    const double step = seconds / kSegments;
    for (int i = 0; i < kSegments; ++i) {
        const double v0 = o(double(i) / kSegments), v1 = o(double(i + 1) / kSegments);
        a->AddCubic(i * step, (float)v0, (float)((v1 - v0) / step), 0.f, 0.f);
    }
    a->End(seconds, 0.f);
    return a;
}

bool ClockRenderer::Animate(HWND hwnd, const AnimLayer* layers, int count, double seconds, double textAlpha) {
    Target* t = EnsureTarget(hwnd);
    if (!t) return false;
    t->visual->RemoveAllVisuals();
    t->anim.clear();

    // Fade-outs first (bottom), then fade-ins, then solid layers on top.
    IDCompositionVisual* below = nullptr;
    for (AnimLayer::Kind pass : { AnimLayer::FadeOut, AnimLayer::FadeIn, AnimLayer::Solid }) {
        for (int i = 0; i < count; ++i) {
            const AnimLayer& L = layers[i];
            if (L.kind != pass || !L.bmp) continue;
            const D2D1_SIZE_U sz = L.bmp->GetPixelSize();
            ComPtr<IDCompositionSurface> surface;
            ComPtr<IDCompositionVisual> v;
            if (FAILED(dcomp_->CreateSurface(sz.width, sz.height, DXGI_FORMAT_B8G8R8A8_UNORM,
                                             DXGI_ALPHA_MODE_PREMULTIPLIED, &surface)) ||
                !CopyToSurface(surface.Get(), L.bmp) || FAILED(dcomp_->CreateVisual(&v)))
                return false;
            v->SetContent(surface.Get());
            if (L.x0 == L.x1) v->SetOffsetX(L.x0); else { auto a = Ease(seconds, L.x0, L.x1); v->SetOffsetX(a.Get()); t->anim.push_back(a); }
            if (L.y0 == L.y1) v->SetOffsetY(L.y0); else { auto a = Ease(seconds, L.y0, L.y1); v->SetOffsetY(a.Get()); t->anim.push_back(a); }
            if (L.kind != AnimLayer::Solid) {
                ComPtr<IDCompositionEffectGroup> fx;
                if (FAILED(dcomp_->CreateEffectGroup(&fx))) return false;
                auto curve = L.kind == AnimLayer::FadeIn ? Ease(seconds, 0.f, 1.f) : FadeOutCurve(seconds, textAlpha);
                fx->SetOpacity(curve.Get());
                v->SetEffect(fx.Get());
                t->anim.push_back(fx);
                t->anim.push_back(curve);
            }
            t->visual->AddVisual(v.Get(), TRUE, below);   // above the previous layer
            below = v.Get();
            t->anim.push_back(surface);
            t->anim.push_back(v);
        }
    }
    // The static frame steps aside in the same commit, so nothing blinks.
    t->visual->SetContent(nullptr);
    t->visual->SetOffsetX(0.f);
    t->visual->SetOffsetY(0.f);
    HRESULT hr = dcomp_->Commit();
    if (FAILED(hr)) { Log(L"Commit failed 0x%08X", hr); return false; }
    return true;
}

bool ClockRenderer::LineInkOffset(const FrameLayout& f, Line line, float& offsetDip, UINT32 timeMask) {
    FrameLayout t = f;   // the line alone, opaque, without shadow
    t.shadow = false; t.guide = false; t.opacity = 100; t.mediaAlpha = 1; t.secondaryAlpha = 1;
    auto bmp = RenderFrame(t, RGB(255, 255, 255), false, line == kDateLine, line == kTimeLine ? timeMask : 0u,
                           line == kWeatherLine, line == kMediaLine);
    if (!bmp) return false;
    const D2D1_SIZE_U sz = bmp->GetPixelSize();
    const D2D1_BITMAP_PROPERTIES1 bp = D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_CPU_READ | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), f.dpi, f.dpi);
    ComPtr<ID2D1Bitmap1> cpu;
    if (FAILED(offDc_->CreateBitmap(sz, nullptr, 0, bp, &cpu)) || FAILED(cpu->CopyFromBitmap(nullptr, bmp.Get(), nullptr)))
        return false;
    D2D1_MAPPED_RECT mr{};
    if (FAILED(cpu->Map(D2D1_MAP_OPTIONS_READ, &mr))) return false;
    std::vector<BYTE> colMax(sz.width, 0);
    for (UINT y = 0; y < sz.height; ++y) {
        const BYTE* row = mr.bits + (size_t)y * mr.pitch;
        for (UINT x = 0; x < sz.width; ++x) colMax[x] = std::max(colMax[x], row[x * 4 + 3]);
    }
    cpu->Unmap();
    int first = -1, last = -1;
    for (UINT x = 0; x < sz.width; ++x) if (colMax[x]) { if (first < 0) first = (int)x; last = (int)x; }
    if (first < 0) return false;
    // An outer column with partial coverage is taken as partly inked (vertical stems at the edges).
    const float left = first + 1.f - colMax[first] / 255.f, right = last + colMax[last] / 255.f;
    offsetDip = ((left + right) / 2.f - f.g.anchorX) * 96.f / f.dpi;
    return true;
}

void ClockRenderer::CentreLines(FrameLayout& f) {
    const float k = f.dpi / 96.f;
    auto snap = [k](float dip) { return std::round(dip * k) / k; };
    const float axis = f.g.anchorX / k;   // the centre line, DIPs
    auto centre = [&](Line line, D2D1_POINT_2F& origin, UINT32 timeMask = ~0u) {
        // The ink's place relative to the line's whole-pixel origin depends only on the text and
        // its style, so it is measured once per text.
        const std::wstring& text = line == kDateLine ? f.dateText : line == kTimeLine ? f.timeText
                                 : line == kWeatherLine ? f.weatherText : f.mediaText;
        const std::wstring key = std::to_wstring((int)line) + L"|" + std::to_wstring(timeMask) + L"|" + f.styleKey + L"|" + text;
        float off = 0;
        auto it = std::find_if(lineCache_.begin(), lineCache_.end(), [&](const auto& e) { return e.first == key; });
        if (it != lineCache_.end()) {
            off = origin.x + it->second - axis;
        } else {
            if (!LineInkOffset(f, line, off, timeMask)) return;
            if (lineCache_.size() >= 64) lineCache_.erase(lineCache_.begin());
            lineCache_.emplace_back(key, axis + off - origin.x);
        }
        off = std::clamp(off, -kLineSlackDip, kLineSlackDip);   // the room Prepare left on each side
        origin.x = snap(origin.x - off);
    };
    if (f.date) centre(kDateLine, f.dateOrigin);
    // The time was placed in Prepare from a measurement at 4 px per DIP; at the frame's own
    // pixels it can still be ~0.7 px off, so with plain ink-box centring it is trued up here too.
    if (f.time && f.colonIndex >= 0) centre(kTimeLine, f.timeOrigin, 1u << f.colonIndex);
    else if (f.time && f.opticalCentring == 0) centre(kTimeLine, f.timeOrigin);
    if (f.weather) centre(kWeatherLine, f.weatherOrigin);
    if (f.media) centre(kMediaLine, f.mediaOrigin);
}

bool ClockRenderer::MeasureInk(IDWriteTextLayout* layout, Ink& ink) {
    constexpr float kScale = 4.f;   // px per DIP
    DWRITE_TEXT_METRICS m{};
    if (!layout || FAILED(layout->GetMetrics(&m)) || m.width <= 0) return false;
    const float pad = m.height * 0.5f;   // room for overhangs on both sides
    const UINT W = (UINT)std::ceil((m.width + 2 * pad) * kScale), H = (UINT)std::ceil(m.height * kScale);
    ComPtr<IWICBitmap> wbmp;
    if (FAILED(wic_->CreateBitmap(W, H, GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnDemand, &wbmp))) return false;
    ComPtr<ID2D1RenderTarget> rt;
    const D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_SOFTWARE, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
        96.f * kScale, 96.f * kScale);
    if (FAILED(d2d_->CreateWicBitmapRenderTarget(wbmp.Get(), props, &rt))) return false;
    ComPtr<ID2D1SolidColorBrush> brush;
    rt->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1, 1), &brush);
    rt->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    rt->BeginDraw();
    rt->Clear(D2D1::ColorF(0, 0, 0, 0));
    rt->DrawTextLayout(D2D1::Point2F(pad, 0), layout, brush.Get(), D2D1_DRAW_TEXT_OPTIONS_NONE);
    if (FAILED(rt->EndDraw())) return false;
    ComPtr<IWICBitmapLock> lock;
    const WICRect all{ 0, 0, (INT)W, (INT)H };
    if (FAILED(wbmp->Lock(&all, WICBitmapLockRead, &lock))) return false;
    UINT stride = 0, size = 0;
    BYTE* bits = nullptr;
    lock->GetStride(&stride);
    if (FAILED(lock->GetDataPointer(&size, &bits))) return false;
    double sum = 0, moment = 0;
    int first = -1, last = -1;
    for (UINT x = 0; x < W; ++x) {
        double col = 0;
        for (UINT y = 0; y < H; ++y) col += bits[y * stride + x * 4 + 3];
        if (col > 0) { if (first < 0) first = (int)x; last = (int)x; }
        sum += col;
        moment += col * (x + 0.5);
    }
    if (first < 0 || sum <= 0) return false;
    ink.left = first / kScale - pad;
    ink.right = (last + 1) / kScale - pad;
    ink.mass = (float)(moment / sum) / kScale - pad;
    return true;
}

HICON ClockRenderer::MakeTrayIcon(int size, bool lightTaskbar) {
    ComPtr<IDWriteFontCollection> sys;
    dw_->GetSystemFontCollection(&sys);
    const wchar_t* family = L"Segoe Fluent Icons";
    UINT32 idx; BOOL exists = FALSE;
    sys->FindFamilyName(family, &idx, &exists);
    if (!exists) family = L"Segoe MDL2 Assets";

    ComPtr<IDWriteTextFormat> fmt;
    if (FAILED(dw_->CreateTextFormat(family, nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
                                     DWRITE_FONT_STRETCH_NORMAL, (float)size, L"en-us", &fmt))) return nullptr;
    fmt->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    fmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);

    ComPtr<IWICBitmap> wbmp;
    if (FAILED(wic_->CreateBitmap(size, size, GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnDemand, &wbmp))) return nullptr;
    ComPtr<ID2D1RenderTarget> rt;
    const D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_SOFTWARE, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96, 96);
    if (FAILED(d2d_->CreateWicBitmapRenderTarget(wbmp.Get(), props, &rt))) return nullptr;
    ComPtr<ID2D1SolidColorBrush> brush;
    rt->CreateSolidColorBrush(lightTaskbar ? D2D1::ColorF(0, 0, 0, 1) : D2D1::ColorF(1, 1, 1, 1), &brush);
    rt->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    rt->BeginDraw();
    rt->Clear(D2D1::ColorF(0, 0, 0, 0));
    const wchar_t glyph[] = L"";   // Clock
    rt->DrawText(glyph, 1, fmt.Get(), D2D1::RectF(0, 0, (float)size, (float)size), brush.Get());
    if (FAILED(rt->EndDraw())) return nullptr;

    std::vector<BYTE> px(size * size * 4);
    wbmp->CopyPixels(nullptr, size * 4, (UINT)px.size(), px.data());
    for (size_t i = 0; i < px.size(); i += 4) {   // icons want straight (non-premultiplied) alpha
        BYTE a = px[i + 3];
        if (a && a < 255) for (int c = 0; c < 3; ++c) px[i + c] = (BYTE)std::min(255, px[i + c] * 255 / a);
    }
    BITMAPINFO bi{};
    bi.bmiHeader = { sizeof(BITMAPINFOHEADER), size, -size, 1, 32, BI_RGB };
    void* bits = nullptr;
    HBITMAP color = CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!color) return nullptr;
    memcpy(bits, px.data(), px.size());
    HBITMAP mask = CreateBitmap(size, size, 1, 1, nullptr);
    ICONINFO ii{ TRUE, 0, 0, mask, color };
    HICON icon = CreateIconIndirect(&ii);
    DeleteObject(color);
    DeleteObject(mask);
    return icon;
}
