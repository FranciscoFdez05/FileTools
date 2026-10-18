#include "floatshelf.h"

#include "gfx.h"
#include "shellutil.h"

#include <shlobj.h>
#include <windowsx.h>
#include <wrl/client.h>

#include <algorithm>

using Microsoft::WRL::ComPtr;
using namespace Gdiplus;

namespace {

constexpr wchar_t kClass[] = L"FileTools.Float";

class Shelf;
std::vector<std::unique_ptr<Shelf>> g_shelves;

class Shelf final : public IDropTarget {
public:
    Shelf(std::vector<fs::path> files, POINT anchor) : files_(std::move(files)) {
        s_ = gfx::DpiScaleAt(anchor);
        W_ = int(150 * s_);
        H_ = int(178 * s_);
        static bool registered = false;
        if (!registered) {
            WNDCLASSEXW wc{sizeof(wc)};
            wc.lpfnWndProc = &Shelf::Proc;
            wc.hInstance = GetModuleHandleW(nullptr);
            wc.hCursor = LoadCursor(nullptr, IDC_HAND);
            wc.lpszClassName = kClass;
            RegisterClassExW(&wc);
            registered = true;
        }
        MONITORINFO mi{sizeof(mi)};
        GetMonitorInfoW(MonitorFromPoint(anchor, MONITOR_DEFAULTTONEAREST), &mi);
        pos_ = {std::clamp<LONG>(anchor.x - W_ / 2, mi.rcWork.left, mi.rcWork.right - W_),
                std::clamp<LONG>(anchor.y - H_ / 2, mi.rcWork.top, mi.rcWork.bottom - H_)};
        hwnd_ = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, kClass,
                                L"FileTools", WS_POPUP, pos_.x, pos_.y, W_, H_, nullptr, nullptr,
                                GetModuleHandleW(nullptr), this);
        SetWindowLongPtrW(hwnd_, GWLP_USERDATA, (LONG_PTR)this);
        RegisterDragDrop(hwnd_, this);
        CoCreateInstance(CLSID_DragDropHelper, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&helper_));
        LoadThumb();
        Render();
        ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
    }

    ~Shelf() {
        if (hwnd_) {
            RevokeDragDrop(hwnd_);
            DestroyWindow(hwnd_);
        }
    }

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
        if (helper_) helper_->DragEnter(hwnd_, data, &p, *effect);
        incoming_ = !FilesFromDataObject(data).empty() && !draggingOut_;
        *effect = incoming_ ? (*effect & DROPEFFECT_LINK ? DROPEFFECT_LINK : DROPEFFECT_COPY) : DROPEFFECT_NONE;
        Render();
        return S_OK;
    }
    STDMETHODIMP DragOver(DWORD, POINTL pt, DWORD* effect) override {
        POINT p{pt.x, pt.y};
        if (helper_) helper_->DragOver(&p, *effect);
        *effect = incoming_ ? (*effect & DROPEFFECT_LINK ? DROPEFFECT_LINK : DROPEFFECT_COPY) : DROPEFFECT_NONE;
        return S_OK;
    }
    STDMETHODIMP DragLeave() override {
        if (helper_) helper_->DragLeave();
        incoming_ = false;
        Render();
        return S_OK;
    }
    STDMETHODIMP Drop(IDataObject* data, DWORD, POINTL pt, DWORD* effect) override {
        POINT p{pt.x, pt.y};
        if (helper_) helper_->Drop(data, &p, *effect);
        if (incoming_) {
            for (auto& f : FilesFromDataObject(data))
                if (std::find(files_.begin(), files_.end(), f) == files_.end()) files_.push_back(f);
            LoadThumb();
        }
        *effect = incoming_ ? DROPEFFECT_COPY : DROPEFFECT_NONE;
        incoming_ = false;
        Render();
        return S_OK;
    }

private:
    static LRESULT CALLBACK Proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
        auto* self = reinterpret_cast<Shelf*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (self) return self->Handle(msg, wp, lp);
        return DefWindowProcW(hwnd, msg, wp, lp);
    }

    LRESULT Handle(UINT msg, WPARAM wp, LPARAM lp) {
        switch (msg) {
            case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
            case WM_MOVE: {
                RECT r;
                GetWindowRect(hwnd_, &r);
                pos_ = {r.left, r.top};
                return 0;
            }
            case WM_LBUTTONDOWN: {
                POINT local{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
                if (PtInRect(&closeRect_, local)) {
                    Close();
                    return 0;
                }
                if (local.y < 26 * s_) {  // grip: move the card
                    ReleaseCapture();
                    SendMessageW(hwnd_, WM_NCLBUTTONDOWN, HTCAPTION, 0);
                    return 0;
                }
                POINT screen{pos_.x + local.x, pos_.y + local.y};
                if (DragDetect(hwnd_, screen)) {
                    std::vector<fs::path> existing;
                    std::error_code ec;
                    for (auto& f : files_)
                        if (fs::exists(f, ec)) existing.push_back(f);
                    draggingOut_ = true;
                    DWORD effect = DragOutFiles(hwnd_, existing);
                    draggingOut_ = false;
                    if (effect & DROPEFFECT_MOVE) Close();
                } else {
                    RevealInExplorer(files_);
                }
                return 0;
            }
            case WM_RBUTTONUP: Close(); return 0;
        }
        return DefWindowProcW(hwnd_, msg, wp, lp);
    }

    void Close() {
        // Callers return straight after this, so deleting ourselves here is safe.
        auto it = std::find_if(g_shelves.begin(), g_shelves.end(), [this](auto& p) { return p.get() == this; });
        if (it == g_shelves.end()) return;
        it->release();
        g_shelves.erase(it);
        HWND h = hwnd_;
        hwnd_ = nullptr;
        RevokeDragDrop(h);
        SetWindowLongPtrW(h, GWLP_USERDATA, 0);
        DestroyWindow(h);
        delete this;
    }

    void LoadThumb() { thumb_ = files_.empty() ? nullptr : ShellThumbnail(files_[0], int(110 * s_)); }

    void Render() {
        if (!hwnd_) return;
        Graphics& g = surface_->Begin(W_, H_);
        float pad = 6 * s_;
        RectF card(pad, pad, W_ - pad * 2, H_ - pad * 2);
        for (int i = 5; i >= 1; --i) {
            GraphicsPath sp;
            float grow = i * 1.3f * s_;
            gfx::AddRoundRect(sp, RectF(card.X - grow, card.Y - grow + 2 * s_, card.Width + 2 * grow, card.Height + 2 * grow),
                              16 * s_ + grow);
            SolidBrush sb(Color(10, 0, 0, 0));
            g.FillPath(&sb, &sp);
        }
        GraphicsPath cp;
        gfx::AddRoundRect(cp, card, 16 * s_);
        LinearGradientBrush bg(PointF(0, card.Y), PointF(0, card.Y + card.Height),
                               incoming_ ? Color(240, 214, 232, 255) : Color(235, 255, 248, 240),
                               incoming_ ? Color(240, 160, 200, 250) : Color(235, 255, 226, 200));
        g.FillPath(&bg, &cp);
        Pen rim(Color(160, 255, 255, 255), 1.2f * s_);
        g.DrawPath(&rim, &cp);

        // Grip.
        SolidBrush grip(Color(90, 120, 70, 30));
        GraphicsPath gp;
        gfx::AddRoundRect(gp, RectF(W_ / 2.0f - 18 * s_, card.Y + 8 * s_, 36 * s_, 4 * s_), 2 * s_);
        g.FillPath(&grip, &gp);

        // Close button.
        float d = 18 * s_;
        RectF close(card.X + card.Width - d - 6 * s_, card.Y + 5 * s_, d, d);
        SolidBrush closeBg(Color(70, 120, 60, 20));
        g.FillEllipse(&closeBg, close);
        Font iconFont(gfx::IconFamily(), 8 * s_, FontStyleRegular, UnitPixel);
        SolidBrush dark(Color(255, 70, 45, 25));
        gfx::DrawCentered(g, std::wstring(1, wchar_t(0xE8BB)), iconFont, dark, close);
        closeRect_ = {LONG(close.X), LONG(close.Y), LONG(close.X + close.Width), LONG(close.Y + close.Height)};

        RectF thumbBox(card.X + 16 * s_, card.Y + 24 * s_, card.Width - 32 * s_, card.Height - 66 * s_);
        if (thumb_) {
            RectF r = gfx::DrawFitted(g, *thumb_, thumbBox);
            if (files_.size() > 1) {
                float b = 24 * s_;
                RectF badge(r.X + r.Width - b * 0.7f, r.Y - b * 0.3f, b, b);
                SolidBrush red(Color(255, 232, 72, 40));
                g.FillEllipse(&red, badge);
                Font f(gfx::TextFamily(), 11.5f * s_, FontStyleBold, UnitPixel);
                SolidBrush white(Color(255, 255, 255, 255));
                gfx::DrawCentered(g, std::to_wstring(files_.size()), f, white, badge);
            }
        }
        std::wstring name = files_.empty() ? L"Suelta archivos"
                                           : (files_.size() == 1 ? files_[0].filename().wstring()
                                                                 : std::to_wstring(files_.size()) + L" archivos");
        Font nameFont(gfx::TextFamily(), 11.5f * s_, FontStyleBold, UnitPixel);
        float tw = std::min(gfx::TextWidth(g, name, nameFont) + 16 * s_, card.Width - 12 * s_);
        RectF tag(W_ / 2.0f - tw / 2, card.Y + card.Height - 34 * s_, tw, 22 * s_);
        GraphicsPath tp;
        gfx::AddRoundRect(tp, tag, 6 * s_);
        SolidBrush blue(Color(240, 0, 103, 214));
        g.FillPath(&blue, &tp);
        SolidBrush white(Color(255, 255, 255, 255));
        gfx::DrawCentered(g, name, nameFont, white, RectF(tag.X + 6 * s_, tag.Y, tag.Width - 12 * s_, tag.Height));
        surface_->Present(hwnd_, pos_, 255);
    }

    HWND hwnd_ = nullptr;
    std::vector<fs::path> files_;
    std::unique_ptr<Gdiplus::Bitmap> thumb_;
    std::unique_ptr<gfx::LayeredSurface> surface_ = std::make_unique<gfx::LayeredSurface>();
    ComPtr<IDropTargetHelper> helper_;
    POINT pos_{};
    RECT closeRect_{};
    float s_ = 1;
    int W_ = 0, H_ = 0;
    bool incoming_ = false;
    bool draggingOut_ = false;
};

}  // namespace

void OpenFloatShelf(const std::vector<fs::path>& files, POINT anchor) {
    g_shelves.push_back(std::make_unique<Shelf>(files, anchor));
}

void CloseAllFloatShelves() { g_shelves.clear(); }
