#include "editor.h"

#include "bgremove.h"
#include "floatshelf.h"
#include "gfx.h"
#include "image.h"

#include <commctrl.h>
#include <dwmapi.h>
#include <windowsx.h>
#include <unknwn.h>
#include <winrt/base.h>

#include <algorithm>
#include <cmath>
#include <execution>
#include <numeric>
#include <thread>

using namespace Gdiplus;

namespace {

constexpr wchar_t kClass[] = L"FileTools.Editor";
constexpr UINT WM_COMMIT_TEXT = WM_APP + 40;
constexpr UINT WM_CANCEL_TEXT = WM_APP + 41;
constexpr UINT WM_MASK_READY = WM_APP + 42;
constexpr wchar_t kGlyphWand = 0xE771;
constexpr wchar_t kGlyphResize = 0xE740;

// Layout at 96 DPI.
constexpr float kToolbarH = 56, kPanelW = 272, kPad = 18, kTabW = 110, kTabStep = 114;
constexpr int kTabCount = 5;

// Scale tab: percent range and presets. Larger outputs are refused on save, they would need gigabytes of memory.
constexpr int kScaleMin = 10, kScaleMax = 400;
constexpr int kScalePresets[] = {25, 50, 75, 150, 200, 400};
constexpr uint32_t kMaxScaledDim = 16384;

const Color kBg(255, 28, 28, 31), kCanvas(255, 20, 20, 22), kPanel(255, 36, 36, 40), kText(255, 238, 238, 242),
    kSub(255, 150, 150, 160), kBtn(255, 52, 52, 58), kBtnHot(255, 66, 66, 74), kAccent(255, 28, 132, 230),
    kAccentHot(255, 62, 154, 240), kLine(255, 60, 60, 66);

const Color kSwatches[] = {Color(255, 235, 64, 52),  Color(255, 242, 102, 43), Color(255, 250, 204, 21),
                           Color(255, 34, 197, 94),  Color(255, 59, 130, 246), Color(255, 20, 20, 20),
                           Color(255, 255, 255, 255)};

enum class Tool { Pen, Marker, Arrow, Rect, Ellipse, Text };

enum Id {
    ID_NONE = 0,
    ID_TAB_ADJUST,
    ID_TAB_ANNOTATE,
    ID_TAB_CROP,
    ID_TAB_BG,
    ID_TAB_RESIZE,
    ID_CUTOUT,
    ID_RESET_BG,
    ID_CANCEL,
    ID_SAVE,
    ID_ROTATE,
    ID_FLIP,
    ID_RESET_ADJUST,
    ID_UNDO,
    ID_CLEAR,
    ID_AUTOCROP,
    ID_RESET_CROP,
    ID_RESET_SCALE,
    ID_TOOL = 100,
    ID_COLOR = 200,
    ID_ASPECT = 300,
    ID_SCALE = 400,
};

struct Aspect {
    const wchar_t* label;
    float ratio;  // w / h; 0 = free, -1 = original
};
const Aspect kAspects[] = {{L"Libre", 0}, {L"Original", -1}, {L"1:1", 1}, {L"4:3", 4.0f / 3}, {L"16:9", 16.0f / 9},
                           {L"9:16", 9.0f / 16}};

struct Adjust {
    int brightness = 0, contrast = 0, saturation = 0, temperature = 0, sharpness = 0;
    bool Neutral() const { return !brightness && !contrast && !saturation && !temperature && !sharpness; }
};

struct Shape {
    Tool tool;
    Color color;
    float width;  // image pixels
    std::vector<PointF> pts;
    std::wstring text;
};

struct Button {
    RectF r;
    std::wstring label;
    wchar_t glyph = 0;
    int id = 0;
    bool active = false;
    bool accent = false;
    const Color* swatch = nullptr;
};

struct Slider {
    RectF r;
    const wchar_t* label;
    int* value;
    int min, max;
    bool adjust;  // affects the image preview
    int def = 0;  // double-click restores it
    const wchar_t* unit = L"";
};

// Result of the background detection thread.
struct MaskResult {
    std::shared_ptr<SubjectMask> mask;
    std::wstring error;
};

::Image ApplyAdjust(const ::Image& in, const Adjust& a) {
    ::Image out = in;
    if (a.Neutral()) return out;
    float bf = a.brightness / 100.0f * 80;
    float cf = a.contrast >= 0 ? 1 + a.contrast / 100.0f * 1.2f : 1 + a.contrast / 100.0f * 0.85f;
    float sf = 1 + a.saturation / 100.0f;
    float tf = a.temperature / 100.0f * 40;
    uint8_t lut[3][256];
    for (int c = 0; c < 3; ++c)
        for (int i = 0; i < 256; ++i) {
            float v = (i - 128) * cf + 128 + bf + (c == 2 ? tf : c == 0 ? -tf : tf * 0.15f);
            lut[c][i] = uint8_t(std::clamp(v, 0.0f, 255.0f));
        }
    // Rows are independent: process them on all cores.
    std::vector<uint32_t> rows(out.h);
    std::iota(rows.begin(), rows.end(), 0u);
    int sfx = int(sf * 256);  // fixed-point saturation
    std::for_each(std::execution::par, rows.begin(), rows.end(), [&](uint32_t y) {
        uint8_t* p = out.Row(y);
        for (uint32_t x = 0; x < out.w; ++x, p += 4) {
            int b = lut[0][p[0]], g = lut[1][p[1]], r = lut[2][p[2]];
            if (sfx != 256) {
                int gray = (29 * b + 150 * g + 77 * r) >> 8;
                b = gray + (((b - gray) * sfx) >> 8);
                g = gray + (((g - gray) * sfx) >> 8);
                r = gray + (((r - gray) * sfx) >> 8);
            }
            p[0] = uint8_t(std::clamp(b, 0, 255));
            p[1] = uint8_t(std::clamp(g, 0, 255));
            p[2] = uint8_t(std::clamp(r, 0, 255));
        }
    });
    if (a.sharpness > 0 && out.w > 2 && out.h > 2) {
        // Unsharp mask with a separable 3x3 box blur.
        int amount = int(a.sharpness / 100.0f * 1.6f * 256);
        const ::Image src = out;
        size_t stride = size_t(out.w) * 4;
        std::vector<uint16_t> hsum(size_t(out.h) * stride);
        std::for_each(std::execution::par, rows.begin(), rows.end(), [&](uint32_t y) {
            const uint8_t* s = src.Row(y);
            uint16_t* h = hsum.data() + y * stride;
            for (uint32_t x = 1; x + 1 < out.w; ++x)
                for (int c = 0; c < 3; ++c) h[x * 4 + c] = uint16_t(s[(x - 1) * 4 + c] + s[x * 4 + c] + s[(x + 1) * 4 + c]);
        });
        std::for_each(std::execution::par, rows.begin() + 1, rows.end() - 1, [&](uint32_t y) {
            const uint16_t* h0 = hsum.data() + (y - 1) * stride;
            const uint16_t* h1 = h0 + stride;
            const uint16_t* h2 = h1 + stride;
            const uint8_t* s = src.Row(y);
            uint8_t* d = out.Row(y);
            for (uint32_t x = 1; x + 1 < out.w; ++x)
                for (int c = 0; c < 3; ++c) {
                    int i = x * 4 + c;
                    int blur = (h0[i] + h1[i] + h2[i]) / 9;
                    int v = s[i] + (((s[i] - blur) * amount) >> 8);
                    d[i] = uint8_t(std::clamp(v, 0, 255));
                }
        });
    }
    return out;
}

void DrawShape(Graphics& g, const Shape& s) {
    switch (s.tool) {
        case Tool::Pen:
        case Tool::Marker: {
            Color c = s.tool == Tool::Marker ? Color(110, s.color.GetR(), s.color.GetG(), s.color.GetB()) : s.color;
            float w = s.tool == Tool::Marker ? s.width * 3 : s.width;
            if (s.pts.size() == 1) {
                SolidBrush b(c);
                g.FillEllipse(&b, s.pts[0].X - w / 2, s.pts[0].Y - w / 2, w, w);
                break;
            }
            Pen p(c, w);
            p.SetLineJoin(LineJoinRound);
            p.SetStartCap(s.tool == Tool::Marker ? LineCapSquare : LineCapRound);
            p.SetEndCap(s.tool == Tool::Marker ? LineCapSquare : LineCapRound);
            g.DrawLines(&p, s.pts.data(), (INT)s.pts.size());
            break;
        }
        case Tool::Arrow: {
            if (s.pts.size() < 2) break;
            Pen p(s.color, s.width);
            AdjustableArrowCap cap(3.2f, 3.2f, TRUE);
            p.SetStartCap(LineCapRound);
            p.SetCustomEndCap(&cap);
            g.DrawLine(&p, s.pts.front(), s.pts.back());
            break;
        }
        case Tool::Rect:
        case Tool::Ellipse: {
            if (s.pts.size() < 2) break;
            RectF r(std::min(s.pts.front().X, s.pts.back().X), std::min(s.pts.front().Y, s.pts.back().Y),
                    std::abs(s.pts.back().X - s.pts.front().X), std::abs(s.pts.back().Y - s.pts.front().Y));
            Pen p(s.color, s.width);
            p.SetLineJoin(LineJoinRound);
            if (s.tool == Tool::Rect) g.DrawRectangle(&p, r);
            else g.DrawEllipse(&p, r);
            break;
        }
        case Tool::Text: {
            if (s.pts.empty() || s.text.empty()) break;
            Font f(gfx::TextFamily(), s.width, FontStyleBold, UnitPixel);
            bool light = s.color.GetR() + s.color.GetG() + s.color.GetB() > 600;
            SolidBrush shadow(light ? Color(150, 0, 0, 0) : Color(110, 255, 255, 255));
            SolidBrush brush(s.color);
            float o = std::max(1.0f, s.width / 18);
            g.DrawString(s.text.c_str(), (INT)s.text.size(), &f, PointF(s.pts[0].X + o, s.pts[0].Y + o), &shadow);
            g.DrawString(s.text.c_str(), (INT)s.text.size(), &f, s.pts[0], &brush);
            break;
        }
    }
}

class Editor {
public:
    Editor(const fs::path& file, EditorMode mode) : file_(file), mode_(mode) {
        orig_ = LoadImageFile(file);
        cutout_ = LoadCutoutSettings();
        ResetCrop();
        if (mode == EditorMode::Crop) AutoCropNow();
        RebuildPreview();

        static bool registered = false;
        if (!registered) {
            WNDCLASSEXW wc{sizeof(wc)};
            wc.style = CS_DBLCLKS;
            wc.lpfnWndProc = &Editor::Proc;
            wc.hInstance = GetModuleHandleW(nullptr);
            wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
            wc.lpszClassName = kClass;
            RegisterClassExW(&wc);
            registered = true;
        }
        POINT cursor;
        GetCursorPos(&cursor);
        MONITORINFO mi{sizeof(mi)};
        GetMonitorInfoW(MonitorFromPoint(cursor, MONITOR_DEFAULTTONEAREST), &mi);
        s_ = gfx::DpiScaleAt(cursor);
        int w = std::min(int(1240 * s_), int(mi.rcWork.right - mi.rcWork.left - 40));
        int h = std::min(int(820 * s_), int(mi.rcWork.bottom - mi.rcWork.top - 40));
        int x = (mi.rcWork.left + mi.rcWork.right - w) / 2, y = (mi.rcWork.top + mi.rcWork.bottom - h) / 2;
        hwnd_ = CreateWindowExW(0, kClass, (file.filename().wstring() + L" — FileTools").c_str(),
                                WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, x, y, w, h, nullptr, nullptr,
                                GetModuleHandleW(nullptr), this);
        BOOL dark = TRUE;
        DwmSetWindowAttribute(hwnd_, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &dark, sizeof(dark));
        s_ = GetDpiForWindow(hwnd_) / 96.0f;
        if (mode == EditorMode::Background) EnableCutout();
        Layout();
        ShowWindow(hwnd_, SW_SHOWNORMAL);
        // We were not the foreground app (the user was dragging in Explorer): a synthetic
        // Alt tap lets SetForegroundWindow succeed instead of just flashing the taskbar.
        INPUT in[2] = {};
        in[0].type = in[1].type = INPUT_KEYBOARD;
        in[0].ki.wVk = in[1].ki.wVk = VK_MENU;
        in[1].ki.dwFlags = KEYEVENTF_KEYUP;
        SendInput(2, in, sizeof(INPUT));
        SetForegroundWindow(hwnd_);
    }

private:
    static LRESULT CALLBACK Proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
        Editor* self = reinterpret_cast<Editor*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (msg == WM_NCCREATE) {
            self = static_cast<Editor*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
            self->hwnd_ = hwnd;
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)self);
        }
        if (!self) return DefWindowProcW(hwnd, msg, wp, lp);
        LRESULT r = self->Handle(msg, wp, lp);
        if (msg == WM_NCDESTROY) {
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
            delete self;
        }
        return r;
    }

    // --- geometry ------------------------------------------------------------

    RectF Client() const {
        RECT rc;
        GetClientRect(hwnd_, &rc);
        return RectF(0, 0, float(rc.right), float(rc.bottom));
    }
    RectF CanvasRect() const {
        RectF c = Client();
        return RectF(0, kToolbarH * s_, c.Width - kPanelW * s_, c.Height - kToolbarH * s_);
    }
    RectF PanelRect() const {
        RectF c = Client();
        return RectF(c.Width - kPanelW * s_, kToolbarH * s_, kPanelW * s_, c.Height - kToolbarH * s_);
    }

    void UpdateView() {
        RectF c = CanvasRect();
        float margin = 28 * s_;
        float sx = (c.Width - margin * 2) / orig_.w, sy = (c.Height - margin * 2) / orig_.h;
        scale_ = std::max(0.01f, std::min({sx, sy, 4.0f}));
        off_ = PointF(std::round(c.X + (c.Width - orig_.w * scale_) / 2), std::round(c.Y + (c.Height - orig_.h * scale_) / 2));
        if (resizeHit_ < 0) FitResizeView();
        displayDirty_ = true;
    }
    PointF ToImage(float x, float y, bool clamp = true) const {
        PointF p((x - off_.X) / scale_, (y - off_.Y) / scale_);
        if (clamp) {
            p.X = std::clamp(p.X, 0.0f, float(orig_.w));
            p.Y = std::clamp(p.Y, 0.0f, float(orig_.h));
        }
        return p;
    }
    PointF ToScreen(float x, float y) const { return PointF(off_.X + x * scale_, off_.Y + y * scale_); }

    // --- image pipeline -------------------------------------------------------

    void RebuildPreview() {
        // The preview never needs more pixels than a large screen can show.
        preview_ = FitWithin(orig_, 1600);
        RecomputeAdjust();
    }
    void RecomputeAdjust() {
        view_.reset();
        adjusted_ = ApplyAdjust(preview_, adjust_);
        if (cutoutOn_ && mask_) ApplyCutout(adjusted_, *mask_, cutout_);
        view_ = std::make_unique<Bitmap>(adjusted_.w, adjusted_.h, adjusted_.w * 4, PixelFormat32bppARGB,
                                         adjusted_.px.data());
        displayDirty_ = true;
    }
    // Slider drags arrive faster than we can redraw: coalesce them into one recompute per frame.
    void ScheduleAdjust() {
        if (!adjustPending_) {
            adjustPending_ = true;
            SetTimer(hwnd_, 1, 16, nullptr);
        }
    }
    // Pre-scaled image (with committed annotations) at display size, so painting is a 1:1 blit.
    void EnsureDisplay() {
        int w = std::max(1, int(std::lround(orig_.w * scale_))), h = std::max(1, int(std::lround(orig_.h * scale_)));
        if (display_ && !displayDirty_ && displayShapes_ == shapes_.size() && (int)display_->GetWidth() == w &&
            (int)display_->GetHeight() == h)
            return;
        display_ = std::make_unique<Bitmap>(w, h, PixelFormat32bppPARGB);
        Graphics g(display_.get());
        g.SetPixelOffsetMode(PixelOffsetModeHalf);
        HatchBrush checker(HatchStyleLargeCheckerBoard, Color(255, 64, 64, 68), Color(255, 48, 48, 52));
        g.FillRectangle(&checker, 0, 0, w, h);
        g.SetInterpolationMode(w > (int)adjusted_.w ? InterpolationModeHighQualityBilinear
                                                     : InterpolationModeHighQualityBicubic);
        g.DrawImage(view_.get(), RectF(0, 0, float(w), float(h)));
        g.SetSmoothingMode(SmoothingModeAntiAlias);
        g.SetTextRenderingHint(TextRenderingHintAntiAlias);
        g.ScaleTransform(scale_, scale_);
        for (const auto& s : shapes_) DrawShape(g, s);
        displayDirty_ = false;
        displayShapes_ = shapes_.size();
    }
    void InvalidateCanvas() {
        RectF c = CanvasRect();
        RECT r{LONG(c.X), LONG(c.Y), LONG(c.X + c.Width) + 1, LONG(c.Y + c.Height) + 1};
        InvalidateRect(hwnd_, &r, FALSE);
    }
    void BakeShapes(::Image& img) const {
        if (shapes_.empty()) return;
        Bitmap bmp(img.w, img.h, img.w * 4, PixelFormat32bppARGB, img.px.data());
        Graphics g(&bmp);
        g.SetSmoothingMode(SmoothingModeAntiAlias);
        g.SetTextRenderingHint(TextRenderingHintAntiAlias);
        g.SetPixelOffsetMode(PixelOffsetModeHalf);
        for (const auto& s : shapes_) DrawShape(g, s);
    }
    void ResetCrop() { crop_ = RectF(0, 0, float(orig_.w), float(orig_.h)); }
    bool CropIsFull() const {
        return crop_.X < 0.5f && crop_.Y < 0.5f && std::abs(crop_.Width - orig_.w) < 0.5f &&
               std::abs(crop_.Height - orig_.h) < 0.5f;
    }
    // What gets scaled: the crop, or the whole image.
    SizeF BaseSize() const {
        return CropIsFull() ? SizeF(float(orig_.w), float(orig_.h)) : SizeF(crop_.Width, crop_.Height);
    }
    // Size of the saved image: the base size times the scale factors.
    SIZE OutputSize() const {
        SizeF b = BaseSize();
        return SIZE{std::max(1L, std::lround(b.Width * scaleX_)), std::max(1L, std::lround(b.Height * scaleY_))};
    }
    bool Scaled() const {
        SIZE o = OutputSize();
        SizeF b = BaseSize();
        return o.cx != std::lround(b.Width) || o.cy != std::lround(b.Height);
    }
    bool Stretched() const { return std::abs(scaleX_ - scaleY_) > 0.005f; }
    bool OutputTooLarge() const {
        SIZE o = OutputSize();
        return uint32_t(o.cx) > kMaxScaledDim || uint32_t(o.cy) > kMaxScaledDim;
    }
    void SetScale(float pct) {
        scalePct_ = int(std::lround(pct));
        scaleX_ = scaleY_ = pct / 100.0f;
    }
    // Fits both the 100 % outline and the scaled frame in the canvas. Not called mid-drag,
    // so the handle stays under the cursor; the view refits on release.
    void FitResizeView() {
        RectF c = CanvasRect();
        SizeF b = BaseSize();
        float margin = 28 * s_;
        float w = b.Width * std::max(1.0f, scaleX_), h = b.Height * std::max(1.0f, scaleY_);
        resizeView_ = std::max(0.001f, std::min({(c.Width - margin * 2) / w, (c.Height - margin * 2) / h, 4.0f}));
    }
    // The scaled frame stays centred in the canvas.
    RectF ResizeFrame() const {
        RectF c = CanvasRect();
        SizeF b = BaseSize();
        float w = b.Width * scaleX_ * resizeView_, h = b.Height * scaleY_ * resizeView_;
        return RectF(c.X + (c.Width - w) / 2, c.Y + (c.Height - h) / 2, w, h);
    }
    int ResizeHit(float x, float y) const {
        auto hs = Handles(ResizeFrame());
        for (int i = 0; i < 8; ++i)
            if (std::abs(hs[i].X - x) < 12 * s_ && std::abs(hs[i].Y - y) < 12 * s_) return i;
        return -1;
    }
    // Corners (even handles) scale both axes by the same factor; sides stretch or shrink one axis.
    void ResizeDrag(float x, float y) {
        RectF c = CanvasRect();
        SizeF b = BaseSize();
        const float lo = kScaleMin / 100.0f, hi = kScaleMax / 100.0f;
        // Centred frame: the cursor's distance to the centre is half the new size.
        float sx = 2 * std::abs(x - (c.X + c.Width / 2)) / (b.Width * resizeView_);
        float sy = 2 * std::abs(y - (c.Y + c.Height / 2)) / (b.Height * resizeView_);
        if (resizeHit_ % 2 == 0) {
            float k = std::max(sx / resizeStart_.X, sy / resizeStart_.Y);
            float kMin = std::max(lo / resizeStart_.X, lo / resizeStart_.Y);
            float kMax = std::max(kMin, std::min(hi / resizeStart_.X, hi / resizeStart_.Y));
            k = std::clamp(k, kMin, kMax);
            scaleX_ = resizeStart_.X * k;
            scaleY_ = resizeStart_.Y * k;
        } else if (resizeHit_ == 3 || resizeHit_ == 7) {
            scaleX_ = std::clamp(sx, lo, hi);
        } else {
            scaleY_ = std::clamp(sy, lo, hi);
        }
        scalePct_ = std::clamp(int(std::lround(std::max(scaleX_, scaleY_) * 100)), kScaleMin, kScaleMax);
        SyncScalePresets();
    }
    void SyncScalePresets() {
        for (auto& b : buttons_)
            if (b.id >= ID_SCALE && b.id < ID_SCALE + int(std::size(kScalePresets)))
                b.active = !Stretched() && scalePct_ == kScalePresets[b.id - ID_SCALE];
    }
    void AutoCropNow() {
        uint32_t x, y, w, h;
        if (AutoCropRect(orig_, x, y, w, h)) crop_ = RectF(float(x), float(y), float(w), float(h));
    }
    void Transform(bool rotate) {
        BakeShapes(orig_);
        shapes_.clear();
        if (rotate) {
            orig_ = Rotate90(orig_);
        } else {
            for (uint32_t y = 0; y < orig_.h; ++y) {
                auto* row = reinterpret_cast<uint32_t*>(orig_.Row(y));
                std::reverse(row, row + orig_.w);
            }
        }
        ResetCrop();
        if (mask_ || detecting_) {  // the subject moved: detect it again
            mask_.reset();
            detecting_ = false;
            ++maskGen_;
            if (cutoutOn_) StartDetect();
        }
        RebuildPreview();
        UpdateView();
    }

    // --- background removal ---------------------------------------------------

    void EnableCutout() {
        cutoutOn_ = true;
        bgError_.clear();
        if (!mask_ && !detecting_) StartDetect();
    }

    // The model takes about a second: run it off the UI thread, the preview updates when it is done.
    void StartDetect() {
        detecting_ = true;
        uint64_t gen = ++maskGen_;
        auto img = std::make_shared<::Image>(orig_);
        HWND hwnd = hwnd_;
        std::thread([hwnd, gen, img] {
            winrt::init_apartment(winrt::apartment_type::multi_threaded);
            auto* r = new MaskResult;
            try {
                r->mask = std::make_shared<SubjectMask>(DetectSubject(*img));
            } catch (const AppError& e) {
                r->error = e.message;
            } catch (const std::exception&) {
                r->error = L"No se pudo detectar el sujeto";
            }
            if (!PostMessageW(hwnd, WM_MASK_READY, (WPARAM)gen, (LPARAM)r)) delete r;
            winrt::uninit_apartment();
        }).detach();
    }

    void OnMaskReady(uint64_t gen, MaskResult* raw) {
        std::unique_ptr<MaskResult> r(raw);
        if (gen != maskGen_) return;  // stale: the image changed meanwhile
        detecting_ = false;
        if (!r->error.empty()) {
            bgError_ = r->error;
            cutoutOn_ = false;
        } else {
            mask_ = r->mask;
        }
        RecomputeAdjust();
        Layout();
    }

    void Save() {
        CommitText();
        if (cutoutOn_ && !mask_) {
            MessageBoxW(hwnd_, L"Espera a que termine de detectar el sujeto.", L"FileTools", MB_OK | MB_ICONINFORMATION);
            return;
        }
        if (OutputTooLarge()) {
            std::wstring msg = L"La imagen escalada superaría " + std::to_wstring(kMaxScaledDim) +
                               L" px de lado. Baja la escala.";
            MessageBoxW(hwnd_, msg.c_str(), L"FileTools", MB_OK | MB_ICONINFORMATION);
            return;
        }
        HCURSOR old = SetCursor(LoadCursor(nullptr, IDC_WAIT));
        try {
            ::Image img = ApplyAdjust(orig_, adjust_);
            bool cut = cutoutOn_ && mask_;
            if (cut) {
                ApplyCutout(img, *mask_, cutout_);
                SaveCutoutSettings(cutout_);
            }
            BakeShapes(img);
            if (!CropIsFull()) {
                int x = std::clamp(int(std::lround(crop_.X)), 0, int(img.w) - 1);
                int y = std::clamp(int(std::lround(crop_.Y)), 0, int(img.h) - 1);
                int w = std::clamp(int(std::lround(crop_.Width)), 1, int(img.w) - x);
                int h = std::clamp(int(std::lround(crop_.Height)), 1, int(img.h) - y);
                ::Image c(w, h);
                for (int r = 0; r < h; ++r) memcpy(c.Row(r), img.Row(y + r) + x * 4, size_t(w) * 4);
                img = std::move(c);
            }
            // Scaling goes last so annotations and the crop keep their original pixel positions.
            bool scaled = Scaled();
            if (scaled) {
                SIZE o = OutputSize();
                img = Resize(img, uint32_t(o.cx), uint32_t(o.cy));
            }
            bool onlyScaled = scaled && !cut && adjust_.Neutral() && shapes_.empty() && CropIsFull();
            ImgFmt fmt;
            if (cut || !ParseImgFmt(ExtOf(file_), fmt) || !CanEncode(fmt)) fmt = ImgFmt::Png;
            fs::path out = OutPath(file_, ImgFmtExt(fmt), cut ? L"sin fondo" : onlyScaled ? L"escalado" : L"editado");
            EncodeOpts opts;
            opts.quality = 95;
            SaveImage(img, out, fmt, opts);
            SetCursor(old);
            RECT wr;
            GetWindowRect(hwnd_, &wr);
            OpenFloatShelf({out}, POINT{(wr.left + wr.right) / 2, (wr.top + wr.bottom) / 2});
            DestroyWindow(hwnd_);
        } catch (const AppError& e) {
            SetCursor(old);
            MessageBoxW(hwnd_, e.message.c_str(), L"No se pudo guardar", MB_OK | MB_ICONERROR);
        }
    }

    // --- layout ---------------------------------------------------------------

    void Layout() {
        UpdateView();
        buttons_.clear();
        sliders_.clear();
        float s = s_;
        RectF c = Client();
        // Toolbar: mode tabs on the left, cancel/save on the right.
        float tx = kPad * s, ty = 10 * s, th = 36 * s;
        const struct {
            int id;
            const wchar_t* label;
            wchar_t glyph;
            EditorMode m;
        } tabs[] = {{ID_TAB_ADJUST, L"Ajustes", 0xE9E9, EditorMode::Adjust},
                    {ID_TAB_ANNOTATE, L"Anotar", 0xE70F, EditorMode::Annotate},
                    {ID_TAB_CROP, L"Recortar", 0xE7A8, EditorMode::Crop},
                    {ID_TAB_BG, L"Fondo", kGlyphWand, EditorMode::Background},
                    {ID_TAB_RESIZE, L"Tamaño", kGlyphResize, EditorMode::Resize}};
        static_assert(std::size(tabs) == kTabCount);
        for (auto& t : tabs) {
            Button b{RectF(tx, ty, kTabW * s, th), t.label, t.glyph, t.id, mode_ == t.m};
            buttons_.push_back(b);
            tx += kTabStep * s;
        }
        buttons_.push_back({RectF(c.Width - kPad * s - 110 * s, ty, 110 * s, th), L"Guardar", 0xE74E, ID_SAVE, false, true});
        buttons_.push_back({RectF(c.Width - kPad * s - 228 * s, ty, 110 * s, th), L"Cancelar", 0, ID_CANCEL});

        RectF p = PanelRect();
        float x = p.X + kPad * s, w = p.Width - kPad * 2 * s, y = p.Y + kPad * s;
        titles_.clear();
        auto title = [&](const wchar_t* t) {
            titles_.push_back({PointF(x, y), t});
            y += 26 * s;
        };
        auto slider = [&](const wchar_t* label, int* v, int mn, int mx, bool adjust, int def = 0,
                          const wchar_t* unit = L"") {
            sliders_.push_back({RectF(x, y, w, 46 * s), label, v, mn, mx, adjust, def, unit});
            y += 54 * s;
        };
        auto fullButton = [&](const wchar_t* label, wchar_t glyph, int id) {
            buttons_.push_back({RectF(x, y, w, 36 * s), label, glyph, id});
            y += 44 * s;
        };
        if (mode_ == EditorMode::Adjust) {
            title(L"AJUSTES");
            slider(L"Brillo", &adjust_.brightness, -100, 100, true);
            slider(L"Contraste", &adjust_.contrast, -100, 100, true);
            slider(L"Saturación", &adjust_.saturation, -100, 100, true);
            slider(L"Temperatura", &adjust_.temperature, -100, 100, true);
            slider(L"Nitidez", &adjust_.sharpness, 0, 100, true);
            y += 6 * s;
            title(L"TRANSFORMAR");
            float half = (w - 8 * s) / 2;
            buttons_.push_back({RectF(x, y, half, 36 * s), L"Rotar", 0xE7AD, ID_ROTATE});
            buttons_.push_back({RectF(x + half + 8 * s, y, half, 36 * s), L"Voltear", 0xE8AB, ID_FLIP});
            y += 44 * s;
            fullButton(L"Restablecer", 0xE72C, ID_RESET_ADJUST);
        } else if (mode_ == EditorMode::Annotate) {
            title(L"HERRAMIENTA");
            const struct {
                Tool t;
                const wchar_t* label;
                wchar_t glyph;
            } tools[] = {{Tool::Pen, L"Lápiz", 0xEE56},    {Tool::Marker, L"Resaltar", 0xE7E6},
                         {Tool::Arrow, L"Flecha", 0xE72A}, {Tool::Rect, L"Rectángulo", 0xE739},
                         {Tool::Ellipse, L"Elipse", 0xEA3A}, {Tool::Text, L"Texto", 0xE8D2}};
            float cw = (w - 16 * s) / 3;
            for (int i = 0; i < 6; ++i) {
                RectF r(x + (i % 3) * (cw + 8 * s), y + (i / 3) * 66 * s, cw, 58 * s);
                buttons_.push_back({r, tools[i].label, tools[i].glyph, ID_TOOL + int(tools[i].t), tool_ == tools[i].t});
            }
            y += 140 * s;
            title(L"COLOR");
            float d = 28 * s, gap = (w - d * 7) / 6;
            for (int i = 0; i < 7; ++i) {
                Button b{RectF(x + i * (d + gap), y, d, d), L"", 0, ID_COLOR + i, color_ == i};
                b.swatch = &kSwatches[i];
                buttons_.push_back(b);
            }
            y += d + 18 * s;
            slider(L"Grosor", &stroke_, 1, 40, false);
            float half = (w - 8 * s) / 2;
            buttons_.push_back({RectF(x, y, half, 36 * s), L"Deshacer", 0xE7A7, ID_UNDO});
            buttons_.push_back({RectF(x + half + 8 * s, y, half, 36 * s), L"Borrar", 0xE74D, ID_CLEAR});
            y += 44 * s;
        } else if (mode_ == EditorMode::Background) {
            title(L"QUITAR FONDO");
            Button toggle{RectF(x, y, w, 36 * s), cutoutOn_ ? L"Fondo quitado" : L"Quitar fondo", kGlyphWand, ID_CUTOUT,
                          cutoutOn_};
            buttons_.push_back(toggle);
            y += 50 * s;
            slider(L"Umbral", &cutout_.threshold, 0, 100, true, 50);
            slider(L"Borde suave", &cutout_.softness, 0, 100, true, 50);
            fullButton(L"Restablecer", 0xE72C, ID_RESET_BG);
            infoY_ = y + 8 * s;
        } else if (mode_ == EditorMode::Resize) {
            title(L"ESCALA");
            slider(L"Escala", &scalePct_, kScaleMin, kScaleMax, false, 100, L" %");
            float half = (w - 8 * s) / 2;
            for (int i = 0; i < int(std::size(kScalePresets)); ++i) {
                RectF r(x + (i % 2) * (half + 8 * s), y + (i / 2) * 44 * s, half, 36 * s);
                std::wstring label = std::to_wstring(kScalePresets[i]) + L" %";
                buttons_.push_back({r, label, 0, ID_SCALE + i, !Stretched() && scalePct_ == kScalePresets[i]});
            }
            y += 140 * s;
            fullButton(L"Restablecer", 0xE72C, ID_RESET_SCALE);
            infoY_ = y + 8 * s;
        } else {
            title(L"PROPORCIÓN");
            float half = (w - 8 * s) / 2;
            for (int i = 0; i < 6; ++i) {
                RectF r(x + (i % 2) * (half + 8 * s), y + (i / 2) * 44 * s, half, 36 * s);
                buttons_.push_back({r, kAspects[i].label, 0, ID_ASPECT + i, aspect_ == i});
            }
            y += 140 * s;
            fullButton(L"Auto (bordes)", 0xE7A8, ID_AUTOCROP);
            fullButton(L"Restablecer", 0xE72C, ID_RESET_CROP);
            infoY_ = y + 8 * s;
        }
        InvalidateRect(hwnd_, nullptr, FALSE);
    }

    // --- painting -------------------------------------------------------------

    // Paints only the invalidated area into a persistent back buffer.
    void Paint(HDC hdc, const RECT& dirty) {
        RectF c = Client();
        int W = std::max(1, int(c.Width)), H = std::max(1, int(c.Height));
        if (!bufDC_ || W != bufW_ || H != bufH_) {
            ReleaseBuffer();
            bufDC_ = CreateCompatibleDC(hdc);
            bufBmp_ = CreateCompatibleBitmap(hdc, W, H);
            bufOld_ = SelectObject(bufDC_, bufBmp_);
            bufW_ = W;
            bufH_ = H;
        }
        {
            Graphics g(bufDC_);
            g.SetSmoothingMode(SmoothingModeAntiAlias);
            g.SetTextRenderingHint(TextRenderingHintClearTypeGridFit);
            g.SetPixelOffsetMode(PixelOffsetModeHalf);
            RectF d(float(dirty.left), float(dirty.top), float(dirty.right - dirty.left), float(dirty.bottom - dirty.top));
            g.SetClip(d);
            auto touches = [&](const RectF& r) { return r.IntersectsWith(d) ? true : false; };
            RectF canvas = CanvasRect(), panel = PanelRect(), bar(0, 0, c.Width, kToolbarH * s_);
            if (touches(canvas)) PaintCanvas(g);
            if (touches(panel)) PaintPanel(g);
            if (touches(bar)) PaintToolbar(g);
        }
        BitBlt(hdc, dirty.left, dirty.top, dirty.right - dirty.left, dirty.bottom - dirty.top, bufDC_, dirty.left,
               dirty.top, SRCCOPY);
    }
    void ReleaseBuffer() {
        if (!bufDC_) return;
        SelectObject(bufDC_, bufOld_);
        DeleteObject(bufBmp_);
        DeleteDC(bufDC_);
        bufDC_ = nullptr;
    }

    void PaintCanvas(Graphics& g) {
        RectF c = CanvasRect();
        GraphicsState outer = g.Save();
        g.IntersectClip(c);
        SolidBrush canvas(kCanvas);
        g.FillRectangle(&canvas, c);
        if (mode_ == EditorMode::Resize) {
            PaintResize(g);
            g.Restore(outer);
            return;
        }
        RectF img(off_.X, off_.Y, orig_.w * scale_, orig_.h * scale_);
        EnsureDisplay();
        g.SetInterpolationMode(InterpolationModeNearestNeighbor);
        g.SetCompositingQuality(CompositingQualityHighSpeed);
        g.DrawImage(display_.get(), off_.X, off_.Y, float(display_->GetWidth()), float(display_->GetHeight()));
        g.SetCompositingQuality(CompositingQualityHighQuality);

        if (drawing_) {
            GraphicsState st = g.Save();
            g.TranslateTransform(off_.X, off_.Y);
            g.ScaleTransform(scale_, scale_);
            g.SetTextRenderingHint(TextRenderingHintAntiAlias);
            DrawShape(g, current_);
            g.Restore(st);
        }

        if (detecting_) {
            Font bf(gfx::TextFamily(), 13 * s_, FontStyleBold, UnitPixel);
            std::wstring msg = L"Detectando el sujeto…";
            float tw = gfx::TextWidth(g, msg, bf) + 40 * s_, th = 40 * s_;
            RectF pill(c.X + (c.Width - tw) / 2, c.Y + (c.Height - th) / 2, tw, th);
            GraphicsPath pp;
            gfx::AddRoundRect(pp, pill, th / 2);
            SolidBrush bg(Color(220, 20, 20, 24));
            g.FillPath(&bg, &pp);
            SolidBrush fg(kText);
            gfx::DrawCentered(g, msg, bf, fg, pill);
        }

        // Crop overlay: always visible when cropped, with handles in crop mode.
        if (mode_ == EditorMode::Crop || !CropIsFull()) {
            PointF a = ToScreen(crop_.X, crop_.Y), b = ToScreen(crop_.X + crop_.Width, crop_.Y + crop_.Height);
            RectF cr(a.X, a.Y, b.X - a.X, b.Y - a.Y);
            Region outside(img);
            outside.Exclude(cr);
            SolidBrush dim(Color(mode_ == EditorMode::Crop ? 150 : 110, 0, 0, 0));
            g.FillRegion(&dim, &outside);
            if (mode_ == EditorMode::Crop) {
                Pen thirds(Color(90, 255, 255, 255), 1);
                for (int i = 1; i <= 2; ++i) {
                    g.DrawLine(&thirds, cr.X + cr.Width * i / 3, cr.Y, cr.X + cr.Width * i / 3, cr.Y + cr.Height);
                    g.DrawLine(&thirds, cr.X, cr.Y + cr.Height * i / 3, cr.X + cr.Width, cr.Y + cr.Height * i / 3);
                }
                Pen border(Color(255, 255, 255, 255), 1.5f * s_);
                g.DrawRectangle(&border, cr);
                SolidBrush handle(Color(255, 255, 255, 255));
                for (const auto& h : Handles(cr)) g.FillEllipse(&handle, h.X - 6 * s_, h.Y - 6 * s_, 12 * s_, 12 * s_);
            }
        }
        g.Restore(outer);
    }

    // Tamaño tab: the (cropped) image stretched into the output frame, over a dashed 100 % outline.
    void PaintResize(Graphics& g) {
        EnsureDisplay();
        RectF frame = ResizeFrame();
        RectF src(crop_.X * scale_, crop_.Y * scale_, crop_.Width * scale_, crop_.Height * scale_);
        ImageAttributes attrs;
        attrs.SetWrapMode(WrapModeTileFlipXY);  // no faded edges when stretching
        g.SetInterpolationMode(InterpolationModeHighQualityBilinear);
        g.DrawImage(display_.get(), frame, src.X, src.Y, src.Width, src.Height, UnitPixel, &attrs);

        SizeF b = BaseSize();
        RectF c = CanvasRect();
        float w = b.Width * resizeView_, h = b.Height * resizeView_;
        Pen outline(Color(150, 255, 255, 255), 1.0f * s_);
        outline.SetDashStyle(DashStyleDash);
        g.DrawRectangle(&outline, RectF(c.X + (c.Width - w) / 2, c.Y + (c.Height - h) / 2, w, h));
        Pen border(Color(255, 255, 255, 255), 1.5f * s_);
        g.DrawRectangle(&border, frame);
        SolidBrush handle(Color(255, 255, 255, 255));
        for (const auto& p : Handles(frame)) g.FillEllipse(&handle, p.X - 6 * s_, p.Y - 6 * s_, 12 * s_, 12 * s_);
    }

    std::vector<PointF> Handles(const RectF& r) const {
        float mx = r.X + r.Width / 2, my = r.Y + r.Height / 2, rx = r.X + r.Width, by = r.Y + r.Height;
        return {PointF(r.X, r.Y), PointF(mx, r.Y), PointF(rx, r.Y), PointF(rx, my),
                PointF(rx, by),   PointF(mx, by),  PointF(r.X, by), PointF(r.X, my)};
    }

    void PaintButton(Graphics& g, const Button& b) {
        bool hot = hot_ == b.id;
        if (b.swatch) {
            SolidBrush fill(*b.swatch);
            g.FillEllipse(&fill, b.r);
            Pen ring(b.active ? kText : Color(255, 80, 80, 88), (b.active ? 2.5f : 1.0f) * s_);
            float grow = b.active ? 3 * s_ : 0;
            g.DrawEllipse(&ring, b.r.X - grow, b.r.Y - grow, b.r.Width + grow * 2, b.r.Height + grow * 2);
            return;
        }
        GraphicsPath p;
        gfx::AddRoundRect(p, b.r, 8 * s_);
        Color fill = b.accent ? (hot ? kAccentHot : kAccent) : b.active ? Color(255, 42, 62, 90) : hot ? kBtnHot : kBtn;
        SolidBrush fb(fill);
        g.FillPath(&fb, &p);
        if (b.active) {
            Pen ring(kAccent, 1.5f * s_);
            g.DrawPath(&ring, &p);
        }
        SolidBrush text(kText);
        Font font(gfx::TextFamily(), 12.5f * s_, FontStyleRegular, UnitPixel);
        Font icon(gfx::IconFamily(), 15 * s_, FontStyleRegular, UnitPixel);
        bool tall = b.r.Height > 50 * s_;
        if (b.glyph && tall) {
            gfx::DrawCentered(g, std::wstring(1, b.glyph), icon, text, RectF(b.r.X, b.r.Y + 6 * s_, b.r.Width, 24 * s_));
            gfx::DrawCentered(g, b.label, font, text, RectF(b.r.X, b.r.Y + 30 * s_, b.r.Width, 22 * s_));
        } else if (b.glyph) {
            float tw = gfx::TextWidth(g, b.label, font) + 24 * s_;
            float x0 = b.r.X + (b.r.Width - tw) / 2;
            gfx::DrawCentered(g, std::wstring(1, b.glyph), icon, text, RectF(x0 - 4 * s_, b.r.Y, 22 * s_, b.r.Height));
            // Same centre as before, but room for DrawString's padding so labels are not cut short.
            gfx::DrawCentered(g, b.label, font, text, RectF(x0 + 12 * s_, b.r.Y, tw - 6 * s_, b.r.Height));
        } else {
            gfx::DrawCentered(g, b.label, font, text, b.r);
        }
    }

    void PaintToolbar(Graphics& g) {
        RectF c = Client();
        SolidBrush bar(kPanel);
        g.FillRectangle(&bar, RectF(0, 0, c.Width, kToolbarH * s_));
        Pen line(kLine, 1);
        g.DrawLine(&line, 0.0f, kToolbarH * s_ - 0.5f, c.Width, kToolbarH * s_ - 0.5f);
        for (const auto& b : buttons_)
            if (b.r.Y < kToolbarH * s_) PaintButton(g, b);
        SolidBrush sub(kSub);
        Font f(gfx::TextFamily(), 12 * s_, FontStyleRegular, UnitPixel);
        std::wstring info = file_.filename().wstring() + L"  ·  " + std::to_wstring(orig_.w) + L" × " +
                            std::to_wstring(orig_.h);
        float left = kPad * s_ + kTabCount * kTabStep * s_, right = c.Width - kPad * s_ - 240 * s_;
        if (right - left > 80 * s_) gfx::DrawCentered(g, info, f, sub, RectF(left, 0, right - left, kToolbarH * s_));
    }

    void PaintPanel(Graphics& g) {
        RectF p = PanelRect();
        SolidBrush panel(kPanel);
        g.FillRectangle(&panel, p);
        Pen line(kLine, 1);
        g.DrawLine(&line, p.X + 0.5f, p.Y, p.X + 0.5f, p.Y + p.Height);
        Font titleFont(gfx::TextFamily(), 11 * s_, FontStyleBold, UnitPixel);
        SolidBrush sub(kSub), text(kText);
        for (const auto& t : titles_) g.DrawString(t.second, -1, &titleFont, t.first, &sub);
        Font f(gfx::TextFamily(), 12.5f * s_, FontStyleRegular, UnitPixel);
        for (const auto& sl : sliders_) {
            g.DrawString(sl.label, -1, &f, PointF(sl.r.X, sl.r.Y), &text);
            std::wstring v = std::to_wstring(*sl.value) + sl.unit;
            StringFormat right;
            right.SetAlignment(StringAlignmentFar);
            g.DrawString(v.c_str(), -1, &f, RectF(sl.r.X, sl.r.Y, sl.r.Width, 20 * s_), &right, &sub);
            float ty = sl.r.Y + 32 * s_;
            float frac = float(*sl.value - sl.min) / (sl.max - sl.min);
            float zero = sl.min < 0 ? 0.5f : 0.0f;
            Pen track(Color(255, 70, 70, 78), 4 * s_);
            track.SetStartCap(LineCapRound);
            track.SetEndCap(LineCapRound);
            g.DrawLine(&track, sl.r.X, ty, sl.r.X + sl.r.Width, ty);
            Pen fill(kAccent, 4 * s_);
            g.DrawLine(&fill, sl.r.X + sl.r.Width * std::min(zero, frac), ty, sl.r.X + sl.r.Width * std::max(zero, frac), ty);
            SolidBrush knob(kText);
            float kx = sl.r.X + sl.r.Width * frac;
            g.FillEllipse(&knob, kx - 8 * s_, ty - 8 * s_, 16 * s_, 16 * s_);
        }
        for (const auto& b : buttons_)
            if (b.r.Y >= kToolbarH * s_) PaintButton(g, b);
        if (mode_ == EditorMode::Background) {
            std::wstring info = detecting_ ? L"Detectando el sujeto…"
                                : !bgError_.empty() ? bgError_
                                : L"Sube el umbral para quitar más fondo y bájalo para conservar más del sujeto. "
                                  L"El borde suave difumina el contorno.\n\nSe guarda como PNG con transparencia.";
            RectF box(p.X + kPad * s_, infoY_, p.Width - kPad * 2 * s_, p.Y + p.Height - infoY_ - kPad * s_);
            SolidBrush tone(!bgError_.empty() ? Color(255, 240, 110, 100) : kSub);
            g.DrawString(info.c_str(), -1, &f, box, nullptr, &tone);
        }
        if (mode_ == EditorMode::Crop) {
            std::wstring size = std::to_wstring(int(std::lround(crop_.Width))) + L" × " +
                                std::to_wstring(int(std::lround(crop_.Height))) + L" px";
            g.DrawString(size.c_str(), -1, &f, PointF(p.X + kPad * s_, infoY_), &sub);
        }
        if (mode_ == EditorMode::Resize) {
            SIZE o = OutputSize();
            std::wstring info = std::to_wstring(o.cx) + L" × " + std::to_wstring(o.cy) + L" px";
            if (Stretched())
                info += L"\nAncho " + std::to_wstring(std::lround(scaleX_ * 100)) + L" % · alto " +
                        std::to_wstring(std::lround(scaleY_ * 100)) + L" %";
            info += L"\n\nArrastra una esquina para escalar o un lado para estirar o encoger solo el ancho o el alto. "
                    L"La línea discontinua marca el tamaño original.";
            if (!CropIsFull()) info += L"\nSe escala la zona recortada.";
            if (OutputTooLarge()) info += L"\nDemasiado grande: máximo " + std::to_wstring(kMaxScaledDim) + L" px de lado.";
            RectF box(p.X + kPad * s_, infoY_, p.Width - kPad * 2 * s_, p.Y + p.Height - infoY_ - kPad * s_);
            SolidBrush tone(OutputTooLarge() ? Color(255, 240, 110, 100) : kSub);
            g.DrawString(info.c_str(), -1, &f, box, nullptr, &tone);
        }
    }

    // --- interaction -------------------------------------------------------------

    int ButtonAt(float x, float y) const {
        for (const auto& b : buttons_) {
            RectF r = b.r;
            if (x >= r.X && x <= r.X + r.Width && y >= r.Y && y <= r.Y + r.Height) return b.id;
        }
        return ID_NONE;
    }
    int SliderAt(float x, float y) const {
        for (size_t i = 0; i < sliders_.size(); ++i) {
            const RectF& r = sliders_[i].r;
            if (x >= r.X - 8 * s_ && x <= r.X + r.Width + 8 * s_ && y >= r.Y + 18 * s_ && y <= r.Y + r.Height)
                return int(i);
        }
        return -1;
    }
    void SetSlider(int i, float x) {
        Slider& sl = sliders_[i];
        float frac = std::clamp((x - sl.r.X) / sl.r.Width, 0.0f, 1.0f);
        int v = sl.min + int(std::lround(frac * (sl.max - sl.min)));
        if (v == *sl.value) return;
        *sl.value = v;
        if (sl.adjust) ScheduleAdjust();
        if (sl.value == &scalePct_) {
            // The slider is proportional: it also undoes a stretch. Presets are synced in place,
            // without a full Layout() on every drag step.
            SetScale(float(v));
            FitResizeView();
            SyncScalePresets();
            InvalidateCanvas();
        }
        RectF p = PanelRect();
        RECT r{LONG(p.X), LONG(p.Y), LONG(p.X + p.Width), LONG(p.Y + p.Height)};
        InvalidateRect(hwnd_, &r, FALSE);
    }
    bool InCanvas(float x, float y) const {
        RectF c = CanvasRect();
        return x >= c.X && x < c.X + c.Width && y >= c.Y && y < c.Y + c.Height;
    }

    void OnButton(int id) {
        if (id == ID_TAB_ADJUST || id == ID_TAB_ANNOTATE || id == ID_TAB_CROP || id == ID_TAB_BG ||
            id == ID_TAB_RESIZE) {
            CommitText();
            mode_ = id == ID_TAB_ADJUST     ? EditorMode::Adjust
                    : id == ID_TAB_ANNOTATE ? EditorMode::Annotate
                    : id == ID_TAB_CROP     ? EditorMode::Crop
                    : id == ID_TAB_RESIZE   ? EditorMode::Resize
                                            : EditorMode::Background;
            if (id == ID_TAB_BG && !cutoutOn_ && bgError_.empty()) {
                EnableCutout();
                RecomputeAdjust();
            }
        } else if (id == ID_CUTOUT) {
            if (cutoutOn_) cutoutOn_ = false;
            else EnableCutout();
            RecomputeAdjust();
        } else if (id == ID_RESET_BG) {
            cutout_ = CutoutSettings();
            RecomputeAdjust();
        } else if (id == ID_SAVE) {
            Save();
            return;
        } else if (id == ID_CANCEL) {
            DestroyWindow(hwnd_);
            return;
        } else if (id == ID_ROTATE || id == ID_FLIP) {
            Transform(id == ID_ROTATE);
        } else if (id == ID_RESET_ADJUST) {
            adjust_ = Adjust();
            RecomputeAdjust();
        } else if (id == ID_UNDO) {
            if (!shapes_.empty()) shapes_.pop_back();
        } else if (id == ID_CLEAR) {
            shapes_.clear();
        } else if (id == ID_AUTOCROP) {
            AutoCropNow();
        } else if (id == ID_RESET_CROP) {
            aspect_ = 0;
            ResetCrop();
        } else if (id >= ID_TOOL && id < ID_TOOL + 6) {
            CommitText();
            tool_ = Tool(id - ID_TOOL);
        } else if (id >= ID_COLOR && id < ID_COLOR + 7) {
            color_ = id - ID_COLOR;
        } else if (id >= ID_ASPECT && id < ID_ASPECT + 6) {
            aspect_ = id - ID_ASPECT;
            ApplyAspect();
        } else if (id >= ID_SCALE && id < ID_SCALE + int(std::size(kScalePresets))) {
            SetScale(float(kScalePresets[id - ID_SCALE]));
        } else if (id == ID_RESET_SCALE) {
            SetScale(100);
        }
        Layout();
    }

    float Ratio() const {
        float r = kAspects[aspect_].ratio;
        return r < 0 ? float(orig_.w) / orig_.h : r;
    }
    void ApplyAspect() {
        float r = Ratio();
        if (r <= 0) return;
        // Largest rect with that ratio centred on the current crop.
        float cx = crop_.X + crop_.Width / 2, cy = crop_.Y + crop_.Height / 2;
        float w = crop_.Width, h = w / r;
        if (h > crop_.Height) {
            h = crop_.Height;
            w = h * r;
        }
        crop_ = RectF(cx - w / 2, cy - h / 2, w, h);
        ClampCrop();
    }
    void ClampCrop() {
        crop_.Width = std::clamp(crop_.Width, 4.0f, float(orig_.w));
        crop_.Height = std::clamp(crop_.Height, 4.0f, float(orig_.h));
        crop_.X = std::clamp(crop_.X, 0.0f, orig_.w - crop_.Width);
        crop_.Y = std::clamp(crop_.Y, 0.0f, orig_.h - crop_.Height);
    }

    // Crop drag: 0..7 handles (see Handles()), 8 = move, 9 = new rect.
    int CropHit(float x, float y) const {
        PointF a = ToScreen(crop_.X, crop_.Y), b = ToScreen(crop_.X + crop_.Width, crop_.Y + crop_.Height);
        RectF cr(a.X, a.Y, b.X - a.X, b.Y - a.Y);
        auto hs = Handles(cr);
        for (int i = 0; i < 8; ++i)
            if (std::abs(hs[i].X - x) < 12 * s_ && std::abs(hs[i].Y - y) < 12 * s_) return i;
        if (x > cr.X && x < cr.X + cr.Width && y > cr.Y && y < cr.Y + cr.Height) return 8;
        return 9;
    }
    void CropDrag(PointF p) {
        float l = cropStart_.X, t = cropStart_.Y, r = l + cropStart_.Width, b = t + cropStart_.Height;
        float ratio = kAspects[aspect_].ratio != 0 ? Ratio() : 0;
        switch (cropHit_) {
            case 8:
                crop_ = RectF(l + p.X - dragStart_.X, t + p.Y - dragStart_.Y, cropStart_.Width, cropStart_.Height);
                ClampCrop();
                return;
            case 9: l = dragStart_.X; t = dragStart_.Y; r = p.X; b = p.Y; break;
            case 0: l = p.X; t = p.Y; break;
            case 1: t = p.Y; break;
            case 2: r = p.X; t = p.Y; break;
            case 3: r = p.X; break;
            case 4: r = p.X; b = p.Y; break;
            case 5: b = p.Y; break;
            case 6: l = p.X; b = p.Y; break;
            case 7: l = p.X; break;
        }
        if (ratio > 0) {
            // Keep the ratio: width drives height, anchored on the side that isn't being dragged.
            bool movesTop = cropHit_ == 0 || cropHit_ == 1 || cropHit_ == 2 || (cropHit_ == 9 && b < t);
            float w = std::abs(r - l);
            if (cropHit_ == 1 || cropHit_ == 5) w = std::abs(b - t) * ratio;
            float h = w / ratio;
            if (cropHit_ == 1 || cropHit_ == 5) {
                float cx = (l + r) / 2;
                l = cx - w / 2;
                r = cx + w / 2;
            }
            if (movesTop) t = b - (b >= t ? h : -h);
            else b = t + (b >= t ? h : -h);
        }
        crop_ = RectF(std::min(l, r), std::min(t, b), std::max(4.0f, std::abs(r - l)), std::max(4.0f, std::abs(b - t)));
        ClampCrop();
    }

    void BeginText(float x, float y) {
        CommitText();
        textPos_ = ToImage(x, y);
        float px = (14 + stroke_ * 1.6f) * s_;
        textFont_ = CreateFontW(-int(px), 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0,
                                L"Segoe UI");
        edit_ = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, int(x), int(y - px * 0.2f),
                                int(320 * s_), int(px * 1.5f), hwnd_, nullptr, GetModuleHandleW(nullptr), nullptr);
        SendMessageW(edit_, WM_SETFONT, (WPARAM)textFont_, TRUE);
        SetWindowSubclass(edit_, &Editor::EditProc, 1, (DWORD_PTR)this);
        SetFocus(edit_);
    }
    static LRESULT CALLBACK EditProc(HWND h, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR ref) {
        auto* self = reinterpret_cast<Editor*>(ref);
        if (msg == WM_KEYDOWN && wp == VK_RETURN) {
            PostMessageW(self->hwnd_, WM_COMMIT_TEXT, 0, 0);
            return 0;
        }
        if (msg == WM_KEYDOWN && wp == VK_ESCAPE) {
            PostMessageW(self->hwnd_, WM_CANCEL_TEXT, 0, 0);
            return 0;
        }
        if (msg == WM_CHAR && (wp == VK_RETURN || wp == VK_ESCAPE)) return 0;
        if (msg == WM_KILLFOCUS) PostMessageW(self->hwnd_, WM_COMMIT_TEXT, 0, 0);
        return DefSubclassProc(h, msg, wp, lp);
    }
    void CommitText(bool keep = true) {
        if (!edit_) return;
        int len = GetWindowTextLengthW(edit_);
        std::wstring text(len, L'\0');
        GetWindowTextW(edit_, text.data(), len + 1);
        HWND e = edit_;
        edit_ = nullptr;
        RemoveWindowSubclass(e, &Editor::EditProc, 1);
        DestroyWindow(e);
        DeleteObject(textFont_);
        textFont_ = nullptr;
        if (keep && !text.empty()) {
            Shape sh{Tool::Text, kSwatches[color_], (14 + stroke_ * 1.6f) * s_ / scale_, {textPos_}, text};
            shapes_.push_back(std::move(sh));
        }
        SetFocus(hwnd_);
        InvalidateRect(hwnd_, nullptr, FALSE);
    }

    LRESULT Handle(UINT msg, WPARAM wp, LPARAM lp) {
        switch (msg) {
            case WM_ERASEBKGND: return 1;
            case WM_PAINT: {
                PAINTSTRUCT ps;
                HDC hdc = BeginPaint(hwnd_, &ps);
                Paint(hdc, ps.rcPaint);
                EndPaint(hwnd_, &ps);
                return 0;
            }
            case WM_SIZE: Layout(); return 0;
            case WM_GETMINMAXINFO: {
                auto* mm = reinterpret_cast<MINMAXINFO*>(lp);
                // Wide enough for the five tabs plus Cancelar/Guardar without overlapping.
                mm->ptMinTrackSize = {LONG(840 * s_), LONG(560 * s_)};
                return 0;
            }
            case WM_DPICHANGED: {
                s_ = HIWORD(wp) / 96.0f;
                auto* r = reinterpret_cast<RECT*>(lp);
                SetWindowPos(hwnd_, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top,
                             SWP_NOZORDER | SWP_NOACTIVATE);
                Layout();
                return 0;
            }
            case WM_TIMER:
                if (wp == 1) {
                    KillTimer(hwnd_, 1);
                    adjustPending_ = false;
                    RecomputeAdjust();
                    InvalidateCanvas();
                }
                return 0;
            case WM_COMMIT_TEXT: CommitText(true); return 0;
            case WM_MASK_READY: OnMaskReady((uint64_t)wp, reinterpret_cast<MaskResult*>(lp)); return 0;
            case WM_CANCEL_TEXT: CommitText(false); return 0;
            case WM_SETCURSOR:
                if (LOWORD(lp) == HTCLIENT) {
                    POINT pt;
                    GetCursorPos(&pt);
                    ScreenToClient(hwnd_, &pt);
                    LPCWSTR cur = IDC_ARROW;
                    if (InCanvas(float(pt.x), float(pt.y))) {
                        if (mode_ == EditorMode::Annotate) cur = tool_ == Tool::Text ? IDC_IBEAM : IDC_CROSS;
                        static const LPCWSTR cursors[] = {IDC_SIZENWSE, IDC_SIZENS, IDC_SIZENESW, IDC_SIZEWE,
                                                          IDC_SIZENWSE, IDC_SIZENS, IDC_SIZENESW, IDC_SIZEWE,
                                                          IDC_SIZEALL,  IDC_CROSS};
                        if (mode_ == EditorMode::Crop) {
                            int h = CropHit(float(pt.x), float(pt.y));
                            cur = cursors[h];
                        } else if (mode_ == EditorMode::Resize) {
                            int h = resizeHit_ >= 0 ? resizeHit_ : ResizeHit(float(pt.x), float(pt.y));
                            if (h >= 0) cur = cursors[h];
                        }
                    } else if (ButtonAt(float(pt.x), float(pt.y)) || SliderAt(float(pt.x), float(pt.y)) >= 0) {
                        cur = IDC_HAND;
                    }
                    SetCursor(LoadCursor(nullptr, cur));
                    return TRUE;
                }
                break;
            case WM_MOUSEMOVE: {
                float x = float(GET_X_LPARAM(lp)), y = float(GET_Y_LPARAM(lp));
                if (activeSlider_ >= 0) {
                    SetSlider(activeSlider_, x);
                    return 0;
                }
                if (drawing_) {
                    PointF p = ToImage(x, y, false);
                    if (current_.tool == Tool::Pen || current_.tool == Tool::Marker) current_.pts.push_back(p);
                    else current_.pts.back() = p;
                    InvalidateCanvas();
                    return 0;
                }
                if (cropHit_ >= 0) {
                    CropDrag(ToImage(x, y));
                    InvalidateRect(hwnd_, nullptr, FALSE);  // canvas + size label
                    return 0;
                }
                if (resizeHit_ >= 0) {
                    ResizeDrag(x, y);
                    InvalidateRect(hwnd_, nullptr, FALSE);  // canvas + slider + size label
                    return 0;
                }
                int hot = ButtonAt(x, y);
                if (hot != hot_) {
                    hot_ = hot;
                    InvalidateRect(hwnd_, nullptr, FALSE);
                }
                return 0;
            }
            case WM_LBUTTONDOWN:
            case WM_LBUTTONDBLCLK: {
                float x = float(GET_X_LPARAM(lp)), y = float(GET_Y_LPARAM(lp));
                SetFocus(hwnd_);
                int sl = SliderAt(x, y);
                if (sl >= 0) {
                    if (msg == WM_LBUTTONDBLCLK && sliders_[sl].adjust) {
                        *sliders_[sl].value = sliders_[sl].def;
                        RecomputeAdjust();
                        InvalidateRect(hwnd_, nullptr, FALSE);
                        return 0;
                    }
                    activeSlider_ = sl;
                    SetCapture(hwnd_);
                    SetSlider(sl, x);
                    return 0;
                }
                pressed_ = ButtonAt(x, y);
                if (pressed_) return 0;
                if (!InCanvas(x, y)) return 0;
                if (mode_ == EditorMode::Annotate) {
                    if (tool_ == Tool::Text) {
                        BeginText(x, y);
                        return 0;
                    }
                    PointF p = ToImage(x, y, false);
                    current_ = Shape{tool_, kSwatches[color_], stroke_ * s_ / scale_, {p}};
                    if (tool_ != Tool::Pen && tool_ != Tool::Marker) current_.pts.push_back(p);
                    drawing_ = true;
                    SetCapture(hwnd_);
                } else if (mode_ == EditorMode::Crop) {
                    cropHit_ = CropHit(x, y);
                    if (cropHit_ == 8 && CropIsFull()) cropHit_ = 9;  // nothing to move yet: draw a new rect
                    dragStart_ = ToImage(x, y);
                    cropStart_ = crop_;
                    SetCapture(hwnd_);
                } else if (mode_ == EditorMode::Resize) {
                    resizeHit_ = ResizeHit(x, y);
                    if (resizeHit_ >= 0) {
                        resizeStart_ = PointF(scaleX_, scaleY_);
                        SetCapture(hwnd_);
                    }
                }
                return 0;
            }
            case WM_LBUTTONUP: {
                float x = float(GET_X_LPARAM(lp)), y = float(GET_Y_LPARAM(lp));
                ReleaseCapture();
                if (activeSlider_ >= 0) {
                    activeSlider_ = -1;
                    return 0;
                }
                if (drawing_) {
                    drawing_ = false;
                    bool tiny = current_.pts.size() >= 2 && current_.tool != Tool::Pen && current_.tool != Tool::Marker &&
                                std::abs(current_.pts[0].X - current_.pts[1].X) * scale_ < 3 &&
                                std::abs(current_.pts[0].Y - current_.pts[1].Y) * scale_ < 3;
                    if (!tiny) shapes_.push_back(std::move(current_));
                    InvalidateRect(hwnd_, nullptr, FALSE);
                    return 0;
                }
                if (cropHit_ >= 0) {
                    cropHit_ = -1;
                    InvalidateRect(hwnd_, nullptr, FALSE);
                    return 0;
                }
                if (resizeHit_ >= 0) {
                    resizeHit_ = -1;
                    Layout();  // refit the view to the new size and refresh the presets
                    return 0;
                }
                int id = ButtonAt(x, y);
                if (id && id == pressed_) OnButton(id);
                pressed_ = ID_NONE;
                return 0;
            }
            case WM_KEYDOWN:
                if (wp == 'Z' && GetKeyState(VK_CONTROL) < 0) {
                    OnButton(ID_UNDO);
                } else if (wp == 'S' && GetKeyState(VK_CONTROL) < 0) {
                    Save();
                } else if (wp == VK_RETURN) {
                    Save();
                } else if (wp == VK_ESCAPE) {
                    DestroyWindow(hwnd_);
                }
                return 0;
            case WM_DESTROY:
                if (edit_) CommitText(false);
                display_.reset();
                view_.reset();
                ReleaseBuffer();
                return 0;
        }
        return DefWindowProcW(hwnd_, msg, wp, lp);
    }

    HWND hwnd_ = nullptr;
    fs::path file_;
    EditorMode mode_;
    float s_ = 1;

    ::Image orig_, preview_, adjusted_;
    std::unique_ptr<Bitmap> view_;
    std::unique_ptr<Bitmap> display_;
    bool displayDirty_ = true, adjustPending_ = false;
    size_t displayShapes_ = 0;
    HDC bufDC_ = nullptr;
    HBITMAP bufBmp_ = nullptr;
    HGDIOBJ bufOld_ = nullptr;
    int bufW_ = 0, bufH_ = 0;
    Adjust adjust_;
    float scale_ = 1;
    PointF off_;

    std::vector<Shape> shapes_;
    Shape current_{};
    bool drawing_ = false;
    Tool tool_ = Tool::Arrow;
    int color_ = 0;
    int stroke_ = 6;

    RectF crop_, cropStart_;
    PointF dragStart_;
    int cropHit_ = -1;
    int aspect_ = 0;
    int scalePct_ = 100;          // slider value, proportional scale
    float scaleX_ = 1, scaleY_ = 1;  // actual factors: differ when a side handle stretched one axis
    float resizeView_ = 1;           // screen pixels per output pixel in the Tamaño tab
    int resizeHit_ = -1;             // handle being dragged (see Handles()), -1 = none
    PointF resizeStart_;             // scaleX_/scaleY_ when the drag began

    std::shared_ptr<SubjectMask> mask_;
    CutoutSettings cutout_;
    bool cutoutOn_ = false, detecting_ = false;
    uint64_t maskGen_ = 0;
    std::wstring bgError_;

    HWND edit_ = nullptr;
    HFONT textFont_ = nullptr;
    PointF textPos_;

    std::vector<Button> buttons_;
    std::vector<Slider> sliders_;
    std::vector<std::pair<PointF, const wchar_t*>> titles_;
    float infoY_ = 0;
    int hot_ = ID_NONE, pressed_ = ID_NONE, activeSlider_ = -1;
};

}  // namespace

void OpenEditor(const fs::path& file, EditorMode mode) {
    try {
        new Editor(file, mode);  // owned by its window, deleted on WM_NCDESTROY
    } catch (const AppError& e) {
        MessageBoxW(nullptr, e.message.c_str(), L"FileTools", MB_OK | MB_ICONERROR);
    }
}
