#include "gfx.h"

#include <shellscalingapi.h>

using namespace Gdiplus;

namespace gfx {

void AddRoundRect(GraphicsPath& path, const RectF& r, float radius) {
    float d = std::min({radius * 2, r.Width, r.Height});
    if (d <= 0.5f) {
        path.AddRectangle(r);
        return;
    }
    path.AddArc(r.X, r.Y, d, d, 180, 90);
    path.AddArc(r.X + r.Width - d, r.Y, d, d, 270, 90);
    path.AddArc(r.X + r.Width - d, r.Y + r.Height - d, d, d, 0, 90);
    path.AddArc(r.X, r.Y + r.Height - d, d, d, 90, 90);
    path.CloseFigure();
}

static const FontFamily* PickFamily(std::initializer_list<const wchar_t*> names) {
    for (auto n : names) {
        auto* f = new FontFamily(n);  // intentionally leaked: must outlive GdiplusShutdown ordering issues
        if (f->GetLastStatus() == Ok && f->IsAvailable()) return f;
        delete f;
    }
    return FontFamily::GenericSansSerif();
}

const FontFamily* TextFamily() {
    static const FontFamily* f = PickFamily({L"Segoe UI Variable Display", L"Segoe UI"});
    return f;
}

const FontFamily* MonoFamily() {
    static const FontFamily* f = PickFamily({L"Cascadia Mono", L"Consolas", L"Courier New"});
    return f;
}

// Icon font embedded as RCDATA 200 (res/icons.ttf). Null if it cannot be loaded.
static const FontFamily* EmbeddedIconFamily() {
    HRSRC res = FindResourceW(nullptr, MAKEINTRESOURCEW(200), RT_RCDATA);
    HGLOBAL data = res ? LoadResource(nullptr, res) : nullptr;
    const void* bytes = data ? LockResource(data) : nullptr;
    if (!bytes) return nullptr;
    // Intentionally leaked, like PickFamily's families: they must outlive every Font built from them.
    auto* fonts = new PrivateFontCollection();
    if (fonts->AddMemoryFont(bytes, (INT)SizeofResource(nullptr, res)) != Ok || fonts->GetFamilyCount() < 1) return nullptr;
    auto* family = new FontFamily();
    INT found = 0;
    if (fonts->GetFamilies(1, family, &found) != Ok || found < 1) return nullptr;
    return family;
}

const FontFamily* IconFamily() {
    static const FontFamily* f = [] {
        const FontFamily* embedded = EmbeddedIconFamily();
        return embedded ? embedded : PickFamily({L"Segoe Fluent Icons", L"Segoe MDL2 Assets"});
    }();
    return f;
}

void DrawCentered(Graphics& g, const std::wstring& text, const Font& font, const Brush& brush, const RectF& r) {
    StringFormat fmt(StringFormatFlagsNoWrap);
    fmt.SetAlignment(StringAlignmentCenter);
    fmt.SetLineAlignment(StringAlignmentCenter);
    fmt.SetTrimming(StringTrimmingEllipsisCharacter);
    g.DrawString(text.c_str(), (INT)text.size(), &font, r, &fmt, &brush);
}

void DrawGlyph(Graphics& g, wchar_t glyph, float sizePx, const Color& color, const RectF& box, float weight) {
    GraphicsPath path;
    StringFormat fmt(StringFormat::GenericTypographic());
    path.AddString(&glyph, 1, IconFamily(), FontStyleRegular, sizePx, PointF(0, 0), &fmt);
    RectF bounds;
    path.GetBounds(&bounds);
    if (bounds.Width <= 0 || bounds.Height <= 0) return;
    // Centre the ink itself (icon fonts sit oddly in their em box) on a whole pixel.
    Matrix m;
    m.Translate(std::round(box.X + (box.Width - bounds.Width) / 2 - bounds.X),
                std::round(box.Y + (box.Height - bounds.Height) / 2 - bounds.Y));
    path.Transform(&m);
    SmoothingMode oldSmooth = g.GetSmoothingMode();
    PixelOffsetMode oldOffset = g.GetPixelOffsetMode();
    // SmoothingModeAntiAlias8x8 (GDI+ 1.1); older headers do not declare it.
    if (g.SetSmoothingMode(static_cast<SmoothingMode>(6)) != Ok) g.SetSmoothingMode(SmoothingModeAntiAlias);
    g.SetPixelOffsetMode(PixelOffsetModeHighQuality);
    SolidBrush brush(color);
    g.FillPath(&brush, &path);
    if (weight > 0) {
        Pen pen(color, weight);
        pen.SetLineJoin(LineJoinRound);
        g.DrawPath(&pen, &path);
    }
    g.SetSmoothingMode(oldSmooth);
    g.SetPixelOffsetMode(oldOffset);
}

float TextWidth(Graphics& g, const std::wstring& text, const Font& font) {
    RectF bounds;
    StringFormat fmt(StringFormat::GenericTypographic());
    fmt.SetFormatFlags(fmt.GetFormatFlags() | StringFormatFlagsMeasureTrailingSpaces);
    g.MeasureString(text.c_str(), (INT)text.size(), &font, PointF(0, 0), &fmt, &bounds);
    return bounds.Width;
}

std::unique_ptr<Bitmap> BitmapFromHBitmap(HBITMAP hbmp) {
    BITMAP bm{};
    if (!GetObjectW(hbmp, sizeof(bm), &bm)) return nullptr;
    int w = bm.bmWidth, h = std::abs(bm.bmHeight);
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    auto bmp = std::make_unique<Bitmap>(w, h, PixelFormat32bppPARGB);
    BitmapData data;
    Rect rect(0, 0, w, h);
    if (bmp->LockBits(&rect, ImageLockModeWrite, PixelFormat32bppPARGB, &data) != Ok) return nullptr;
    HDC dc = GetDC(nullptr);
    GetDIBits(dc, hbmp, 0, h, data.Scan0, &bi, DIB_RGB_COLORS);
    ReleaseDC(nullptr, dc);
    // Shell thumbnails without alpha come back with A=0 everywhere: make them opaque.
    auto* px = static_cast<uint8_t*>(data.Scan0);
    bool anyAlpha = false;
    for (int i = 0; i < w * h && !anyAlpha; ++i) anyAlpha = px[i * 4 + 3] != 0;
    if (!anyAlpha)
        for (int i = 0; i < w * h; ++i) px[i * 4 + 3] = 255;
    bmp->UnlockBits(&data);
    return bmp;
}

RectF DrawFitted(Graphics& g, Gdiplus::Image& img, const RectF& box) {
    float iw = (float)img.GetWidth(), ih = (float)img.GetHeight();
    if (iw <= 0 || ih <= 0) return box;
    float s = std::min(box.Width / iw, box.Height / ih);
    RectF r(box.X + (box.Width - iw * s) / 2, box.Y + (box.Height - ih * s) / 2, iw * s, ih * s);
    g.DrawImage(&img, r);
    return r;
}

float DpiScaleAt(POINT pt) {
    HMONITOR mon = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
    UINT dx = 96, dy = 96;
    if (FAILED(GetDpiForMonitor(mon, MDT_EFFECTIVE_DPI, &dx, &dy))) dx = 96;
    return dx / 96.0f;
}

LayeredSurface::~LayeredSurface() { Release(); }

void LayeredSurface::Release() {
    graphics_.reset();
    canvas_.reset();
    if (dc_) {
        SelectObject(dc_, old_);
        DeleteDC(dc_);
        dc_ = nullptr;
    }
    if (bitmap_) {
        DeleteObject(bitmap_);
        bitmap_ = nullptr;
    }
    w_ = h_ = 0;
}

Graphics& LayeredSurface::Begin(int width, int height) {
    if (width != w_ || height != h_ || !dc_) {
        Release();
        BITMAPINFO bi{};
        bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
        bi.bmiHeader.biWidth = width;
        bi.bmiHeader.biHeight = -height;
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        dc_ = CreateCompatibleDC(nullptr);
        bitmap_ = CreateDIBSection(dc_, &bi, DIB_RGB_COLORS, &bits_, nullptr, 0);
        old_ = SelectObject(dc_, bitmap_);
        w_ = width;
        h_ = height;
        canvas_ = std::make_unique<Bitmap>(width, height, width * 4, PixelFormat32bppPARGB, static_cast<BYTE*>(bits_));
        graphics_ = std::make_unique<Graphics>(canvas_.get());
    }
    Graphics& g = *graphics_;
    g.ResetTransform();
    g.ResetClip();
    g.SetCompositingMode(CompositingModeSourceCopy);
    g.Clear(Color(0, 0, 0, 0));
    g.SetCompositingMode(CompositingModeSourceOver);
    g.SetCompositingQuality(CompositingQualityHighQuality);
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    g.SetPixelOffsetMode(PixelOffsetModeHalf);
    g.SetInterpolationMode(InterpolationModeHighQualityBicubic);
    g.SetTextRenderingHint(TextRenderingHintAntiAlias);
    return g;
}

void LayeredSurface::Present(HWND hwnd, POINT screenPos, BYTE alpha) {
    if (!dc_) return;
    graphics_->Flush(FlushIntentionSync);
    GdiFlush();
    SIZE size{w_, h_};
    POINT src{0, 0};
    BLENDFUNCTION blend{AC_SRC_OVER, 0, alpha, AC_SRC_ALPHA};
    UpdateLayeredWindow(hwnd, nullptr, &screenPos, &size, dc_, &src, 0, &blend, ULW_ALPHA);
}

}  // namespace gfx

namespace gfx {
bool LayeredSurface::SavePng(const fs::path& path) {
    if (!canvas_) return false;
    graphics_->Flush(Gdiplus::FlushIntentionSync);
    UINT num = 0, size = 0;
    Gdiplus::GetImageEncodersSize(&num, &size);
    std::vector<BYTE> buf(size);
    auto* codecs = reinterpret_cast<Gdiplus::ImageCodecInfo*>(buf.data());
    Gdiplus::GetImageEncoders(num, size, codecs);
    for (UINT i = 0; i < num; ++i)
        if (!wcscmp(codecs[i].MimeType, L"image/png")) return canvas_->Save(path.c_str(), &codecs[i].Clsid) == Gdiplus::Ok;
    return false;
}
}  // namespace gfx
