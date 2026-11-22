#include "icons.h"
#include <cmath>
#include <initializer_list>

using Microsoft::WRL::ComPtr;

namespace icon {
namespace {

using G = ComPtr<ID2D1Geometry>;
constexpr float kTol = 0.002f;   // flattening tolerance, icon units (the icon is ~20-40 px tall)
constexpr float kGap = 0.06f;    // gap where a shape sits in front of another
constexpr float kStroke = 0.10f; // precipitation, fog, wind and umbrella strokes (= the sunrise icon's)
constexpr float kRay = 0.085f;   // sun rays
constexpr float kPi = 3.14159265f;

struct Kit {
    ID2D1Factory1* f;
    ComPtr<ID2D1StrokeStyle> round;

    explicit Kit(ID2D1Factory1* factory) : f(factory) {
        const auto p = D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND,
                                                   D2D1_LINE_JOIN_ROUND);
        f->CreateStrokeStyle(p, nullptr, 0, &round);
    }

    G Circle(float cx, float cy, float r) {
        ComPtr<ID2D1EllipseGeometry> e;
        f->CreateEllipseGeometry(D2D1::Ellipse(D2D1::Point2F(cx, cy), r, r), &e);
        return e;
    }
    G Pill(float l, float t, float r, float b) {
        ComPtr<ID2D1RoundedRectangleGeometry> g;
        const float rad = (b - t) / 2.f;
        f->CreateRoundedRectangleGeometry(D2D1::RoundedRect(D2D1::RectF(l, t, r, b), rad, rad), &g);
        return g;
    }
    G Combine(const G& a, const G& b, D2D1_COMBINE_MODE mode) {
        if (!a || !b) return mode == D2D1_COMBINE_MODE_UNION ? (a ? a : b) : a;
        ComPtr<ID2D1PathGeometry> p;
        ComPtr<ID2D1GeometrySink> s;
        if (FAILED(f->CreatePathGeometry(&p)) || FAILED(p->Open(&s))) return a;
        a->CombineWithGeometry(b.Get(), mode, nullptr, kTol, s.Get());
        s->Close();
        return p;
    }
    G Union(const G& a, const G& b) { return Combine(a, b, D2D1_COMBINE_MODE_UNION); }
    G Minus(const G& a, const G& b) { return Combine(a, b, D2D1_COMBINE_MODE_EXCLUDE); }
    G Outline(const G& g, float width) {   // the area a round-capped stroke of g covers
        ComPtr<ID2D1PathGeometry> p;
        ComPtr<ID2D1GeometrySink> s;
        if (!g || FAILED(f->CreatePathGeometry(&p)) || FAILED(p->Open(&s))) return nullptr;
        g->Widen(width, round.Get(), nullptr, kTol, s.Get());
        s->Close();
        return p;
    }
    G Grow(const G& g, float by) { return Union(g, Outline(g, 2.f * by)); }
    // `back` with room cut around `front`, then `front` on top.
    G Behind(const G& back, const G& front) { return Union(Minus(back, Grow(front, kGap)), front); }

    // An open path, stroked: points joined by lines.
    G Polyline(std::initializer_list<D2D1_POINT_2F> pts, float width) {
        ComPtr<ID2D1PathGeometry> p;
        ComPtr<ID2D1GeometrySink> s;
        if (FAILED(f->CreatePathGeometry(&p)) || FAILED(p->Open(&s))) return nullptr;
        auto it = pts.begin();
        s->BeginFigure(*it, D2D1_FIGURE_BEGIN_HOLLOW);
        for (++it; it != pts.end(); ++it) s->AddLine(*it);
        s->EndFigure(D2D1_FIGURE_END_OPEN);
        s->Close();
        return Outline(p, width);
    }
    G Line(float x0, float y0, float x1, float y1, float width) {
        return Polyline({ D2D1::Point2F(x0, y0), D2D1::Point2F(x1, y1) }, width);
    }

    // A cloud of width 0.88*k, height 0.57*k with its top-left near (ox, oy).
    G Cloud(float ox, float oy, float k) {
        auto P = [&](float v) { return v * k; };
        G g = Circle(ox + P(0.52f), oy + P(0.30f), P(0.27f));
        g = Union(g, Circle(ox + P(0.26f), oy + P(0.42f), P(0.18f)));
        g = Union(g, Circle(ox + P(0.77f), oy + P(0.43f), P(0.16f)));
        g = Union(g, Pill(ox + P(0.08f), oy + P(0.32f), ox + P(0.93f), oy + P(0.60f)));
        return g;
    }
    // Sun: disc of radius r and 8 rays.
    G Sun(float cx, float cy, float r) {
        G g = Circle(cx, cy, r);
        for (int i = 0; i < 8; ++i) {
            const float a = i * kPi / 4.f;
            g = Union(g, Line(cx + 1.5f * r * std::cos(a), cy + 1.5f * r * std::sin(a),
                              cx + 1.95f * r * std::cos(a), cy + 1.95f * r * std::sin(a), kRay));
        }
        return g;
    }
    // Crescent moon of radius r, open towards the upper right.
    G Moon(float cx, float cy, float r) {
        return Minus(Circle(cx, cy, r), Circle(cx + 0.55f * r, cy - 0.4f * r, 0.82f * r));
    }
    // Moon in phase i (0 new .. 4 full .. 7), lit from the right while waxing.
    G MoonPhase(int i) {
        const float cx = 0.45f, cy = 0.5f, r = 0.36f;
        G disc = Circle(cx, cy, r);
        if (i == 0) return Outline(disc, 0.07f);   // new moon: just the rim
        if (i == 4) return disc;
        const bool waxing = i < 4;
        const float theta = i * kPi / 4.f;
        const float lit = (1.f - std::cos(theta)) / 2.f;   // illuminated fraction
        ComPtr<ID2D1RectangleGeometry> side;
        f->CreateRectangleGeometry(waxing ? D2D1::RectF(cx, cy - r - 0.1f, cx + r + 0.1f, cy + r + 0.1f)
                                          : D2D1::RectF(cx - r - 0.1f, cy - r - 0.1f, cx, cy + r + 0.1f), &side);
        G half = Combine(disc, side, D2D1_COMBINE_MODE_INTERSECT);
        // The terminator is half an ellipse. Crescents and gibbous moons use a fixed 0.5 r instead
        // of the true cos(45 deg) = 0.71 r, so a crescent is thick enough to read at icon size.
        const float rx = (i % 2) ? 0.5f * r : r * std::fabs(std::cos(theta));
        if (rx < 0.002f) return half;                       // quarter
        ComPtr<ID2D1EllipseGeometry> term;
        f->CreateEllipseGeometry(D2D1::Ellipse(D2D1::Point2F(cx, cy), rx, r), &term);
        return lit < 0.5f ? Minus(half, term) : Union(half, term);
    }
    // The cloud used above precipitation: top part of the box.
    G HighCloud(float ox = 0.05f) { return Cloud(ox, -0.01f, 0.95f); }
    G RainLines(float x0 = 0.38f) {
        G g;
        for (int i = 0; i < 3; ++i) {
            const float x = x0 + i * 0.22f;
            g = Union(g, Line(x, 0.68f, x - 0.08f, 0.94f, kStroke));
        }
        return g;
    }
};

}  // namespace

float Width(wchar_t c) {
    switch (c) {
    case Clear:             return 1.0f;
    case ClearNight:        return 0.8f;
    case MainlyClear:
    case MainlyClearNight:  return 1.04f;
    case PartlyCloudy:
    case PartlyCloudyNight: return 1.04f;
    case Overcast:          return 1.1f;
    case Wind:              return 1.0f;
    case Umbrella:          return 0.92f;
    case Music:             return 0.86f;
    default:
        if (c >= MoonPhase0 && c < MoonPhase0 + 8) return 0.9f;
        return 1.0f;   // cloud with something below it
    }
}

G Build(ID2D1Factory1* factory, wchar_t c) {
    Kit k(factory);
    switch (c) {
    case Clear:
        return k.Sun(0.5f, 0.5f, 0.22f);
    case ClearNight:
        return k.Moon(0.42f, 0.5f, 0.34f);
    case MainlyClear:        // a big sun, a small cloud in front of it
        return k.Behind(k.Sun(0.40f, 0.40f, 0.18f), k.Cloud(0.40f, 0.47f, 0.66f));
    case MainlyClearNight:
        return k.Behind(k.Moon(0.36f, 0.38f, 0.30f), k.Cloud(0.40f, 0.47f, 0.66f));
    case PartlyCloudy:       // a big cloud, the sun peeking out behind it
        return k.Behind(k.Sun(0.33f, 0.31f, 0.13f), k.Cloud(0.12f, 0.30f, 0.98f));
    case PartlyCloudyNight:
        return k.Behind(k.Moon(0.30f, 0.30f, 0.24f), k.Cloud(0.12f, 0.30f, 0.98f));
    case Overcast:           // two clouds
        return k.Behind(k.Cloud(0.30f, 0.10f, 0.82f), k.Cloud(0.0f, 0.30f, 0.98f));
    case Fog: {
        G g = k.Cloud(0.08f, 0.0f, 0.88f);
        g = k.Union(g, k.Line(0.10f, 0.68f, 0.92f, 0.68f, kStroke));
        return k.Union(g, k.Line(0.24f, 0.87f, 0.78f, 0.87f, kStroke));
    }
    case Drizzle: {          // short dashes
        G g = k.HighCloud();
        for (int i = 0; i < 3; ++i) { const float x = 0.34f + i * 0.24f; g = k.Union(g, k.Line(x, 0.70f, x - 0.035f, 0.79f, kStroke)); }
        for (int i = 0; i < 2; ++i) { const float x = 0.44f + i * 0.24f; g = k.Union(g, k.Line(x, 0.88f, x - 0.035f, 0.97f, kStroke)); }
        return g;
    }
    case Rain:               // long slanted lines
        return k.Union(k.HighCloud(), k.RainLines());
    case Showers:            // rain with the sun peeking out
        return k.Union(k.Behind(k.Sun(0.27f, 0.20f, 0.11f), k.HighCloud(0.12f)), k.RainLines(0.44f));
    case ShowersNight:
        return k.Union(k.Behind(k.Moon(0.26f, 0.20f, 0.19f), k.HighCloud(0.12f)), k.RainLines(0.44f));
    case Snow: {             // dots
        G g = k.HighCloud();
        for (int i = 0; i < 3; ++i) g = k.Union(g, k.Circle(0.30f + i * 0.23f, 0.74f, 0.058f));
        for (int i = 0; i < 2; ++i) g = k.Union(g, k.Circle(0.415f + i * 0.23f, 0.92f, 0.058f));
        return g;
    }
    case Thunder: {          // a bolt in front of the cloud
        ComPtr<ID2D1PathGeometry> bolt;
        ComPtr<ID2D1GeometrySink> s;
        G g = k.HighCloud();
        if (SUCCEEDED(factory->CreatePathGeometry(&bolt)) && SUCCEEDED(bolt->Open(&s))) {
            const D2D1_POINT_2F pts[] = { { 0.60f, 0.44f }, { 0.40f, 0.74f }, { 0.53f, 0.74f }, { 0.45f, 1.0f },
                                          { 0.72f, 0.64f }, { 0.58f, 0.64f }, { 0.69f, 0.44f } };
            s->BeginFigure(pts[0], D2D1_FIGURE_BEGIN_FILLED);
            for (size_t i = 1; i < ARRAYSIZE(pts); ++i) s->AddLine(pts[i]);
            s->EndFigure(D2D1_FIGURE_END_CLOSED);
            s->Close();
            g = k.Behind(g, bolt);
        }
        return g;
    }
    case Music: {            // two beamed eighth notes
        G g = k.Union(k.Circle(0.22f, 0.80f, 0.13f), k.Circle(0.66f, 0.72f, 0.13f));
        g = k.Union(g, k.Line(0.335f, 0.80f, 0.335f, 0.22f, kStroke));
        g = k.Union(g, k.Line(0.775f, 0.72f, 0.775f, 0.14f, kStroke));
        ComPtr<ID2D1PathGeometry> beam;
        ComPtr<ID2D1GeometrySink> s;
        if (SUCCEEDED(factory->CreatePathGeometry(&beam)) && SUCCEEDED(beam->Open(&s))) {
            const D2D1_POINT_2F pts[] = { { 0.29f, 0.14f }, { 0.82f, 0.04f }, { 0.82f, 0.20f }, { 0.29f, 0.30f } };
            s->BeginFigure(pts[0], D2D1_FIGURE_BEGIN_FILLED);
            for (size_t i = 1; i < ARRAYSIZE(pts); ++i) s->AddLine(pts[i]);
            s->EndFigure(D2D1_FIGURE_END_CLOSED);
            s->Close();
            g = k.Union(g, beam);
        }
        return g;
    }
    case Wind: {             // three gusts, the outer two curling
        ComPtr<ID2D1PathGeometry> p;
        ComPtr<ID2D1GeometrySink> s;
        if (FAILED(factory->CreatePathGeometry(&p)) || FAILED(p->Open(&s))) return nullptr;
        s->BeginFigure(D2D1::Point2F(0.06f, 0.36f), D2D1_FIGURE_BEGIN_HOLLOW);   // top: curls up
        s->AddLine(D2D1::Point2F(0.66f, 0.36f));
        s->AddArc(D2D1::ArcSegment(D2D1::Point2F(0.56f, 0.21f), D2D1::SizeF(0.12f, 0.12f), 0.f,
                                   D2D1_SWEEP_DIRECTION_COUNTER_CLOCKWISE, D2D1_ARC_SIZE_LARGE));
        s->EndFigure(D2D1_FIGURE_END_OPEN);
        s->BeginFigure(D2D1::Point2F(0.06f, 0.56f), D2D1_FIGURE_BEGIN_HOLLOW);   // middle: straight
        s->AddLine(D2D1::Point2F(0.90f, 0.56f));
        s->EndFigure(D2D1_FIGURE_END_OPEN);
        s->BeginFigure(D2D1::Point2F(0.06f, 0.76f), D2D1_FIGURE_BEGIN_HOLLOW);   // bottom: curls down
        s->AddLine(D2D1::Point2F(0.52f, 0.76f));
        s->AddArc(D2D1::ArcSegment(D2D1::Point2F(0.43f, 0.89f), D2D1::SizeF(0.10f, 0.10f), 0.f,
                                   D2D1_SWEEP_DIRECTION_CLOCKWISE, D2D1_ARC_SIZE_LARGE));
        s->EndFigure(D2D1_FIGURE_END_OPEN);
        s->Close();
        return k.Outline(p, kStroke);
    }
    case Umbrella: {         // canopy with a scalloped edge, and a hooked handle
        ComPtr<ID2D1PathGeometry> canopy;
        ComPtr<ID2D1GeometrySink> s;
        if (FAILED(factory->CreatePathGeometry(&canopy)) || FAILED(canopy->Open(&s))) return nullptr;
        const float l = 0.06f, r = 0.86f, y = 0.50f, R = (r - l) / 2.f, w = (r - l) / 3.f;
        s->BeginFigure(D2D1::Point2F(l, y), D2D1_FIGURE_BEGIN_FILLED);
        s->AddArc(D2D1::ArcSegment(D2D1::Point2F(r, y), D2D1::SizeF(R, R * 0.92f), 0.f, D2D1_SWEEP_DIRECTION_CLOCKWISE,
                                   D2D1_ARC_SIZE_SMALL));
        for (int i = 1; i <= 3; ++i)
            s->AddArc(D2D1::ArcSegment(D2D1::Point2F(r - i * w, y), D2D1::SizeF(w / 2.f, w * 0.32f), 0.f,
                                       D2D1_SWEEP_DIRECTION_COUNTER_CLOCKWISE, D2D1_ARC_SIZE_SMALL));
        s->EndFigure(D2D1_FIGURE_END_CLOSED);
        s->Close();
        const float cx = (l + r) / 2.f;
        ComPtr<ID2D1PathGeometry> handle;
        ComPtr<ID2D1GeometrySink> h;
        G g = canopy;
        if (SUCCEEDED(factory->CreatePathGeometry(&handle)) && SUCCEEDED(handle->Open(&h))) {
            h->BeginFigure(D2D1::Point2F(cx, 0.46f), D2D1_FIGURE_BEGIN_HOLLOW);
            h->AddLine(D2D1::Point2F(cx, 0.84f));
            h->AddArc(D2D1::ArcSegment(D2D1::Point2F(cx - 0.18f, 0.84f), D2D1::SizeF(0.09f, 0.09f), 0.f,
                                       D2D1_SWEEP_DIRECTION_CLOCKWISE, D2D1_ARC_SIZE_SMALL));
            h->EndFigure(D2D1_FIGURE_END_OPEN);
            h->Close();
            g = k.Union(g, k.Outline(handle, kStroke));
        }
        return g;
    }
    }
    if (c >= MoonPhase0 && c < MoonPhase0 + 8) return k.MoonPhase(c - MoonPhase0);
    return nullptr;
}

const wchar_t* Name(wchar_t c) {
    static const wchar_t* kPhases[] = { L"[new moon]", L"[waxing crescent]", L"[first quarter]", L"[waxing gibbous]",
                                        L"[full moon]", L"[waning gibbous]", L"[last quarter]", L"[waning crescent]" };
    if (c >= MoonPhase0 && c < MoonPhase0 + 8) return kPhases[c - MoonPhase0];
    switch (c) {
    case Clear: return L"[clear]";
    case ClearNight: return L"[clear night]";
    case MainlyClear: return L"[mainly clear]";
    case MainlyClearNight: return L"[mainly clear night]";
    case PartlyCloudy: return L"[partly cloudy]";
    case PartlyCloudyNight: return L"[partly cloudy night]";
    case Overcast: return L"[overcast]";
    case Fog: return L"[fog]";
    case Drizzle: return L"[drizzle]";
    case Rain: return L"[rain]";
    case Showers: return L"[showers]";
    case ShowersNight: return L"[showers night]";
    case Snow: return L"[snow]";
    case Thunder: return L"[thunder]";
    case Wind: return L"[wind]";
    case Umbrella: return L"[umbrella]";
    case Music: return L"[music]";
    }
    return L"[?]";
}

}  // namespace icon
