#include "capture.h"

#include "editor.h"
#include "gfx.h"
#include "image.h"
#include "shellutil.h"

#include <dwmapi.h>
#include <shlobj.h>
#include <windowsx.h>

#include <ctime>

using namespace Gdiplus;

namespace capture {
namespace {

constexpr wchar_t kClass[] = L"FileTools.Capture";
// The launcher wheel is hidden when its action runs; this gives the compositor time to take it
// off the screen before the screen is frozen.
constexpr UINT kStartDelayMs = 120;

// Geometry at 96 DPI.
constexpr float kHandle = 9;     // side of the square resize handles
constexpr float kGrab = 7;       // distance from an edge that still grabs it
constexpr float kBarH = 40;      // toolbar height
constexpr float kGap = 8;        // between the selection and the toolbar or size label
constexpr int kClickSlop = 4;    // a press that moves less than this is a click, not a drag
constexpr BYTE kDim = 115;       // darkening of the frozen screen outside the selection

const Color kAccent(255, 0, 120, 215);
const Color kPanel(238, 28, 28, 32);

enum Edge : int { kNone = 0, kLeft = 1, kTop = 2, kRight = 4, kBottom = 8, kInside = 16 };
enum class Drag { None, New, Move, Resize };
enum class Command { Copy, Save, Edit, Cancel, CopyAndSave };

struct Button {
    Command command;
    wchar_t glyph;
    const wchar_t* label;  // empty: icon only
    RectF rect;
};

constexpr wchar_t kGlyphCopy = 0xE8C8;
constexpr wchar_t kGlyphSave = 0xE74E;
constexpr wchar_t kGlyphEdit = 0xE70F;
constexpr wchar_t kGlyphClose = 0xE8BB;

// A 32bpp top-down DIB selected into its own memory DC.
class DibCanvas {
public:
    DibCanvas(int width, int height) : w_(width), h_(height) {
        BITMAPINFO bi{};
        bi.bmiHeader = {sizeof(bi.bmiHeader), width, -height, 1, 32, BI_RGB};
        HDC screen = GetDC(nullptr);
        dc_ = CreateCompatibleDC(screen);
        void* bits = nullptr;
        bitmap_ = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
        ReleaseDC(nullptr, screen);
        if (!bitmap_) {
            DeleteDC(dc_);
            Fail(L"No hay memoria para capturar la pantalla");
        }
        bits_ = static_cast<uint8_t*>(bits);
        old_ = SelectObject(dc_, bitmap_);
    }
    DibCanvas(const DibCanvas&) = delete;
    DibCanvas& operator=(const DibCanvas&) = delete;
    ~DibCanvas() {
        SelectObject(dc_, old_);
        DeleteObject(bitmap_);
        DeleteDC(dc_);
    }

    HDC Dc() const { return dc_; }
    const uint8_t* Bits() const { return bits_; }
    int Width() const { return w_; }
    int Height() const { return h_; }

private:
    int w_, h_;
    HDC dc_ = nullptr;
    HBITMAP bitmap_ = nullptr;
    HGDIOBJ old_ = nullptr;
    uint8_t* bits_ = nullptr;
};

bool IsCloaked(HWND hwnd) {
    DWORD cloaked = 0;
    return SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) && cloaked;
}

RECT MonitorRectAt(POINT screen) {
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(MonitorFromPoint(screen, MONITOR_DEFAULTTONEAREST), &mi);
    return mi.rcMonitor;
}

// Visible frame of the topmost real window under `screen`, skipping FileTools' own windows. The
// desktop itself counts as the monitor it is on.
RECT WindowRectAt(POINT screen) {
    struct Search {
        POINT pt;
        RECT found;
        bool ok;
    } search{screen, {}, false};
    EnumWindows(
        [](HWND hwnd, LPARAM lp) -> BOOL {
            auto& s = *reinterpret_cast<Search*>(lp);
            DWORD pid = 0;
            GetWindowThreadProcessId(hwnd, &pid);
            if (pid == GetCurrentProcessId() || !IsWindowVisible(hwnd) || IsIconic(hwnd) || IsCloaked(hwnd)) return TRUE;
            if (GetWindowLongW(hwnd, GWL_EXSTYLE) & WS_EX_TRANSPARENT) return TRUE;
            RECT r{};
            // The extended frame leaves out the invisible resize borders Windows 10/11 add around windows.
            if (FAILED(DwmGetWindowAttribute(hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, &r, sizeof(r)))) GetWindowRect(hwnd, &r);
            if (IsRectEmpty(&r) || !PtInRect(&r, s.pt)) return TRUE;
            wchar_t cls[32] = {};
            GetClassNameW(hwnd, cls, 32);
            s.found = !wcscmp(cls, L"Progman") || !wcscmp(cls, L"WorkerW") ? MonitorRectAt(s.pt) : r;
            s.ok = true;
            return FALSE;
        },
        reinterpret_cast<LPARAM>(&search));
    return search.ok ? search.found : MonitorRectAt(screen);
}

fs::path KnownFolderPath(REFKNOWNFOLDERID id) {
    PWSTR raw = nullptr;
    fs::path result;
    if (SUCCEEDED(SHGetKnownFolderPath(id, KF_FLAG_CREATE, nullptr, &raw))) result = raw;
    CoTaskMemFree(raw);
    return result;
}

// Same place Windows keeps its own screenshots unless the settings send results to Downloads or
// the Desktop.
fs::path NewCapturePath() {
    fs::path base = KnownFolderPath(FOLDERID_Screenshots);
    if (base.empty()) base = KnownFolderPath(FOLDERID_Pictures);
    fs::path dir = OutputDir(base / L"captura");
    std::error_code ec;
    fs::create_directories(dir, ec);
    wchar_t stamp[32];
    std::time_t now = std::time(nullptr);
    std::tm local{};
    localtime_s(&local, &now);
    wcsftime(stamp, 32, L"%Y-%m-%d %H.%M.%S", &local);
    return UniquePath(dir, std::wstring(L"Captura ") + stamp, L"png");
}

std::wstring SizeText(const RECT& r) {
    return std::to_wstring(r.right - r.left) + L" × " + std::to_wstring(r.bottom - r.top);
}

class Overlay {
public:
    void Create(HINSTANCE inst, Notifier notifier) {
        inst_ = inst;
        notifier_ = std::move(notifier);
        WNDCLASSEXW wc{sizeof(wc)};
        wc.style = CS_DBLCLKS;
        wc.lpfnWndProc = &Overlay::Proc;
        wc.hInstance = inst;
        wc.lpszClassName = kClass;
        RegisterClassExW(&wc);
    }

    void Destroy() { Close(); }

    void Start(Target target, POINT at) {
        if (hwnd_) {
            SetForegroundWindow(hwnd_);
            return;
        }
        if (pending_) return;
        target_ = target;
        at_ = at;
        pending_ = SetTimer(nullptr, 0, kStartDelayMs, &Overlay::OnStartTimer);
    }

private:
    static void CALLBACK OnStartTimer(HWND, UINT, UINT_PTR id, DWORD);
    static LRESULT CALLBACK Proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    LRESULT Handle(UINT msg, WPARAM wp, LPARAM lp);

    void Begin() {
        KillTimer(nullptr, pending_);
        pending_ = 0;
        try {
            Freeze();
        } catch (const AppError& e) {
            if (notifier_) notifier_(JobResult{false, {}, e.message});
            return;
        }
        s_ = gfx::DpiScaleAt(at_);
        drag_ = Drag::None;
        hoverButton_ = pressedButton_ = -1;
        hasSel_ = target_ != Target::Region;
        if (target_ == Target::Window) sel_ = ToClient(WindowRectAt(at_));
        else if (target_ == Target::Screen) sel_ = ToClient(MonitorRectAt(at_));
        ClampSelection();
        LayoutToolbar();
        hwnd_ = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, kClass, L"Captura de pantalla", WS_POPUP, origin_.x,
                                origin_.y, shot_->Width(), shot_->Height(), nullptr, nullptr, inst_, nullptr);
        if (!hwnd_) {
            shot_.reset();
            return;
        }
        ShowWindow(hwnd_, SW_SHOW);
        SetFocus(hwnd_);
        shownBounds_ = Bounds();
    }

    // Copies the whole virtual desktop (every monitor) so the overlay can show it frozen.
    void Freeze() {
        origin_ = {GetSystemMetrics(SM_XVIRTUALSCREEN), GetSystemMetrics(SM_YVIRTUALSCREEN)};
        int w = GetSystemMetrics(SM_CXVIRTUALSCREEN), h = GetSystemMetrics(SM_CYVIRTUALSCREEN);
        DwmFlush();
        shot_ = std::make_unique<DibCanvas>(w, h);
        HDC screen = GetDC(nullptr);
        // CAPTUREBLT includes layered windows (tooltips, menus, other apps' overlays).
        BitBlt(shot_->Dc(), 0, 0, w, h, screen, origin_.x, origin_.y, SRCCOPY | CAPTUREBLT);
        ReleaseDC(nullptr, screen);
        GdiFlush();
    }

    void Close() {
        if (pending_) {
            KillTimer(nullptr, pending_);
            pending_ = 0;
        }
        HWND hwnd = hwnd_;
        hwnd_ = nullptr;
        if (hwnd) DestroyWindow(hwnd);
        shot_.reset();
    }

    RECT ToClient(RECT r) const {
        OffsetRect(&r, -origin_.x, -origin_.y);
        return r;
    }

    void ClampSelection() {
        if (!hasSel_) return;
        RECT all{0, 0, shot_->Width(), shot_->Height()};
        IntersectRect(&sel_, &sel_, &all);
        if (IsRectEmpty(&sel_)) hasSel_ = false;
    }

    // --- layout --------------------------------------------------------------------

    // Rect of the monitor holding `client`, in client coordinates: labels and the toolbar stay on it.
    RectF MonitorAt(POINT client) const {
        RECT m = ToClient(MonitorRectAt(POINT{client.x + origin_.x, client.y + origin_.y}));
        return RectF(float(m.left), float(m.top), float(m.right - m.left), float(m.bottom - m.top));
    }

    POINT SelCenter() const { return {(sel_.left + sel_.right) / 2, (sel_.top + sel_.bottom) / 2}; }

    Font LabelFont() const { return Font(gfx::TextFamily(), 12.5f * s_, FontStyleBold, UnitPixel); }

    float MeasureText(const std::wstring& text, const Font& font) const {
        Bitmap probe(1, 1, PixelFormat32bppARGB);
        Graphics g(&probe);
        return gfx::TextWidth(g, text, font);
    }

    RectF SizeLabelRect() const {
        Font font = LabelFont();
        float w = MeasureText(SizeText(sel_), font) + 20 * s_, h = 26 * s_;
        RectF mon = MonitorAt(SelCenter());
        float x = std::clamp(float(sel_.left), mon.X, mon.GetRight() - w);
        float y = sel_.top - kGap * s_ - h;
        if (y < mon.Y) y = sel_.top + kGap * s_;  // no room above: inside the top-left corner
        return RectF(x, y, w, h);
    }

    void LayoutToolbar() {
        buttons_ = {{Command::Copy, kGlyphCopy, L"Copiar", {}},
                    {Command::Save, kGlyphSave, L"Guardar", {}},
                    {Command::Edit, kGlyphEdit, L"Editar", {}},
                    {Command::Cancel, kGlyphClose, L"", {}}};
        if (!hasSel_) return;
        Font font = LabelFont();
        float pad = 6 * s_, h = kBarH * s_, bh = h - 2 * pad;
        float total = pad;
        std::vector<float> widths;
        for (const Button& b : buttons_) {
            float w = *b.label ? 12 * s_ + 18 * s_ + 6 * s_ + MeasureText(b.label, font) + 12 * s_ : bh;
            widths.push_back(w);
            total += w + pad / 2;
        }
        total += pad / 2;
        RectF mon = MonitorAt(SelCenter());
        float x = std::clamp(float(sel_.right) - total, mon.X + kGap * s_, mon.GetRight() - total - kGap * s_);
        float y = sel_.bottom + kGap * s_;
        if (y + h > mon.GetBottom()) y = sel_.top - kGap * s_ - h;      // no room below: above
        if (y < mon.Y) y = sel_.bottom - kGap * s_ - h;                  // nor above: inside
        bar_ = RectF(x, y, total, h);
        float bx = x + pad;
        for (size_t i = 0; i < buttons_.size(); ++i) {
            buttons_[i].rect = RectF(bx, y + pad, widths[i], bh);
            bx += widths[i] + pad / 2;
        }
    }

    RectF HintRect() const {
        RectF mon = MonitorAt(POINT{at_.x - origin_.x, at_.y - origin_.y});
        float w = MeasureText(HintText(), LabelFont()) + 32 * s_, h = 36 * s_;
        return RectF(mon.X + (mon.Width - w) / 2, mon.Y + 28 * s_, w, h);
    }

    static const wchar_t* HintText() {
        return L"Arrastra para elegir la zona  ·  clic en una ventana para capturarla  ·  Esc para salir";
    }

    bool ShowHandles() const { return hasSel_ && drag_ != Drag::New; }
    bool ShowToolbar() const { return hasSel_ && drag_ == Drag::None; }
    bool ShowHint() const { return !hasSel_ && drag_ == Drag::None; }

    // Everything drawn on top of the frozen screen, so a change only repaints what moved.
    RECT Bounds() const {
        RECT b{};
        auto add = [&](RectF r) {
            RECT ir{LONG(std::floor(r.X)) - 3, LONG(std::floor(r.Y)) - 3, LONG(std::ceil(r.GetRight())) + 3,
                    LONG(std::ceil(r.GetBottom())) + 3};
            UnionRect(&b, &b, &ir);
        };
        if (hasSel_) {
            float m = kHandle * s_;
            add(RectF(sel_.left - m, sel_.top - m, sel_.right - sel_.left + 2 * m, sel_.bottom - sel_.top + 2 * m));
            add(SizeLabelRect());
            if (ShowToolbar()) add(bar_);
        }
        if (ShowHint()) add(HintRect());
        return b;
    }

    // Repaints the old and the new decorations; the dimming changes exactly within those too.
    void Refresh() {
        if (!hwnd_) return;
        LayoutToolbar();
        RECT now = Bounds(), both;
        UnionRect(&both, &now, &shownBounds_);
        InvalidateRect(hwnd_, &both, FALSE);
        shownBounds_ = now;
    }

    // --- hit testing ---------------------------------------------------------------

    int EdgesAt(POINT p) const {
        if (!hasSel_) return kNone;
        int g = std::max(3, int(kGrab * s_));
        bool inX = p.x >= sel_.left - g && p.x <= sel_.right + g;
        bool inY = p.y >= sel_.top - g && p.y <= sel_.bottom + g;
        int dl = std::abs(p.x - sel_.left), dr = std::abs(p.x - sel_.right);
        int dt = std::abs(p.y - sel_.top), db = std::abs(p.y - sel_.bottom);
        int edges = kNone;
        // On a tiny selection both edges are in reach; the nearer one wins.
        if (inY && std::min(dl, dr) <= g) edges |= dl <= dr ? kLeft : kRight;
        if (inX && std::min(dt, db) <= g) edges |= dt <= db ? kTop : kBottom;
        if (!edges && PtInRect(&sel_, p)) edges = kInside;
        return edges;
    }

    int ButtonAt(POINT p) const {
        if (!ShowToolbar()) return -1;
        for (size_t i = 0; i < buttons_.size(); ++i)
            if (buttons_[i].rect.Contains(float(p.x), float(p.y))) return int(i);
        return -1;
    }

    LPCWSTR CursorFor(POINT p) const {
        if (drag_ == Drag::Move) return IDC_SIZEALL;
        if (drag_ == Drag::New) return IDC_CROSS;
        if (ButtonAt(p) >= 0 || (ShowToolbar() && bar_.Contains(float(p.x), float(p.y)))) return IDC_HAND;
        int e = drag_ == Drag::Resize ? resizeEdges_ : EdgesAt(p);
        if (e == kInside) return IDC_SIZEALL;
        if (e == (kLeft | kTop) || e == (kRight | kBottom)) return IDC_SIZENWSE;
        if (e == (kRight | kTop) || e == (kLeft | kBottom)) return IDC_SIZENESW;
        if (e & (kLeft | kRight)) return IDC_SIZEWE;
        if (e & (kTop | kBottom)) return IDC_SIZENS;
        return IDC_CROSS;
    }

    // --- mouse and keyboard --------------------------------------------------------

    void OnButtonDown(POINT p) {
        SetCapture(hwnd_);
        dragStart_ = p;
        selStart_ = sel_;
        moved_ = false;
        pressedButton_ = ButtonAt(p);
        if (pressedButton_ >= 0) return;
        if (ShowToolbar() && bar_.Contains(float(p.x), float(p.y))) return;  // gap between buttons
        int e = EdgesAt(p);
        if (e == kInside) {
            drag_ = Drag::Move;
        } else if (e != kNone) {
            drag_ = Drag::Resize;
            resizeEdges_ = e;
        } else {
            drag_ = Drag::New;
        }
    }

    void OnMouseMove(POINT p) {
        if (!moved_ && (std::abs(p.x - dragStart_.x) > kClickSlop || std::abs(p.y - dragStart_.y) > kClickSlop))
            moved_ = true;
        int dx = p.x - dragStart_.x, dy = p.y - dragStart_.y;
        int w = shot_->Width(), h = shot_->Height();
        switch (drag_) {
            case Drag::None: {
                int hover = ButtonAt(p);
                if (hover != hoverButton_) {
                    hoverButton_ = hover;
                    Refresh();
                }
                return;
            }
            case Drag::New:
                if (!moved_) return;
                hasSel_ = true;
                sel_ = {std::min(dragStart_.x, p.x), std::min(dragStart_.y, p.y), std::max(dragStart_.x, p.x) + 1,
                        std::max(dragStart_.y, p.y) + 1};
                break;
            case Drag::Move: {
                int sw = selStart_.right - selStart_.left, sh = selStart_.bottom - selStart_.top;
                int x = std::clamp(int(selStart_.left) + dx, 0, w - sw), y = std::clamp(int(selStart_.top) + dy, 0, h - sh);
                sel_ = {x, y, x + sw, y + sh};
                break;
            }
            case Drag::Resize:
                sel_ = selStart_;
                // An edge stops one pixel short of the opposite one instead of flipping the selection.
                if (resizeEdges_ & kLeft) sel_.left = std::clamp(int(selStart_.left) + dx, 0, int(sel_.right) - 1);
                if (resizeEdges_ & kRight) sel_.right = std::clamp(int(selStart_.right) + dx, int(sel_.left) + 1, w);
                if (resizeEdges_ & kTop) sel_.top = std::clamp(int(selStart_.top) + dy, 0, int(sel_.bottom) - 1);
                if (resizeEdges_ & kBottom) sel_.bottom = std::clamp(int(selStart_.bottom) + dy, int(sel_.top) + 1, h);
                break;
        }
        ClampSelection();
        Refresh();
    }

    void OnButtonUp(POINT p) {
        ReleaseCapture();
        Drag drag = drag_;
        drag_ = Drag::None;
        if (pressedButton_ >= 0) {
            int pressed = pressedButton_;
            pressedButton_ = -1;
            if (ButtonAt(p) == pressed) Execute(buttons_[pressed].command);
            return;
        }
        // A click outside the selection picks the window under it, like Windows' own snipping.
        if (drag == Drag::New && !moved_) {
            sel_ = ToClient(WindowRectAt(POINT{p.x + origin_.x, p.y + origin_.y}));
            hasSel_ = true;
            ClampSelection();
        }
        Refresh();
    }

    void OnKey(UINT vk) {
        bool ctrl = GetKeyState(VK_CONTROL) < 0, shift = GetKeyState(VK_SHIFT) < 0;
        switch (vk) {
            case VK_ESCAPE: Close(); return;
            case VK_RETURN:
                if (hasSel_) Execute(Command::CopyAndSave);
                return;
            case 'C':
                if (ctrl && hasSel_) Execute(Command::Copy);
                return;
            case 'S':
                if (ctrl && hasSel_) Execute(Command::Save);
                return;
            case 'E':
                if (ctrl && hasSel_) Execute(Command::Edit);
                return;
            case VK_LEFT:
            case VK_RIGHT:
            case VK_UP:
            case VK_DOWN: Nudge(vk, shift ? 10 : 1, ctrl); return;
        }
    }

    // Arrows move the selection; with Ctrl they move its right or bottom edge instead.
    void Nudge(UINT vk, int step, bool resize) {
        if (!hasSel_ || drag_ != Drag::None) return;
        int dx = vk == VK_LEFT ? -step : vk == VK_RIGHT ? step : 0;
        int dy = vk == VK_UP ? -step : vk == VK_DOWN ? step : 0;
        int w = shot_->Width(), h = shot_->Height();
        if (resize) {
            sel_.right = std::clamp(int(sel_.right) + dx, int(sel_.left) + 1, w);
            sel_.bottom = std::clamp(int(sel_.bottom) + dy, int(sel_.top) + 1, h);
        } else {
            dx = std::clamp(dx, -int(sel_.left), w - int(sel_.right));
            dy = std::clamp(dy, -int(sel_.top), h - int(sel_.bottom));
            OffsetRect(&sel_, dx, dy);
        }
        Refresh();
    }

    // --- output --------------------------------------------------------------------

    ::Image Crop() const {
        int w = sel_.right - sel_.left, h = sel_.bottom - sel_.top;
        ::Image img(w, h);
        for (int y = 0; y < h; ++y) {
            const uint8_t* src = shot_->Bits() + (size_t(sel_.top + y) * shot_->Width() + sel_.left) * 4;
            uint8_t* dst = img.Row(y);
            memcpy(dst, src, size_t(w) * 4);
            // BitBlt leaves the alpha byte at zero; the screen is opaque.
            for (int x = 0; x < w; ++x) dst[x * 4 + 3] = 255;
        }
        return img;
    }

    void Execute(Command command) {
        if (command == Command::Cancel) {
            Close();
            return;
        }
        bool copy = command == Command::Copy || command == Command::CopyAndSave;
        bool save = command != Command::Copy;  // editing needs the file too
        JobResult r;
        fs::path out;
        try {
            ::Image img = Crop();
            if (copy && !CopyImageToClipboard(hwnd_, img)) Fail(L"No se pudo acceder al portapapeles");
            if (save) {
                out = NewCapturePath();
                SaveImage(img, out, ImgFmt::Png);
                r.outputs = {out};
            }
            r.message = (copy ? L"Copiada al portapapeles · " : L"") + SizeText(sel_);
        } catch (const AppError& e) {
            r = JobResult{false, {}, e.message};
        }
        Close();
        if (command == Command::Edit && r.ok) OpenEditor(out, EditorMode::Annotate);
        else if (notifier_ && (!r.ok || !r.outputs.empty())) notifier_(r);
    }

    // --- drawing -------------------------------------------------------------------

    void Paint(HDC target, const RECT& area) {
        int w = area.right - area.left, h = area.bottom - area.top;
        if (w <= 0 || h <= 0 || !shot_) return;
        DibCanvas back(w, h);
        BitBlt(back.Dc(), 0, 0, w, h, shot_->Dc(), area.left, area.top, SRCCOPY);
        {
            Graphics g(back.Dc());
            g.TranslateTransform(float(-area.left), float(-area.top));
            DrawDimming(g);
            g.SetSmoothingMode(SmoothingModeAntiAlias);
            g.SetPixelOffsetMode(PixelOffsetModeHalf);
            g.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);
            if (hasSel_) {
                DrawFrame(g);
                DrawPill(g, SizeText(sel_), SizeLabelRect());
            }
            if (ShowToolbar()) DrawToolbar(g);
            if (ShowHint()) DrawPill(g, HintText(), HintRect());
        }
        BitBlt(target, area.left, area.top, w, h, back.Dc(), 0, 0, SRCCOPY);
    }

    void DrawDimming(Graphics& g) {
        SolidBrush dim(Color(kDim, 0, 0, 0));
        float W = float(shot_->Width()), H = float(shot_->Height());
        if (!hasSel_) {
            g.FillRectangle(&dim, 0.0f, 0.0f, W, H);
            return;
        }
        float l = float(sel_.left), t = float(sel_.top), r = float(sel_.right), b = float(sel_.bottom);
        g.FillRectangle(&dim, 0.0f, 0.0f, W, t);
        g.FillRectangle(&dim, 0.0f, b, W, H - b);
        g.FillRectangle(&dim, 0.0f, t, l, b - t);
        g.FillRectangle(&dim, r, t, W - r, b - t);
    }

    void DrawFrame(Graphics& g) {
        RectF r(float(sel_.left), float(sel_.top), float(sel_.right - sel_.left), float(sel_.bottom - sel_.top));
        Pen outline(kAccent, std::max(1.0f, 1.5f * s_));
        g.DrawRectangle(&outline, r.X - 0.5f, r.Y - 0.5f, r.Width + 1, r.Height + 1);
        if (!ShowHandles()) return;
        float hs = kHandle * s_;
        SolidBrush fill(Color(255, 255, 255, 255));
        Pen border(kAccent, std::max(1.0f, 1.2f * s_));
        // Mid-edge handles only when the side is long enough to tell them from the corners.
        bool midX = r.Width > hs * 3, midY = r.Height > hs * 3;
        float xs[] = {r.X, r.X + r.Width / 2, r.GetRight()}, ys[] = {r.Y, r.Y + r.Height / 2, r.GetBottom()};
        for (int ix = 0; ix < 3; ++ix)
            for (int iy = 0; iy < 3; ++iy) {
                if (ix == 1 && iy == 1) continue;
                if ((ix == 1 && !midX) || (iy == 1 && !midY)) continue;
                RectF hr(xs[ix] - hs / 2, ys[iy] - hs / 2, hs, hs);
                g.FillRectangle(&fill, hr);
                g.DrawRectangle(&border, hr);
            }
    }

    void DrawPill(Graphics& g, const std::wstring& text, const RectF& r) {
        GraphicsPath p;
        gfx::AddRoundRect(p, r, std::min(r.Height / 2, 10 * s_));
        SolidBrush bg(kPanel);
        g.FillPath(&bg, &p);
        SolidBrush white(Color(255, 255, 255, 255));
        gfx::DrawCentered(g, text, LabelFont(), white, r);
    }

    void DrawToolbar(Graphics& g) {
        GraphicsPath p;
        gfx::AddRoundRect(p, bar_, 10 * s_);
        SolidBrush bg(kPanel);
        g.FillPath(&bg, &p);
        Font font = LabelFont();
        SolidBrush white(Color(255, 255, 255, 255));
        for (size_t i = 0; i < buttons_.size(); ++i) {
            const Button& b = buttons_[i];
            bool hot = int(i) == hoverButton_;
            if (hot) {
                GraphicsPath hp;
                gfx::AddRoundRect(hp, b.rect, 7 * s_);
                SolidBrush hb(b.command == Command::Cancel ? Color(255, 196, 43, 28) : kAccent);
                g.FillPath(&hb, &hp);
            }
            Color ink(255, 255, 255, 255);
            if (!*b.label) {
                gfx::DrawGlyph(g, b.glyph, 15 * s_, ink, b.rect);
                continue;
            }
            RectF glyphBox(b.rect.X + 10 * s_, b.rect.Y, 20 * s_, b.rect.Height);
            gfx::DrawGlyph(g, b.glyph, 15 * s_, ink, glyphBox);
            RectF textBox(glyphBox.GetRight() + 4 * s_, b.rect.Y, b.rect.GetRight() - glyphBox.GetRight() - 10 * s_,
                          b.rect.Height);
            gfx::DrawCentered(g, b.label, font, white, textBox);
        }
    }

    HINSTANCE inst_ = nullptr;
    Notifier notifier_;
    HWND hwnd_ = nullptr;
    UINT_PTR pending_ = 0;
    Target target_ = Target::Region;
    POINT at_{};

    std::unique_ptr<DibCanvas> shot_;  // frozen virtual desktop
    POINT origin_{};                   // its top-left corner on screen
    float s_ = 1;

    bool hasSel_ = false;
    RECT sel_{};  // client coordinates, right/bottom exclusive
    Drag drag_ = Drag::None;
    int resizeEdges_ = kNone;
    POINT dragStart_{};
    RECT selStart_{};
    bool moved_ = false;

    std::vector<Button> buttons_;
    RectF bar_;
    int hoverButton_ = -1, pressedButton_ = -1;
    RECT shownBounds_{};
};

Overlay g_overlay;

void CALLBACK Overlay::OnStartTimer(HWND, UINT, UINT_PTR, DWORD) { g_overlay.Begin(); }

LRESULT CALLBACK Overlay::Proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (g_overlay.hwnd_ == hwnd) return g_overlay.Handle(msg, wp, lp);
    return DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT Overlay::Handle(UINT msg, WPARAM wp, LPARAM lp) {
    POINT p{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
    switch (msg) {
        case WM_ERASEBKGND: return 1;
        case WM_DPICHANGED: return 0;  // the overlay covers every monitor and must not be resized
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(hwnd_, &ps);
            try {
                Paint(dc, ps.rcPaint);
            } catch (const AppError&) {
                // Out of memory for the back buffer: this frame is skipped, the next one may fit.
            }
            EndPaint(hwnd_, &ps);
            return 0;
        }
        case WM_SETCURSOR:
            if (LOWORD(lp) == HTCLIENT) {
                POINT c;
                GetCursorPos(&c);
                ScreenToClient(hwnd_, &c);
                SetCursor(LoadCursorW(nullptr, CursorFor(c)));
                return TRUE;
            }
            break;
        case WM_LBUTTONDOWN: OnButtonDown(p); return 0;
        case WM_MOUSEMOVE: OnMouseMove(p); return 0;
        case WM_LBUTTONUP: OnButtonUp(p); return 0;
        case WM_LBUTTONDBLCLK:
            if (EdgesAt(p) == kInside) Execute(Command::CopyAndSave);
            return 0;
        case WM_RBUTTONUP: Close(); return 0;
        case WM_KEYDOWN: OnKey(UINT(wp)); return 0;
        case WM_ACTIVATE:
            // An overlay left behind another window would block the screen with no way to reach Esc.
            if (LOWORD(wp) == WA_INACTIVE) Close();
            return 0;
    }
    return DefWindowProcW(hwnd_, msg, wp, lp);
}

}  // namespace

void Create(HINSTANCE instance, Notifier onSaved) { g_overlay.Create(instance, std::move(onSaved)); }
void Destroy() { g_overlay.Destroy(); }
void Start(Target target, POINT at) { g_overlay.Start(target, at); }

}  // namespace capture
