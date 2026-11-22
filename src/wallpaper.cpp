// Wallpaper lightness sampling for auto-contrast.
//
// Resolves the wallpaper shown on a monitor through IDesktopWallpaper, decodes a small
// (<= 512 px) copy with WIC, and maps the clock's screen region into image space using
// the wallpaper position mode. The decoded copy is cached until the wallpaper changes.
#include "wallpaper.h"
#include "settings.h"

#include <shobjidl.h>
#include <shlobj.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cwchar>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace {

struct ResolvedWallpaper {
    std::wstring path;                  // empty = solid colour only
    DESKTOP_WALLPAPER_POSITION pos = DWPOS_FILL;
    COLORREF bg = RGB(0, 0, 0);
    RECT monitor{};                     // monitor rect as IDesktopWallpaper reports it
    RECT virtualRect{};                 // union of all wallpaper monitors (for span / tile)
    FILETIME mtime{};
    ULONGLONG size = 0;
    const wchar_t* source = L"none";
};

struct Cache {
    bool valid = false;
    // key
    std::wstring path;
    FILETIME mtime{};
    ULONGLONG size = 0;
    DESKTOP_WALLPAPER_POSITION pos = DWPOS_FILL;
    RECT monitor{};
    COLORREF bg = 0;
    // data
    bool hasImage = false;
    UINT origW = 0, origH = 0;          // full image size
    UINT w = 0, h = 0;                  // decoded (downscaled) size
    std::vector<uint32_t> px;           // 32bppBGR, w*h
};

std::vector<Cache> g_caches;   // one per monitor (keyed by its rect)

bool SameRect(const RECT& a, const RECT& b) {
    return a.left == b.left && a.top == b.top && a.right == b.right && a.bottom == b.bottom;
}

LONG Overlap(const RECT& a, const RECT& b) {
    RECT r;
    if (!IntersectRect(&r, &a, &b)) return 0;
    return (r.right - r.left) * (r.bottom - r.top);
}

bool FileInfo(const std::wstring& path, FILETIME& mtime, ULONGLONG& size) {
    WIN32_FILE_ATTRIBUTE_DATA fa;
    if (path.empty() || !GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fa)) return false;
    if (fa.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) return false;
    mtime = fa.ftLastWriteTime;
    size = (ULONGLONG(fa.nFileSizeHigh) << 32) | fa.nFileSizeLow;
    return size > 0;
}

std::wstring ThemesDir() {
    PWSTR appdata = nullptr;
    std::wstring dir;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &appdata))) {
        dir = std::wstring(appdata) + L"\\Microsoft\\Windows\\Themes\\";
    }
    CoTaskMemFree(appdata);
    return dir;
}

bool Resolve(const RECT& monitorRect, ResolvedWallpaper& out) {
    ComPtr<IDesktopWallpaper> dw;
    // Fresh instance per call: a cached proxy would die with Explorer.
    HRESULT hr = CoCreateInstance(CLSID_DesktopWallpaper, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&dw));
    if (FAILED(hr)) {
        Log(L"wallpaper: IDesktopWallpaper unavailable (0x%08X)", (unsigned)hr);
        return false;
    }

    UINT count = 0;
    dw->GetMonitorDevicePathCount(&count);

    int best = -1;
    LONG bestOverlap = 0;
    bool exact = false;
    std::wstring bestId;
    RECT bestRect{};
    RECT virt{};
    bool haveVirt = false;

    for (UINT i = 0; i < count; ++i) {
        LPWSTR id = nullptr;
        if (FAILED(dw->GetMonitorDevicePathAt(i, &id)) || !id) continue;
        RECT r{};
        if (dw->GetMonitorRECT(id, &r) == S_OK) {
            if (!haveVirt) { virt = r; haveVirt = true; }
            else UnionRect(&virt, &virt, &r);
            if (!exact) {
                if (SameRect(r, monitorRect)) {
                    exact = true; best = (int)i; bestId = id; bestRect = r;
                } else {
                    LONG ov = Overlap(r, monitorRect);
                    if (ov > bestOverlap) { bestOverlap = ov; best = (int)i; bestId = id; bestRect = r; }
                }
            }
        }
        CoTaskMemFree(id);
    }
    if (best < 0) {
        Log(L"wallpaper: no IDesktopWallpaper monitor matches the target (%u monitors)", count);
        return false;
    }

    out.monitor = bestRect;
    out.virtualRect = virt;
    dw->GetPosition(&out.pos);
    dw->GetBackgroundColor(&out.bg);

    LPWSTR wp = nullptr;
    if (SUCCEEDED(dw->GetWallpaper(bestId.c_str(), &wp)) && wp) {
        out.path = wp;
    }
    CoTaskMemFree(wp);

    if (FileInfo(out.path, out.mtime, out.size)) {
        out.source = L"IDesktopWallpaper";
        return true;
    }

    // Fallbacks: Explorer's per-monitor transcoded copies, then the shared one.
    std::wstring themes = ThemesDir();
    if (!themes.empty()) {
        wchar_t name[32];
        swprintf_s(name, L"Transcoded_%03d", best);
        std::wstring p = themes + name;
        if (FileInfo(p, out.mtime, out.size)) {
            out.path = p; out.source = L"Transcoded_NNN";
            return true;
        }
        p = themes + L"TranscodedWallpaper";
        if (FileInfo(p, out.mtime, out.size)) {
            out.path = p; out.source = L"TranscodedWallpaper";
            return true;
        }
    }

    out.path.clear();
    out.source = L"solid colour";
    return true;
}

bool Decode(const std::wstring& path, Cache& c) {
    ComPtr<IWICImagingFactory> wic;
    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic));
    if (FAILED(hr)) return false;

    ComPtr<IWICBitmapDecoder> dec;
    hr = wic->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &dec);
    if (FAILED(hr)) { Log(L"wallpaper: cannot decode %s (0x%08X)", path.c_str(), (unsigned)hr); return false; }

    ComPtr<IWICBitmapFrameDecode> frame;
    if (FAILED(dec->GetFrame(0, &frame))) return false;

    UINT iw = 0, ih = 0;
    if (FAILED(frame->GetSize(&iw, &ih)) || !iw || !ih) return false;

    const UINT kMax = 512;
    UINT w = iw, h = ih;
    if (iw >= ih && iw > kMax) { w = kMax; h = (UINT)((double)ih * kMax / iw + 0.5); }
    else if (ih > iw && ih > kMax) { h = kMax; w = (UINT)((double)iw * kMax / ih + 0.5); }
    if (w < 1) w = 1;
    if (h < 1) h = 1;

    ComPtr<IWICBitmapSource> src = frame;
    if (w != iw || h != ih) {
        ComPtr<IWICBitmapScaler> scaler;
        if (FAILED(wic->CreateBitmapScaler(&scaler))) return false;
        if (FAILED(scaler->Initialize(frame.Get(), w, h, WICBitmapInterpolationModeFant))) return false;
        src = scaler;
    }

    ComPtr<IWICFormatConverter> conv;
    if (FAILED(wic->CreateFormatConverter(&conv))) return false;
    if (FAILED(conv->Initialize(src.Get(), GUID_WICPixelFormat32bppBGR, WICBitmapDitherTypeNone,
                                nullptr, 0.0, WICBitmapPaletteTypeCustom))) return false;

    std::vector<uint32_t> px((size_t)w * h);
    if (FAILED(conv->CopyPixels(nullptr, w * 4, (UINT)(px.size() * 4), reinterpret_cast<BYTE*>(px.data()))))
        return false;

    c.origW = iw; c.origH = ih;
    c.w = w; c.h = h;
    c.px.swap(px);
    return true;
}

double SrgbToLinear(int v) {
    double c = v / 255.0;
    return c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4);
}

struct Lut {
    double v[256];
    Lut() { for (int i = 0; i < 256; ++i) v[i] = SrgbToLinear(i); }
};
const Lut& LinLut() { static Lut lut; return lut; }

struct Lin { double r, g, b; };   // linear-light sRGB

Lin LinearBGR(uint32_t bgr) {
    const Lut& L = LinLut();
    return { L.v[(bgr >> 16) & 0xFF], L.v[(bgr >> 8) & 0xFF], L.v[bgr & 0xFF] };
}

Lin LinearColorref(COLORREF c) {
    const Lut& L = LinLut();
    return { L.v[GetRValue(c)], L.v[GetGValue(c)], L.v[GetBValue(c)] };
}

double Wrap(double v, double m) {
    double r = std::fmod(v, m);
    return r < 0 ? r + m : r;
}

// Linear colour of the wallpaper at screen point (sx, sy).
Lin SamplePixel(const Cache& c, const ResolvedWallpaper& rw, double sx, double sy, const Lin& bg) {
    if (!c.hasImage) return bg;

    const double iw = c.origW, ih = c.origH;
    const RECT& M = rw.monitor;
    double mw = double(M.right - M.left), mh = double(M.bottom - M.top);
    double rx = sx - M.left, ry = sy - M.top;
    double ix = 0, iy = 0;   // position in full-size image pixels

    switch (rw.pos) {
    case DWPOS_STRETCH:
        ix = rx * iw / mw; iy = ry * ih / mh;
        break;
    case DWPOS_FIT: {
        double s = (std::min)(mw / iw, mh / ih);
        ix = (rx - (mw - iw * s) / 2) / s;
        iy = (ry - (mh - ih * s) / 2) / s;
        break;
    }
    case DWPOS_CENTER:
        ix = rx - (mw - iw) / 2;
        iy = ry - (mh - ih) / 2;
        break;
    case DWPOS_TILE:
        ix = Wrap(sx - rw.virtualRect.left, iw);
        iy = Wrap(sy - rw.virtualRect.top, ih);
        break;
    case DWPOS_SPAN: {
        const RECT& V = rw.virtualRect;
        double vw = double(V.right - V.left), vh = double(V.bottom - V.top);
        double s = (std::max)(vw / iw, vh / ih);
        ix = (sx - V.left - (vw - iw * s) / 2) / s;
        iy = (sy - V.top - (vh - ih * s) / 2) / s;
        break;
    }
    case DWPOS_FILL:
    default: {
        double s = (std::max)(mw / iw, mh / ih);
        ix = (rx - (mw - iw * s) / 2) / s;
        iy = (ry - (mh - ih * s) / 2) / s;
        break;
    }
    }

    if (ix < 0 || iy < 0 || ix >= iw || iy >= ih) return bg;   // letterbox / outside a centred image
    UINT bx = (UINT)(ix * c.w / iw), by = (UINT)(iy * c.h / ih);
    if (bx >= c.w) bx = c.w - 1;
    if (by >= c.h) by = c.h - 1;
    return LinearBGR(c.px[(size_t)by * c.w + bx]);
}

const wchar_t* PosName(DESKTOP_WALLPAPER_POSITION p) {
    switch (p) {
    case DWPOS_CENTER: return L"center";
    case DWPOS_TILE: return L"tile";
    case DWPOS_STRETCH: return L"stretch";
    case DWPOS_FIT: return L"fit";
    case DWPOS_FILL: return L"fill";
    case DWPOS_SPAN: return L"span";
    default: return L"?";
    }
}

} // namespace

void InvalidateWallpaperCache() {
    g_caches.clear();
}

bool SampleWallpaper(const RECT& monitorRect, const RECT& region, WallpaperSample& out) {
    ResolvedWallpaper rw;
    if (!Resolve(monitorRect, rw)) return false;

    auto it = std::find_if(g_caches.begin(), g_caches.end(), [&](const Cache& k) { return SameRect(k.monitor, rw.monitor); });
    if (it == g_caches.end()) { g_caches.emplace_back(); it = g_caches.end() - 1; }
    Cache& c = *it;
    bool keyMatch = c.valid && c.path == rw.path && c.size == rw.size &&
                    CompareFileTime(&c.mtime, &rw.mtime) == 0 && c.pos == rw.pos &&
                    SameRect(c.monitor, rw.monitor) && c.bg == rw.bg;

    if (!keyMatch) {
        Cache n;
        n.path = rw.path; n.mtime = rw.mtime; n.size = rw.size;
        n.pos = rw.pos; n.monitor = rw.monitor; n.bg = rw.bg;
        if (!rw.path.empty()) {
            LARGE_INTEGER f, t0, t1;
            QueryPerformanceFrequency(&f);
            QueryPerformanceCounter(&t0);
            n.hasImage = Decode(rw.path, n);
            QueryPerformanceCounter(&t1);
            Log(L"wallpaper: %s via %s, %ux%u -> %ux%u, mode=%s, decoded in %.1f ms",
                rw.path.c_str(), rw.source, n.origW, n.origH, n.w, n.h, PosName(rw.pos),
                double(t1.QuadPart - t0.QuadPart) * 1000.0 / double(f.QuadPart));
        } else {
            Log(L"wallpaper: no image, solid colour #%02X%02X%02X",
                GetRValue(rw.bg), GetGValue(rw.bg), GetBValue(rw.bg));
        }
        n.valid = true;
        c = std::move(n);
    }

    const int kCols = 48, kRows = 16;
    double rwid = double(region.right - region.left), rhei = double(region.bottom - region.top);
    if (rwid <= 0 || rhei <= 0) return false;

    const Lin bg = LinearColorref(rw.bg);
    Lin sum{ 0, 0, 0 };
    for (int y = 0; y < kRows; ++y) {
        double sy = region.top + (y + 0.5) * rhei / kRows;
        for (int x = 0; x < kCols; ++x) {
            double sx = region.left + (x + 0.5) * rwid / kCols;
            Lin p = SamplePixel(c, rw, sx, sy, bg);
            sum.r += p.r; sum.g += p.g; sum.b += p.b;
        }
    }
    const double n = kCols * kRows;
    out.r = sum.r / n; out.g = sum.g / n; out.b = sum.b / n;
    const double Y = 0.2126 * out.r + 0.7152 * out.g + 0.0722 * out.b;
    out.lightness = Y > 0.008856 ? 116.0 * std::cbrt(Y) - 16.0 : 903.3 * Y;
    return true;
}

bool WallpaperFile(const RECT& monitorRect, std::wstring& path, int& position, COLORREF& bg) {
    ResolvedWallpaper rw;
    if (!Resolve(monitorRect, rw)) return false;
    path = rw.path;
    position = (int)rw.pos;
    bg = rw.bg;
    return true;
}
