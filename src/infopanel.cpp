#include "infopanel.h"

#include "fileinfo.h"
#include "gfx.h"
#include "shellutil.h"

#include <d2d1.h>
#include <dwmapi.h>
#include <dwrite.h>
#include <shellscalingapi.h>
#include <shobjidl.h>
#include <uxtheme.h>
#include <windowsx.h>
#include <wrl/client.h>
#include <unknwn.h>
#include <winrt/base.h>

#include <algorithm>
#include <cmath>
#include <cwctype>
#include <optional>
#include <set>
#include <unordered_map>
#include <thread>

using Microsoft::WRL::ComPtr;
using namespace Gdiplus;

// The window is drawn entirely by hand so it can share the wheel's glass look: on Windows 11 the
// client area sits on the system Mica backdrop and every element is painted with alpha on top
// (shapes with GDI+, text with DirectWrite, which is much sharper). Without Mica (Windows 10) an
// opaque frost is used.

namespace {

constexpr wchar_t kClass[] = L"FileTools.Info";
constexpr UINT WM_STRIP_DONE = WM_APP + 1;
constexpr UINT WM_SHEET_READY = WM_APP + 2;
constexpr UINT_PTR TIMER_CARET = 1;
enum : int { BTN_NONE = 0, BTN_STRIP, BTN_COPY, BTN_COPYALL, BTN_CLOSE, BTN_EXIFTOOL };
enum class Tone { Normal, Good, Bad };
enum class Focus { List, Filter };

// Palette: dark ink on frosted white, Windows blue as the accent.
const Color kInk(255, 22, 24, 30);
const Color kLabel(255, 70, 74, 86);   // property names
const Color kMuted(255, 96, 100, 112);
const Color kGood(255, 16, 124, 16);
const Color kBad(255, 196, 43, 28);
const Color kAccentInk(255, 0, 90, 170);  // group titles
const Color kWhite(255, 255, 255, 255);
// Accent gradient, left to right.
constexpr BYTE kAccent1[3] = {28, 132, 230}, kAccent2[3] = {0, 96, 196};

// DWM attributes newer than some SDK headers.
constexpr DWORD kDwmDarkMode = 20, kDwmCorners = 33, kDwmBackdrop = 38;
constexpr int kCornersRound = 2, kBackdropMica = 2;

class Panel;
std::vector<std::unique_ptr<Panel>> g_panels;

std::wstring Trim(const std::wstring& s) {
    size_t a = s.find_first_not_of(L" \t\r\n");
    if (a == std::wstring::npos) return {};
    return s.substr(a, s.find_last_not_of(L" \t\r\n") - a + 1);
}

bool Contains(const std::wstring& haystack, const std::wstring& lowerNeedle) {
    return ToLower(haystack).find(lowerNeedle) != std::wstring::npos;
}

// Brings the window to the front even though the drag left another app in the foreground.
void ForceForeground(HWND hwnd) {
    HWND fg = GetForegroundWindow();
    DWORD fgThread = fg ? GetWindowThreadProcessId(fg, nullptr) : 0, me = GetCurrentThreadId();
    bool attached = fgThread && fgThread != me && AttachThreadInput(me, fgThread, TRUE);
    SetForegroundWindow(hwnd);
    BringWindowToTop(hwnd);
    if (attached) AttachThreadInput(me, fgThread, FALSE);
}

// Frosted pane: translucent white, lighter at the top. A faint dark edge keeps it visible on
// light backgrounds and a bright inner rim gives the glass bevel.
void FillGlass(Graphics& g, const RectF& r, float radius, BYTE top, BYTE bottom, BYTE rim, float s, BYTE edge = 26) {
    if (r.Width <= 0 || r.Height <= 0) return;
    GraphicsPath p;
    gfx::AddRoundRect(p, r, radius);
    LinearGradientBrush b(PointF(0, r.Y - 1), PointF(0, r.Y + r.Height + 1), Color(top, 255, 255, 255),
                          Color(bottom, 244, 246, 250));
    g.FillPath(&b, &p);
    Pen outer(Color(edge, 20, 30, 50), 1.0f * s);
    g.DrawPath(&outer, &p);
    GraphicsPath inner;
    gfx::AddRoundRect(inner, RectF(r.X + s, r.Y + s, r.Width - 2 * s, r.Height - 2 * s), std::max(0.0f, radius - s));
    Pen pen(Color(rim, 255, 255, 255), 1.0f * s);
    g.DrawPath(&pen, &inner);
}

// Soft drop shadow under a pane.
void Shadow(Graphics& g, const RectF& r, float radius, float s) {
    for (int i = 4; i >= 1; --i) {
        GraphicsPath p;
        float grow = i * 1.6f * s;
        gfx::AddRoundRect(p, RectF(r.X - grow, r.Y - grow + 2 * s, r.Width + 2 * grow, r.Height + 2 * grow), radius + grow);
        SolidBrush b(Color(7, 10, 20, 40));
        g.FillPath(&b, &p);
    }
}

// Selection and primary button: blue glass.
void FillAccent(Graphics& g, const RectF& r, float radius, float s, BYTE alpha = 240) {
    if (r.Width <= 0 || r.Height <= 0) return;
    GraphicsPath p;
    gfx::AddRoundRect(p, r, radius);
    LinearGradientBrush b(PointF(r.X - 1, 0), PointF(r.X + r.Width + 1, 0),
                          Color(alpha, kAccent1[0], kAccent1[1], kAccent1[2]),
                          Color(alpha, kAccent2[0], kAccent2[1], kAccent2[2]));
    g.FillPath(&b, &p);
    // Light catching the top half of the glass.
    LinearGradientBrush sheen(PointF(0, r.Y - 1), PointF(0, r.Y + r.Height / 2 + 1), Color(60, 255, 255, 255),
                              Color(0, 255, 255, 255));
    g.FillPath(&sheen, &p);
    Pen rim(Color(110, 190, 225, 255), 1.0f * s);
    g.DrawPath(&rim, &p);
}

Color Accent(BYTE alpha) { return Color(alpha, kAccent1[0], kAccent1[1], kAccent1[2]); }

enum class FontKind { Title, Small, Body, Group, Mono, Button, Count };

// DirectWrite draws much sharper text than GDI+: hinted glyphs on whole pixels.
struct TextEngine {
    ComPtr<ID2D1Factory> d2d;
    ComPtr<IDWriteFactory> dw;
    ComPtr<IDWriteRenderingParams> params;
    std::wstring textFamily = L"Segoe UI", monoFamily = L"Consolas";
};

bool HasFamily(IDWriteFactory* dw, const wchar_t* name) {
    ComPtr<IDWriteFontCollection> fonts;
    UINT32 index = 0;
    BOOL exists = FALSE;
    return SUCCEEDED(dw->GetSystemFontCollection(&fonts)) && SUCCEEDED(fonts->FindFamilyName(name, &index, &exists)) &&
           exists;
}

TextEngine& Engine() {
    static TextEngine e = [] {
        TextEngine t;
        D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, t.d2d.GetAddressOf());
        DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                            reinterpret_cast<IUnknown**>(t.dw.GetAddressOf()));
        if (t.dw) {
            // Grayscale antialiasing (the surface is translucent, so no ClearType), GDI-style hinting.
            t.dw->CreateCustomRenderingParams(1.8f, 1.0f, 0.0f, DWRITE_PIXEL_GEOMETRY_FLAT,
                                              DWRITE_RENDERING_MODE_GDI_CLASSIC, &t.params);
            if (HasFamily(t.dw.Get(), L"Segoe UI Variable Text")) t.textFamily = L"Segoe UI Variable Text";
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

// Text queued while GDI+ paints the shapes, then drawn on top with DirectWrite.
struct TextOp {
    std::wstring text;
    FontKind font;
    Color color;
    RectF r;
    DWRITE_TEXT_ALIGNMENT align;
    bool trim;
    std::optional<RectF> clip;
};

class Panel {
public:
    Panel(std::shared_ptr<const InfoSheet> sheet, POINT anchor) : sheet_(std::move(sheet)) {
        static bool registered = false;
        HINSTANCE inst = GetModuleHandleW(nullptr);
        if (!registered) {
            BufferedPaintInit();
            WNDCLASSEXW wc{sizeof(wc)};
            wc.style = CS_DBLCLKS;
            wc.lpfnWndProc = &Panel::Proc;
            wc.hInstance = inst;
            wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
            wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
            wc.lpszClassName = kClass;
            RegisterClassExW(&wc);
            registered = true;
        }
        HMONITOR mon = MonitorFromPoint(anchor, MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi{sizeof(mi)};
        GetMonitorInfoW(mon, &mi);
        UINT dpi = 96, dpiY = 96;
        GetDpiForMonitor(mon, MDT_EFFECTIVE_DPI, &dpi, &dpiY);
        s_ = dpi / 96.0f;
        MakeFonts();
        Rebuild();
        if (sheet_->files.size() == 1) icon_ = ShellThumbnail(sheet_->files[0], int(std::round(44 * s_)));

        const RECT& wa = mi.rcWork;
        SIZE client = PreferredClientSize(wa);
        DWORD style = WS_OVERLAPPEDWINDOW;
        RECT r{0, 0, client.cx, client.cy};
        AdjustWindowRectExForDpi(&r, style, FALSE, 0, dpi);
        int w = std::min(int(r.right - r.left), int(wa.right - wa.left));
        int h = std::min(int(r.bottom - r.top), int(wa.bottom - wa.top));
        int x = wa.left + (wa.right - wa.left - w) / 2, y = wa.top + (wa.bottom - wa.top - h) / 2;
        CreateWindowExW(0, kClass, sheet_->title.c_str(), style, x, y, w, h, nullptr, nullptr, inst, this);
        if (!hwnd_) return;
        ApplyGlass();
        status_ = sheet_->note;
        Layout();
        ShowWindow(hwnd_, SW_SHOWNORMAL);
        ForceForeground(hwnd_);
        SetFocus(hwnd_);
    }

    HWND hwnd() const { return hwnd_; }

private:
    struct Line {
        bool group = false;
        size_t row = 0;                  // index in sheet_->rows (rows only)
        std::wstring text;               // group title
        std::vector<std::wstring> parts; // value, split over several lines when long
        float y = 0, h = 0;              // within the list content
    };
    struct Button {
        int id;
        std::wstring label;
        RectF r;
        bool primary;
    };

    // --- metrics -----------------------------------------------------------------

    float M() const { return 18 * s_; }
    float GroupH() const { return 30 * s_; }
    float LineH() const { return 20 * s_; }
    float RowPad() const { return 4 * s_; }
    bool IsHash() const { return sheet_->type == InfoSheet::Hash; }
    FontKind ValueKind() const { return IsHash() ? FontKind::Mono : FontKind::Body; }

    // Same layout type as the one drawn, so measured widths match what appears on screen. Layouts are
    // cached: building them for every row on every repaint is what made hovering stutter.
    IDWriteTextLayout* MakeLayout(const std::wstring& text, FontKind k, float w, float h,
                                  DWRITE_TEXT_ALIGNMENT align = DWRITE_TEXT_ALIGNMENT_LEADING, bool trim = true) const {
        std::wstring key = std::to_wstring(int(k)) + L'|' + std::to_wstring(std::lround(w)) + L'|' +
                           std::to_wstring(std::lround(h)) + L'|' + std::to_wstring(int(align)) + (trim ? L"t|" : L"n|") + text;
        auto it = layouts_.find(key);
        if (it != layouts_.end()) return it->second.Get();
        if (layouts_.size() > 4000) layouts_.clear();
        ComPtr<IDWriteTextLayout> layout;
        if (FAILED(Engine().dw->CreateGdiCompatibleTextLayout(text.c_str(), (UINT32)text.size(), formats_[int(k)].Get(), w,
                                                              h, 1.0f, nullptr, FALSE, &layout)))
            return nullptr;
        layout->SetTextAlignment(align);
        if (!trim) {
            DWRITE_TRIMMING none{DWRITE_TRIMMING_GRANULARITY_NONE, 0, 0};
            layout->SetTrimming(&none, nullptr);
        }
        return (layouts_[key] = layout).Get();
    }

    float Measure(const std::wstring& text, FontKind k) const {
        if (text.empty() || !Engine().dw) return 0;
        IDWriteTextLayout* layout = MakeLayout(text, k, 1e6f, 1e4f);
        DWRITE_TEXT_METRICS m{};
        if (!layout || FAILED(layout->GetMetrics(&m))) return 0;
        return m.widthIncludingTrailingWhitespace;
    }

    void MakeFonts() {
        auto& e = Engine();
        if (!e.dw) return;
        layouts_.clear();
        struct Spec {
            FontKind kind;
            bool mono;
            float px;
            DWRITE_FONT_WEIGHT weight;
        } specs[] = {{FontKind::Title, false, 19, DWRITE_FONT_WEIGHT_SEMI_BOLD},
                     {FontKind::Small, false, 12, DWRITE_FONT_WEIGHT_NORMAL},
                     {FontKind::Body, false, 13, DWRITE_FONT_WEIGHT_NORMAL},
                     {FontKind::Group, false, 12, DWRITE_FONT_WEIGHT_SEMI_BOLD},
                     {FontKind::Mono, true, 13, DWRITE_FONT_WEIGHT_NORMAL},
                     {FontKind::Button, false, 13, DWRITE_FONT_WEIGHT_NORMAL}};
        for (const auto& sp : specs) {
            ComPtr<IDWriteTextFormat> fmt;
            // Whole-pixel sizes render crisper.
            if (FAILED(e.dw->CreateTextFormat((sp.mono ? e.monoFamily : e.textFamily).c_str(), nullptr, sp.weight,
                                              DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                              std::round(sp.px * s_), L"es-es", &fmt)))
                continue;
            fmt->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
            ComPtr<IDWriteInlineObject> ellipsis;
            e.dw->CreateEllipsisTrimmingSign(fmt.Get(), &ellipsis);
            DWRITE_TRIMMING trimming{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
            fmt->SetTrimming(&trimming, ellipsis.Get());
            formats_[int(sp.kind)] = fmt;
        }
    }

    void Text(const std::wstring& text, FontKind k, const Color& c, const RectF& r,
              DWRITE_TEXT_ALIGNMENT align = DWRITE_TEXT_ALIGNMENT_LEADING, bool trim = true) {
        if (text.empty() || r.Width <= 0 || r.Height <= 0) return;
        ops_.push_back({text, k, c, r, align, trim, clip_});
    }

    void RenderText(HDC dc, const RECT& rc) {
        auto& e = Engine();
        std::vector<TextOp> ops;
        ops.swap(ops_);
        if (!e.d2d || !e.dw) return;
        if (!target_) {
            auto props = D2D1::RenderTargetProperties(
                D2D1_RENDER_TARGET_TYPE_SOFTWARE,
                D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96, 96);
            if (FAILED(e.d2d->CreateDCRenderTarget(&props, &target_))) return;
        }
        if (FAILED(target_->BindDC(dc, &rc))) return;
        target_->BeginDraw();
        target_->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
        if (e.params) target_->SetTextRenderingParams(e.params.Get());
        ComPtr<ID2D1SolidColorBrush> brush;
        target_->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0, 1), &brush);
        for (const auto& op : ops) {
            IDWriteTextLayout* layout = MakeLayout(op.text, op.font, op.trim ? op.r.Width : 1e6f, op.r.Height, op.align, op.trim);
            if (!layout || !brush) continue;
            DWRITE_TEXT_METRICS m{};
            layout->GetMetrics(&m);
            // Whole-pixel origin: a fractional one is exactly what made the text look blurry.
            D2D1_POINT_2F origin = D2D1::Point2F(std::round(op.r.X), std::round(op.r.Y + (op.r.Height - m.height) / 2));
            brush->SetColor(ToD2D(op.color));
            if (op.clip)
                target_->PushAxisAlignedClip(
                    D2D1::RectF(op.clip->X, op.clip->Y, op.clip->X + op.clip->Width, op.clip->Y + op.clip->Height),
                    D2D1_ANTIALIAS_MODE_ALIASED);
            target_->DrawTextLayout(origin, layout, brush.Get());
            if (op.clip) target_->PopAxisAlignedClip();
        }
        if (target_->EndDraw() == D2DERR_RECREATE_TARGET) target_.Reset();
    }

    void ApplyGlass() {
        BOOL dark = FALSE;
        DwmSetWindowAttribute(hwnd_, kDwmDarkMode, &dark, sizeof(dark));
        int corners = kCornersRound;
        DwmSetWindowAttribute(hwnd_, kDwmCorners, &corners, sizeof(corners));
        int backdrop = kBackdropMica;
        glass_ = SUCCEEDED(DwmSetWindowAttribute(hwnd_, kDwmBackdrop, &backdrop, sizeof(backdrop)));
        if (glass_) {
            MARGINS all{-1, -1, -1, -1};
            DwmExtendFrameIntoClientArea(hwnd_, &all);
        }
    }

    // Hash sheets get just the room they need; metadata gets a comfortable reading size.
    SIZE PreferredClientSize(const RECT& work) {
        float maxW = (work.right - work.left) * 0.92f, maxH = (work.bottom - work.top) * 0.85f;
        if (!IsHash()) return {LONG(std::min(820 * s_, maxW)), LONG(std::min(640 * s_, maxH))};
        float valueW = 0;
        for (const auto& l : lines_)
            for (const auto& p : l.parts) valueW = std::max(valueW, Measure(p, FontKind::Mono));
        float listW = NameColumn(0) + valueW + 12 * s_ /*card*/ + 10 * s_ /*scrollbar*/ + 28 * s_ /*air*/;
        float w = std::max(listW + 2 * M(), BottomBarWidth() + 180 * s_);
        float h = M() + 54 * s_ + 34 * s_ + 12 * s_ + (contentH_ + 12 * s_) + 12 * s_ + 34 * s_ + M();
        return {LONG(std::min(w, maxW)), LONG(std::min(h, maxH))};
    }

    float NameColumn(float viewW) const {
        float w = maxNameW_ + 30 * s_;
        if (IsHash() || viewW <= 0) return w;
        return std::clamp(w, 140 * s_, std::max(140 * s_, viewW * 0.42f));
    }

    std::vector<Button> MakeButtons() const {
        std::vector<Button> out;
        out.push_back({BTN_CLOSE, L"Cerrar", {}, false});
        out.push_back({BTN_COPYALL, L"Copiar todo", {}, false});
        out.push_back({BTN_COPY, L"Copiar", {}, false});
        if (sheet_->canStrip)
            out.push_back({BTN_STRIP, stripOutputs_.empty() ? L"Quitar metadatos" : L"Mostrar en carpeta", {}, true});
        return out;
    }

    float ButtonWidth(const std::wstring& label) const { return std::max(92 * s_, Measure(label, FontKind::Button) + 40 * s_); }

    float BottomBarWidth() const {
        float w = 0;
        for (const auto& b : MakeButtons()) w += ButtonWidth(b.label) + 8 * s_;
        return w;
    }

    std::wstring ExifLabel() const {
        return ExifToolPath().empty() ? L"Conectar ExifTool…" : L"✓ ExifTool · cambiar ruta…";
    }

    // --- content -----------------------------------------------------------------

    std::vector<std::wstring> SplitValue(std::wstring v) const {
        std::replace_if(v.begin(), v.end(), [](wchar_t c) { return c == L'\r' || c == L'\n' || c == L'\t'; }, L' ');
        // Long hashes (SHA-512) wrap in 64-character halves instead of widening the window.
        constexpr size_t kChunk = 64;
        if (!IsHash() || v.size() <= kChunk || v.find(L' ') != std::wstring::npos) return {v};
        std::vector<std::wstring> parts;
        for (size_t i = 0; i < v.size(); i += kChunk) parts.push_back(v.substr(i, kChunk));
        return parts;
    }

    void Rebuild() {
        std::wstring q = IsHash() ? L"" : ToLower(Trim(filter_));
        lines_.clear();
        selected_.clear();
        cursor_ = anchor_ = -1;
        maxNameW_ = 0;
        float y = 0;
        int shown = 0;
        std::wstring lastGroup;
        for (size_t i = 0; i < sheet_->rows.size(); ++i) {
            const InfoRow& row = sheet_->rows[i];
            if (!q.empty() && !Contains(row.name, q) && !Contains(row.value, q) && !Contains(row.group, q)) continue;
            if (shown == 0 || row.group != lastGroup) {
                Line g;
                g.group = true;
                g.text = lastGroup = row.group;
                g.y = y;
                g.h = GroupH();
                y += g.h;
                lines_.push_back(std::move(g));
            }
            Line l;
            l.row = i;
            l.parts = SplitValue(row.value);
            l.y = y;
            l.h = RowPad() * 2 + LineH() * l.parts.size();
            y += l.h;
            maxNameW_ = std::max(maxNameW_, Measure(row.name, FontKind::Body));
            lines_.push_back(std::move(l));
            ++shown;
        }
        contentH_ = y;
        ClampScroll();
        if (!hwnd_) return;
        if (q.empty()) SetStatus(sheet_->note, Tone::Normal);
        else if (shown) SetStatus(std::to_wstring(shown) + (shown == 1 ? L" coincidencia" : L" coincidencias"), Tone::Normal);
        else SetStatus(L"Sin coincidencias", Tone::Bad);
    }

    // --- layout ------------------------------------------------------------------

    void Layout() {
        RECT rc;
        GetClientRect(hwnd_, &rc);
        float W = float(rc.right), H = float(rc.bottom), m = M();
        buttons_.clear();
        if (!IsHash()) {
            std::wstring label = ExifLabel();
            float w = ButtonWidth(label);
            buttons_.push_back({BTN_EXIFTOOL, label, RectF(W - m - w, m, w, 32 * s_), false});
            headerW_ = W - 2 * m - w - 12 * s_;
        } else {
            headerW_ = W - 2 * m;
        }
        filterRect_ = RectF(m, m + 54 * s_, W - 2 * m, 34 * s_);
        float bh = 34 * s_, by = H - m - bh, x = W - m;
        for (auto& b : MakeButtons()) {
            float w = ButtonWidth(b.label);
            x -= w;
            b.r = RectF(x, by, w, bh);
            x -= 8 * s_;
            buttons_.push_back(b);
        }
        statusRect_ = RectF(m + 4 * s_, by, std::max(0.0f, x - m - 4 * s_), bh);
        float top = filterRect_.Y + filterRect_.Height + 12 * s_;
        card_ = RectF(m, top, W - 2 * m, std::max(0.0f, by - 12 * s_ - top));
        view_ = RectF(card_.X + 6 * s_, card_.Y + 6 * s_, std::max(0.0f, card_.Width - 12 * s_ - 10 * s_),
                      std::max(0.0f, card_.Height - 12 * s_));
        nameW_ = std::round(NameColumn(view_.Width));
        filterRect_ = Snap(filterRect_);
        statusRect_ = Snap(statusRect_);
        card_ = Snap(card_);
        view_ = Snap(view_);
        for (auto& b : buttons_) b.r = Snap(b.r);
        baseDirty_ = true;
        ClampScroll();
        Invalidate();
    }

    float MaxScroll() const { return std::max(0.0f, contentH_ - view_.Height); }
    void ClampScroll() { scroll_ = std::clamp(scroll_, 0.0f, MaxScroll()); }

    bool HasScrollbar() const { return contentH_ > view_.Height + 0.5f; }
    RectF TrackRect() const { return RectF(card_.X + card_.Width - 14 * s_, view_.Y, 10 * s_, view_.Height); }
    RectF ThumbRect() const {
        RectF t = TrackRect();
        float h = std::max(28 * s_, t.Height * view_.Height / std::max(contentH_, 1.0f));
        float y = t.Y + (t.Height - h) * (MaxScroll() > 0 ? scroll_ / MaxScroll() : 0);
        return RectF(t.X + 2 * s_, y, 6 * s_, h);
    }

    // --- painting ----------------------------------------------------------------

    void Invalidate() {
        if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
    }

    void Paint() {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd_, &ps);
        RECT rc;
        GetClientRect(hwnd_, &rc);
        BP_PAINTPARAMS params{sizeof(params)};
        HDC mem = nullptr;
        HPAINTBUFFER pb = BeginBufferedPaint(hdc, &rc, BPBF_TOPDOWNDIB, &params, &mem);
        if (pb) {
            RGBQUAD* bits = nullptr;
            int rowPixels = 0;
            if (SUCCEEDED(GetBufferedPaintBits(pb, &bits, &rowPixels)) && rc.right > 0 && rc.bottom > 0) {
                if (baseDirty_ || baseW_ != rc.right || baseH_ != rc.bottom) BuildBase(rc.right, rc.bottom);
                for (int y = 0; y < rc.bottom; ++y)
                    memcpy(bits + size_t(y) * rowPixels, base_.data() + size_t(y) * baseW_, size_t(baseW_) * 4);
                {
                    Bitmap canvas(rc.right, rc.bottom, rowPixels * 4, PixelFormat32bppPARGB, reinterpret_cast<BYTE*>(bits));
                    Graphics g(&canvas);
                    g.SetSmoothingMode(SmoothingModeAntiAlias);
                    g.SetPixelOffsetMode(PixelOffsetModeHalf);
                    g.SetCompositingQuality(CompositingQualityHighSpeed);
                    Draw(g);
                }  // GDI+ flushes here; the text goes on top
                RenderText(mem, rc);
            }
            EndBufferedPaint(pb, TRUE);
        }
        EndPaint(hwnd_, &ps);
    }

    // Everything that does not change on hover, selection or typing. The large antialiased fills are
    // the expensive part of a repaint, so they are drawn once and copied afterwards.
    void BuildBase(int w, int h) {
        base_.assign(size_t(w) * h, 0);
        baseW_ = w;
        baseH_ = h;
        baseDirty_ = false;
        Bitmap bmp(w, h, w * 4, PixelFormat32bppPARGB, reinterpret_cast<BYTE*>(base_.data()));
        Graphics g(&bmp);
        g.SetSmoothingMode(SmoothingModeAntiAlias);
        g.SetPixelOffsetMode(PixelOffsetModeHalf);
        g.SetCompositingQuality(CompositingQualityHighQuality);
        g.SetInterpolationMode(InterpolationModeHighQualityBicubic);
        float W = float(w), H = float(h), m = M();
        // Backdrop: a light wash over Mica, or an opaque frost when there is none.
        if (glass_) {
            LinearGradientBrush wash(PointF(0, 0), PointF(W, H), Color(150, 255, 255, 255), Color(95, 246, 249, 255));
            g.FillRectangle(&wash, 0.0f, 0.0f, W, H);
        } else {
            LinearGradientBrush wash(PointF(0, 0), PointF(0, H), Color(255, 248, 248, 251), Color(255, 234, 235, 241));
            g.FillRectangle(&wash, 0.0f, 0.0f, W, H);
        }
        if (icon_) {
            float box = std::round(44 * s_), iw = float(icon_->GetWidth()), ih = float(icon_->GetHeight());
            if (iw <= box && ih <= box)
                g.DrawImage(icon_.get(), std::round(m + (box - iw) / 2), std::round(m + (box - ih) / 2), iw, ih);
            else
                gfx::DrawFitted(g, *icon_, RectF(m, m, box, box));
        }
        Shadow(g, card_, 14 * s_, s_);
        FillGlass(g, card_, 14 * s_, 205, 175, 230, s_);
    }

    void Draw(Graphics& g) {
        float m = M();
        const auto& files = sheet_->files;
        std::wstring heading = files.size() == 1 ? files[0].filename().wstring() : std::to_wstring(files.size()) + L" archivos";
        float tx = icon_ ? m + std::round(44 * s_) + 12 * s_ : m;
        Text(heading, FontKind::Title, kInk, RectF(tx, m - 2 * s_, headerW_ - (tx - m), 28 * s_));
        Text(sheet_->summary, FontKind::Small, kMuted, RectF(tx, m + 26 * s_, headerW_ - (tx - m), 20 * s_));

        DrawFilter(g);
        DrawList(g);
        for (const auto& b : buttons_) DrawButton(g, b);

        Color tone = tone_ == Tone::Good ? kGood : tone_ == Tone::Bad ? kBad : kMuted;
        Text(status_, FontKind::Small, tone, statusRect_);
    }

    void DrawFilter(Graphics& g) {
        bool focused = focus_ == Focus::Filter && active_;
        RectF r = filterRect_;
        FillGlass(g, r, r.Height / 2, focused ? 235 : 200, focused ? 215 : 170, 230, s_);
        if (focused) {
            GraphicsPath p;
            gfx::AddRoundRect(p, r, r.Height / 2);
            Pen ring(Accent(220), 1.6f * s_);
            g.DrawPath(&ring, &p);
        }
        RectF inner(r.X + 16 * s_, r.Y, r.Width - 32 * s_, r.Height);
        FontKind font = ValueKind();
        Region old;
        g.GetClip(&old);
        g.SetClip(inner);
        clip_ = inner;
        float originX = inner.X - filterScroll_;
        if (filter_.empty()) {
            std::wstring cue = IsHash() ? L"Pega aquí un hash para comprobarlo" : L"Buscar en los metadatos…";
            Text(cue, FontKind::Body, Color(170, 92, 96, 108), inner);
        } else {
            if (filterAll_ && focused) {
                float w = Measure(filter_, font);
                GraphicsPath sel;
                gfx::AddRoundRect(sel, RectF(originX - 2 * s_, r.Y + 7 * s_, w + 4 * s_, r.Height - 14 * s_), 4 * s_);
                SolidBrush selBrush(Accent(70));
                g.FillPath(&selBrush, &sel);
            }
            Text(filter_, font, kInk, RectF(originX, r.Y, 1e5f, r.Height), DWRITE_TEXT_ALIGNMENT_LEADING, false);
        }
        if (focused && caretOn_ && !filterAll_) {
            float x = std::round(inner.X - filterScroll_ + Measure(filter_.substr(0, caret_), font)) + 0.5f;
            Pen caret(Accent(255), 1.5f * s_);
            g.DrawLine(&caret, x, r.Y + 9 * s_, x, r.Y + r.Height - 9 * s_);
        }
        g.SetClip(&old);
        clip_.reset();
    }

    void DrawList(Graphics& g) {
        Region old;
        g.GetClip(&old);
        g.SetClip(RectF(card_.X + 1, card_.Y + 1, card_.Width - 2, card_.Height - 2));
        clip_ = RectF(card_.X + 1, card_.Y + 1, card_.Width - 2, card_.Height - 2);
        float scroll = std::round(scroll_);
        if (lines_.empty()) {
            Text(L"Sin coincidencias", FontKind::Body, kMuted, view_, DWRITE_TEXT_ALIGNMENT_CENTER);
        }
        for (int i = 0; i < (int)lines_.size(); ++i) {
            const Line& l = lines_[i];
            float top = std::round(view_.Y + l.y - scroll);
            if (top + l.h < view_.Y - 1 || top > view_.Y + view_.Height + 1) continue;
            if (l.group) {
                float tw = Measure(l.text, FontKind::Group);
                Text(l.text, FontKind::Group, kAccentInk, RectF(view_.X + 10 * s_, top + 4 * s_, view_.Width - 20 * s_, l.h - 4 * s_));
                float lx = std::round(view_.X + 10 * s_ + tw + 12 * s_), ly = std::round(top + 4 * s_ + (l.h - 4 * s_) / 2);
                if (lx < view_.X + view_.Width - 10 * s_) {
                    LinearGradientBrush fade(PointF(lx, 0), PointF(view_.X + view_.Width, 0), Accent(80), Accent(0));
                    g.FillRectangle(&fade, lx, ly, view_.X + view_.Width - 10 * s_ - lx, 1.0f * s_);
                }
                continue;
            }
            const InfoRow& row = sheet_->rows[l.row];
            bool sel = selected_.count(l.row) > 0;
            RectF rr(view_.X, top + 1 * s_, view_.Width, l.h - 2 * s_);
            if (sel) {
                FillAccent(g, rr, 8 * s_, s_);
            } else if (i == hoverLine_) {
                GraphicsPath p;
                gfx::AddRoundRect(p, rr, 8 * s_);
                SolidBrush hover(Accent(26));
                g.FillPath(&hover, &p);
            }
            // Hairline between consecutive rows (not next to a highlighted one).
            bool nextRow = i + 1 < (int)lines_.size() && !lines_[i + 1].group;
            if (nextRow && !sel && !selected_.count(lines_[i + 1].row) && i != hoverLine_ && i + 1 != hoverLine_) {
                SolidBrush sep(Color(18, 20, 30, 50));
                g.FillRectangle(&sep, view_.X + 12 * s_, std::round(top + l.h) - 1.0f, view_.Width - 24 * s_, 1.0f);
            }
            if (i == cursor_ && focus_ == Focus::List && active_ && !sel) {
                GraphicsPath p;
                gfx::AddRoundRect(p, rr, 8 * s_);
                Pen ring(Accent(150), 1.2f * s_);
                g.DrawPath(&ring, &p);
            }
            Color nameColor = sel ? kWhite : kLabel, valueColor = sel ? kWhite : kInk;
            float y = top + RowPad();
            Text(row.name, FontKind::Body, nameColor, RectF(view_.X + 12 * s_, y, nameW_ - 20 * s_, LineH()));
            for (const auto& part : l.parts) {
                Text(part, ValueKind(), valueColor, RectF(view_.X + nameW_, y, view_.Width - nameW_ - 10 * s_, LineH()));
                y += LineH();
            }
        }
        g.SetClip(&old);
        clip_.reset();
        if (HasScrollbar()) {
            RectF t = ThumbRect();
            GraphicsPath p;
            gfx::AddRoundRect(p, t, t.Width / 2);
            SolidBrush thumb(Color(thumbHot_ || dragging_ ? 150 : 85, 70, 66, 80));
            g.FillPath(&thumb, &p);
        }
    }

    bool ButtonEnabled(int id) const {
        if (id == BTN_STRIP) return !stripping_;
        if (id == BTN_EXIFTOOL) return !checkingExif_;
        return true;
    }

    void DrawButton(Graphics& g, const Button& b) {
        bool enabled = ButtonEnabled(b.id), hot = enabled && hoverBtn_ == b.id, down = hot && pressedBtn_ == b.id;
        float radius = b.r.Height / 2;
        if (b.primary) {
            FillAccent(g, b.r, radius, s_, BYTE(!enabled ? 120 : down ? 255 : hot ? 250 : 225));
        } else {
            FillGlass(g, b.r, radius, down ? 150 : 225, down ? 130 : 195, 240, s_, hot ? 70 : 34);
            if (hot) {
                GraphicsPath p;
                gfx::AddRoundRect(p, b.r, radius);
                SolidBrush tint(Accent(down ? 46 : 26));
                g.FillPath(&tint, &p);
            }
        }
        Color c = b.primary ? kWhite : kInk;
        if (!enabled) c = Color(120, c.GetR(), c.GetG(), c.GetB());
        Text(b.label, FontKind::Button, c, RectF(b.r.X + 8 * s_, b.r.Y, b.r.Width - 16 * s_, b.r.Height),
                 DWRITE_TEXT_ALIGNMENT_CENTER);
    }

    // --- hit testing ---------------------------------------------------------------

    static bool In(const RectF& r, POINT p) {
        return p.x >= r.X && p.x < r.X + r.Width && p.y >= r.Y && p.y < r.Y + r.Height;
    }

    int HitButton(POINT p) const {
        for (const auto& b : buttons_)
            if (In(b.r, p) && ButtonEnabled(b.id)) return b.id;
        return BTN_NONE;
    }

    int HitLine(POINT p) const {
        if (!In(view_, p) || (HasScrollbar() && In(TrackRect(), p))) return -1;
        float y = p.y - view_.Y + scroll_;
        for (int i = 0; i < (int)lines_.size(); ++i)
            if (y >= lines_[i].y && y < lines_[i].y + lines_[i].h) return lines_[i].group ? -1 : i;
        return -1;
    }

    size_t CaretFromX(float x) const {
        FontKind font = ValueKind();
        float local = x - (filterRect_.X + 16 * s_) + filterScroll_;
        size_t best = 0;
        float bestD = 1e9f;
        for (size_t i = 0; i <= filter_.size(); ++i) {
            float d = std::abs(Measure(filter_.substr(0, i), font) - local);
            if (d < bestD) {
                bestD = d;
                best = i;
            }
        }
        return best;
    }

    // --- window procedure ------------------------------------------------------------

    static LRESULT CALLBACK Proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
        if (msg == WM_NCCREATE) {
            auto* self = static_cast<Panel*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
            self->hwnd_ = hwnd;
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)self);
        }
        auto* self = reinterpret_cast<Panel*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (!self) return DefWindowProcW(hwnd, msg, wp, lp);
        if (msg == WM_NCDESTROY) {
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
            auto it = std::find_if(g_panels.begin(), g_panels.end(), [self](auto& p) { return p.get() == self; });
            if (it != g_panels.end()) g_panels.erase(it);  // deletes `self`
            return DefWindowProcW(hwnd, msg, wp, lp);
        }
        return self->Handle(msg, wp, lp);
    }

    LRESULT Handle(UINT msg, WPARAM wp, LPARAM lp) {
        switch (msg) {
            case WM_ERASEBKGND: return 1;
            case WM_PAINT: Paint(); return 0;
            case WM_SIZE: Layout(); return 0;
            case WM_GETMINMAXINFO: {
                auto* mm = reinterpret_cast<MINMAXINFO*>(lp);
                mm->ptMinTrackSize = {LONG(BottomBarWidth() + 200 * s_), LONG(300 * s_)};
                return 0;
            }
            case WM_DPICHANGED: {
                s_ = HIWORD(wp) / 96.0f;
                MakeFonts();
                Rebuild();
                const RECT* r = reinterpret_cast<const RECT*>(lp);
                SetWindowPos(hwnd_, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top,
                             SWP_NOZORDER | SWP_NOACTIVATE);
                Layout();
                return 0;
            }
            case WM_ACTIVATE:
                active_ = LOWORD(wp) != WA_INACTIVE;
                UpdateCaretTimer();
                Invalidate();
                break;
            case WM_TIMER:
                if (wp == TIMER_CARET) {
                    caretOn_ = !caretOn_;
                    RECT r{LONG(filterRect_.X), LONG(filterRect_.Y), LONG(filterRect_.X + filterRect_.Width),
                           LONG(filterRect_.Y + filterRect_.Height)};
                    InvalidateRect(hwnd_, &r, FALSE);
                }
                return 0;
            case WM_SETCURSOR:
                if (LOWORD(lp) == HTCLIENT) {
                    POINT p;
                    GetCursorPos(&p);
                    ScreenToClient(hwnd_, &p);
                    LPCWSTR cur = In(filterRect_, p) ? IDC_IBEAM : HitButton(p) ? IDC_HAND : IDC_ARROW;
                    SetCursor(LoadCursor(nullptr, cur));
                    return TRUE;
                }
                break;
            case WM_MOUSEMOVE: OnMouseMove({GET_X_LPARAM(lp), GET_Y_LPARAM(lp)}); return 0;
            case WM_MOUSELEAVE:
                tracking_ = false;
                if (hoverLine_ != -1 || hoverBtn_ || thumbHot_) {
                    hoverLine_ = -1;
                    hoverBtn_ = BTN_NONE;
                    thumbHot_ = false;
                    Invalidate();
                }
                return 0;
            case WM_LBUTTONDOWN: OnMouseDown({GET_X_LPARAM(lp), GET_Y_LPARAM(lp)}, wp); return 0;
            case WM_LBUTTONUP: OnMouseUp({GET_X_LPARAM(lp), GET_Y_LPARAM(lp)}); return 0;
            case WM_LBUTTONDBLCLK: {
                int line = HitLine({GET_X_LPARAM(lp), GET_Y_LPARAM(lp)});
                if (line >= 0) CopyRow(lines_[line].row);
                else OnMouseDown({GET_X_LPARAM(lp), GET_Y_LPARAM(lp)}, wp);
                return 0;
            }
            case WM_MOUSEWHEEL:
                scroll_ -= GET_WHEEL_DELTA_WPARAM(wp) / float(WHEEL_DELTA) * 3 * 27 * s_;
                ClampScroll();
                Invalidate();
                return 0;
            case WM_CAPTURECHANGED:
                dragging_ = false;
                pressedBtn_ = BTN_NONE;
                return 0;
            case WM_KEYDOWN: OnKey((UINT)wp); return 0;
            case WM_CHAR: OnChar((wchar_t)wp); return 0;
            case WM_STRIP_DONE: OnStripDone(reinterpret_cast<JobResult*>(lp)); return 0;
            case WM_SHEET_READY: OnSheetReady(reinterpret_cast<JobResult*>(lp)); return 0;
            case WM_CLOSE: DestroyWindow(hwnd_); return 0;
        }
        return DefWindowProcW(hwnd_, msg, wp, lp);
    }

    void OnMouseMove(POINT p) {
        if (!tracking_) {
            TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, hwnd_, 0};
            tracking_ = TrackMouseEvent(&tme) != FALSE;
        }
        if (dragging_) {
            RectF t = TrackRect(), th = ThumbRect();
            float range = t.Height - th.Height;
            if (range > 0) scroll_ = dragScroll_ + (p.y - dragY_) / range * MaxScroll();
            ClampScroll();
            Invalidate();
            return;
        }
        int btn = HitButton(p), line = HitLine(p);
        bool thumb = HasScrollbar() && In(TrackRect(), p);
        if (btn != hoverBtn_ || line != hoverLine_ || thumb != thumbHot_) {
            hoverBtn_ = btn;
            hoverLine_ = line;
            thumbHot_ = thumb;
            Invalidate();
        }
    }

    void OnMouseDown(POINT p, WPARAM keys) {
        SetFocus(hwnd_);
        if (int btn = HitButton(p)) {
            pressedBtn_ = btn;
            SetCapture(hwnd_);
            Invalidate();
            return;
        }
        if (In(filterRect_, p)) {
            SetFocusTo(Focus::Filter);
            filterAll_ = false;
            caret_ = CaretFromX(float(p.x));
            RestartCaret();
            return;
        }
        if (HasScrollbar() && In(TrackRect(), p)) {
            RectF th = ThumbRect();
            if (p.y < th.Y) scroll_ -= view_.Height * 0.9f;
            else if (p.y > th.Y + th.Height) scroll_ += view_.Height * 0.9f;
            else {
                dragging_ = true;
                dragY_ = float(p.y);
                dragScroll_ = scroll_;
                SetCapture(hwnd_);
            }
            ClampScroll();
            Invalidate();
            return;
        }
        int line = HitLine(p);
        SetFocusTo(Focus::List);
        if (line < 0) return;
        size_t row = lines_[line].row;
        if ((keys & MK_SHIFT) && anchor_ >= 0) {
            SelectRange(anchor_, line);
        } else if (keys & MK_CONTROL) {
            if (!selected_.erase(row)) selected_.insert(row);
            anchor_ = line;
        } else {
            selected_ = {row};
            anchor_ = line;
        }
        cursor_ = line;
        Invalidate();
    }

    void OnMouseUp(POINT p) {
        int pressed = pressedBtn_;
        bool wasDragging = dragging_;
        pressedBtn_ = BTN_NONE;
        dragging_ = false;
        if (pressed || wasDragging) ReleaseCapture();
        if (pressed && HitButton(p) == pressed) OnButton(pressed);
        Invalidate();
    }

    void OnButton(int id) {
        switch (id) {
            case BTN_STRIP: Strip(); break;
            case BTN_COPY: CopySelected(); break;
            case BTN_COPYALL: CopyAll(); break;
            case BTN_CLOSE: PostMessageW(hwnd_, WM_CLOSE, 0, 0); break;
            case BTN_EXIFTOOL: ConnectExifTool(); break;
        }
    }

    void SelectRange(int a, int b) {
        selected_.clear();
        for (int i = std::min(a, b); i <= std::max(a, b); ++i)
            if (!lines_[i].group) selected_.insert(lines_[i].row);
    }

    void SetFocusTo(Focus f) {
        focus_ = f;
        UpdateCaretTimer();
        Invalidate();
    }

    void UpdateCaretTimer() {
        if (focus_ == Focus::Filter && active_) RestartCaret();
        else KillTimer(hwnd_, TIMER_CARET);
    }

    void RestartCaret() {
        caretOn_ = true;
        SetTimer(hwnd_, TIMER_CARET, GetCaretBlinkTime(), nullptr);
        KeepCaretVisible();
        Invalidate();
    }

    void KeepCaretVisible() {
        FontKind font = ValueKind();
        float inner = filterRect_.Width - 32 * s_;
        float x = Measure(filter_.substr(0, caret_), font);
        if (x - filterScroll_ > inner) filterScroll_ = x - inner;
        if (x - filterScroll_ < 0) filterScroll_ = x;
        filterScroll_ = std::max(0.0f, filterScroll_);
    }

    void EnsureVisible(int line) {
        if (line < 0) return;
        const Line& l = lines_[line];
        if (l.y < scroll_) scroll_ = l.y;
        else if (l.y + l.h > scroll_ + view_.Height) scroll_ = l.y + l.h - view_.Height;
        // Keep the group title above the first row in view.
        if (line > 0 && lines_[line - 1].group && lines_[line - 1].y < scroll_) scroll_ = lines_[line - 1].y;
        ClampScroll();
    }

    int NextRow(int from, int dir) const {
        for (int i = from + dir; i >= 0 && i < (int)lines_.size(); i += dir)
            if (!lines_[i].group) return i;
        return from;
    }

    // --- keyboard ------------------------------------------------------------------------

    void OnKey(UINT vk) {
        bool ctrl = GetKeyState(VK_CONTROL) < 0, shift = GetKeyState(VK_SHIFT) < 0;
        if (vk == VK_ESCAPE) {
            if (!filter_.empty()) SetFilter(L"", 0);
            else PostMessageW(hwnd_, WM_CLOSE, 0, 0);
            return;
        }
        if (vk == VK_TAB) {
            SetFocusTo(focus_ == Focus::List ? Focus::Filter : Focus::List);
            return;
        }
        if (focus_ == Focus::Filter) {
            switch (vk) {
                case VK_LEFT: caret_ = filterAll_ ? 0 : caret_ > 0 ? caret_ - 1 : 0; filterAll_ = false; break;
                case VK_RIGHT: caret_ = filterAll_ ? filter_.size() : std::min(filter_.size(), caret_ + 1); filterAll_ = false; break;
                case VK_HOME: caret_ = 0; filterAll_ = false; break;
                case VK_END: caret_ = filter_.size(); filterAll_ = false; break;
                case VK_BACK:
                    if (filterAll_) SetFilter(L"", 0);
                    else if (caret_ > 0) SetFilter(filter_.substr(0, caret_ - 1) + filter_.substr(caret_), caret_ - 1);
                    return;
                case VK_DELETE:
                    if (filterAll_) SetFilter(L"", 0);
                    else if (caret_ < filter_.size()) SetFilter(filter_.substr(0, caret_) + filter_.substr(caret_ + 1), caret_);
                    return;
                case VK_DOWN: SetFocusTo(Focus::List); MoveCursor(cursor_ < 0 ? -1 : cursor_, 1, false); return;
                default:
                    if (ctrl && vk == 'A') filterAll_ = true;
                    else if (ctrl && vk == 'V') PasteIntoFilter();
                    else if (ctrl && vk == 'C' && !filter_.empty()) CopyTextToClipboard(hwnd_, filter_);
                    else return;
            }
            RestartCaret();
            return;
        }
        switch (vk) {
            case VK_UP: MoveCursor(cursor_, -1, shift); break;
            case VK_DOWN: MoveCursor(cursor_, 1, shift); break;
            case VK_HOME: MoveCursor(-1, 1, shift); break;
            case VK_END: MoveCursor((int)lines_.size(), -1, shift); break;
            case VK_PRIOR:
            case VK_NEXT: {
                int dir = vk == VK_NEXT ? 1 : -1, line = cursor_;
                for (float moved = 0; moved < view_.Height * 0.9f;) {
                    int next = NextRow(line, dir);
                    if (next == line) break;
                    moved += std::abs(lines_[next].y - (line >= 0 ? lines_[line].y : 0));
                    line = next;
                }
                MoveCursor(line, 0, shift);
                break;
            }
            case VK_RETURN:
                if (cursor_ >= 0) CopyRow(lines_[cursor_].row);
                break;
            default:
                if (ctrl && vk == 'C') CopySelected();
                else if (ctrl && vk == 'A') {
                    for (const auto& l : lines_)
                        if (!l.group) selected_.insert(l.row);
                    Invalidate();
                } else if (ctrl && vk == 'F') SetFocusTo(Focus::Filter);
                else if (ctrl && vk == 'V') {
                    SetFocusTo(Focus::Filter);
                    PasteIntoFilter();
                }
        }
    }

    // dir 0: go to `from` itself (already a row).
    void MoveCursor(int from, int dir, bool extend) {
        int line = dir == 0 ? from : NextRow(from, dir);
        if (line < 0 || line >= (int)lines_.size() || lines_[line].group) return;
        cursor_ = line;
        if (extend && anchor_ >= 0) SelectRange(anchor_, line);
        else {
            selected_ = {lines_[line].row};
            anchor_ = line;
        }
        EnsureVisible(line);
        Invalidate();
    }

    void OnChar(wchar_t ch) {
        if (ch < 32 || ch == 127) return;  // control keys are handled in OnKey
        if (focus_ == Focus::List) {  // typing in the list searches
            SetFocusTo(Focus::Filter);
            caret_ = filter_.size();
        }
        if (filterAll_) SetFilter(std::wstring(1, ch), 1);
        else SetFilter(filter_.substr(0, caret_) + ch + filter_.substr(caret_), caret_ + 1);
    }

    void PasteIntoFilter() {
        if (!OpenClipboard(hwnd_)) return;
        std::wstring text;
        if (HANDLE h = GetClipboardData(CF_UNICODETEXT))
            if (auto* p = static_cast<const wchar_t*>(GlobalLock(h))) {
                text = p;
                GlobalUnlock(h);
            }
        CloseClipboard();
        std::replace_if(text.begin(), text.end(), [](wchar_t c) { return c == L'\r' || c == L'\n' || c == L'\t'; }, L' ');
        text = Trim(text);
        if (text.empty()) return;
        if (filterAll_) SetFilter(text, text.size());
        else SetFilter(filter_.substr(0, caret_) + text + filter_.substr(caret_), caret_ + text.size());
    }

    void SetFilter(std::wstring text, size_t caret) {
        filter_ = std::move(text);
        caret_ = std::min(caret, filter_.size());
        filterAll_ = false;
        if (IsHash()) Compare();
        else {
            scroll_ = 0;
            Rebuild();
        }
        RestartCaret();
    }

    // Hash sheet: tells whether the pasted hash matches one of ours.
    void Compare() {
        std::wstring q;
        for (wchar_t c : ToLower(filter_))
            if (!iswspace(c)) q += c;
        if (size_t colon = q.find(L':'); colon != std::wstring::npos && colon < 8) q.erase(0, colon + 1);  // "sha256:…"
        selected_.clear();
        if (q.empty()) {
            SetStatus(sheet_->note, Tone::Normal);
            return;
        }
        bool multi = sheet_->files.size() > 1;
        for (int i = 0; i < (int)lines_.size(); ++i) {
            if (lines_[i].group) continue;
            const InfoRow& row = sheet_->rows[lines_[i].row];
            if (row.name == L"Tamaño" || ToLower(row.value) != q) continue;
            selected_ = {lines_[i].row};
            cursor_ = anchor_ = i;
            EnsureVisible(i);
            SetStatus(L"✓ Coincide con el " + row.name + (multi ? L" de " + row.group : L""), Tone::Good);
            return;
        }
        SetStatus(L"✗ No coincide con ningún hash", Tone::Bad);
    }

    // --- actions ------------------------------------------------------------------------

    void SetStatus(const std::wstring& text, Tone tone) {
        status_ = text;
        tone_ = tone;
        Invalidate();
    }

    void CopyRow(size_t row) {
        const InfoRow& r = sheet_->rows[row];
        if (CopyTextToClipboard(hwnd_, r.value)) SetStatus(L"Copiado: " + r.name, Tone::Good);
    }

    void CopySelected() {
        std::vector<size_t> rows;
        for (const auto& l : lines_)
            if (!l.group && selected_.count(l.row)) rows.push_back(l.row);
        if (rows.empty()) {
            SetStatus(L"Selecciona una fila para copiarla (o doble clic)", Tone::Normal);
            return;
        }
        if (rows.size() == 1) {
            CopyRow(rows[0]);
            return;
        }
        std::wstring text;
        for (size_t r : rows) text += sheet_->rows[r].name + L": " + sheet_->rows[r].value + L"\r\n";
        if (CopyTextToClipboard(hwnd_, text)) SetStatus(std::to_wstring(rows.size()) + L" filas copiadas", Tone::Good);
        else SetStatus(L"No se pudo acceder al portapapeles", Tone::Bad);
    }

    void CopyAll() {
        std::wstring text;
        int n = 0;
        for (const auto& l : lines_) {
            if (l.group) {
                text += (text.empty() ? L"[" : L"\r\n[") + l.text + L"]\r\n";
                continue;
            }
            const InfoRow& row = sheet_->rows[l.row];
            text += row.name + L": " + row.value + L"\r\n";
            ++n;
        }
        if (CopyTextToClipboard(hwnd_, text)) SetStatus(std::to_wstring(n) + L" filas copiadas", Tone::Good);
        else SetStatus(L"No se pudo acceder al portapapeles", Tone::Bad);
    }

    void Strip() {
        if (!stripOutputs_.empty()) {
            RevealInExplorer(stripOutputs_);
            return;
        }
        if (stripping_) return;
        std::vector<fs::path> files;
        std::error_code ec;
        for (const auto& f : sheet_->files)
            if (!fs::is_directory(f, ec)) files.push_back(f);
        if (files.empty()) return;
        stripping_ = true;
        SetStatus(L"Quitando metadatos…", Tone::Normal);
        std::thread([hwnd = hwnd_, files] {
            winrt::init_apartment(winrt::apartment_type::multi_threaded);
            auto* r = new JobResult(RunAction("tool:metadata", files, [](int, int) {}));
            if (!PostMessageW(hwnd, WM_STRIP_DONE, 0, (LPARAM)r)) delete r;
            winrt::uninit_apartment();
        }).detach();
    }

    void OnStripDone(JobResult* raw) {
        std::unique_ptr<JobResult> r(raw);
        stripping_ = false;
        if (!r->ok || r->outputs.empty()) {
            SetStatus(r->message.empty() ? L"No se pudieron quitar los metadatos" : r->message, Tone::Bad);
            return;
        }
        stripOutputs_ = r->outputs;
        Layout();  // the button now reads "Mostrar en carpeta"
        std::wstring name = r->outputs.size() == 1 ? r->outputs[0].filename().wstring()
                                                   : std::to_wstring(r->outputs.size()) + L" archivos";
        SetStatus(L"✓ Guardado: " + name + (r->message.empty() ? L"" : L" · " + r->message), Tone::Good);
    }

    // Lets the user point at exiftool.exe; the path is remembered and the sheet is read again with it.
    void ConnectExifTool() {
        ComPtr<IFileOpenDialog> dlg;
        if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) return;
        COMDLG_FILTERSPEC types[] = {{L"ExifTool", L"exiftool*.exe"}, {L"Programas", L"*.exe"}};
        dlg->SetFileTypes(2, types);
        dlg->SetTitle(L"Selecciona exiftool.exe");
        fs::path current = ExifToolPath();
        ComPtr<IShellItem> folder;
        if (!current.empty() &&
            SUCCEEDED(SHCreateItemFromParsingName(current.parent_path().c_str(), nullptr, IID_PPV_ARGS(&folder))))
            dlg->SetFolder(folder.Get());
        ComPtr<IShellItem> item;
        PWSTR raw = nullptr;
        if (FAILED(dlg->Show(hwnd_)) || FAILED(dlg->GetResult(&item)) ||
            FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &raw)))
            return;
        fs::path picked = raw;
        CoTaskMemFree(raw);

        checkingExif_ = true;
        SetStatus(L"Comprobando ExifTool…", Tone::Normal);
        std::thread([hwnd = hwnd_, picked, files = sheet_->files] {
            winrt::init_apartment(winrt::apartment_type::multi_threaded);
            auto* r = new JobResult;
            std::wstring version = ExifToolVersion(picked);
            if (version.empty()) {
                *r = JobResult{false, {}, picked.filename().wstring() + L" no responde como ExifTool"};
            } else {
                SetConfiguredExifTool(picked);
                *r = RunAction("tool:info", files, [](int, int) {});
                r->message = L"ExifTool " + version + L" conectado";
            }
            if (!PostMessageW(hwnd, WM_SHEET_READY, 0, (LPARAM)r)) delete r;
            winrt::uninit_apartment();
        }).detach();
    }

    void OnSheetReady(JobResult* raw) {
        std::unique_ptr<JobResult> r(raw);
        checkingExif_ = false;
        if (!r->ok || !r->sheet) {
            SetStatus(r->message.empty() ? L"No se pudo conectar ExifTool" : r->message, Tone::Bad);
            return;
        }
        sheet_ = r->sheet;
        SetWindowTextW(hwnd_, sheet_->title.c_str());
        Rebuild();
        Layout();
        SetStatus(L"✓ " + r->message, Tone::Good);
    }

    std::shared_ptr<const InfoSheet> sheet_;
    HWND hwnd_ = nullptr;
    float s_ = 1;
    bool glass_ = false, active_ = false, tracking_ = false;
    ComPtr<IDWriteTextFormat> formats_[int(FontKind::Count)];
    ComPtr<ID2D1DCRenderTarget> target_;
    std::vector<TextOp> ops_;
    std::optional<RectF> clip_;
    mutable std::unordered_map<std::wstring, ComPtr<IDWriteTextLayout>> layouts_;
    std::vector<uint32_t> base_;  // static layer, premultiplied BGRA
    int baseW_ = 0, baseH_ = 0;
    bool baseDirty_ = true;
    std::unique_ptr<Bitmap> icon_;  // file icon in the header

    std::vector<Line> lines_;
    float contentH_ = 0, scroll_ = 0, maxNameW_ = 0, nameW_ = 0, headerW_ = 0;
    std::set<size_t> selected_;
    int cursor_ = -1, anchor_ = -1, hoverLine_ = -1;
    RectF filterRect_, card_, view_, statusRect_;
    std::vector<Button> buttons_;
    int hoverBtn_ = BTN_NONE, pressedBtn_ = BTN_NONE;
    bool thumbHot_ = false, dragging_ = false;
    float dragY_ = 0, dragScroll_ = 0;

    Focus focus_ = Focus::List;
    std::wstring filter_;
    size_t caret_ = 0;
    bool filterAll_ = false, caretOn_ = true;
    float filterScroll_ = 0;

    std::wstring status_;
    Tone tone_ = Tone::Normal;
    bool stripping_ = false, checkingExif_ = false;
    std::vector<fs::path> stripOutputs_;
};

}  // namespace

void OpenInfoPanel(std::shared_ptr<const InfoSheet> sheet, POINT anchor) {
    if (!sheet) return;
    auto panel = std::make_unique<Panel>(std::move(sheet), anchor);
    if (panel->hwnd()) g_panels.push_back(std::move(panel));
}

void CloseAllInfoPanels() {
    std::vector<HWND> windows;
    for (auto& p : g_panels) windows.push_back(p->hwnd());
    for (HWND h : windows) DestroyWindow(h);
    g_panels.clear();
}

bool InfoPanelsOpen() { return !g_panels.empty(); }
