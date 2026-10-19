#include "sendpanel.h"

#include "fileserver.h"
#include "glass.h"
#include "media.h"
#include "qrcode.h"
#include "shellutil.h"

#include <shellscalingapi.h>
#include <uxtheme.h>
#include <windowsx.h>
#include <unknwn.h>
#include <winrt/base.h>

#include <algorithm>
#include <cmath>
#include <optional>
#include <thread>

using Microsoft::WRL::ComPtr;
using namespace Gdiplus;

// Drawn by hand with the same glass look as the info panel (see glass.h).

namespace {

using namespace glass;

constexpr wchar_t kClass[] = L"FileTools.Send";
constexpr UINT WM_PREPARED = WM_APP + 1;
constexpr UINT_PTR TIMER_STATS = 1;
constexpr DWORD kStyle = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
enum : int { BTN_NONE = 0, BTN_COPY, BTN_BLUETOOTH, BTN_SHARE, BTN_STOP, HIT_URL, HIT_NETWORK };
enum class Tone { Normal, Good, Bad };
enum class FontKind { Title, Small, Body, Mono, Button, Count };

// What is actually served: folders become temporary ZIPs.
struct Prepared {
    std::vector<fs::path> files;
    fs::path tempDir;
    std::wstring error;
};

Prepared Prepare(const std::vector<fs::path>& files) {
    Prepared p;
    try {
        for (const auto& f : files) {
            std::error_code ec;
            if (fs::is_directory(f, ec)) {
                if (f.filename().empty()) Fail(L"No se puede enviar una unidad entera");
                if (p.tempDir.empty()) p.tempDir = MakeTempDir();
                fs::path zip = UniquePath(p.tempDir, f.filename().wstring(), L"zip");
                CreateArchive(zip, f.parent_path(), {f.filename().wstring()});
                p.files.push_back(zip);
            } else if (fs::is_regular_file(f, ec)) {
                p.files.push_back(f);
            }
        }
        if (p.files.empty()) p.error = L"No hay archivos que enviar";
    } catch (const AppError& e) {
        p.error = e.message;
    } catch (const std::exception& e) {
        p.error = Utf8ToWide(e.what());
    }
    return p;
}

void RemoveTemp(const fs::path& dir) {
    std::error_code ec;
    if (!dir.empty()) fs::remove_all(dir, ec);
}

class Panel;
std::vector<std::unique_ptr<Panel>> g_panels;

class Panel {
public:
    Panel(std::vector<fs::path> files, POINT anchor) : files_(std::move(files)) {
        static bool registered = false;
        HINSTANCE inst = GetModuleHandleW(nullptr);
        if (!registered) {
            BufferedPaintInit();
            WNDCLASSEXW wc{sizeof(wc)};
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
        LoadThumb();

        float W = std::round(440 * s_);
        Layout(W);
        RECT r{0, 0, LONG(W), LONG(clientH_)};
        AdjustWindowRectExForDpi(&r, kStyle, FALSE, 0, dpi);
        const RECT& wa = mi.rcWork;
        int w = r.right - r.left, h = std::min(int(r.bottom - r.top), int(wa.bottom - wa.top));
        int x = wa.left + (wa.right - wa.left - w) / 2, y = wa.top + (wa.bottom - wa.top - h) / 2;
        CreateWindowExW(0, kClass, L"Enviar a otro dispositivo", kStyle, x, y, w, h, nullptr, nullptr, inst, this);
        if (!hwnd_) return;
        glass_ = ApplyBackdrop(hwnd_);
        ShowWindow(hwnd_, SW_SHOWNORMAL);
        ForceForeground(hwnd_);
        SetTimer(hwnd_, TIMER_STATS, 1000, nullptr);

        bool folders = std::any_of(files_.begin(), files_.end(), [](const fs::path& f) {
            std::error_code ec;
            return fs::is_directory(f, ec);
        });
        if (!folders) {
            OnPrepared(Prepare(files_));
            return;
        }
        SetStatus(L"Comprimiendo la carpeta…", Tone::Normal);
        std::thread([hwnd = hwnd_, files = files_] {
            auto* p = new Prepared(Prepare(files));
            if (!PostMessageW(hwnd, WM_PREPARED, 0, (LPARAM)p)) {
                RemoveTemp(p->tempDir);
                delete p;
            }
        }).detach();
    }

    ~Panel() {
        server_.reset();  // stops the server before its files go away
        RemoveTemp(prepared_.tempDir);
    }

    HWND hwnd() const { return hwnd_; }

private:
    struct Button {
        int id;
        std::wstring label;
        RectF r;
        bool primary;
    };

    // --- text ------------------------------------------------------------------------

    void MakeFonts() {
        using F = TextQueue::Family;
        text_.SetFont(int(FontKind::Title), F::Display, 20 * s_, DWRITE_FONT_WEIGHT_SEMI_BOLD);
        text_.SetFont(int(FontKind::Small), F::Text, 13 * s_, DWRITE_FONT_WEIGHT_NORMAL);
        text_.SetFont(int(FontKind::Body), F::Text, 14 * s_, DWRITE_FONT_WEIGHT_NORMAL);
        text_.SetFont(int(FontKind::Mono), F::Mono, 12 * s_, DWRITE_FONT_WEIGHT_NORMAL);
        text_.SetFont(int(FontKind::Button), F::Text, 14 * s_, DWRITE_FONT_WEIGHT_NORMAL);
    }

    float Measure(const std::wstring& text, FontKind k) const { return text_.Measure(text, int(k)); }

    void Text(const std::wstring& text, FontKind k, const Color& c, const RectF& r,
              DWRITE_TEXT_ALIGNMENT align = DWRITE_TEXT_ALIGNMENT_LEADING, bool wrap = false) {
        text_.Add(text, int(k), c, r, align, wrap);
    }

    // --- layout ----------------------------------------------------------------------

    float ButtonWidth(const std::wstring& label) const { return std::max(92 * s_, Measure(label, FontKind::Button) + 40 * s_); }

    void Layout(float W) {
        float m = 20 * s_, y = m + 58 * s_;
        float card = std::round(260 * s_);
        qrCard_ = Snap(RectF(std::round((W - card) / 2), y, card, card));
        y += card + 8 * s_;
        hintRect_ = Snap(RectF(m, y, W - 2 * m, 18 * s_));
        y += 18 * s_ + 12 * s_;

        buttons_.clear();
        float bh = 34 * s_, copyW = ButtonWidth(L"Copiar");
        urlRect_ = Snap(RectF(m, y, W - 2 * m - copyW - 8 * s_, bh));
        buttons_.push_back({BTN_COPY, L"Copiar", Snap(RectF(W - m - copyW, y, copyW, bh)), false});
        y += bh + 10 * s_;
        netRect_ = Snap(RectF(m + 4 * s_, y, W - 2 * m - 8 * s_, 18 * s_));
        y += 18 * s_ + 6 * s_;
        statusRect_ = Snap(RectF(m + 4 * s_, y, W - 2 * m - 8 * s_, 36 * s_));
        y += 36 * s_ + 14 * s_;

        float x = m;
        for (auto [id, label] : {std::pair{BTN_BLUETOOTH, L"Bluetooth…"}, std::pair{BTN_SHARE, L"Compartir…"}}) {
            float w = ButtonWidth(label);
            buttons_.push_back({id, label, Snap(RectF(x, y, w, bh)), false});
            x += w + 8 * s_;
        }
        float stopW = ButtonWidth(L"Detener");
        buttons_.push_back({BTN_STOP, L"Detener", Snap(RectF(W - m - stopW, y, stopW, bh)), true});
        y += bh + m;
        clientH_ = std::round(y);
    }

    void Relayout() {
        RECT rc;
        GetClientRect(hwnd_, &rc);
        Layout(float(rc.right));
        Invalidate();
    }

    void LoadThumb() {
        icon_.reset();
        if (files_.size() == 1) icon_ = ShellThumbnail(files_[0], int(std::round(40 * s_)));
    }

    // --- painting --------------------------------------------------------------------

    void Invalidate() {
        if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
    }

    void Paint() {
        PaintBuffered(hwnd_, text_, [this](Graphics& g, float W, float H) { Draw(g, W, H); });
    }

    std::wstring Summary() const {
        const auto& list = prepared_.files.empty() ? files_ : prepared_.files;
        uint64_t total = 0;
        for (const auto& f : list) {
            std::error_code ec;
            if (fs::is_regular_file(f, ec)) total += fs::file_size(f, ec);
        }
        std::wstring what = list.size() == 1 ? list[0].filename().wstring() : std::to_wstring(list.size()) + L" archivos";
        return total ? what + L" · " + FormatSize(total) : what;
    }

    void Draw(Graphics& g, float W, float H) {
        float m = 20 * s_;
        FillBackdrop(g, W, H, glass_);
        float tx = m;
        if (icon_) {
            gfx::DrawFitted(g, *icon_, RectF(m, m, std::round(40 * s_), std::round(40 * s_)));
            tx += std::round(40 * s_) + 12 * s_;
        }
        Text(L"Enviar a otro dispositivo", FontKind::Title, kInk, RectF(tx, m - 2 * s_, W - m - tx, 26 * s_));
        Text(Summary(), FontKind::Small, kMuted, RectF(tx, m + 24 * s_, W - m - tx, 18 * s_));

        DrawQr(g);
        Text(qr_ ? L"Escanéalo con la cámara del otro dispositivo" : L"", FontKind::Small, kMuted, hintRect_,
             DWRITE_TEXT_ALIGNMENT_CENTER);

        // Link: click to copy.
        bool urlHot = hover_ == HIT_URL && !url_.empty();
        FillGlass(g, urlRect_, urlRect_.Height / 2, 225, 195, 240, s_, urlHot ? 70 : 34);
        if (urlHot) {
            GraphicsPath p;
            gfx::AddRoundRect(p, urlRect_, urlRect_.Height / 2);
            SolidBrush tint(Accent(26));
            g.FillPath(&tint, &p);
        }
        RectF urlText(urlRect_.X + 10 * s_, urlRect_.Y, urlRect_.Width - 20 * s_, urlRect_.Height);
        if (url_.empty()) Text(server_ ? L"Sin enlace" : L"…", FontKind::Body, kMuted, urlText, DWRITE_TEXT_ALIGNMENT_CENTER);
        else Text(url_, FontKind::Mono, kAccentInk, urlText, DWRITE_TEXT_ALIGNMENT_CENTER);

        if (!addrs_.empty()) {
            const LanAddress& a = addrs_[addrIndex_];
            std::wstring net = L"Red: " + a.adapter + L" · " + a.ip;
            if (addrs_.size() > 1) net += L"   ·   Cambiar de red";
            Text(net, FontKind::Small, hover_ == HIT_NETWORK && addrs_.size() > 1 ? kAccentInk : kMuted, netRect_);
        }
        Color tone = tone_ == Tone::Good ? kGood : tone_ == Tone::Bad ? kBad : kLabel;
        Text(status_, FontKind::Small, tone, statusRect_, DWRITE_TEXT_ALIGNMENT_LEADING, true);

        for (const auto& b : buttons_) DrawButton(g, b);
    }

    void DrawQr(Graphics& g) {
        Shadow(g, qrCard_, 16 * s_, s_);
        FillGlass(g, qrCard_, 16 * s_, 255, 255, 255, s_, 40);
        if (!qr_) {
            std::wstring text = !server_ ? (prepared_.error.empty() ? L"Preparando…" : L"No disponible")
                                         : L"Conéctate a una red Wi-Fi";
            Text(text, FontKind::Body, kMuted, qrCard_, DWRITE_TEXT_ALIGNMENT_CENTER);
            return;
        }
        // Whole-pixel modules with a 4-module quiet zone inside the white card.
        int n = qr_->size;
        int module = std::max(1, int((qrCard_.Width - 8 * s_) / (n + 8)));
        int side = module * n;
        int ox = int(qrCard_.X + (qrCard_.Width - side) / 2), oy = int(qrCard_.Y + (qrCard_.Height - side) / 2);
        SmoothingMode oldSmooth = g.GetSmoothingMode();
        PixelOffsetMode oldOffset = g.GetPixelOffsetMode();
        g.SetSmoothingMode(SmoothingModeNone);
        g.SetPixelOffsetMode(PixelOffsetModeNone);
        SolidBrush ink(Color(255, 0, 0, 0));
        for (int y = 0; y < n; ++y)
            for (int x = 0; x < n;) {
                if (!qr_->Dark(x, y)) {
                    ++x;
                    continue;
                }
                int run = x;
                while (run < n && qr_->Dark(run, y)) ++run;
                g.FillRectangle(&ink, ox + x * module, oy + y * module, (run - x) * module, module);
                x = run;
            }
        g.SetSmoothingMode(oldSmooth);
        g.SetPixelOffsetMode(oldOffset);
    }

    bool ButtonEnabled(int id) const {
        if (id == BTN_COPY) return !url_.empty();
        if (id == BTN_BLUETOOTH || id == BTN_SHARE) return !prepared_.files.empty();
        return true;
    }

    void DrawButton(Graphics& g, const Button& b) {
        glass::DrawButton(g, text_, int(FontKind::Button), b.r, b.label, b.primary, ButtonEnabled(b.id), hover_ == b.id,
                          pressed_ == b.id, s_);
    }

    // --- hit testing -----------------------------------------------------------------

    static bool In(const RectF& r, POINT p) {
        return p.x >= r.X && p.x < r.X + r.Width && p.y >= r.Y && p.y < r.Y + r.Height;
    }

    int Hit(POINT p) const {
        for (const auto& b : buttons_)
            if (In(b.r, p)) return ButtonEnabled(b.id) ? b.id : BTN_NONE;
        if (In(urlRect_, p) && !url_.empty()) return HIT_URL;
        if (In(netRect_, p) && addrs_.size() > 1) return HIT_NETWORK;
        return BTN_NONE;
    }

    // --- server ----------------------------------------------------------------------

    void OnPrepared(Prepared p) {
        prepared_ = std::move(p);
        if (!prepared_.error.empty()) {
            SetStatus(prepared_.error, Tone::Bad);
            return;
        }
        try {
            server_ = std::make_unique<FileServer>(prepared_.files);
        } catch (const AppError& e) {
            SetStatus(e.message, Tone::Bad);
            return;
        }
        started_ = GetTickCount64();
        RefreshAddresses();
        UpdateStatus();
    }

    void RefreshAddresses() {
        if (!server_) return;
        std::wstring current = addrs_.empty() ? L"" : addrs_[addrIndex_].ip;
        addrs_ = LanAddresses();
        addrIndex_ = 0;
        for (size_t i = 0; i < addrs_.size(); ++i)
            if (addrs_[i].ip == current) addrIndex_ = i;
        UpdateUrl();
    }

    void UpdateUrl() {
        std::wstring url = addrs_.empty() ? L"" : server_->Url(addrs_[addrIndex_].ip);
        if (url == url_) return;
        url_ = url;
        qr_.reset();
        if (!url_.empty()) {
            try {
                qr_ = EncodeQr(WideToUtf8(url_));
            } catch (const AppError&) {
            }
        }
        Invalidate();
    }

    void UpdateStatus() {
        if (!server_ || GetTickCount64() < messageUntil_) return;
        auto st = server_->GetStats();
        if (addrs_.empty()) {
            SetStatus(L"El PC no está conectado a ninguna red. Conéctalo a la misma Wi-Fi que el otro dispositivo.",
                      Tone::Bad);
        } else if (st.active > 0) {
            int pct = st.total ? int(st.sent * 100 / st.total) : 0;
            SetStatus(L"Enviando… " + std::to_wstring(pct) + L" % · " + FormatSize(st.sent) + L" de " +
                          FormatSize(st.total) + (st.active > 1 ? L" (" + std::to_wstring(st.active) + L" descargas)" : L""),
                      Tone::Normal);
        } else if (st.completed > 0) {
            SetStatus(L"✓ " + std::to_wstring(st.completed) +
                          (st.completed == 1 ? L" descarga completada" : L" descargas completadas") +
                          L". Sigue disponible hasta que pulses Detener.",
                      Tone::Good);
        } else if (!st.lastClient.empty()) {
            SetStatus(L"✓ Conectado desde " + st.lastClient, Tone::Good);
        } else if (GetTickCount64() - started_ > 30000) {
            SetStatus(L"¿No carga? Los dos dispositivos deben estar en la misma red y el Firewall de Windows debe "
                      L"permitir FileTools.",
                      Tone::Normal);
        } else {
            SetStatus(L"Abre el enlace en un dispositivo conectado a la misma red (Wi-Fi o cable).", Tone::Normal);
        }
    }

    // A message that stays a few seconds before the live status comes back.
    void Flash(const std::wstring& text, Tone tone) {
        messageUntil_ = 0;
        SetStatus(text, tone);
        messageUntil_ = GetTickCount64() + 4000;
    }

    void SetStatus(const std::wstring& text, Tone tone) {
        if (text == status_ && tone == tone_) return;
        status_ = text;
        tone_ = tone;
        Invalidate();
    }

    void OnButton(int id) {
        switch (id) {
            case BTN_COPY:
            case HIT_URL: CopyLink(); break;
            case HIT_NETWORK:
                addrIndex_ = (addrIndex_ + 1) % addrs_.size();
                UpdateUrl();
                break;
            case BTN_BLUETOOTH:
                if (SendViaBluetooth(prepared_.files)) Flash(L"Elige el dispositivo Bluetooth en el asistente", Tone::Normal);
                else Flash(L"Este PC no tiene Bluetooth o está desactivado", Tone::Bad);
                break;
            case BTN_SHARE:
                if (ShareFiles(hwnd_, prepared_.files))
                    Flash(L"Elige «Uso compartido cercano» para enviarlo por Wi-Fi o Bluetooth a otro PC", Tone::Normal);
                else Flash(L"No se pudo abrir el panel Compartir", Tone::Bad);
                break;
            case BTN_STOP: PostMessageW(hwnd_, WM_CLOSE, 0, 0); break;
        }
    }

    void CopyLink() {
        if (url_.empty()) return;
        if (CopyTextToClipboard(hwnd_, url_)) Flash(L"✓ Enlace copiado", Tone::Good);
        else Flash(L"No se pudo acceder al portapapeles", Tone::Bad);
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
            KillTimer(hwnd, TIMER_STATS);
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
            case WM_SIZE: Relayout(); return 0;
            case WM_DPICHANGED: {
                s_ = HIWORD(wp) / 96.0f;
                MakeFonts();
                LoadThumb();
                float W = std::round(440 * s_);
                Layout(W);
                RECT r{0, 0, LONG(W), LONG(clientH_)};
                AdjustWindowRectExForDpi(&r, kStyle, FALSE, 0, HIWORD(wp));
                const RECT* suggested = reinterpret_cast<const RECT*>(lp);
                SetWindowPos(hwnd_, nullptr, suggested->left, suggested->top, r.right - r.left, r.bottom - r.top,
                             SWP_NOZORDER | SWP_NOACTIVATE);
                return 0;
            }
            case WM_TIMER:
                if (wp == TIMER_STATS) {
                    if (server_ && (addrs_.empty() || ++ticks_ % 5 == 0)) RefreshAddresses();
                    UpdateStatus();
                }
                return 0;
            case WM_SETCURSOR:
                if (LOWORD(lp) == HTCLIENT) {
                    POINT p;
                    GetCursorPos(&p);
                    ScreenToClient(hwnd_, &p);
                    SetCursor(LoadCursor(nullptr, Hit(p) ? IDC_HAND : IDC_ARROW));
                    return TRUE;
                }
                break;
            case WM_MOUSEMOVE: {
                if (!tracking_) {
                    TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, hwnd_, 0};
                    tracking_ = TrackMouseEvent(&tme) != FALSE;
                }
                int h = Hit({GET_X_LPARAM(lp), GET_Y_LPARAM(lp)});
                if (h != hover_) {
                    hover_ = h;
                    Invalidate();
                }
                return 0;
            }
            case WM_MOUSELEAVE:
                tracking_ = false;
                if (hover_) {
                    hover_ = BTN_NONE;
                    Invalidate();
                }
                return 0;
            case WM_LBUTTONDOWN:
                if (int h = Hit({GET_X_LPARAM(lp), GET_Y_LPARAM(lp)})) {
                    pressed_ = h;
                    SetCapture(hwnd_);
                    Invalidate();
                }
                return 0;
            case WM_LBUTTONUP: {
                int pressed = pressed_;
                pressed_ = BTN_NONE;
                if (pressed) ReleaseCapture();
                if (pressed && Hit({GET_X_LPARAM(lp), GET_Y_LPARAM(lp)}) == pressed) OnButton(pressed);
                Invalidate();
                return 0;
            }
            case WM_CAPTURECHANGED: pressed_ = BTN_NONE; return 0;
            case WM_KEYDOWN:
                if (wp == VK_ESCAPE) PostMessageW(hwnd_, WM_CLOSE, 0, 0);
                else if (wp == 'C' && GetKeyState(VK_CONTROL) < 0) CopyLink();
                return 0;
            case WM_PREPARED: {
                std::unique_ptr<Prepared> p(reinterpret_cast<Prepared*>(lp));
                OnPrepared(std::move(*p));
                return 0;
            }
            case WM_CLOSE: DestroyWindow(hwnd_); return 0;
        }
        return DefWindowProcW(hwnd_, msg, wp, lp);
    }

    std::vector<fs::path> files_;
    Prepared prepared_;
    std::unique_ptr<FileServer> server_;
    std::vector<LanAddress> addrs_;
    size_t addrIndex_ = 0;
    std::wstring url_;
    std::optional<QrCode> qr_;
    ULONGLONG started_ = 0, messageUntil_ = 0;
    unsigned ticks_ = 0;

    HWND hwnd_ = nullptr;
    float s_ = 1, clientH_ = 0;
    bool glass_ = false, tracking_ = false;
    TextQueue text_;
    std::unique_ptr<Bitmap> icon_;
    RectF qrCard_, hintRect_, urlRect_, netRect_, statusRect_;
    std::vector<Button> buttons_;
    int hover_ = BTN_NONE, pressed_ = BTN_NONE;
    std::wstring status_;
    Tone tone_ = Tone::Normal;
};

}  // namespace

void OpenSendPanel(const std::vector<fs::path>& files, POINT anchor) {
    if (files.empty()) return;
    auto panel = std::make_unique<Panel>(files, anchor);
    if (panel->hwnd()) g_panels.push_back(std::move(panel));
}

void CloseAllSendPanels() {
    std::vector<HWND> windows;
    for (auto& p : g_panels) windows.push_back(p->hwnd());
    for (HWND h : windows) DestroyWindow(h);
    g_panels.clear();
}

bool SendPanelsOpen() { return !g_panels.empty(); }
