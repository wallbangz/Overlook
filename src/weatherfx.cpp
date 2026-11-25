#include "weatherfx.h"
#include "render.h"
#include <d2d1_1helper.h>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <algorithm>
#include <vector>

// Everything here runs once per Show(): shapes are drawn into a few composition surfaces, then
// bound to looping DirectComposition animations. DWM evaluates the curves each frame; this
// process never wakes for the effect.
namespace weatherfx {
namespace {

using Keep = std::vector<ComPtr<IUnknown>>;
constexpr double kPi = 3.14159265358979323846;

// xorshift32: tiny, deterministic across builds (std:: distributions are not), so a seed
// always draws the same scene in self-tests.
struct Rng {
    uint32_t s;
    explicit Rng(unsigned seed) : s((seed ^ 0xA5A5A5A5u) * 0x9E3779B9u) { if (!s) s = 0x6D2B79F5u; Next(); Next(); }
    uint32_t Next() { s ^= s << 13; s ^= s >> 17; s ^= s << 5; return s; }
    double Unit() { return (Next() >> 8) * (1.0 / 16777216.0); }          // [0, 1)
    double Range(double a, double b) { return a + (b - a) * Unit(); }
};

// One cubic piece of a periodic curve: v(t) = c0 + c1 u + c2 u^2 + c3 u^3, u = t - t0.
struct Seg { double t0, dur, c[4]; };

// Smoothstep from a to b over h seconds (zero slope at both ends, so pieces join smoothly).
Seg Smooth(double t0, double h, double a, double b) {
    const double d = b - a;
    return { t0, h, { a, 0, 3 * d / (h * h), -2 * d / (h * h * h) } };
}

// Binds a curve of period P to an animation, started `phase` seconds into it, repeating
// forever. The period is walked twice and clipped to [phase, phase + P), so a piece cut by the
// phase is re-expressed around its new start (polynomial shift: p(u + s)).
ComPtr<IDCompositionAnimation> Loop(IDCompositionDevice* dc, const std::vector<Seg>& segs, double P, double phase) {
    ComPtr<IDCompositionAnimation> a;
    if (!dc || FAILED(dc->CreateAnimation(&a))) return nullptr;
    phase = std::fmod(std::max(phase, 0.0), P);
    const size_t n = segs.size();
    for (size_t k = 0; k < 2 * n; ++k) {
        const Seg& g = segs[k % n];
        const double t0 = g.t0 + (k >= n ? P : 0), lo = std::max(t0, phase), hi = std::min(t0 + g.dur, phase + P);
        if (hi - lo < 1e-6) continue;
        const double s = lo - t0, *c = g.c;
        const double a0 = c[0] + s * (c[1] + s * (c[2] + s * c[3]));
        const double a1 = c[1] + s * (2 * c[2] + 3 * c[3] * s);
        const double a2 = c[2] + 3 * c[3] * s;
        if (FAILED(a->AddCubic(lo - phase, (float)a0, (float)a1, (float)a2, (float)c[3]))) return nullptr;
    }
    // AddRepeat(begin, d): from `begin` on, replay the d seconds before it, forever.
    if (FAILED(a->AddRepeat(P, P))) return nullptr;
    return a;
}

// a -> b at constant speed over T, then jump back: seamless when a and b are one tile apart.
ComPtr<IDCompositionAnimation> LinearLoop(IDCompositionDevice* dc, double a, double b, double T) {
    return Loop(dc, { { 0, T, { a, (b - a) / T, 0, 0 } } }, T, 0);
}

// lo -> hi -> lo with smoothstep halves: close enough to a sine for slow breathing.
ComPtr<IDCompositionAnimation> Breathe(IDCompositionDevice* dc, double lo, double hi, double P, double phase) {
    return Loop(dc, { Smooth(0, P / 2, lo, hi), Smooth(P / 2, P / 2, hi, lo) }, P, phase);
}

// One-shot fade (same curve as ClockRenderer::Ease, which is private).
ComPtr<IDCompositionAnimation> Fade(IDCompositionDevice* dc, double T, float from, float to) {
    ComPtr<IDCompositionAnimation> a;
    if (!dc || FAILED(dc->CreateAnimation(&a))) return nullptr;
    const double d = to - from;
    a->AddCubic(0.0, from, 0.f, (float)(3 * d / (T * T)), (float)(-2 * d / (T * T * T)));
    a->End(T, to);
    return a;
}

D2D1_COLOR_F Color(COLORREF c, float alpha) {
    return D2D1::ColorF(GetRValue(c) / 255.f, GetGValue(c) / 255.f, GetBValue(c) / 255.f, alpha);
}

// Opens a surface for drawing in pixels, its origin at (0,0) and clipped to its own w x h (a
// composition surface may live in a shared atlas; the clip keeps strokes off the neighbours).
bool Begin(IDCompositionSurface* s, int w, int h, ComPtr<ID2D1DeviceContext>& dc) {
    POINT off{};
    HRESULT hr = s->BeginDraw(nullptr, IID_PPV_ARGS(&dc), &off);
    if (FAILED(hr)) { Log(L"weatherfx: surface BeginDraw failed 0x%08X", hr); return false; }
    dc->SetDpi(96, 96);
    dc->SetTransform(D2D1::Matrix3x2F::Translation((float)off.x, (float)off.y));
    dc->PushAxisAlignedClip(D2D1::RectF(0, 0, (float)w, (float)h), D2D1_ANTIALIAS_MODE_ALIASED);
    dc->Clear(D2D1::ColorF(0, 0, 0, 0));
    return true;
}

bool End(IDCompositionSurface* s, ID2D1DeviceContext* dc) {
    dc->PopAxisAlignedClip();
    HRESULT hr = s->EndDraw();
    if (FAILED(hr)) { Log(L"weatherfx: surface EndDraw failed 0x%08X", hr); return false; }
    return true;
}

ComPtr<IDCompositionSurface> NewSurface(IDCompositionDevice* dc, int w, int h) {
    ComPtr<IDCompositionSurface> s;
    HRESULT hr = dc->CreateSurface(w, h, DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_ALPHA_MODE_PREMULTIPLIED, &s);
    if (FAILED(hr)) { Log(L"weatherfx: CreateSurface %dx%d failed 0x%08X", w, h, hr); return nullptr; }
    return s;
}

void Reset(IDCompositionVisual* layer, Keep* keep) {
    layer->RemoveAllVisuals();
    layer->SetEffect(nullptr);
    keep->clear();
}

// ---- Clouds: clusters of soft discs drifting with the wind across the top of the screen. ----
// Like real cumulus: each cloud is a body with smaller lobes and a flattish base, brighter on top
// and greyer below, with sky between clouds; the ones higher up (overhead) are bigger. The whole
// set drifts sideways, periodic in X so the loop never shows a seam, and each cloud slowly swells
// and settles. No per-disc effect groups: the shades are baked into three surfaces, so DWM only
// moves and scales bitmaps.
constexpr int kDisc = 512;                       // surface side, px; scaled up, so it stays soft
constexpr float kDiscR = 255, kDiscC = 256;
constexpr double kShade[3] = { 1.0, 0.8, 0.55 }; // top, middle, base lobes: share of the cloud's opacity

bool BuildClouds(IDCompositionDevice* dcomp, IDCompositionVisual* layer, Keep& keep, const Params& p, size_t& bytes) {
    const double W = p.width, H = p.height, k = p.dpi / 96.0, gain = p.opacity / 0.4;
    // One lobe's core on its own (overlaps add up); a heavier sky is a little more solid.
    const double lobeOp = std::min(0.13 * gain * (0.85 + 0.15 * p.density), 0.8);
    // ... and hangs lower: bases down to 29 % of the height when light, 40 % when overcast.
    const double baseMax = 0.21 + 0.12 * std::clamp((double)p.density, 0.5, 1.6);

    // A disc with a dense core and a feathered rim, one per shade.
    ComPtr<IDCompositionSurface> disc[3];
    for (int s = 0; s < 3; ++s) {
        disc[s] = NewSurface(dcomp, kDisc, kDisc);
        if (!disc[s]) return false;
        ComPtr<ID2D1DeviceContext> dc;
        if (!Begin(disc[s].Get(), kDisc, kDisc, dc)) return false;
        const float a = (float)(lobeOp * kShade[s]);
        const D2D1_GRADIENT_STOP stops[] = {
            { 0.00f, Color(p.color, a) }, { 0.50f, Color(p.color, a) },
            { 0.78f, Color(p.color, a * 0.6f) }, { 1.00f, Color(p.color, 0.f) },
        };
        ComPtr<ID2D1GradientStopCollection> coll;
        ComPtr<ID2D1RadialGradientBrush> brush;
        if (SUCCEEDED(dc->CreateGradientStopCollection(stops, ARRAYSIZE(stops), &coll)) &&
            SUCCEEDED(dc->CreateRadialGradientBrush(
                D2D1::RadialGradientBrushProperties(D2D1::Point2F(kDiscC, kDiscC), D2D1::Point2F(0, 0), kDiscR, kDiscR),
                coll.Get(), &brush)))
            dc->FillEllipse(D2D1::Ellipse(D2D1::Point2F(kDiscC, kDiscC), kDiscR, kDiscR), brush.Get());
        if (!End(disc[s].Get(), dc.Get()) || !brush) return false;
        keep.push_back(disc[s]);
        bytes += size_t(kDisc) * kDisc * 4;
    }

    // Drift: the clouds repeat every P px in X, P wider than the screen plus the widest cloud,
    // and the drift container slides one period and starts over (an identical picture).
    Rng rng(p.seed);
    const double P = W * 1.5;
    const double driftDip = std::clamp(std::abs(p.windVx) * 0.12, 6.0, 40.0);   // DIPs/s
    const double sign = p.windVx < 0 ? -1 : 1, T = P / (driftDip * k);
    ComPtr<IDCompositionVisual> drift;
    if (FAILED(dcomp->CreateVisual(&drift))) return false;
    auto driftX = LinearLoop(dcomp, sign > 0 ? -P : 0, sign > 0 ? 0 : -P, T);
    if (!driftX) return false;
    drift->SetOffsetX(driftX.Get());
    if (FAILED(layer->AddVisual(drift.Get(), TRUE, nullptr))) return false;
    keep.push_back(drift); keep.push_back(driftX);

    struct Lobe { double x, y, r; int shade; };
    struct Cloud { double cx, base, r0; bool ceiling; std::vector<Lobe> lobes; };
    // Over 1.5 screens: at density 1 about 6 in view with small gaps; 0.5 = a few scattered
    // clouds, 1.6 = nearly overcast.
    const int n = std::clamp((int)std::lround(9 * p.density), 4, 16);
    // Above normal amounts, an overcast sky also closes up overhead: a ceiling of big clouds
    // along the top edge, behind the others (7 extra at 1.6).
    const int nTop = p.density > 1 ? (int)std::lround((p.density - 1) * 12) : 0;
    std::vector<Cloud> clouds;
    for (int i = 0; i < n + nTop; ++i) {
        Cloud c;
        const bool ceiling = i >= n;
        c.ceiling = ceiling;
        if (ceiling) {
            const int t = i - n;
            c.cx = (t + rng.Range(0.1, 0.9)) / nTop * P;
            c.r0 = W * rng.Range(0.08, 0.11);
            c.base = c.r0 * 0.85 + rng.Range(0.0, 0.06) * H;   // the body just inside the top edge
        } else {
            c.cx = (i + 0.5 + rng.Range(-0.3, 0.3)) / n * P;    // stratified over one period, sky between
            c.base = rng.Range(0.12, baseMax) * H;               // where its flattish base sits
            // Perspective: a cloud higher up (overhead) is bigger than one further down.
            const double overhead = 1 - (c.base / H - 0.12) / (baseMax - 0.12);   // 1 at the top of the range, 0 at the bottom
            c.r0 = W * rng.Range(0.06, 0.09) * (0.75 + 0.6 * overhead);
        }
        // The body, then lobes spread along it: big ones in the middle, small at the ends;
        // their bottoms near the base (flat), their tops at different heights (puffy). They
        // overlap heavily, so the cloud reads as one mass rather than a string of dots.
        const int m = 5 + (int)(rng.Unit() * 4);
        c.lobes.push_back({ 0, c.base - c.r0 * 0.75, c.r0, 0 });
        for (int j = 0; j < m; ++j) {
            const double side = (j % 2 ? -1 : 1) * rng.Range(0.25, 1.1) * c.r0;
            const double r = c.r0 * rng.Range(0.55, 0.9) * (1.1 - 0.3 * std::abs(side) / (1.1 * c.r0));
            const bool low = rng.Unit() < 0.45;               // a base lobe (greyer) or a top one
            const double y = low ? c.base - r * rng.Range(0.45, 0.65) : c.base - c.r0 * rng.Range(0.6, 1.1) - r * 0.3;
            c.lobes.push_back({ side, y, r, low ? 2 : (rng.Unit() < 0.5 ? 0 : 1) });
        }
        // The base lobes behind, the bright tops in front.
        std::stable_sort(c.lobes.begin(), c.lobes.end(), [](const Lobe& a, const Lobe& b) { return a.shade > b.shade; });
        clouds.push_back(std::move(c));
    }
    // The ceiling at the back, then the big (near) clouds in front.
    std::stable_sort(clouds.begin(), clouds.end(), [](const Cloud& a, const Cloud& b) {
        return a.ceiling != b.ceiling ? a.ceiling : a.r0 < b.r0;
    });

    int visuals = 0;
    IDCompositionVisual* belowCloud = nullptr;
    for (const Cloud& c : clouds) {
        // Each cloud at x - P, x and x + P, so every position of the drift is covered.
        const double swell = rng.Range(0.025, 0.045), period = rng.Range(22, 38), phase = rng.Range(0, period);
        for (int copy = -1; copy <= 1; ++copy) {
            const double cx = c.cx + copy * P;
            ComPtr<IDCompositionVisual> cv;
            ComPtr<IDCompositionScaleTransform> st;
            if (FAILED(dcomp->CreateVisual(&cv)) || FAILED(dcomp->CreateScaleTransform(&st))) return false;
            // The cloud slowly swells and settles about its base, as cumulus build and fade.
            auto sx = Breathe(dcomp, 1 - swell, 1 + swell, period, phase);
            auto sy = Breathe(dcomp, 1 - swell, 1 + swell, period, phase);
            if (!sx || !sy) return false;
            st->SetCenterX((float)cx); st->SetCenterY((float)c.base);
            st->SetScaleX(sx.Get()); st->SetScaleY(sy.Get());
            cv->SetTransform(st.Get());
            if (FAILED(drift->AddVisual(cv.Get(), TRUE, belowCloud))) return false;
            belowCloud = cv.Get();
            keep.push_back(cv); keep.push_back(st); keep.push_back(sx); keep.push_back(sy);

            IDCompositionVisual* belowLobe = nullptr;
            for (const Lobe& l : c.lobes) {
                ComPtr<IDCompositionVisual> v;
                ComPtr<IDCompositionScaleTransform> ls;
                if (FAILED(dcomp->CreateVisual(&v)) || FAILED(dcomp->CreateScaleTransform(&ls))) return false;
                const float s = (float)(l.r / kDiscR);
                ls->SetCenterX(kDiscC); ls->SetCenterY(kDiscC);
                ls->SetScaleX(s); ls->SetScaleY(s);
                v->SetContent(disc[l.shade].Get());
                v->SetTransform(ls.Get());
                v->SetOffsetX((float)(cx + l.x - kDiscC));
                v->SetOffsetY((float)(l.y - kDiscC));
                if (FAILED(cv->AddVisual(v.Get(), TRUE, belowLobe))) return false;
                belowLobe = v.Get();
                keep.push_back(v); keep.push_back(ls);
                ++visuals;
            }
        }
    }
    Log(L"weatherfx: clouds: %d clouds, %d disc visuals, drift %.0f DIP/s over %.0f s", n, visuals, sign * driftDip, T);
    return true;
}

// ---- Rain: three depth layers of streaks on tiles that the compositor scrolls. ----
struct RainLayer { int tileW, tileH; double speed, length, width, alpha, perMP; };
constexpr RainLayer kRain[] = {   // far .. near; tile heights co-prime-ish so the pattern rarely lines up
    { 520, 509,  700, 22, 1.5, 0.45, 14 },
    { 560, 613, 1000, 32, 2.0, 0.70, 10 },
    { 600, 727, 1400, 46, 2.6, 1.00,  7 },
};
constexpr double kMaxSlant = 25 * kPi / 180;

bool BuildRain(IDCompositionDevice* dcomp, IDCompositionVisual* layer, Keep& keep, const Params& p, size_t& bytes) {
    const double k = p.dpi / 96.0;
    Rng rng(p.seed);
    IDCompositionVisual* belowLayer = nullptr;
    int total = 0;
    for (const RainLayer& L : kRain) {
        const int W = std::max(16, (int)std::lround(L.tileW * k)), H = std::max(16, (int)std::lround(L.tileH * k));
        const double vy = L.speed * k;
        // Streaks lean along their velocity; a strong wind is capped so rain never goes sideways.
        const double ang = std::clamp(std::atan2(p.windVx * k, vy), -kMaxSlant, kMaxSlant);
        const double vx = vy * std::tan(ang), dirX = std::sin(ang), dirY = std::cos(ang);
        const int count = (int)std::lround(p.density * (double(W) * H / 1e6) * L.perMP);

        auto tile = NewSurface(dcomp, W, H);
        if (!tile) return false;
        {
            ComPtr<ID2D1DeviceContext> dc;
            if (!Begin(tile.Get(), W, H, dc)) return false;
            ComPtr<ID2D1Factory> factory;
            dc->GetFactory(&factory);
            ComPtr<ID2D1StrokeStyle> caps;
            ComPtr<ID2D1GradientStopCollection> stops;
            ComPtr<ID2D1LinearGradientBrush> brush;
            const D2D1_GRADIENT_STOP gs[2] = { { 0.f, Color(p.color, 0.f) },
                                               { 1.f, Color(p.color, (float)(L.alpha * p.opacity)) } };
            HRESULT hr = factory->CreateStrokeStyle(
                D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND), nullptr, 0, &caps);
            if (SUCCEEDED(hr)) hr = dc->CreateGradientStopCollection(gs, 2, &stops);
            if (SUCCEEDED(hr)) hr = dc->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties({}, {}), stops.Get(), &brush);
            if (SUCCEEDED(hr)) {
                for (int i = 0; i < count; ++i) {
                    const double x = rng.Range(0, W), y = rng.Range(0, H);
                    const double len = L.length * k * rng.Range(0.75, 1.25), w = L.width * k * rng.Range(0.85, 1.15);
                    brush->SetOpacity((float)rng.Range(0.6, 1.0));
                    // Head at (x, y), tail back up the direction of travel; drawn at each
                    // neighbouring tile offset too, so a streak crossing an edge wraps.
                    for (int oy = -1; oy <= 1; ++oy)
                        for (int ox = -1; ox <= 1; ++ox) {
                            const D2D1_POINT_2F head = D2D1::Point2F((float)(x + ox * W), (float)(y + oy * H));
                            const D2D1_POINT_2F tail = D2D1::Point2F((float)(head.x - dirX * len), (float)(head.y - dirY * len));
                            brush->SetStartPoint(tail);
                            brush->SetEndPoint(head);
                            dc->DrawLine(tail, head, brush.Get(), (float)w, caps.Get());
                        }
                }
            } else {
                Log(L"weatherfx: rain: brush setup failed 0x%08X", hr);
            }
            if (!End(tile.Get(), dc.Get()) || FAILED(hr)) return false;
        }
        keep.push_back(tile);
        bytes += size_t(W) * H * 4;

        // The layer: a grid of visuals sharing the tile, one tile larger than the monitor each
        // way, scrolled by exactly one tile per loop so the jump back is invisible. The offset
        // stays within [-W, 0] x [-H, 0], so columns 0..ceil(mon/W)+1 always cover the monitor.
        ComPtr<IDCompositionVisual> lv;
        if (FAILED(dcomp->CreateVisual(&lv))) return false;
        keep.push_back(lv);
        const int cols = (p.width + W - 1) / W + 2, rows = (p.height + H - 1) / H + 2;
        IDCompositionVisual* below = nullptr;
        for (int j = 0; j < rows; ++j)
            for (int i = 0; i < cols; ++i) {
                ComPtr<IDCompositionVisual> v;
                if (FAILED(dcomp->CreateVisual(&v))) return false;
                v->SetContent(tile.Get());
                v->SetOffsetX((float)(i * W));
                v->SetOffsetY((float)(j * H));
                if (FAILED(lv->AddVisual(v.Get(), TRUE, below))) return false;
                below = v.Get();
                keep.push_back(v);
            }
        auto ay = LinearLoop(dcomp, -H, 0, H / vy);
        if (!ay) return false;
        lv->SetOffsetY(ay.Get());
        keep.push_back(ay);
        if (std::fabs(vx) > 1) {
            auto ax = vx > 0 ? LinearLoop(dcomp, -W, 0, W / vx) : LinearLoop(dcomp, 0, -W, W / -vx);
            if (!ax) return false;
            lv->SetOffsetX(ax.Get());
            keep.push_back(ax);
        } else {
            lv->SetOffsetX(-W / 2.f);
        }
        // Far first (bottom), near last (top).
        if (FAILED(layer->AddVisual(lv.Get(), TRUE, belowLayer))) return false;
        belowLayer = lv.Get();
        total += count;
        Log(L"weatherfx: rain layer %dx%d px, %d streaks, %d tiles, %.0f px/s, slant %.1f deg",
            W, H, count, cols * rows, vy, ang * 180 / kPi);
    }
    Log(L"weatherfx: rain: %d streaks in 3 layers", total);
    return true;
}

}   // namespace

std::wstring Describe(const Params& p) {
    static const wchar_t* kNames[] = { L"none", L"clouds", L"rain" };
    const int kind = std::clamp((int)p.kind, 0, 2);
    wchar_t buf[160];
    swprintf_s(buf, L"%s %dx%d@%ddpi #%02X%02X%02X a%d w%d d%d s%u", kNames[kind], p.width, p.height,
               (int)std::lround(p.dpi), GetRValue(p.color), GetGValue(p.color), GetBValue(p.color),
               (int)std::lround(p.opacity * 100), (int)std::lround(p.windVx), (int)std::lround(p.density * 100), p.seed);
    return buf;
}

bool Show(ClockRenderer& r, HWND hwnd, const Params& p, double fadeSeconds) {
    Keep* keep = nullptr;
    IDCompositionVisual* layer = r.FxLayer(hwnd, &keep);
    IDCompositionDevice* dcomp = r.Dcomp();
    if (!layer || !keep || !dcomp) { Log(L"weatherfx: no composition layer"); return false; }
    Reset(layer, keep);
    if (p.kind == Kind::None || p.width <= 0 || p.height <= 0) return r.Commit();

    size_t bytes = 0;
    bool ok = p.kind == Kind::Clouds ? BuildClouds(dcomp, layer, *keep, p, bytes)
                                     : BuildRain(dcomp, layer, *keep, p, bytes);
    if (ok && fadeSeconds > 0) {
        // One effect group on the container fades the whole scene in (nested groups multiply).
        ComPtr<IDCompositionEffectGroup> fx;
        auto fade = Fade(dcomp, fadeSeconds, 0.f, 1.f);
        ok = fade && SUCCEEDED(dcomp->CreateEffectGroup(&fx)) && SUCCEEDED(fx->SetOpacity(fade.Get())) &&
             SUCCEEDED(layer->SetEffect(fx.Get()));
        if (ok) { keep->push_back(fx); keep->push_back(fade); }
        else Log(L"weatherfx: fade-in setup failed");
    }
    if (!ok) {
        // Never leave a half-built scene on screen.
        Log(L"weatherfx: %s failed; layer cleared", Describe(p).c_str());
        Reset(layer, keep);
        r.Commit();
        return false;
    }
    Log(L"weatherfx: showing %s, %zu objects, surfaces ~%zu KB, fade %.1f s",
        Describe(p).c_str(), keep->size(), bytes / 1024, fadeSeconds);
    return r.Commit();
}

void Clear(ClockRenderer& r, HWND hwnd) {
    Keep* keep = nullptr;
    IDCompositionVisual* layer = r.FxLayer(hwnd, &keep);
    if (!layer || !keep) return;
    Reset(layer, keep);
    r.Commit();
}

}   // namespace weatherfx
