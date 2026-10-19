#include "glass.h"

#include <dwmapi.h>
#include <uxtheme.h>

#include <cmath>

using namespace Gdiplus;

namespace glass {

namespace {
// DWM attributes newer than some SDK headers.
constexpr DWORD kDwmDarkMode = 20, kDwmCorners = 33, kDwmBackdrop = 38;
constexpr int kCornersRound = 2, kBackdropMica = 2;

bool HasFamily(IDWriteFactory* dw, const wchar_t* name) {
    Microsoft::WRL::ComPtr<IDWriteFontCollection> fonts;
    UINT32 index = 0;
    BOOL exists = FALSE;
    return SUCCEEDED(dw->GetSystemFontCollection(&fonts)) && SUCCEEDED(fonts->FindFamilyName(name, &index, &exists)) &&
           exists;
}
}  // namespace

Color Accent(BYTE alpha) { return Color(alpha, kAccent1[0], kAccent1[1], kAccent1[2]); }

void ForceForeground(HWND hwnd) {
    HWND fg = GetForegroundWindow();
    DWORD fgThread = fg ? GetWindowThreadProcessId(fg, nullptr) : 0, me = GetCurrentThreadId();
    bool attached = fgThread && fgThread != me && AttachThreadInput(me, fgThread, TRUE);
    SetForegroundWindow(hwnd);
    BringWindowToTop(hwnd);
    if (attached) AttachThreadInput(me, fgThread, FALSE);
}

bool ApplyBackdrop(HWND hwnd) {
    BOOL dark = FALSE;
    DwmSetWindowAttribute(hwnd, kDwmDarkMode, &dark, sizeof(dark));
    int corners = kCornersRound;
    DwmSetWindowAttribute(hwnd, kDwmCorners, &corners, sizeof(corners));
    int backdrop = kBackdropMica;
    if (FAILED(DwmSetWindowAttribute(hwnd, kDwmBackdrop, &backdrop, sizeof(backdrop)))) return false;
    MARGINS all{-1, -1, -1, -1};
    DwmExtendFrameIntoClientArea(hwnd, &all);
    return true;
}

void StrokeInside(Graphics& g, const RectF& r, float radius, float inset, const Color& color, float width) {
    // Centre the stroke inside the edge so a whole-pixel rectangle gets a whole-pixel line, not a blurred 2 px one.
    float d = inset + width / 2;
    if (r.Width <= 2 * d || r.Height <= 2 * d) return;
    GraphicsPath p;
    gfx::AddRoundRect(p, RectF(r.X + d, r.Y + d, r.Width - 2 * d, r.Height - 2 * d), std::max(0.0f, radius - d));
    Pen pen(color, width);
    g.DrawPath(&pen, &p);
}

void SetQuality(Graphics& g) {
    // SmoothingModeAntiAlias8x8 (GDI+ 1.1): smoother curves than the default 4x4; older headers do not declare it.
    if (g.SetSmoothingMode(static_cast<SmoothingMode>(6)) != Ok) g.SetSmoothingMode(SmoothingModeAntiAlias);
    g.SetPixelOffsetMode(PixelOffsetModeHalf);
    g.SetInterpolationMode(InterpolationModeHighQualityBicubic);
}

void FillGlass(Graphics& g, const RectF& r, float radius, BYTE top, BYTE bottom, BYTE rim, float s, BYTE edge) {
    if (r.Width <= 0 || r.Height <= 0) return;
    GraphicsPath p;
    gfx::AddRoundRect(p, r, radius);
    LinearGradientBrush b(PointF(0, r.Y - 1), PointF(0, r.Y + r.Height + 1), Color(top, 255, 255, 255),
                          Color(bottom, 244, 246, 250));
    g.FillPath(&b, &p);
    float line = std::round(std::max(1.0f, s));
    if (edge) StrokeInside(g, r, radius, 0, Color(edge, 20, 30, 50), line);
    StrokeInside(g, r, radius, line, Color(rim, 255, 255, 255), line);
}

void Shadow(Graphics& g, const RectF& r, float radius, float s) {
    for (int i = 4; i >= 1; --i) {
        GraphicsPath p;
        float grow = i * 1.6f * s;
        gfx::AddRoundRect(p, RectF(r.X - grow, r.Y - grow + 2 * s, r.Width + 2 * grow, r.Height + 2 * grow), radius + grow);
        SolidBrush b(Color(7, 10, 20, 40));
        g.FillPath(&b, &p);
    }
}

void FillAccent(Graphics& g, const RectF& r, float radius, float s, BYTE alpha) {
    if (r.Width <= 0 || r.Height <= 0) return;
    GraphicsPath p;
    gfx::AddRoundRect(p, r, radius);
    LinearGradientBrush b(PointF(r.X - 1, 0), PointF(r.X + r.Width + 1, 0),
                          Color(alpha, kAccent1[0], kAccent1[1], kAccent1[2]),
                          Color(alpha, kAccent2[0], kAccent2[1], kAccent2[2]));
    g.FillPath(&b, &p);
    // Light catching the top of the glass, fading out smoothly towards the bottom. The gradient spans the whole
    // shape: GDI+ tiles gradients past their end, so a half-height one repeats and leaves a seam in the middle.
    LinearGradientBrush sheen(PointF(0, r.Y - 1), PointF(0, r.Y + r.Height + 1), Color(46, 255, 255, 255),
                              Color(0, 255, 255, 255));
    REAL factors[] = {0.0f, 0.55f, 1.0f}, positions[] = {0.0f, 0.5f, 1.0f};
    sheen.SetBlend(factors, positions, 3);
    g.FillPath(&sheen, &p);
    StrokeInside(g, r, radius, 0, Color(110, 190, 225, 255), std::round(std::max(1.0f, s)));
}

void FillBackdrop(Graphics& g, float w, float h, bool translucent) {
    if (translucent) {
        LinearGradientBrush wash(PointF(0, 0), PointF(w, h), Color(150, 255, 255, 255), Color(95, 246, 249, 255));
        g.FillRectangle(&wash, 0.0f, 0.0f, w, h);
    } else {
        LinearGradientBrush wash(PointF(0, 0), PointF(0, h), Color(255, 248, 248, 251), Color(255, 234, 235, 241));
        g.FillRectangle(&wash, 0.0f, 0.0f, w, h);
    }
}

TextEngine& Engine() {
    static TextEngine e = [] {
        TextEngine t;
        D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, t.d2d.GetAddressOf());
        DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                            reinterpret_cast<IUnknown**>(t.dw.GetAddressOf()));
        if (t.dw) {
            // Grayscale antialiasing (the surface is translucent, so no ClearType) with the smooth, symmetric
            // rendering of modern Windows apps; extra contrast keeps small dark text from looking thin.
            t.dw->CreateCustomRenderingParams(1.8f, 1.5f, 0.0f, DWRITE_PIXEL_GEOMETRY_FLAT,
                                              DWRITE_RENDERING_MODE_NATURAL_SYMMETRIC, &t.params);
            if (HasFamily(t.dw.Get(), L"Segoe UI Variable Text")) t.textFamily = L"Segoe UI Variable Text";
            t.displayFamily = HasFamily(t.dw.Get(), L"Segoe UI Variable Display") ? L"Segoe UI Variable Display"
                                                                                   : t.textFamily;
            if (HasFamily(t.dw.Get(), L"Cascadia Mono")) t.monoFamily = L"Cascadia Mono";
        }
        return t;
    }();
    return e;
}

D2D1_COLOR_F ToD2D(const Color& c) {
    return D2D1::ColorF(c.GetR() / 255.0f, c.GetG() / 255.0f, c.GetB() / 255.0f, c.GetA() / 255.0f);
}

RectF Snap(const RectF& r) {
    return RectF(std::round(r.X), std::round(r.Y), std::round(r.Width), std::round(r.Height));
}

void TextQueue::SetFont(int index, Family family, float px, DWRITE_FONT_WEIGHT weight) {
    auto& e = Engine();
    if (index >= (int)formats_.size()) formats_.resize(index + 1);
    formats_[index].Reset();
    if (!e.dw) return;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> fmt;
    // Whole-pixel sizes render crisper.
    const std::wstring& name = family == Family::Mono      ? e.monoFamily
                               : family == Family::Display ? e.displayFamily
                                                           : e.textFamily;
    if (FAILED(e.dw->CreateTextFormat(name.c_str(), nullptr, weight,
                                      DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, std::round(px), L"es-es",
                                      &fmt)))
        return;
    Microsoft::WRL::ComPtr<IDWriteInlineObject> ellipsis;
    e.dw->CreateEllipsisTrimmingSign(fmt.Get(), &ellipsis);
    DWRITE_TRIMMING trimming{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
    fmt->SetTrimming(&trimming, ellipsis.Get());
    formats_[index] = fmt;
}

Microsoft::WRL::ComPtr<IDWriteTextLayout> TextQueue::Layout(const std::wstring& text, int font, float w, float h,
                                                            bool wrap) const {
    Microsoft::WRL::ComPtr<IDWriteTextLayout> layout;
    if (!Engine().dw || font < 0 || font >= (int)formats_.size() || !formats_[font] ||
        FAILED(Engine().dw->CreateTextLayout(text.c_str(), (UINT32)text.size(), formats_[font].Get(), w, h, &layout)))
        return nullptr;
    layout->SetWordWrapping(wrap ? DWRITE_WORD_WRAPPING_WRAP : DWRITE_WORD_WRAPPING_NO_WRAP);
    return layout;
}

float TextQueue::Measure(const std::wstring& text, int font) const {
    auto layout = Layout(text, font, 1e6f, 1e4f, false);
    DWRITE_TEXT_METRICS m{};
    if (!layout || FAILED(layout->GetMetrics(&m))) return 0;
    return m.widthIncludingTrailingWhitespace;
}

void TextQueue::Add(const std::wstring& text, int font, const Color& color, const RectF& r, DWRITE_TEXT_ALIGNMENT align,
                    bool wrap) {
    if (!text.empty() && r.Width > 0 && r.Height > 0) ops_.push_back({text, font, color, r, align, wrap});
}

void TextQueue::Render(HDC dc, const RECT& rc) {
    auto& e = Engine();
    std::vector<Op> ops;
    ops.swap(ops_);
    if (!e.d2d || !e.dw) return;
    if (!target_) {
        auto props = D2D1::RenderTargetProperties(
            D2D1_RENDER_TARGET_TYPE_SOFTWARE, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
            96, 96);
        if (FAILED(e.d2d->CreateDCRenderTarget(&props, &target_))) return;
    }
    if (FAILED(target_->BindDC(dc, &rc))) return;
    target_->BeginDraw();
    target_->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    if (e.params) target_->SetTextRenderingParams(e.params.Get());
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brush;
    target_->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0, 1), &brush);
    for (const auto& op : ops) {
        auto layout = Layout(op.text, op.font, op.r.Width, op.r.Height, op.wrap);
        if (!layout || !brush) continue;
        layout->SetTextAlignment(op.align);
        DWRITE_TEXT_METRICS m{};
        layout->GetMetrics(&m);
        // Whole-pixel origin: a fractional one makes the text blurry.
        D2D1_POINT_2F origin = D2D1::Point2F(std::round(op.r.X), std::round(op.r.Y + (op.r.Height - m.height) / 2));
        brush->SetColor(ToD2D(op.color));
        target_->DrawTextLayout(origin, layout.Get(), brush.Get());
    }
    if (target_->EndDraw() == D2DERR_RECREATE_TARGET) target_.Reset();
}

void PaintBuffered(HWND hwnd, TextQueue& text, const std::function<void(Graphics&, float, float)>& draw) {
    PAINTSTRUCT ps;
    HDC hdc = BeginPaint(hwnd, &ps);
    RECT rc;
    GetClientRect(hwnd, &rc);
    BP_PAINTPARAMS params{sizeof(params)};
    HDC mem = nullptr;
    HPAINTBUFFER pb = BeginBufferedPaint(hdc, &rc, BPBF_TOPDOWNDIB, &params, &mem);
    if (pb) {
        RGBQUAD* bits = nullptr;
        int rowPixels = 0;
        if (SUCCEEDED(GetBufferedPaintBits(pb, &bits, &rowPixels)) && rc.right > 0 && rc.bottom > 0) {
            for (int y = 0; y < rc.bottom; ++y) memset(bits + size_t(y) * rowPixels, 0, size_t(rc.right) * 4);
            {
                Bitmap canvas(rc.right, rc.bottom, rowPixels * 4, PixelFormat32bppPARGB, reinterpret_cast<BYTE*>(bits));
                Graphics g(&canvas);
                SetQuality(g);
                draw(g, float(rc.right), float(rc.bottom));
            }  // GDI+ flushes here; the text goes on top
            text.Render(mem, rc);
        }
        EndBufferedPaint(pb, TRUE);
    }
    EndPaint(hwnd, &ps);
}

void DrawButton(Graphics& g, TextQueue& text, int font, const RectF& r, const std::wstring& label, bool primary,
                bool enabled, bool hot, bool down, float s) {
    hot = hot && enabled;
    down = down && hot;
    float radius = r.Height / 2;
    if (primary) {
        FillAccent(g, r, radius, s, BYTE(!enabled ? 120 : down ? 255 : hot ? 250 : 225));
    } else {
        FillGlass(g, r, radius, down ? 150 : 225, down ? 130 : 195, 240, s, hot ? 70 : 34);
        if (hot) {
            GraphicsPath p;
            gfx::AddRoundRect(p, r, radius);
            SolidBrush tint(Accent(down ? 46 : 26));
            g.FillPath(&tint, &p);
        }
    }
    Color c = primary ? kWhite : kInk;
    if (!enabled) c = Color(120, c.GetR(), c.GetG(), c.GetB());
    text.Add(label, font, c, RectF(r.X + 8 * s, r.Y, r.Width - 16 * s, r.Height), DWRITE_TEXT_ALIGNMENT_CENTER);
}

}  // namespace glass
