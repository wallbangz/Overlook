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

// ---- Rain: storm clouds above three depth layers of streaks on scrolled tiles. ----
// Like rain in photos: long, thin, motion-blurred streaks, many faint far ones that read as a
// haze and a few brighter near ones, all leaning a little with the wind.
struct RainLayer { int tileW, tileH; double speed, length, width, alpha, perMP; };
constexpr RainLayer kRain[] = {   // far .. near; tile heights co-prime-ish so the pattern rarely lines up
    { 520, 509,  900, 40, 1.0, 0.35, 70 },
    { 560, 613, 1300, 62, 1.4, 0.55, 38 },
    { 600, 727, 1800, 92, 1.9, 0.80, 16 },
};
constexpr double kMaxSlant = 25 * kPi / 180;
constexpr double kStillSlant = 4 * kPi / 180;   // a touch of lean even in still air: dead-vertical reads as scratches

// Rain clouds are the cloud colour pushed toward storm: darker on a bright wallpaper (where the
// shapes are already mid-grey), greyer on a dark one (where they are light).
double Luminance(COLORREF c) {   // rough, gamma-encoded: only to tell light shapes from dark ones
    return (0.2126 * GetRValue(c) + 0.7152 * GetGValue(c) + 0.0722 * GetBValue(c)) / 255;
}

COLORREF StormColor(COLORREF c) {
    const double f = Luminance(c) < 0.6 ? 0.62 : 0.80;
    return RGB((BYTE)(GetRValue(c) * f), (BYTE)(GetGValue(c) * f), (BYTE)(GetBValue(c) * f));
}

bool BuildRain(IDCompositionDevice* dcomp, IDCompositionVisual* layer, Keep& keep, const Params& p, size_t& bytes) {
    const double k = p.dpi / 96.0, gain = p.opacity / 0.4;
    // Tiles are drawn at no more than 125 % and enlarged by the compositor (f): soft streaks
    // don't need the pixels, and on a 4K monitor at 200 % that saves about two thirds of the memory.
    const double ts = std::min(k, 1.25), f = k / ts;
    // Freezing rain and sleet: slower, shorter, chunkier drops with an icy tint.
    const double speedK = p.icy ? 0.62 : 1, lengthK = p.icy ? 0.45 : 1, widthK = p.icy ? 1.7 : 1;
    const COLORREF ice = Luminance(p.color) < 0.6 ? RGB(0x3C, 0x5F, 0x82) : RGB(0xC8, 0xE6, 0xFF);
    auto mix = [](BYTE a, BYTE b, double t) { return (BYTE)std::lround(a + (b - a) * t); };
    const COLORREF col = p.icy ? RGB(mix(GetRValue(p.color), GetRValue(ice), 0.45), mix(GetGValue(p.color), GetGValue(ice), 0.45),
                                     mix(GetBValue(p.color), GetBValue(ice), 0.45))
                               : p.color;
    Rng rng(p.seed);
    IDCompositionVisual* belowLayer = nullptr;
    int total = 0;
    for (const RainLayer& L0 : kRain) {
        RainLayer L = L0;
        L.speed *= speedK; L.length *= lengthK; L.width *= widthK;
        const int W = std::max(16, (int)std::lround(L.tileW * ts)), H = std::max(16, (int)std::lround(L.tileH * ts));
        const double Ws = W * f, Hs = H * f;   // a tile's size on screen
        const double vy = L.speed * k;         // screen px/s
        // Streaks lean along their velocity; a strong wind is capped so rain never goes sideways.
        const double still = p.windVx < 0 ? -kStillSlant : kStillSlant;
        const double ang = std::clamp(std::atan2(p.windVx * k, vy) + still, -kMaxSlant, kMaxSlant);
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
            const D2D1_GRADIENT_STOP gs[2] = { { 0.f, Color(col, 0.f) },
                                               { 1.f, Color(col, (float)std::min(L.alpha * gain, 1.0)) } };
            HRESULT hr = factory->CreateStrokeStyle(
                D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND), nullptr, 0, &caps);
            if (SUCCEEDED(hr)) hr = dc->CreateGradientStopCollection(gs, 2, &stops);
            if (SUCCEEDED(hr)) hr = dc->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties({}, {}), stops.Get(), &brush);
            if (SUCCEEDED(hr)) {
                for (int i = 0; i < count; ++i) {
                    const double x = rng.Range(0, W), y = rng.Range(0, H);
                    const double len = L.length * ts * rng.Range(0.6, 1.4), w = L.width * ts * rng.Range(0.85, 1.15);
                    brush->SetOpacity((float)rng.Range(0.45, 1.0));
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
        if (f > 1.001) {   // children in tile px, enlarged to screen px; the offset is in screen px
            ComPtr<IDCompositionScaleTransform> st;
            if (FAILED(dcomp->CreateScaleTransform(&st))) return false;
            st->SetScaleX((float)f); st->SetScaleY((float)f);
            lv->SetTransform(st.Get());
            keep.push_back(st);
        }
        const int cols = (int)std::ceil(p.width / Ws) + 2, rows = (int)std::ceil(p.height / Hs) + 2;
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
        auto ay = LinearLoop(dcomp, -Hs, 0, Hs / vy);
        if (!ay) return false;
        lv->SetOffsetY(ay.Get());
        keep.push_back(ay);
        if (std::fabs(vx) > 1) {
            auto ax = vx > 0 ? LinearLoop(dcomp, -Ws, 0, Ws / vx) : LinearLoop(dcomp, 0, -Ws, Ws / -vx);
            if (!ax) return false;
            lv->SetOffsetX(ax.Get());
            keep.push_back(ax);
        } else {
            lv->SetOffsetX((float)(-Ws / 2));
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

// ---- Fog: a pale veil over everything, thicker mist banks low down, drifting slowly. ----
// Fog hides the distance rather than filling the sky: a faint even veil, then long soft banks
// over the lower part of the screen (where the land would be), denser toward the bottom. The
// banks drift sideways in a seamless loop like the clouds.
bool BuildFog(IDCompositionDevice* dcomp, IDCompositionVisual* layer, Keep& keep, const Params& p, size_t& bytes) {
    const double W = p.width, H = p.height, k = p.dpi / 96.0, gain = p.opacity / 0.4;
    // Fog lightens: the shape colour pulled halfway to white.
    const COLORREF col = RGB((GetRValue(p.color) + 255) / 2, (GetGValue(p.color) + 255) / 2, (GetBValue(p.color) + 255) / 2);
    const double dens = std::clamp((double)p.density, 0.5, 1.6);

    // The veil: a tiny solid surface stretched over the monitor.
    auto veilS = NewSurface(dcomp, 8, 8);
    if (!veilS) return false;
    {
        ComPtr<ID2D1DeviceContext> dc;
        if (!Begin(veilS.Get(), 8, 8, dc)) return false;
        dc->Clear(Color(col, (float)std::min((Luminance(p.color) < 0.6 ? 0.07 : 0.04) * gain * dens, 0.4)));
        if (!End(veilS.Get(), dc.Get())) return false;
    }
    keep.push_back(veilS);
    bytes += 8 * 8 * 4;
    ComPtr<IDCompositionVisual> veil;
    ComPtr<IDCompositionScaleTransform> vs;
    if (FAILED(dcomp->CreateVisual(&veil)) || FAILED(dcomp->CreateScaleTransform(&vs))) return false;
    vs->SetScaleX((float)(W / 8)); vs->SetScaleY((float)(H / 8));
    veil->SetContent(veilS.Get());
    veil->SetTransform(vs.Get());
    if (FAILED(layer->AddVisual(veil.Get(), TRUE, nullptr))) return false;
    keep.push_back(veil); keep.push_back(vs);

    // A soft blob, stretched wide and flat into a mist bank.
    auto blob = NewSurface(dcomp, kDisc, kDisc);
    if (!blob) return false;
    {
        ComPtr<ID2D1DeviceContext> dc;
        if (!Begin(blob.Get(), kDisc, kDisc, dc)) return false;
        // Pale mist stands out strongly on a dark wallpaper and hardly at all on a bright one.
        const float a = (float)std::min(gain * (Luminance(p.color) < 0.6 ? 0.35 : 0.09), 0.8);
        const D2D1_GRADIENT_STOP gs[3] = { { 0.f, Color(col, a) }, { 0.35f, Color(col, a * 0.7f) }, { 1.f, Color(col, 0.f) } };
        ComPtr<ID2D1GradientStopCollection> stops;
        ComPtr<ID2D1RadialGradientBrush> brush;
        if (SUCCEEDED(dc->CreateGradientStopCollection(gs, 3, &stops)) &&
            SUCCEEDED(dc->CreateRadialGradientBrush(D2D1::RadialGradientBrushProperties(
                D2D1::Point2F(kDiscC, kDiscC), {}, kDiscR, kDiscR), stops.Get(), &brush)))
            dc->FillEllipse(D2D1::Ellipse(D2D1::Point2F(kDiscC, kDiscC), kDiscR, kDiscR), brush.Get());
        if (!End(blob.Get(), dc.Get()) || !brush) return false;
    }
    keep.push_back(blob);
    bytes += size_t(kDisc) * kDisc * 4;

    Rng rng(p.seed ^ 0xF06u);
    const double P = W * 1.5;
    const double driftDip = std::clamp(std::abs(p.windVx) * 0.1, 4.0, 25.0);
    const double sign = p.windVx < 0 ? -1 : 1, T = P / (driftDip * k);
    ComPtr<IDCompositionVisual> drift;
    if (FAILED(dcomp->CreateVisual(&drift))) return false;
    auto driftX = LinearLoop(dcomp, sign > 0 ? -P : 0, sign > 0 ? 0 : -P, T);
    if (!driftX) return false;
    drift->SetOffsetX(driftX.Get());
    if (FAILED(layer->AddVisual(drift.Get(), TRUE, veil.Get()))) return false;
    keep.push_back(drift); keep.push_back(driftX);

    const int n = (int)std::lround(14 * dens);
    IDCompositionVisual* below = nullptr;
    int visuals = 0;
    for (int i = 0; i < n; ++i) {
        // Banks low on the screen, more of them toward the bottom; wide and flat.
        const double u = rng.Unit();
        // Centres in the bottom third (a ground fog: the sky above stays clear).
        const double cy = H * (0.70 + 0.38 * std::sqrt(u)), cx = (i + rng.Range(0.0, 1.0)) / n * P;
        // About 4 times wider than tall: flatter reads as stripes, taller fills the screen.
        const double rx = W * rng.Range(0.22, 0.36), ry = H * rng.Range(0.07, 0.11);
        for (int copy = -1; copy <= 1; ++copy) {
            ComPtr<IDCompositionVisual> v;
            ComPtr<IDCompositionScaleTransform> st;
            if (FAILED(dcomp->CreateVisual(&v)) || FAILED(dcomp->CreateScaleTransform(&st))) return false;
            st->SetCenterX(kDiscC); st->SetCenterY(kDiscC);
            st->SetScaleX((float)(rx / kDiscR)); st->SetScaleY((float)(ry / kDiscR));
            v->SetContent(blob.Get());
            v->SetTransform(st.Get());
            v->SetOffsetX((float)(cx + copy * P - kDiscC));
            v->SetOffsetY((float)(cy - kDiscC));
            if (FAILED(drift->AddVisual(v.Get(), TRUE, below))) return false;
            below = v.Get();
            keep.push_back(v); keep.push_back(st);
            ++visuals;
        }
    }
    Log(L"weatherfx: fog: veil + %d banks (%d visuals), drift %.0f DIP/s", n, visuals, sign * driftDip);
    return true;
}

// ---- Lightning: a soft glow in the sky behind the clouds, now and then. ----
// Not a full-screen flash (harsh to work next to, and photosensitivity guidance allows at most
// three flashes a second): one gentle pulse, 80 ms up and ~450 ms down, at most once every
// 6 s, somewhere along the top, lighting the clouds from behind. The whole schedule is baked
// into one long repeating animation, so lightning costs no wake-ups either.
constexpr int kGlow = 256;
constexpr double kStormPeriod = 97;   // s; prime-ish, so it doesn't line up with the cloud drift

bool BuildLightning(IDCompositionDevice* dcomp, IDCompositionVisual* layer, Keep& keep, const Params& p, size_t& bytes) {
    auto glow = NewSurface(dcomp, kGlow, kGlow);
    if (!glow) return false;
    {
        ComPtr<ID2D1DeviceContext> dc;
        if (!Begin(glow.Get(), kGlow, kGlow, dc)) return false;
        // Brightest at the top centre, fading out downwards and sideways; a cool white whatever
        // the wallpaper, since lightning lights things up.
        const D2D1_COLOR_F c = D2D1::ColorF(0.92f, 0.94f, 1.0f, 1.f), c0 = D2D1::ColorF(0.92f, 0.94f, 1.0f, 0.f);
        const D2D1_GRADIENT_STOP gs[3] = { { 0.f, c }, { 0.4f, D2D1::ColorF(c.r, c.g, c.b, 0.45f) }, { 1.f, c0 } };
        ComPtr<ID2D1GradientStopCollection> stops;
        ComPtr<ID2D1RadialGradientBrush> brush;
        if (SUCCEEDED(dc->CreateGradientStopCollection(gs, 3, &stops)) &&
            SUCCEEDED(dc->CreateRadialGradientBrush(D2D1::RadialGradientBrushProperties(
                D2D1::Point2F(kGlow / 2.f, 0), {}, kGlow / 2.f, (float)kGlow), stops.Get(), &brush)))
            dc->FillRectangle(D2D1::RectF(0, 0, (float)kGlow, (float)kGlow), brush.Get());
        if (!End(glow.Get(), dc.Get()) || !brush) return false;
    }
    keep.push_back(glow);
    bytes += size_t(kGlow) * kGlow * 4;

    // On a bright wallpaper the glow lies over dark storm clouds and has a pale sky around it, so
    // it needs more strength to read as a flash.
    const double W = p.width, H = p.height, gain = p.opacity / 0.4 * (Luminance(p.color) < 0.6 ? 2.2 : 1.0);
    Rng rng(p.seed ^ 0x7C3Fu);
    std::vector<Seg> op, xs;
    double t = rng.Range(3, 9), last = 0, lastX = 0;
    int flashes = 0;
    while (t < kStormPeriod - 1.5) {
        const double peak = std::min(rng.Range(0.16, 0.28) * gain, 0.6), up = 0.08, down = rng.Range(0.35, 0.55);
        const double x = rng.Range(0.1, 0.9) * W;
        op.push_back({ last, t - last, { 0, 0, 0, 0 } });                       // dark
        op.push_back({ t, up, { 0, peak / up, 0, 0 } });                       // rise
        op.push_back({ t + up, down, { peak, -2 * peak / down, peak / (down * down), 0 } });   // ease out to 0
        xs.push_back({ lastX, t + up + down - lastX, { x - W * 0.6, 0, 0, 0 } });   // moved while dark
        last = lastX = t + up + down;
        t = last + rng.Range(6, 20);
        ++flashes;
    }
    op.push_back({ last, kStormPeriod - last, { 0, 0, 0, 0 } });
    xs.push_back({ lastX, kStormPeriod - lastX, { xs.empty() ? 0 : xs.back().c[0], 0, 0, 0 } });

    ComPtr<IDCompositionVisual> v;
    ComPtr<IDCompositionScaleTransform> st;
    ComPtr<IDCompositionEffectGroup> fx;
    if (FAILED(dcomp->CreateVisual(&v)) || FAILED(dcomp->CreateScaleTransform(&st)) || FAILED(dcomp->CreateEffectGroup(&fx)))
        return false;
    auto opacity = Loop(dcomp, op, kStormPeriod, 0), offX = Loop(dcomp, xs, kStormPeriod, 0);
    if (!opacity || !offX) return false;
    st->SetScaleX((float)(W * 1.2 / kGlow));
    st->SetScaleY((float)(H * 0.75 / kGlow));
    v->SetContent(glow.Get());
    v->SetTransform(st.Get());
    fx->SetOpacity(opacity.Get());
    v->SetEffect(fx.Get());
    v->SetOffsetX(offX.Get());
    if (FAILED(layer->AddVisual(v.Get(), TRUE, nullptr))) return false;
    keep.push_back(v); keep.push_back(st); keep.push_back(fx); keep.push_back(opacity); keep.push_back(offX);
    Log(L"weatherfx: lightning: %d flashes per %.0f s", flashes, kStormPeriod);
    return true;
}

// ---- Snow: soft flakes in three depth layers, drifting down and swaying. ----
// Snow falls slowly and wanders: near flakes are bigger, faster and sway wider than far ones.
// Flakes stay clear of the tile's left and right edges, so each column of tiles can sway on
// its own phase (no flake is cut between two columns) and the layer never moves in lockstep.
struct SnowLayer { int tileW, tileH; double speed, rMin, rMax, alpha, perMP, sway, swayPeriod; };
constexpr SnowLayer kSnow[] = {   // far .. near
    { 500, 509, 32, 2.2, 3.4, 0.55, 140, 10, 6.5 },
    { 540, 613, 50, 3.6, 5.4, 0.75,  68, 16, 5.0 },
    { 580, 727, 78, 5.6, 8.8, 0.95,  26, 24, 3.8 },
};
// Sleet's ice pellets: small, hard and fast, barely swaying; sparse among the short streaks.
constexpr SnowLayer kPellets[] = {
    { 500, 509, 210, 1.5, 2.2, 0.75, 80, 2, 2.5 },
    { 540, 613, 300, 2.1, 3.0, 0.90, 45, 3, 2.1 },
    { 580, 727, 420, 2.8, 4.0, 1.00, 20, 4, 1.7 },
};

bool BuildSnow(IDCompositionDevice* dcomp, IDCompositionVisual* layer, Keep& keep, const Params& p, size_t& bytes,
               bool pellets = false) {
    const double k = p.dpi / 96.0, gain = p.opacity / 0.4;
    const double ts = std::min(k, 1.25), f = k / ts;   // as rain: tiles at <= 125 %, enlarged on screen
    Rng rng(p.seed ^ (pellets ? 0x1CEu : 0x5A0Bu));
    IDCompositionVisual* belowLayer = nullptr;
    int total = 0;
    for (const SnowLayer& L : pellets ? kPellets : kSnow) {
        const int W = std::max(16, (int)std::lround(L.tileW * ts)), H = std::max(16, (int)std::lround(L.tileH * ts));
        const double Ws = W * f, Hs = H * f;
        const double vy = L.speed * k;
        // Snow is light: the wind carries it further sideways than rain, slower layers lean more.
        const double vx = std::clamp(p.windVx * k * 0.5, -vy * 1.2, vy * 1.2);
        const int count = (int)std::lround(p.density * (double(W) * H / 1e6) * L.perMP);
        const double margin = L.rMax * ts + 1;

        auto tile = NewSurface(dcomp, W, H);
        if (!tile) return false;
        {
            ComPtr<ID2D1DeviceContext> dc;
            if (!Begin(tile.Get(), W, H, dc)) return false;
            const float a = (float)std::min(L.alpha * gain, 1.0);
            // Pellets are ice: a harder edge than a flake.
            const D2D1_GRADIENT_STOP gs[3] = { { 0.f, Color(p.color, a) }, { pellets ? 0.7f : 0.45f, Color(p.color, a * 0.85f) },
                                               { 1.f, Color(p.color, 0.f) } };
            ComPtr<ID2D1GradientStopCollection> stops;
            ComPtr<ID2D1RadialGradientBrush> brush;
            HRESULT hr = dc->CreateGradientStopCollection(gs, 3, &stops);
            if (SUCCEEDED(hr)) hr = dc->CreateRadialGradientBrush(D2D1::RadialGradientBrushProperties({}, {}, 1, 1),
                                                                  stops.Get(), &brush);
            if (SUCCEEDED(hr)) {
                for (int i = 0; i < count; ++i) {
                    const double x = rng.Range(margin, W - margin), y = rng.Range(0, H);
                    const float r = (float)(rng.Range(L.rMin, L.rMax) * ts);
                    brush->SetOpacity((float)rng.Range(0.55, 1.0));
                    brush->SetRadiusX(r); brush->SetRadiusY(r);
                    // Wrapped vertically only: the tile scrolls down; sideways it never wraps.
                    for (int oy = -1; oy <= 1; ++oy) {
                        const D2D1_POINT_2F c = D2D1::Point2F((float)x, (float)(y + oy * H));
                        brush->SetCenter(c);
                        dc->FillEllipse(D2D1::Ellipse(c, r, r), brush.Get());
                    }
                }
            } else {
                Log(L"weatherfx: snow: brush setup failed 0x%08X", hr);
            }
            if (!End(tile.Get(), dc.Get()) || FAILED(hr)) return false;
        }
        keep.push_back(tile);
        bytes += size_t(W) * H * 4;

        // Layer (screen px, scrolled down one tile per loop and drifting with the wind), scaled
        // by f; inside it one visual per column (tile px) swaying on its own phase.
        ComPtr<IDCompositionVisual> lv;
        if (FAILED(dcomp->CreateVisual(&lv))) return false;
        keep.push_back(lv);
        if (f > 1.001) {
            ComPtr<IDCompositionScaleTransform> st;
            if (FAILED(dcomp->CreateScaleTransform(&st))) return false;
            st->SetScaleX((float)f); st->SetScaleY((float)f);
            lv->SetTransform(st.Get());
            keep.push_back(st);
        }
        const bool drift = std::fabs(vx) > 1;
        const int cols = (int)std::ceil(p.width / Ws) + 2, rows = (int)std::ceil(p.height / Hs) + 2;
        const double amp = L.sway * ts;   // tile px
        IDCompositionVisual* belowCol = nullptr;
        for (int i = 0; i < cols; ++i) {
            ComPtr<IDCompositionVisual> col;
            if (FAILED(dcomp->CreateVisual(&col))) return false;
            // A drifting layer loops sideways by one column, so all columns must sway alike or
            // the loop would show a jump; still air lets each column keep its own phase.
            const double phase = drift ? 0 : rng.Range(0, L.swayPeriod);
            auto sx = Loop(dcomp, { Smooth(0, L.swayPeriod / 2, i * W - amp, i * W + amp),
                                    Smooth(L.swayPeriod / 2, L.swayPeriod / 2, i * W + amp, i * W - amp) },
                           L.swayPeriod, phase);
            if (!sx) return false;
            col->SetOffsetX(sx.Get());
            keep.push_back(col); keep.push_back(sx);
            IDCompositionVisual* below = nullptr;
            for (int j = 0; j < rows; ++j) {
                ComPtr<IDCompositionVisual> v;
                if (FAILED(dcomp->CreateVisual(&v))) return false;
                v->SetContent(tile.Get());
                v->SetOffsetY((float)(j * H));
                if (FAILED(col->AddVisual(v.Get(), TRUE, below))) return false;
                below = v.Get();
                keep.push_back(v);
            }
            if (FAILED(lv->AddVisual(col.Get(), TRUE, belowCol))) return false;
            belowCol = col.Get();
        }
        auto ay = LinearLoop(dcomp, -Hs, 0, Hs / vy);
        if (!ay) return false;
        lv->SetOffsetY(ay.Get());
        keep.push_back(ay);
        if (drift) {
            auto ax = vx > 0 ? LinearLoop(dcomp, -Ws, 0, Ws / vx) : LinearLoop(dcomp, 0, -Ws, Ws / -vx);
            if (!ax) return false;
            lv->SetOffsetX(ax.Get());
            keep.push_back(ax);
        } else {
            lv->SetOffsetX((float)(-Ws / 2));
        }
        if (FAILED(layer->AddVisual(lv.Get(), TRUE, belowLayer))) return false;
        belowLayer = lv.Get();
        total += count;
        Log(L"weatherfx: snow layer %dx%d px, %d flakes, %dx%d tiles, %.0f px/s, drift %.0f px/s",
            W, H, count, cols, rows, vy, vx);
    }
    Log(L"weatherfx: snow: %d flakes in 3 layers", total);
    return true;
}

}   // namespace

std::wstring Describe(const Params& p) {
    static const wchar_t* kNames[] = { L"none", L"clouds", L"rain", L"snow", L"fog" };
    const int kind = std::clamp((int)p.kind, 0, 4);
    wchar_t buf[160];
    swprintf_s(buf, L"%s%s%s %dx%d@%ddpi #%02X%02X%02X a%d w%d d%d s%u", kNames[kind],
               p.icy ? L"+icy" : L"", p.lightning ? L"+lightning" : L"", p.width, p.height,
               (int)std::lround(p.dpi), GetRValue(p.color), GetGValue(p.color), GetBValue(p.color),
               (int)std::lround(p.opacity * 100), (int)std::lround(p.windVx), (int)std::lround(p.density * 100), p.seed);
    return buf;
}

bool Show(ClockRenderer& r, HWND hwnd, const Params& day, double fadeSeconds) {
    // At night the sky is quieter: shapes a quarter fainter and a little bluer.
    Params p = day;
    if (!p.isDay) {
        p.opacity *= 0.75f;
        auto cool = [](BYTE v, BYTE to) { return (BYTE)std::lround(v + (to - v) * 0.25); };
        p.color = RGB(cool(GetRValue(p.color), 0x70), cool(GetGValue(p.color), 0x84), cool(GetBValue(p.color), 0xB0));
    }
    Keep* keep = nullptr;
    IDCompositionVisual* layer = r.FxLayer(hwnd, &keep);
    IDCompositionDevice* dcomp = r.Dcomp();
    if (!layer || !keep || !dcomp) { Log(L"weatherfx: no composition layer"); return false; }
    Reset(layer, keep);
    if (p.kind == Kind::None || p.width <= 0 || p.height <= 0) return r.Commit();

    size_t bytes = 0;
    bool ok;
    if (p.kind == Kind::Clouds) {
        ok = BuildClouds(dcomp, layer, *keep, p, bytes);
    } else if (p.kind == Kind::Fog) {
        ok = BuildFog(dcomp, layer, *keep, p, bytes);
    } else {
        // Rain and snow fall from a cloud cover: the particles in one container, the clouds in a
        // second one explicitly above it (so they hide where the particles start).
        ComPtr<IDCompositionVisual> fall, sky;
        ok = SUCCEEDED(dcomp->CreateVisual(&fall)) && SUCCEEDED(dcomp->CreateVisual(&sky)) &&
             SUCCEEDED(layer->AddVisual(fall.Get(), TRUE, nullptr)) &&
             SUCCEEDED(layer->AddVisual(sky.Get(), TRUE, fall.Get()));
        if (ok) {
            keep->push_back(fall); keep->push_back(sky);
            Params particles = p, clouds = p;
            clouds.density = (float)std::clamp(0.9 + 0.5 * p.density, 1.0, 1.6);   // light: broken; heavy: overcast
            if (p.kind == Kind::Rain) {
                clouds.color = StormColor(p.color);
                // On a bright wallpaper mid-grey streaks would vanish: they take the storm tone too.
                if (Luminance(p.color) < 0.6) particles.color = clouds.color;
                ok = BuildRain(dcomp, fall.Get(), *keep, particles, bytes);
                if (ok && p.icy) {   // sleet: ice pellets among the drops, in their own container
                    ComPtr<IDCompositionVisual> ice;
                    Params pel = particles;
                    // Bright ice on a dark wallpaper; on a bright one the icy streak tone carries it.
                    if (Luminance(p.color) >= 0.6) pel.color = RGB(0xF4, 0xFA, 0xFF);
                    ok = SUCCEEDED(dcomp->CreateVisual(&ice)) && SUCCEEDED(layer->AddVisual(ice.Get(), TRUE, fall.Get())) &&
                         BuildSnow(dcomp, ice.Get(), *keep, pel, bytes, true);
                    if (ice) keep->push_back(ice);
                }
                if (ok && p.lightning) {
                    // On a dark wallpaper, between the rain and the clouds: it lights them from behind.
                    // On a bright one a pale glow behind them vanishes into the sky, so it goes over
                    // the dark storm clouds instead and lights them up.
                    IDCompositionVisual* below = Luminance(p.color) < 0.6 ? sky.Get() : fall.Get();
                    ComPtr<IDCompositionVisual> storm;
                    ok = SUCCEEDED(dcomp->CreateVisual(&storm)) && SUCCEEDED(layer->AddVisual(storm.Get(), TRUE, below)) &&
                         BuildLightning(dcomp, storm.Get(), *keep, p, bytes);
                    if (storm) keep->push_back(storm);
                }
            } else {
                // Snow clouds keep the plain cloud tone, and the flakes the shape colour: light on
                // a dark wallpaper, mid-grey on a bright one (where white flakes would vanish).
                ok = BuildSnow(dcomp, fall.Get(), *keep, particles, bytes);
            }
            ok = ok && BuildClouds(dcomp, sky.Get(), *keep, clouds, bytes);
        }
    }
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
