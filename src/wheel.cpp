#include "wheel.h"

#include "gfx.h"
#include "infopanel.h"
#include "launcher.h"
#include "shellutil.h"

#include <shlobj.h>
#include <dwmapi.h>
#include <windowsx.h>
#include <wrl/client.h>

#include <unknwn.h>
#include <winrt/base.h>

#include <cmath>
#include <execution>
#include <map>
#include <numeric>
#include <thread>

using Microsoft::WRL::ComPtr;
using namespace Gdiplus;

namespace wheel {
namespace {

constexpr wchar_t kClass[] = L"FileTools.Wheel";
constexpr UINT WM_JOB_DONE = WM_APP + 20;
constexpr UINT WM_JOB_PROGRESS = WM_APP + 21;
constexpr UINT WM_RUN_UI = WM_APP + 22;
constexpr UINT_PTR TIMER_ANIM = 1, TIMER_DRAG_END = 2, TIMER_AUTOHIDE = 3;
constexpr double kPi = 3.14159265358979323846;

// Geometry at 96 DPI.
constexpr float kRing = 184;    // outer glass disc
constexpr float kOuter = 173;   // segment outer radius
constexpr float kInner = 76;    // segment inner radius
constexpr float kHole = 66;     // centre well
constexpr float kGap = 7;       // gap between segments
constexpr float kCorner = 14;   // segment corner radius
constexpr float kMargin = 18;   // shadow room
constexpr float kTagRoom = 74;  // space for the name tag under the wheel

enum class State { Hidden, Open, Working, Done, Failed };

std::wstring SizeChange(uint64_t a, uint64_t b) { return FormatSize(a) + L" \u2192 " + FormatSize(b); }
double Deg(double rad) { return rad * 180.0 / kPi; }
float Ease(float t) { return 1 - (1 - t) * (1 - t) * (1 - t); }

class Wheel final : public IDropTarget {
public:
    // IUnknown: the object is static, reference counting is a no-op.
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (riid == IID_IUnknown || riid == IID_IDropTarget) {
            *ppv = static_cast<IDropTarget*>(this);
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return 1; }
    STDMETHODIMP_(ULONG) Release() override { return 1; }

    STDMETHODIMP DragEnter(IDataObject* data, DWORD, POINTL pt, DWORD* effect) override {
        POINT p{pt.x, pt.y};
        if (helper_) {
            helper_->DragEnter(hwnd_, data, &p, *effect);
            // Explorer's drag image (thumbnail and name under the cursor) would sit off-centre on top
            // of the well, which shows its own centred thumbnail and name.
            helper_->Show(FALSE);
        }
        auto files = FilesFromDataObject(data);
        if (!files.empty() && files != files_) {
            files_ = std::move(files);
            Rebuild();
        }
        dragOver_ = true;
        KillTimer(hwnd_, TIMER_DRAG_END);
        UpdateHover(p);
        *effect = DropEffect(*effect);
        Render();
        return S_OK;
    }

    STDMETHODIMP DragOver(DWORD, POINTL pt, DWORD* effect) override {
        POINT p{pt.x, pt.y};
        if (helper_) helper_->DragOver(&p, *effect);
        if (UpdateHover(p)) Render();
        *effect = DropEffect(*effect);
        return S_OK;
    }

    STDMETHODIMP DragLeave() override {
        if (helper_) {
            helper_->Show(TRUE);
            helper_->DragLeave();
        }
        dragOver_ = false;
        hover_ = -1;
        Render();
        return S_OK;
    }

    STDMETHODIMP Drop(IDataObject* data, DWORD, POINTL pt, DWORD* effect) override {
        POINT p{pt.x, pt.y};
        if (helper_) helper_->Drop(data, &p, *effect);
        dragOver_ = false;
        auto files = FilesFromDataObject(data);
        if (!files.empty() && files != files_) {
            files_ = std::move(files);
            Rebuild();
        }
        int hit = HitTest(p);
        *effect = DROPEFFECT_NONE;
        if (hit >= 0 && state_ != State::Working && !files_.empty()) {
            *effect = DropEffect(*effect | DROPEFFECT_COPY) & ~DROPEFFECT_MOVE;
            // Run after returning so the source's drag loop isn't blocked.
            pendingLaunch_ = hit;
            PostMessageW(hwnd_, WM_RUN_UI, 1, 0);
        }
        hover_ = -1;
        Render();
        return S_OK;
    }

    void Create(HINSTANCE inst, Notifier notifier) {
        notifier_ = std::move(notifier);
        WNDCLASSEXW wc{sizeof(wc)};
        wc.lpfnWndProc = &Wheel::Proc;
        wc.hInstance = inst;
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.lpszClassName = kClass;
        RegisterClassExW(&wc);
        hwnd_ = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, kClass, L"FileTools",
                                WS_POPUP, 0, 0, 10, 10, nullptr, nullptr, inst, nullptr);
        RegisterDragDrop(hwnd_, this);
        CoCreateInstance(CLSID_DragDropHelper, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&helper_));
    }

    void Destroy() {
        if (!hwnd_) return;
        RevokeDragDrop(hwnd_);
        DestroyWindow(hwnd_);
        hwnd_ = nullptr;
        helper_.Reset();
        thumb_.reset();
        backdrop_.reset();
        glass_.reset();
        ring_.reset();
        surface_.reset();
    }

    void ShowForDrag(POINT pt, Mode mode) {
        if (Visible() && (ContainsPoint(pt) || state_ == State::Working)) {
            SetMode(mode);
            return;
        }
        Reset();
        mode_ = mode;
        state_ = State::Open;
        keyboard_ = false;
        Place(pt);
        Rebuild();
        Appear();
    }

    void ShowForFiles(POINT pt, std::vector<fs::path> files) {
        if (state_ == State::Working) return;
        Reset();
        files_ = std::move(files);
        mode_ = Mode::Convert;
        state_ = State::Open;
        keyboard_ = true;
        Place(pt);
        Rebuild();
        hover_ = items_.empty() ? -1 : 0;
        Appear();
    }

    // Screen captures and pinned programs; needs no files, so it opens from its own shortcut.
    void ShowLauncher(POINT pt) {
        if (state_ == State::Working) return;
        Reset();
        icons_.clear();  // a program may have been updated, or the wheel moved to a monitor with another DPI
        mode_ = Mode::Launcher;
        state_ = State::Open;
        keyboard_ = true;
        Place(pt);
        Rebuild();
        hover_ = items_.empty() ? -1 : 0;
        Appear();
    }

    void SetMode(Mode mode) {
        // The launcher has nothing in common with the file wheels, so modifier keys don't switch it.
        if (mode == mode_ || !Visible() || mode == Mode::Launcher || mode_ == Mode::Launcher) return;
        mode_ = mode;
        Rebuild();
        hover_ = keyboard_ && !items_.empty() ? 0 : -1;
        Render();
    }

    void Hide() {
        if (!hwnd_) return;
        ShowWindow(hwnd_, SW_HIDE);
        KillTimer(hwnd_, TIMER_ANIM);
        KillTimer(hwnd_, TIMER_DRAG_END);
        KillTimer(hwnd_, TIMER_AUTOHIDE);
        animating_ = false;
        Reset();
        state_ = State::Hidden;
        keyboard_ = false;
    }

    void OnDragEnded() {
        if (Visible() && state_ == State::Open && !keyboard_) SetTimer(hwnd_, TIMER_DRAG_END, 350, nullptr);
    }

    void OnGlobalMouseDown(POINT pt) {
        if (Visible() && !ContainsPoint(pt)) Hide();
    }

    void OnNavKey(UINT vk) {
        if (!Visible()) return;
        int n = (int)items_.size();
        switch (vk) {
            case VK_ESCAPE: Hide(); return;
            case VK_TAB:
                if (mode_ != Mode::Launcher) SetMode(Mode((int(mode_) + 1) % 3));
                return;
            case VK_LEFT:
            case VK_UP:
                if (n) hover_ = hover_ < 0 ? n - 1 : (hover_ - 1 + n) % n;
                break;
            case VK_RIGHT:
            case VK_DOWN:
                if (n) hover_ = hover_ < 0 ? 0 : (hover_ + 1) % n;
                break;
            case VK_RETURN:
                if (hover_ >= 0 && hover_ < n && state_ != State::Working) Launch(hover_);
                else if (state_ == State::Done && !outputs_.empty()) {
                    RevealInExplorer(outputs_);
                    Hide();
                }
                return;
        }
        Render();
    }

    // Offscreen render used by --preview.
    void Preview(std::vector<fs::path> files, Mode mode, int hover, std::vector<fs::path> outputs, const fs::path& png) {
        Reset();
        files_ = std::move(files);
        mode_ = mode;
        state_ = outputs.empty() ? State::Open : State::Done;
        Place(POINT{600, 600});
        Rebuild();
        hover_ = hover;
        shownAt_ = GetTickCount64() - 10000;
        CaptureBackdrop();
        if (!outputs.empty()) {
            outputs_ = std::move(outputs);
            message_ = SizeChange(SizeOnDisk(files_[0]), SizeOnDisk(outputs_[0]));
            thumb_ = ShellThumbnail(outputs_[0], int(256 * s_));
        }
        Render();
        surface_->SavePng(png);
        state_ = State::Hidden;
    }

    bool Visible() const { return hwnd_ && state_ != State::Hidden && IsWindowVisible(hwnd_); }
    bool KeyboardActive() const { return Visible() && keyboard_; }

    bool ContainsPoint(POINT pt) const {
        if (!Visible()) return false;
        double dx = pt.x - (origin_.x + cx_), dy = pt.y - (origin_.y + cy_);
        return std::sqrt(dx * dx + dy * dy) <= kRing * s_ || PtInRect(&tagRect_, pt);
    }

private:
    static LRESULT CALLBACK Proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    LRESULT Handle(UINT msg, WPARAM wp, LPARAM lp);

    void Reset() {
        files_.clear();
        items_.clear();
        outputs_.clear();
        message_.clear();
        thumb_.reset();
        hover_ = -1;
        pressed_ = -1;
        dragOver_ = false;
        progressDone_ = progressTotal_ = 0;
        SetRectEmpty(&tagRect_);
    }

    void Rebuild() {
        items_ = ActionsFor(mode_, files_);
        if (hover_ >= (int)items_.size()) hover_ = -1;
        ++ringVersion_;
    }

    void Place(POINT pt) {
        s_ = gfx::DpiScaleAt(pt);
        W_ = int(std::ceil((kRing + kMargin) * 2 * s_));
        H_ = int(std::ceil(((kRing + kMargin) * 2 + kTagRoom) * s_));
        cx_ = W_ / 2.0f;
        cy_ = (kRing + kMargin) * s_;
        MONITORINFO mi{sizeof(mi)};
        GetMonitorInfoW(MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST), &mi);
        const RECT& wa = mi.rcWork;
        int x = pt.x - int(cx_), y = pt.y - int(cy_);
        x = std::clamp(x, int(wa.left - kMargin * s_), int(wa.right - W_ + kMargin * s_));
        y = std::clamp(y, int(wa.top - kMargin * s_), int(wa.bottom - H_ + kMargin * s_));
        origin_ = {x, y};
        ++ringVersion_;
    }

    // Snapshot of the screen under the wheel. From it we build a sharp refracting lens for the
    // disc (glass_) and a blurred copy for the frosted buttons (backdrop_).
    void CaptureBackdrop() {
        backdrop_.reset();
        glass_.reset();
        ++ringVersion_;
        bool flush = false;
        if (IsWindowVisible(hwnd_)) {
            ShowWindow(hwnd_, SW_HIDE);
            flush = true;
        }
        // Keep the shell's drag image out of the snapshot, or it stays frozen inside the glass.
        HWND dragImage = FindWindowW(L"SysDragImage", nullptr);
        bool hidDrag = dragImage && IsWindowVisible(dragImage);
        if (hidDrag) {
            ShowWindow(dragImage, SW_HIDE);
            flush = true;
        }
        if (flush) DwmFlush();

        BITMAPINFO bi{};
        bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
        bi.bmiHeader.biWidth = W_;
        bi.bmiHeader.biHeight = -H_;
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        HDC screen = GetDC(nullptr);
        HDC mem = CreateCompatibleDC(screen);
        void* bits = nullptr;
        HBITMAP dib = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
        HGDIOBJ old = SelectObject(mem, dib);
        BitBlt(mem, 0, 0, W_, H_, screen, origin_.x, origin_.y, SRCCOPY);
        GdiFlush();
        std::vector<uint8_t> shot(static_cast<uint8_t*>(bits), static_cast<uint8_t*>(bits) + size_t(W_) * H_ * 4);
        SelectObject(mem, old);
        DeleteObject(dib);
        DeleteDC(mem);
        ReleaseDC(nullptr, screen);
        if (hidDrag) ShowWindow(dragImage, SW_SHOWNOACTIVATE);

        BuildLens(shot);

        // Frosted copy: 6x box downscale, then blur.
        int sw = std::max(8, W_ / 6), sh = std::max(8, H_ / 6);
        auto mini = std::make_unique<Bitmap>(sw, sh, PixelFormat32bppARGB);
        {
            BitmapData md;
            Rect mr(0, 0, sw, sh);
            if (mini->LockBits(&mr, ImageLockModeWrite, PixelFormat32bppARGB, &md) == Ok) {
                for (int y = 0; y < sh; ++y) {
                    uint8_t* row = static_cast<uint8_t*>(md.Scan0) + y * md.Stride;
                    for (int x = 0; x < sw; ++x) {
                        int sum[3] = {};
                        int n = 0;
                        for (int yy = y * 6; yy < std::min(H_, y * 6 + 6); ++yy)
                            for (int xx = x * 6; xx < std::min(W_, x * 6 + 6); ++xx, ++n)
                                for (int c = 0; c < 3; ++c) sum[c] += shot[(size_t(yy) * W_ + xx) * 4 + c];
                        for (int c = 0; c < 3; ++c) row[x * 4 + c] = uint8_t(n ? sum[c] / n : 0);
                        row[x * 4 + 3] = 255;
                    }
                }
                mini->UnlockBits(&md);
            }
        }

        BitmapData d;
        Rect rect(0, 0, sw, sh);
        if (mini->LockBits(&rect, ImageLockModeRead | ImageLockModeWrite, PixelFormat32bppARGB, &d) != Ok) return;
        auto* px = static_cast<uint8_t*>(d.Scan0);
        std::vector<int> tmp(size_t(std::max(sw, sh)) * 4);
        auto blurLine = [&](uint8_t* start, int count, int step) {
            const int r = 2;
            for (int i = 0; i < count; ++i)
                for (int c = 0; c < 3; ++c) tmp[i * 4 + c] = start[i * step + c];
            for (int i = 0; i < count; ++i)
                for (int c = 0; c < 3; ++c) {
                    int sum = 0, n = 0;
                    for (int k = std::max(0, i - r); k <= std::min(count - 1, i + r); ++k, ++n) sum += tmp[k * 4 + c];
                    start[i * step + c] = uint8_t(sum / std::max(n, 1));
                }
        };
        for (int pass = 0; pass < 3; ++pass) {
            for (int y = 0; y < sh; ++y) blurLine(px + y * d.Stride, sw, 4);
            for (int x = 0; x < sw; ++x) blurLine(px + x * 4, sh, d.Stride);
        }
        // Liquid glass lifts and saturates what it refracts.
        for (int y = 0; y < sh; ++y) {
            uint8_t* row = px + y * d.Stride;
            for (int x = 0; x < sw; ++x) {
                uint8_t* p = row + x * 4;
                float gray = 0.114f * p[0] + 0.587f * p[1] + 0.299f * p[2];
                for (int c = 0; c < 3; ++c) {
                    float v = gray + (p[c] - gray) * 1.25f;
                    v = v * 0.92f + 255 * 0.08f;
                    p[c] = uint8_t(std::clamp(v, 0.0f, 255.0f));
                }
                p[3] = 255;
            }
        }
        mini->UnlockBits(&d);
        backdrop_ = std::move(mini);
    }

    // Clear liquid-glass lens: the screen shows through untouched in the middle and bends
    // towards the rim like a thick bevel, with a touch of chromatic aberration.
    void BuildLens(const std::vector<uint8_t>& shot) {
        glassPx_.assign(size_t(W_) * H_ * 4, 0);
        const float R = kRing * s_;
        auto sample = [&](float sx, float sy, int c) {
            sx = std::clamp(sx - 0.5f, 0.0f, float(W_ - 1));
            sy = std::clamp(sy - 0.5f, 0.0f, float(H_ - 1));
            int x0 = int(sx), y0 = int(sy), x1 = std::min(x0 + 1, W_ - 1), y1 = std::min(y0 + 1, H_ - 1);
            float fx = sx - x0, fy = sy - y0;
            auto at = [&](int x, int y) { return float(shot[(size_t(y) * W_ + x) * 4 + c]); };
            return (at(x0, y0) * (1 - fx) + at(x1, y0) * fx) * (1 - fy) + (at(x0, y1) * (1 - fx) + at(x1, y1) * fx) * fy;
        };
        std::vector<int> rows(H_);
        std::iota(rows.begin(), rows.end(), 0);
        std::for_each(std::execution::par, rows.begin(), rows.end(), [&](int y) {
            uint8_t* out = glassPx_.data() + size_t(y) * W_ * 4;
            for (int x = 0; x < W_; ++x) {
                float dx = x + 0.5f - cx_, dy = y + 0.5f - cy_;
                float dist = std::sqrt(dx * dx + dy * dy);
                if (dist > R + 1) continue;
                float d = dist / R;
                float t = std::clamp((d - 0.68f) / 0.32f, 0.0f, 1.0f);
                float edge = t * t * (3 - 2 * t);  // smoothstep
                float k = 0.975f - 0.24f * edge * edge;
                float ca = 0.02f * edge;
                float b = sample(cx_ + dx * (k + ca), cy_ + dy * (k + ca), 0);
                float g = sample(cx_ + dx * k, cy_ + dy * k, 1);
                float r = sample(cx_ + dx * (k - ca), cy_ + dy * (k - ca), 2);
                float lift = 34 * edge * edge * edge;  // light caught by the bevel
                float cov = std::clamp(R - dist + 0.5f, 0.0f, 1.0f);
                out[x * 4 + 0] = uint8_t(std::min(255.0f, b + lift) * cov);
                out[x * 4 + 1] = uint8_t(std::min(255.0f, g + lift) * cov);
                out[x * 4 + 2] = uint8_t(std::min(255.0f, r + lift) * cov);
                out[x * 4 + 3] = uint8_t(255 * cov);
            }
        });
        glass_ = std::make_unique<Bitmap>(W_, H_, W_ * 4, PixelFormat32bppPARGB, glassPx_.data());
    }

    void Appear() {
        shownAt_ = GetTickCount64();
        CaptureBackdrop();
        Render();
        SetWindowPos(hwnd_, HWND_TOPMOST, origin_.x, origin_.y, W_, H_, SWP_NOACTIVATE | SWP_SHOWWINDOW);
        StartAnim();
    }

    void StartAnim() {
        if (!animating_) {
            animating_ = true;
            SetTimer(hwnd_, TIMER_ANIM, 15, nullptr);
        }
    }

    float AppearT() const { return std::min(1.0f, (GetTickCount64() - shownAt_) / 170.0f); }

    DWORD DropEffect(DWORD allowed) const {
        if (hover_ < 0 || state_ == State::Working || files_.empty() || mode_ == Mode::Launcher) return DROPEFFECT_NONE;
        if (allowed & DROPEFFECT_COPY) return DROPEFFECT_COPY;
        if (allowed & DROPEFFECT_LINK) return DROPEFFECT_LINK;
        return DROPEFFECT_NONE;
    }

    // -1 = nothing, -2 = centre, >= 0 = segment index.
    int HitTest(POINT screen) const {
        double dx = screen.x - (origin_.x + cx_), dy = screen.y - (origin_.y + cy_);
        double d = std::sqrt(dx * dx + dy * dy);
        if (d < kInner * s_) return -2;
        if (d > (kRing + 6) * s_ || items_.empty()) return -1;
        double span = 2 * kPi / items_.size();
        double a = std::atan2(dy, dx) + kPi / 2 + span / 2;
        a = std::fmod(a + 4 * kPi, 2 * kPi);
        return std::min(int(a / span), (int)items_.size() - 1);
    }

    bool UpdateHover(POINT screen) {
        int h = HitTest(screen);
        if (h == -1 && keyboard_) return false;
        if (h == hover_) return false;
        hover_ = h;
        return true;
    }

    POINT Center() const { return {origin_.x + int(cx_), origin_.y + int(cy_)}; }

    void Launch(int index) {
        if (index < 0 || index >= (int)items_.size() || (files_.empty() && mode_ != Mode::Launcher)) return;
        const ActionDef action = items_[index];
        if (action.id == "app:add") {
            AddProgram();
            return;
        }
        KillTimer(hwnd_, TIMER_DRAG_END);
        KillTimer(hwnd_, TIMER_AUTOHIDE);
        state_ = State::Working;
        busyLabel_ = action.label;
        progressDone_ = progressTotal_ = 0;
        thumb_.reset();
        outputs_.clear();
        hover_ = -1;
        StartAnim();
        Render();
        if (action.ui) {
            POINT anchor{origin_.x + int(cx_), origin_.y + int(cy_)};
            ShowResult(RunUiAction(action.id, files_, hwnd_, anchor));
            return;
        }
        uint64_t job = ++jobId_;
        HWND hwnd = hwnd_;
        std::thread([hwnd, job, id = action.id, files = files_] {
            winrt::init_apartment(winrt::apartment_type::multi_threaded);
            JobResult r = RunAction(id, files, [hwnd, job](int done, int total) {
                PostMessageW(hwnd, WM_JOB_PROGRESS, (WPARAM)job, MAKELPARAM(done, total));
            });
            auto* heap = new JobResult(std::move(r));
            if (!PostMessageW(hwnd, WM_JOB_DONE, (WPARAM)job, (LPARAM)heap)) delete heap;
            winrt::uninit_apartment();
        }).detach();
    }

    // The file dialog takes the focus and any click on it would close the wheel anyway, so the
    // launcher steps aside and comes back with the new program in place.
    void AddProgram() {
        POINT at = Center();
        Hide();
        AddLauncherApp(nullptr);
        ShowLauncher(at);
    }

    // Right-click on a pinned program: remove it or show where it lives.
    void ProgramMenu(int index) {
        const ActionDef& item = items_[index];
        if (!item.id.starts_with("app:") || item.icon.empty()) return;
        size_t app = std::stoul(item.id.substr(4));
        fs::path path = item.icon;
        POINT at = Center();
        enum { ID_REMOVE = 1, ID_REVEAL };
        HMENU menu = CreatePopupMenu();
        AppendMenuW(menu, MF_STRING, ID_REMOVE, L"Quitar de la Lanzadera");
        AppendMenuW(menu, MF_STRING, ID_REVEAL, L"Abrir ubicación");
        POINT pt;
        GetCursorPos(&pt);
        SetForegroundWindow(hwnd_);
        UINT cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY, pt.x, pt.y, 0, hwnd_, nullptr);
        DestroyMenu(menu);
        if (cmd == ID_REMOVE) {
            RemoveLauncherApp(app);
            // Clicking the menu counts as a click outside the wheel and may already have closed it.
            ShowLauncher(at);
        } else if (cmd == ID_REVEAL) {
            RevealInExplorer({path});
            Hide();
        }
    }

    // Program icons for the launcher, extracted once per path.
    Gdiplus::Bitmap* ProgramIcon(const fs::path& path) {
        auto it = icons_.find(path);
        if (it == icons_.end()) it = icons_.emplace(path, ShellThumbnail(path, int(32 * s_))).first;
        return it->second.get();
    }

    void ShowResult(const JobResult& r) {
        if (r.sheet) {
            POINT anchor{origin_.x + int(cx_), origin_.y + int(cy_)};
            Hide();
            OpenInfoPanel(r.sheet, anchor);
            return;
        }
        if (r.hide) {
            Hide();
            return;
        }
        KillTimer(hwnd_, TIMER_AUTOHIDE);
        message_ = r.message;
        outputs_ = r.outputs;
        hover_ = -1;
        if (!r.ok) {
            state_ = State::Failed;
            SetTimer(hwnd_, TIMER_AUTOHIDE, 7000, nullptr);
        } else {
            state_ = State::Done;
            thumb_ = outputs_.empty() ? nullptr : ShellThumbnail(outputs_[0], int(256 * s_));
            SetTimer(hwnd_, TIMER_AUTOHIDE, outputs_.empty() ? 4500 : 90000, nullptr);
        }
        Render();
    }

    void OnJobDone(uint64_t job, JobResult* raw) {
        std::unique_ptr<JobResult> r(raw);
        if (job != jobId_ || state_ != State::Working || !Visible()) {
            if (notifier_) notifier_(*r);
            return;
        }
        ShowResult(*r);
    }

    // --- drawing ---------------------------------------------------------------

    void SegmentPath(GraphicsPath& path, double a0, double a1, double R, double ri) const {
        double g = kGap * s_ / 2, r = kCorner * s_;
        double u0x = std::cos(a0), u0y = std::sin(a0), n0x = -std::sin(a0), n0y = std::cos(a0);
        double u1x = std::cos(a1), u1y = std::sin(a1), n1x = std::sin(a1), n1y = -std::cos(a1);
        // Fillet circle centre tangent to an edge (offset g) at distance `dist` from the wheel centre.
        auto fillet = [&](double ux, double uy, double nx, double ny, double dist) {
            double off = g + r;
            double along = std::sqrt(std::max(0.0, dist * dist - off * off));
            return std::pair<double, double>{along * ux + off * nx, along * uy + off * ny};
        };
        auto [olx, oly] = fillet(u0x, u0y, n0x, n0y, R - r);
        auto [orx, ory] = fillet(u1x, u1y, n1x, n1y, R - r);
        auto [ilx, ily] = fillet(u0x, u0y, n0x, n0y, ri + r);
        auto [irx, iry] = fillet(u1x, u1y, n1x, n1y, ri + r);
        // Build the outline from sampled points: GDI+ AddArc chains mis-render wide sectors.
        std::vector<PointF> pts;
        auto angle = [](double x, double y) { return std::atan2(y, x); };
        auto sample = [&](double ccx, double ccy, double rad, double start, double sweep) {
            int steps = std::max(4, int(std::abs(sweep) * rad / (3.0 * s_)));
            for (int k = 0; k <= steps; ++k) {
                double a = start + sweep * k / steps;
                pts.emplace_back(float(cx_ + ccx + std::cos(a) * rad), float(cy_ + ccy + std::sin(a) * rad));
            }
        };
        auto shortest = [](double from, double to) {
            double d = std::fmod(to - from + 3 * kPi, 2 * kPi);
            if (d < 0) d += 2 * kPi;
            return d - kPi;
        };
        auto positive = [](double from, double to) {
            double d = std::fmod(to - from + 4 * kPi, 2 * kPi);
            return d < 0 ? d + 2 * kPi : d;
        };
        double sOL = angle(olx, oly), sOR = angle(orx, ory);
        sample(0, 0, R, sOL, positive(sOL, sOR));
        double eR = angle(-n1x, -n1y);
        sample(orx, ory, r, sOR, shortest(sOR, eR));
        double tIR = angle(-irx, -iry);
        sample(irx, iry, r, eR, shortest(eR, tIR));
        double sIR = angle(irx, iry), sIL = angle(ilx, ily);
        sample(0, 0, ri, sIR, -positive(sIL, sIR));
        double tIL = angle(-ilx, -ily), eL = angle(-n0x, -n0y);
        sample(ilx, ily, r, tIL, shortest(tIL, eL));
        sample(olx, oly, r, eL, shortest(eL, sOL));
        path.AddPolygon(pts.data(), (INT)pts.size());
    }

    void DrawDisc(Graphics& g) {
        float R = kRing * s_;
        // Soft drop shadow.
        for (int i = 8; i >= 1; --i) {
            float grow = i * 1.6f * s_;
            SolidBrush b(Color(BYTE(5), 0, 0, 0));
            g.FillEllipse(&b, cx_ - R - grow, cy_ - R - grow + 5 * s_, 2 * (R + grow), 2 * (R + grow));
        }
        // Fully transparent liquid glass: the refracting lens built from the screen snapshot.
        if (glass_) {
            g.SetInterpolationMode(InterpolationModeNearestNeighbor);
            g.DrawImage(glass_.get(), 0, 0, W_, H_);
            g.SetInterpolationMode(InterpolationModeHighQualityBilinear);
        } else {
            SolidBrush frost(Color(110, 238, 238, 242));
            g.FillEllipse(&frost, cx_ - R, cy_ - R, 2 * R, 2 * R);
        }
        // Faint specular sheen from the top-left.
        LinearGradientBrush sheen(PointF(cx_ - R, cy_ - R), PointF(cx_, cy_), Color(40, 255, 255, 255),
                                  Color(0, 255, 255, 255));
        sheen.SetWrapMode(WrapModeTileFlipXY);
        GraphicsPath disc;
        disc.AddEllipse(cx_ - R, cy_ - R, 2 * R, 2 * R);
        g.FillPath(&sheen, &disc);
        // Bright refracting rim plus a faint dark hairline outside.
        LinearGradientBrush rimBrush(PointF(cx_ - R, cy_ - R), PointF(cx_ + R, cy_ + R), Color(230, 255, 255, 255),
                                     Color(90, 255, 255, 255));
        Pen rim(&rimBrush, 1.6f * s_);
        g.DrawEllipse(&rim, cx_ - R + 0.8f * s_, cy_ - R + 0.8f * s_, 2 * R - 1.6f * s_, 2 * R - 1.6f * s_);
        Pen inner(Color(40, 255, 255, 255), 3.0f * s_);
        g.DrawEllipse(&inner, cx_ - R + 4 * s_, cy_ - R + 4 * s_, 2 * R - 8 * s_, 2 * R - 8 * s_);
        Pen outer(Color(45, 0, 0, 0), 1.0f * s_);
        g.DrawEllipse(&outer, cx_ - R, cy_ - R, 2 * R, 2 * R);
    }

    void DrawSegments(Graphics& g) {
        int n = (int)items_.size();
        // Placeholders until we know what is being dragged; a plain ring when nothing applies.
        int count = n ? n : (files_.empty() ? 8 : 1);
        double span = 2 * kPi / count;
        Font labelFont(gfx::TextFamily(), 15.5f * s_, FontStyleBold, UnitPixel);
        for (int i = 0; i < count; ++i) {
            double mid = -kPi / 2 + i * span;
            double a0 = mid - span / 2, a1 = mid + span / 2;
            bool hot = i == hover_ && n;
            bool busy = state_ == State::Working;
            double R = (kOuter + (hot ? 4 : 0)) * s_, ri = kInner * s_;
            GraphicsPath path;
            if (count == 1) {
                path.AddEllipse(float(cx_ - R), float(cy_ - R), float(2 * R), float(2 * R));
                path.AddEllipse(float(cx_ - ri), float(cy_ - ri), float(2 * ri), float(2 * ri));
            } else {
                SegmentPath(path, a0, a1, R, ri);
            }
            PointF outerPt(float(cx_ + std::cos(mid) * R), float(cy_ + std::sin(mid) * R));
            // The gradient must span the whole sector: wide sectors reach closer to the centre
            // at their inner corners than at their midline.
            double innerRad = count == 1 ? -R : ri * std::cos(span / 2) * 0.9;
            PointF innerPt(float(cx_ + std::cos(mid) * innerRad), float(cy_ + std::sin(mid) * innerRad));
            // Frosted glass tiles: the blurred screen behind them, washed with translucent white.
            if (backdrop_ && !hot) {
                TextureBrush frost(backdrop_.get(), WrapModeTileFlipXY);
                frost.ScaleTransform(float(W_) / backdrop_->GetWidth(), float(H_) / backdrop_->GetHeight());
                g.FillPath(&frost, &path);
            }
            BYTE alphaRim = busy ? 90 : 150, alphaIn = busy ? 60 : 112;
            Color c1 = hot ? Color(232, 28, 132, 230) : Color(alphaRim, 255, 255, 255);
            Color c2 = hot ? Color(232, 0, 96, 196) : Color(alphaIn, 248, 248, 252);
            LinearGradientBrush fill(outerPt, innerPt, c1, c2);
            fill.SetWrapMode(WrapModeTileFlipXY);
            g.FillPath(&fill, &path);
            Pen edge(hot ? Color(150, 190, 225, 255) : Color(210, 255, 255, 255), 1.0f * s_);
            g.DrawPath(&edge, &path);
            if (!n) continue;

            const ActionDef& item = items_[i];
            double labelR = (kOuter + kInner) / 2.0 * s_ + (hot ? 2 * s_ : 0);
            float lx = float(cx_ + std::cos(mid) * labelR), ly = float(cy_ + std::sin(mid) * labelR);
            float chord = float(2 * labelR * std::sin(std::min(span, kPi) / 2) - (kGap + 2 * kCorner) * s_ * 0.6f);
            float maxW = std::max(40.0f * s_, std::min(chord, (kOuter - kInner) * s_ * 1.25f));
            Color tc = hot ? Color(255, 255, 255, 255) : Color(busy ? 120 : 255, 28, 28, 32);
            SolidBrush textBrush(tc);
            float size = item.glyph ? 11.5f * s_ : 15.5f * s_;
            Font font(gfx::TextFamily(), size, FontStyleBold, UnitPixel);
            float tw = gfx::TextWidth(g, item.label, font);
            if (tw > maxW) size = std::max(8.0f * s_, size * maxW / tw);
            Font fitted(gfx::TextFamily(), size, FontStyleBold, UnitPixel);
            Gdiplus::Bitmap* icon = item.icon.empty() ? nullptr : ProgramIcon(item.icon);
            if (icon) {
                float side = 26 * s_;
                gfx::DrawFitted(g, *icon, RectF(lx - side / 2, ly - 2 * s_ - side, side, side));
                gfx::DrawCentered(g, item.label, fitted, textBrush, RectF(lx - 60 * s_, ly + 2 * s_, 120 * s_, 20 * s_));
            } else if (item.glyph) {
                gfx::DrawGlyph(g, item.glyph, 21.0f * s_, tc, RectF(lx - 30 * s_, ly - 26 * s_, 60 * s_, 26 * s_),
                               0.5f * s_);
                gfx::DrawCentered(g, item.label, fitted, textBrush, RectF(lx - 60 * s_, ly + 2 * s_, 120 * s_, 20 * s_));
            } else {
                gfx::DrawCentered(g, item.label, fitted, textBrush, RectF(lx - 60 * s_, ly - 12 * s_, 120 * s_, 24 * s_));
            }
        }
        (void)labelFont;
    }

    std::wstring CenterText() const {
        if (state_ == State::Working) {
            if (progressTotal_ > 1) return std::to_wstring(progressDone_ + 1) + L" / " + std::to_wstring(progressTotal_);
            return busyLabel_;
        }
        if (state_ == State::Failed) return L"Error";
        if (state_ == State::Done) return outputs_.empty() ? L"✓ Hecho" : L"✓ Listo";
        if (hover_ >= 0 && hover_ < (int)items_.size()) return items_[hover_].label;
        if (mode_ == Mode::Launcher) return L"Elige";
        if (files_.empty()) return L"Suelta aquí";
        if (items_.empty()) return mode_ == Mode::Convert ? L"Sin conversiones" : L"Sin herramientas";
        return filesLabel_;
    }

    // Size label for the dragged files, computed once per file set (folders are not walked).
    void UpdateFilesLabel() {
        if (files_ == labelFiles_) return;
        labelFiles_ = files_;
        uint64_t total = 0;
        int folders = 0;
        for (const auto& f : files_) {
            std::error_code ec;
            if (fs::is_directory(f, ec)) {
                ++folders;
            } else {
                auto sz = fs::file_size(f, ec);
                if (!ec) total += sz;
            }
        }
        if (files_.size() == 1) filesLabel_ = folders ? L"Carpeta" : FormatSize(total);
        else filesLabel_ = std::to_wstring(files_.size()) + L" · " + (folders ? L"elementos" : FormatSize(total));
        filesName_ = files_.size() == 1 ? files_[0].filename().wstring() : std::to_wstring(files_.size()) + L" archivos";
        filesThumb_ = files_.empty() ? nullptr : ShellThumbnail(files_[0], int(96 * s_));
    }

    // Rounded thumbnail card with a soft shadow, fitted and centred in `area`; `count` > 1 adds a badge.
    void DrawThumbCard(Graphics& g, Bitmap& img, const RectF& area, size_t count) {
        float iw = (float)img.GetWidth(), ih = (float)img.GetHeight();
        float sc = std::min(area.Width / iw, area.Height / ih);
        float w = std::round(iw * sc), h = std::round(ih * sc);
        RectF r(std::round(area.X + (area.Width - w) / 2), std::round(area.Y + (area.Height - h) / 2), w, h);
        float corner = std::min(7 * s_, std::min(w, h) / 4);
        for (int i = 5; i >= 1; --i) {
            GraphicsPath sp;
            float grow = i * 1.6f * s_;
            gfx::AddRoundRect(sp, RectF(r.X - grow, r.Y - grow + 3 * s_, r.Width + 2 * grow, r.Height + 2 * grow),
                              corner + grow);
            SolidBrush sb(Color(16, 0, 0, 0));
            g.FillPath(&sb, &sp);
        }
        GraphicsPath card;
        gfx::AddRoundRect(card, r, corner);
        g.SetInterpolationMode(InterpolationModeHighQualityBicubic);
        TextureBrush pic(&img, WrapModeClamp);
        pic.TranslateTransform(r.X, r.Y);
        pic.ScaleTransform(sc, sc);
        g.FillPath(&pic, &card);
        Pen frame(Color(230, 255, 255, 255), 1.5f * s_);
        g.DrawPath(&frame, &card);
        if (count > 1) {
            float d = 22 * s_;
            RectF badge(r.X + r.Width - d * 0.6f, r.Y - d * 0.4f, d, d);
            SolidBrush red(Color(255, 232, 72, 40));
            g.FillEllipse(&red, badge);
            Font f(gfx::TextFamily(), 11 * s_, FontStyleBold, UnitPixel);
            SolidBrush white(Color(255, 255, 255, 255));
            gfx::DrawCentered(g, std::to_wstring(count), f, white, badge);
        }
    }

    void DrawCenter(Graphics& g) {
        float hole = kHole * s_;
        // Recessed glass well: darker at the top like an inner shadow.
        LinearGradientBrush well(PointF(cx_, cy_ - hole), PointF(cx_, cy_ + hole), Color(38, 0, 0, 0),
                                 Color(4, 0, 0, 0));
        g.FillEllipse(&well, cx_ - hole, cy_ - hole, 2 * hole, 2 * hole);
        LinearGradientBrush wellRimBrush(PointF(cx_, cy_ - hole), PointF(cx_, cy_ + hole), Color(60, 255, 255, 255),
                                         Color(200, 255, 255, 255));
        Pen wellRim(&wellRimBrush, 1.3f * s_);
        g.DrawEllipse(&wellRim, cx_ - hole, cy_ - hole, 2 * hole, 2 * hole);

        bool showThumb = state_ == State::Done && thumb_ && (hover_ < 0 || hover_ == -2);
        if (showThumb) {
            // Frost the well so the result stands out from whatever is behind the glass.
            if (backdrop_) {
                TextureBrush frost(backdrop_.get(), WrapModeTileFlipXY);
                frost.ScaleTransform(float(W_) / backdrop_->GetWidth(), float(H_) / backdrop_->GetHeight());
                g.FillEllipse(&frost, cx_ - hole, cy_ - hole, 2 * hole, 2 * hole);
            }
            SolidBrush wash(Color(90, 255, 255, 255));
            g.FillEllipse(&wash, cx_ - hole, cy_ - hole, 2 * hole, 2 * hole);
            g.DrawEllipse(&wellRim, cx_ - hole, cy_ - hole, 2 * hole, 2 * hole);

            float box = hole * 1.42f;
            DrawThumbCard(g, *thumb_, RectF(cx_ - box / 2, cy_ - box / 2, box, box), outputs_.size());
            if (hover_ == -2) {
                Pen hint(Color(200, 255, 255, 255), 2.0f * s_);
                hint.SetDashStyle(DashStyleDash);
                g.DrawEllipse(&hint, cx_ - hole + 4 * s_, cy_ - hole + 4 * s_, 2 * hole - 8 * s_, 2 * hole - 8 * s_);
            }
            return;
        }

        Font cap(gfx::TextFamily(), 9.0f * s_, FontStyleBold, UnitPixel);
        SolidBrush capShadow(Color(110, 0, 0, 0));
        SolidBrush capBrush(Color(240, 255, 255, 255));
        auto caption = [&](const std::wstring& s, RectF r, const Font& f) {
            gfx::DrawCentered(g, s, f, capShadow, RectF(r.X, r.Y + 1 * s_, r.Width, r.Height));
            gfx::DrawCentered(g, s, f, capBrush, r);
        };

        // Idle with files: what is being dragged, centred in the well.
        if (state_ == State::Open && filesThumb_ && hover_ < 0 && !files_.empty()) {
            caption(ModeName(mode_), RectF(cx_ - hole, cy_ - 52 * s_, 2 * hole, 12 * s_), cap);
            float box = 46 * s_;
            DrawThumbCard(g, *filesThumb_, RectF(cx_ - box / 2, cy_ - 36 * s_, box, box), files_.size());
            Font name(gfx::TextFamily(), 11.0f * s_, FontStyleBold, UnitPixel);
            Font size(gfx::TextFamily(), 9.5f * s_, FontStyleRegular, UnitPixel);
            float nameW = 2 * hole - 34 * s_;
            caption(filesName_, RectF(cx_ - nameW / 2, cy_ + 15 * s_, nameW, 15 * s_), name);
            caption(keyboard_ ? L"Tab · ← → · Intro" : filesLabel_, RectF(cx_ - hole, cy_ + 31 * s_, 2 * hole, 13 * s_), size);
            return;
        }

        std::wstring text = CenterText();
        float fontSize = 14.5f * s_, maxText = hole * 2 - 30 * s_;
        float tw = gfx::TextWidth(g, text, Font(gfx::TextFamily(), fontSize, FontStyleBold, UnitPixel));
        if (tw > maxText) {
            fontSize = std::max(9.0f * s_, fontSize * maxText / tw);
            tw = maxText;
        }
        Font font(gfx::TextFamily(), fontSize, FontStyleBold, UnitPixel);
        float pw = std::clamp(tw + 34 * s_, 96 * s_, hole * 2 - 6 * s_), ph = 44 * s_;
        RectF pill(cx_ - pw / 2, cy_ - ph / 2, pw, ph);
        GraphicsPath pp;
        gfx::AddRoundRect(pp, pill, ph / 2);
        Color pillColor = state_ == State::Failed ? Color(235, 214, 48, 40) : Color(215, 255, 255, 255);
        SolidBrush pillBrush(pillColor);
        g.FillPath(&pillBrush, &pp);
        Pen pillRim(Color(230, 255, 255, 255), 1.0f * s_);
        g.DrawPath(&pillRim, &pp);
        SolidBrush textBrush(state_ == State::Failed ? Color(255, 255, 255, 255) : Color(255, 28, 28, 32));
        gfx::DrawCentered(g, text, font, textBrush, RectF(pill.X + 8 * s_, pill.Y, pill.Width - 16 * s_, pill.Height));

        if (state_ == State::Open) {
            caption(ModeName(mode_), RectF(cx_ - hole, cy_ - ph / 2 - 20 * s_, 2 * hole, 14 * s_), cap);
            if (keyboard_)
                caption(mode_ == Mode::Launcher ? L"← → · Intro" : L"Tab · ← → · Intro",
                        RectF(cx_ - hole, cy_ + ph / 2 + 6 * s_, 2 * hole, 14 * s_), cap);
        }
        if (state_ == State::Working) {
            float r = hole - 5 * s_;
            float start = float((GetTickCount64() / 3) % 360);
            Pen spin(Color(255, 28, 132, 230), 3.5f * s_);
            spin.SetStartCap(LineCapRound);
            spin.SetEndCap(LineCapRound);
            g.DrawArc(&spin, cx_ - r, cy_ - r, 2 * r, 2 * r, start, 95);
            if (progressTotal_ > 1) {
                Pen track(Color(255, 255, 255, 255), 3.5f * s_);
                float frac = float(progressDone_) / progressTotal_;
                Pen prog(Color(230, 255, 255, 255), 2.0f * s_);
                g.DrawArc(&prog, cx_ - r + 5 * s_, cy_ - r + 5 * s_, 2 * r - 10 * s_, 2 * r - 10 * s_, -90,
                          std::max(1.0f, 360 * frac));
            }
        }
    }

    void DrawTag(Graphics& g) {
        SetRectEmpty(&tagRect_);
        std::wstring title, detail;
        Color bg(240, 0, 103, 214);
        if (state_ == State::Done && !outputs_.empty()) {
            title = outputs_.size() == 1 ? outputs_[0].filename().wstring()
                                         : std::to_wstring(outputs_.size()) + L" archivos";
            detail = message_;
        } else if (state_ == State::Done && !message_.empty()) {
            title = message_;
        } else if (state_ == State::Failed) {
            title = message_.empty() ? L"No se pudo completar" : message_;
            bg = Color(240, 196, 43, 28);
        } else {
            return;
        }
        float maxW = W_ - 24 * s_;
        Font titleFont(gfx::TextFamily(), 13.5f * s_, FontStyleBold, UnitPixel);
        Font detailFont(gfx::TextFamily(), 11.5f * s_, FontStyleRegular, UnitPixel);
        StringFormat fmt;
        fmt.SetAlignment(StringAlignmentCenter);
        fmt.SetTrimming(StringTrimmingEllipsisWord);
        RectF titleBox;
        g.MeasureString(title.c_str(), (INT)title.size(), &titleFont, RectF(0, 0, maxW - 20 * s_, 60 * s_), &fmt,
                        &titleBox);
        titleBox.Height = std::min(titleBox.Height, 38 * s_);
        RectF detailBox(0, 0, 0, 0);
        if (!detail.empty())
            g.MeasureString(detail.c_str(), (INT)detail.size(), &detailFont, RectF(0, 0, maxW - 20 * s_, 20 * s_), &fmt,
                            &detailBox);
        float w = std::min(maxW, std::max(titleBox.Width, detailBox.Width) + 22 * s_);
        float h = titleBox.Height + (detail.empty() ? 0 : detailBox.Height) + 10 * s_;
        RectF tag(cx_ - w / 2, cy_ + kRing * s_ + 10 * s_, w, h);
        GraphicsPath p;
        gfx::AddRoundRect(p, tag, 8 * s_);
        SolidBrush b(bg);
        g.FillPath(&b, &p);
        SolidBrush white(Color(255, 255, 255, 255));
        SolidBrush soft(Color(225, 255, 255, 255));
        g.DrawString(title.c_str(), (INT)title.size(), &titleFont,
                     RectF(tag.X + 10 * s_, tag.Y + 5 * s_, tag.Width - 20 * s_, titleBox.Height), &fmt, &white);
        if (!detail.empty())
            g.DrawString(detail.c_str(), (INT)detail.size(), &detailFont,
                         RectF(tag.X + 10 * s_, tag.Y + 5 * s_ + titleBox.Height, tag.Width - 20 * s_, detailBox.Height),
                         &fmt, &soft);
        tagRect_ = {origin_.x + int(tag.X), origin_.y + int(tag.Y), origin_.x + int(tag.X + tag.Width),
                    origin_.y + int(tag.Y + tag.Height)};
    }

    void Render() {
        if (!hwnd_ || state_ == State::Hidden) return;
        // The glass disc and the segments only change with hover/items/state, so they are
        // rendered once into a cached layer; each frame just blits it and draws the centre.
        int hotSeg = hover_ >= 0 ? hover_ : -1;
        bool busy = state_ == State::Working;
        if (!ring_ || ringVersion_ != ringBuiltVersion_ || hotSeg != ringHover_ || busy != ringBusy_) {
            ring_ = std::make_unique<Bitmap>(W_, H_, PixelFormat32bppPARGB);
            Graphics rg(ring_.get());
            rg.SetSmoothingMode(SmoothingModeAntiAlias);
            rg.SetPixelOffsetMode(PixelOffsetModeHalf);
            rg.SetCompositingQuality(CompositingQualityHighQuality);
            rg.SetInterpolationMode(InterpolationModeHighQualityBilinear);
            rg.SetTextRenderingHint(TextRenderingHintAntiAlias);
            DrawDisc(rg);
            DrawSegments(rg);
            ringBuiltVersion_ = ringVersion_;
            ringHover_ = hotSeg;
            ringBusy_ = busy;
        }
        UpdateFilesLabel();

        Graphics& g = surface_->Begin(W_, H_);
        float t = Ease(AppearT());
        if (t < 1) {
            float sc = 0.88f + 0.12f * t;
            g.TranslateTransform(cx_, cy_);
            g.ScaleTransform(sc, sc);
            g.TranslateTransform(-cx_, -cy_);
            g.SetInterpolationMode(InterpolationModeBilinear);
        } else {
            g.SetInterpolationMode(InterpolationModeNearestNeighbor);  // exact 1:1 copy
        }
        g.SetCompositingQuality(CompositingQualityHighSpeed);
        g.DrawImage(ring_.get(), 0, 0, W_, H_);
        g.SetCompositingQuality(CompositingQualityHighQuality);
        g.SetInterpolationMode(InterpolationModeHighQualityBicubic);  // thumbnails in the centre
        DrawCenter(g);
        g.ResetTransform();
        DrawTag(g);
        surface_->Present(hwnd_, origin_, BYTE(255 * t));
    }

    HWND hwnd_ = nullptr;
    Notifier notifier_;
    ComPtr<IDropTargetHelper> helper_;
    std::unique_ptr<gfx::LayeredSurface> surface_ = std::make_unique<gfx::LayeredSurface>();

    State state_ = State::Hidden;
    Mode mode_ = Mode::Convert;
    bool keyboard_ = false;
    bool dragOver_ = false;
    bool animating_ = false;
    std::vector<fs::path> files_;
    std::vector<ActionDef> items_;
    int hover_ = -1;
    int pressed_ = -1;
    int pendingLaunch_ = -1;

    float s_ = 1;
    int W_ = 0, H_ = 0;
    float cx_ = 0, cy_ = 0;
    POINT origin_{};
    RECT tagRect_{};
    ULONGLONG shownAt_ = 0;

    uint64_t jobId_ = 0;
    std::wstring busyLabel_;
    int progressDone_ = 0, progressTotal_ = 0;
    std::vector<fs::path> outputs_;
    std::wstring message_;
    std::unique_ptr<Gdiplus::Bitmap> thumb_;
    std::unique_ptr<Gdiplus::Bitmap> backdrop_;
    std::unique_ptr<Gdiplus::Bitmap> glass_;
    std::vector<uint8_t> glassPx_;

    // Cached ring layer (see Render()).
    std::unique_ptr<Gdiplus::Bitmap> ring_;
    uint64_t ringVersion_ = 1, ringBuiltVersion_ = 0;
    int ringHover_ = -2;
    bool ringBusy_ = false;
    std::vector<fs::path> labelFiles_;
    std::wstring filesLabel_, filesName_;
    std::unique_ptr<Gdiplus::Bitmap> filesThumb_;  // first dragged file
    std::map<fs::path, std::unique_ptr<Gdiplus::Bitmap>> icons_;  // see ProgramIcon()
};

Wheel g_wheel;

LRESULT CALLBACK Wheel::Proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (g_wheel.hwnd_ == hwnd) return g_wheel.Handle(msg, wp, lp);
    return DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT Wheel::Handle(UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
        case WM_DPICHANGED: return 0;
        case WM_TIMER:
            if (wp == TIMER_ANIM) {
                if (AppearT() >= 1 && state_ != State::Working) {
                    KillTimer(hwnd_, TIMER_ANIM);
                    animating_ = false;
                } else if (AppearT() >= 1) {
                    SetTimer(hwnd_, TIMER_ANIM, 33, nullptr);
                }
                Render();
            } else if (wp == TIMER_DRAG_END) {
                KillTimer(hwnd_, TIMER_DRAG_END);
                if (state_ == State::Open && !keyboard_ && !dragOver_) Hide();
            } else if (wp == TIMER_AUTOHIDE) {
                KillTimer(hwnd_, TIMER_AUTOHIDE);
                if (state_ == State::Done || state_ == State::Failed) Hide();
            }
            return 0;
        case WM_JOB_PROGRESS:
            if ((uint64_t)wp == jobId_ && state_ == State::Working) {
                progressDone_ = LOWORD(lp);
                progressTotal_ = HIWORD(lp);
            }
            return 0;
        case WM_JOB_DONE: OnJobDone((uint64_t)wp, reinterpret_cast<JobResult*>(lp)); return 0;
        case WM_RUN_UI:
            if (pendingLaunch_ >= 0) {
                int i = pendingLaunch_;
                pendingLaunch_ = -1;
                Launch(i);
            }
            return 0;
        case WM_MOUSEMOVE: {
            if (dragOver_) return 0;
            TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, hwnd_, 0};
            TrackMouseEvent(&tme);
            POINT p{origin_.x + GET_X_LPARAM(lp), origin_.y + GET_Y_LPARAM(lp)};
            int h = HitTest(p);
            // The launcher opens centred on the cursor, so the pointer starts on the centre; that must
            // not wipe the option picked for Intro.
            bool keepKeyboardPick = keyboard_ && mode_ == Mode::Launcher && h == -2;
            if (h != hover_ && (h != -1 || !keyboard_) && !keepKeyboardPick) {
                hover_ = h;
                Render();
            }
            return 0;
        }
        case WM_MOUSELEAVE:
            if (!keyboard_ && hover_ != -1) {
                hover_ = -1;
                Render();
            }
            return 0;
        case WM_LBUTTONDOWN: {
            POINT p{origin_.x + GET_X_LPARAM(lp), origin_.y + GET_Y_LPARAM(lp)};
            int h = HitTest(p);
            pressed_ = -1;
            bool onTag = PtInRect(&tagRect_, p);
            if ((h == -2 || onTag) && state_ == State::Done && !outputs_.empty()) {
                if (DragDetect(hwnd_, p)) {
                    auto outs = outputs_;
                    DWORD effect = DragOutFiles(hwnd_, outs);
                    if (effect != DROPEFFECT_NONE && state_ == State::Done) Hide();
                } else {
                    RevealInExplorer(outputs_);
                    Hide();
                }
                return 0;
            }
            if (h >= 0 && state_ != State::Working) pressed_ = h;
            return 0;
        }
        case WM_LBUTTONUP: {
            POINT p{origin_.x + GET_X_LPARAM(lp), origin_.y + GET_Y_LPARAM(lp)};
            int h = HitTest(p);
            if (pressed_ >= 0 && h == pressed_) Launch(h);
            pressed_ = -1;
            return 0;
        }
        case WM_RBUTTONUP: {
            POINT p{origin_.x + GET_X_LPARAM(lp), origin_.y + GET_Y_LPARAM(lp)};
            int h = HitTest(p);
            if (mode_ == Mode::Launcher && h >= 0 && state_ == State::Open && !items_[h].icon.empty()) ProgramMenu(h);
            else Hide();
            return 0;
        }
    }
    return DefWindowProcW(hwnd_, msg, wp, lp);
}

}  // namespace

void Preview(std::vector<fs::path> files, Mode mode, int hover, std::vector<fs::path> outputs, const fs::path& png) {
    g_wheel.Preview(std::move(files), mode, hover, std::move(outputs), png);
}
void Create(HINSTANCE instance, Notifier onBackgroundResult) { g_wheel.Create(instance, std::move(onBackgroundResult)); }
void Destroy() { g_wheel.Destroy(); }
void ShowForDrag(POINT pt, Mode mode) { g_wheel.ShowForDrag(pt, mode); }
void ShowForFiles(POINT pt, std::vector<fs::path> files) { g_wheel.ShowForFiles(pt, std::move(files)); }
void ShowLauncher(POINT pt) { g_wheel.ShowLauncher(pt); }
void SetMode(Mode mode) { g_wheel.SetMode(mode); }
void Hide() { g_wheel.Hide(); }
void OnDragEnded() { g_wheel.OnDragEnded(); }
void OnGlobalMouseDown(POINT pt) { g_wheel.OnGlobalMouseDown(pt); }
void OnNavKey(UINT vk) { g_wheel.OnNavKey(vk); }
bool IsVisible() { return g_wheel.Visible(); }
bool KeyboardActive() { return g_wheel.KeyboardActive(); }
bool ContainsPoint(POINT pt) { return g_wheel.ContainsPoint(pt); }

}  // namespace wheel
