#include "settingspanel.h"

#include "fileinfo.h"
#include "floatshelf.h"
#include "glass.h"
#include "media.h"

#include <shellscalingapi.h>
#include <uxtheme.h>
#include <shobjidl.h>
#include <windowsx.h>

#include <algorithm>
#include <cmath>
#include <thread>

using Microsoft::WRL::ComPtr;
using namespace Gdiplus;

// Drawn by hand with the same glass look as the other windows (see glass.h). Changes apply and are saved
// at once; there is no OK/Cancel.

namespace {

using namespace glass;

constexpr wchar_t kClass[] = L"FileTools.Settings";
constexpr UINT WM_COMPONENTS = WM_APP + 1;
constexpr UINT WM_EXIF_CHECKED = WM_APP + 2;
constexpr DWORD kStyle = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
constexpr DWORD kQualities[] = {60, 75, 85, 90, 95, 100};
constexpr const wchar_t* kOutputs[] = {L"Junto al original", L"Descargas", L"Escritorio"};

enum : int {
    HIT_NONE = 0,
    HIT_OUT = 100,      // + output mode
    HIT_QUALITY = 200,  // + index in kQualities
    HIT_STARTUP = 300,
    HIT_NOTIFY,
    BTN_EXIF = 400,
    BTN_FLOATS,
    BTN_QUIT,
    BTN_CLOSE,
    HIT_KEY = 500,    // + shortcut index: click to type a new one
    BTN_RESET = 510,  // + shortcut index: back to the default
};
constexpr int kShortcuts = 3;  // 0 = wheel, 1 = settings, 2 = launcher
constexpr DWORD kDefaultHotkeys[kShortcuts] = {kDefaultWheelHotkey, kDefaultSettingsHotkey, kDefaultLauncherHotkey};
constexpr const wchar_t* kShortcutUse[kShortcuts] = {L"Abrir la rueda con la selección del Explorador",
                                                     L"Abrir estos ajustes",
                                                     L"Abrir la Lanzadera (capturas y programas)"};

DWORD& HotkeyOf(int i) {
    Settings& s = AppSettings();
    return i == 0 ? s.wheelHotkey : i == 1 ? s.settingsHotkey : s.launcherHotkey;
}

// Index of the other shortcut already set to `hotkey`, or -1.
int ShortcutUsing(DWORD hotkey, int except) {
    for (int i = 0; i < kShortcuts; ++i)
        if (i != except && HotkeyOf(i) == hotkey) return i;
    return -1;
}

// "Ctrl+Alt+" while the user is still holding the modifiers.
std::wstring ModifiersName(UINT mods) {
    std::wstring s;
    if (mods & MOD_CONTROL) s += L"Ctrl+";
    if (mods & MOD_ALT) s += L"Alt+";
    if (mods & MOD_SHIFT) s += L"Mayús+";
    return s;
}
enum Font : int { F_TITLE, F_SMALL, F_BODY, F_GROUP, F_BUTTON };
enum class Tone { Normal, Good, Bad };

struct Components {
    bool ffmpeg = false;
    std::wstring exifVersion;  // empty: not found
};

struct ExifCheck {
    fs::path path;
    std::wstring version;
};

class Panel;
std::unique_ptr<Panel> g_panel;

class Panel {
public:
    Panel(POINT anchor, SettingsActions actions) : actions_(std::move(actions)) {
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

        float W = std::round(500 * s_);
        Layout(W);
        RECT r{0, 0, LONG(W), LONG(clientH_)};
        AdjustWindowRectExForDpi(&r, kStyle, FALSE, 0, dpi);
        const RECT& wa = mi.rcWork;
        int w = r.right - r.left, h = std::min(int(r.bottom - r.top), int(wa.bottom - wa.top));
        int x = wa.left + (wa.right - wa.left - w) / 2, y = wa.top + (wa.bottom - wa.top - h) / 2;
        CreateWindowExW(0, kClass, L"Ajustes de FileTools", kStyle, x, y, w, h, nullptr, nullptr, inst, this);
        if (!hwnd_) return;
        glass_ = ApplyBackdrop(hwnd_);
        status_ = L"Los cambios se guardan al momento";
        ShowWindow(hwnd_, SW_SHOWNORMAL);
        ForceForeground(hwnd_);
        CheckComponents();
    }

    ~Panel() {
        // Closed while waiting for a new shortcut: the old ones were unregistered.
        if (capturing_ >= 0 && actions_.applyHotkeys) actions_.applyHotkeys();
    }

    HWND hwnd() const { return hwnd_; }

    void Activate() {
        if (IsIconic(hwnd_)) ShowWindow(hwnd_, SW_RESTORE);
        ForceForeground(hwnd_);
    }

private:
    struct Button {
        int id;
        std::wstring label;
        RectF r;
        bool primary;
    };

    // --- layout ----------------------------------------------------------------------

    void MakeFonts() {
        using F = TextQueue::Family;
        text_.SetFont(F_TITLE, F::Display, 24 * s_, DWRITE_FONT_WEIGHT_SEMI_BOLD);
        text_.SetFont(F_SMALL, F::Text, 13 * s_, DWRITE_FONT_WEIGHT_NORMAL);
        text_.SetFont(F_BODY, F::Text, 14 * s_, DWRITE_FONT_WEIGHT_NORMAL);
        text_.SetFont(F_GROUP, F::Text, 12 * s_, DWRITE_FONT_WEIGHT_SEMI_BOLD);
        text_.SetFont(F_BUTTON, F::Text, 14 * s_, DWRITE_FONT_WEIGHT_NORMAL);
    }

    float M() const { return 20 * s_; }
    float RowH() const { return 42 * s_; }
    float ShortcutH() const { return 44 * s_; }

    RectF KeyChip(int i) const {
        RectF row = Row(shortcutsCard_, i, ShortcutH());
        return Snap(RectF(row.X + 8 * s_, row.Y + 7 * s_, 160 * s_, row.Height - 14 * s_));
    }

    bool CanReset(int i) const { return HotkeyOf(i) != kDefaultHotkeys[i] && capturing_ != i; }

    RectF ResetButton(int i) const {
        RectF row = Row(shortcutsCard_, i, ShortcutH());
        float w = ButtonWidth(L"Restablecer"), h = 28 * s_;
        return Snap(RectF(row.X + row.Width - 8 * s_ - w, row.Y + (row.Height - h) / 2, w, h));
    }
    float ButtonWidth(const std::wstring& label) const {
        return std::max(92 * s_, text_.Measure(label, F_BUTTON) + 40 * s_);
    }

    void Layout(float W) {
        float m = M(), y = m + 62 * s_, inner = W - 2 * m;
        auto section = [&](RectF& label) {
            label = Snap(RectF(m + 4 * s_, y, inner, 18 * s_));
            y += 22 * s_;
        };
        section(outLabel_);
        outRect_ = Snap(RectF(m, y, inner, 34 * s_));
        y += 34 * s_ + 16 * s_;
        section(qualityLabel_);
        qualityRect_ = Snap(RectF(m, y, inner, 34 * s_));
        y += 34 * s_ + 16 * s_;

        section(generalLabel_);
        generalCard_ = Snap(RectF(m, y, inner, RowH() * 2));
        y += generalCard_.Height + 16 * s_;

        section(componentsLabel_);
        componentsCard_ = Snap(RectF(m, y, inner, RowH() * 2));
        buttons_.clear();
        float ew = ButtonWidth(L"Elegir…"), eh = 28 * s_;
        exifButton_ = {BTN_EXIF, L"Elegir…", Snap(RectF(m + inner - 10 * s_ - ew, y + RowH() + (RowH() - eh) / 2, ew, eh)),
                       false};
        buttons_.push_back(exifButton_);
        y += componentsCard_.Height + 16 * s_;

        section(shortcutsLabel_);
        shortcutsCard_ = Snap(RectF(m, y, inner, ShortcutH() * kShortcuts));
        y += shortcutsCard_.Height + 12 * s_;

        statusRect_ = Snap(RectF(m + 4 * s_, y, inner - 8 * s_, 18 * s_));
        y += 18 * s_ + 10 * s_;

        float bh = 34 * s_, x = m;
        for (auto [id, label] : {std::pair{BTN_FLOATS, L"Cerrar tarjetas flotantes"}, std::pair{BTN_QUIT, L"Salir de FileTools"}}) {
            float w = ButtonWidth(label);
            buttons_.push_back({id, label, Snap(RectF(x, y, w, bh)), false});
            x += w + 8 * s_;
        }
        float cw = ButtonWidth(L"Cerrar");
        buttons_.push_back({BTN_CLOSE, L"Cerrar", Snap(RectF(W - m - cw, y, cw, bh)), true});
        y += bh + m;
        clientH_ = std::round(y);
    }

    RectF Segment(const RectF& r, int count, int i) const {
        float w = (r.Width - 8 * s_) / count;
        return RectF(r.X + 4 * s_ + i * w, r.Y + 4 * s_, w, r.Height - 8 * s_);
    }

    RectF Row(const RectF& card, int i, float h) const { return RectF(card.X, card.Y + i * h, card.Width, h); }

    RectF Toggle(const RectF& row) const {
        float w = 40 * s_, h = 22 * s_;
        return Snap(RectF(row.X + row.Width - 14 * s_ - w, row.Y + (row.Height - h) / 2, w, h));
    }

    // --- painting --------------------------------------------------------------------

    void Invalidate() {
        if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
    }

    void Draw(Graphics& g, float W, float H) {
        float m = M();
        FillBackdrop(g, W, H, glass_);
        text_.Add(L"Ajustes", F_TITLE, kInk, RectF(m, m - 2 * s_, W - 2 * m, 34 * s_));
        text_.Add(L"FileTools " + Utf8ToWide(FT_APP_VERSION), F_SMALL, kMuted,
                  RectF(m, m + 32 * s_, W - 2 * m, 18 * s_));

        const Settings& st = AppSettings();
        text_.Add(L"GUARDAR RESULTADOS EN", F_GROUP, kAccentInk, outLabel_);
        std::vector<std::wstring> outs(std::begin(kOutputs), std::end(kOutputs));
        DrawSegmented(g, outRect_, outs, int(st.outputMode), HIT_OUT);

        text_.Add(L"CALIDAD AL CONVERTIR (JPG, WEBP, AVIF, HEIC)", F_GROUP, kAccentInk, qualityLabel_);
        std::vector<std::wstring> qualities;
        int selected = -1;
        for (int i = 0; i < (int)std::size(kQualities); ++i) {
            qualities.push_back(std::to_wstring(kQualities[i]) + L" %");
            if (st.quality == kQualities[i]) selected = i;
        }
        DrawSegmented(g, qualityRect_, qualities, selected, HIT_QUALITY);

        text_.Add(L"GENERAL", F_GROUP, kAccentInk, generalLabel_);
        DrawCard(g, generalCard_, 2, RowH());
        DrawToggleRow(g, Row(generalCard_, 0, RowH()), L"Iniciar con Windows", st.startWithWindows != 0, HIT_STARTUP);
        DrawToggleRow(g, Row(generalCard_, 1, RowH()), L"Avisar cuando termine una tarea en segundo plano",
                      st.notifications != 0, HIT_NOTIFY);

        text_.Add(L"COMPONENTES OPCIONALES", F_GROUP, kAccentInk, componentsLabel_);
        DrawCard(g, componentsCard_, 2, RowH());
        RectF r0 = Row(componentsCard_, 0, RowH()), r1 = Row(componentsCard_, 1, RowH());
        text_.Add(L"ffmpeg · audio y vídeo", F_BODY, kInk, RectF(r0.X + 14 * s_, r0.Y, r0.Width * 0.55f, r0.Height));
        std::wstring ff = !components_ ? L"Comprobando…" : components_->ffmpeg ? L"✓ Detectado" : L"No instalado";
        text_.Add(ff, F_SMALL, components_ && components_->ffmpeg ? kGood : kMuted,
                  RectF(r0.X, r0.Y, r0.Width - 14 * s_, r0.Height), DWRITE_TEXT_ALIGNMENT_TRAILING);
        text_.Add(L"ExifTool · todos los metadatos", F_BODY, kInk, RectF(r1.X + 14 * s_, r1.Y, r1.Width * 0.55f, r1.Height));
        std::wstring ex = checkingExif_ || !components_ ? L"Comprobando…"
                          : components_->exifVersion.empty() ? L"No conectado"
                                                             : L"✓ " + components_->exifVersion;
        bool exOk = components_ && !components_->exifVersion.empty() && !checkingExif_;
        text_.Add(ex, F_SMALL, exOk ? kGood : kMuted,
                  RectF(r1.X, r1.Y, exifButton_.r.X - r1.X - 10 * s_, r1.Height), DWRITE_TEXT_ALIGNMENT_TRAILING);

        text_.Add(L"ATAJOS DE TECLADO · HAZ CLIC EN UNO PARA CAMBIARLO", F_GROUP, kAccentInk, shortcutsLabel_);
        DrawCard(g, shortcutsCard_, kShortcuts, ShortcutH());
        for (int i = 0; i < kShortcuts; ++i) {
            RectF row = Row(shortcutsCard_, i, ShortcutH()), chip = KeyChip(i);
            bool capturing = capturing_ == i, hot = hover_ == HIT_KEY + i;
            FillGlass(g, chip, chip.Height / 2, capturing || hot ? 245 : 225, capturing || hot ? 225 : 195, 240, s_,
                      capturing ? 0 : 40);
            if (capturing) {
                StrokeInside(g, chip, chip.Height / 2, 0, Accent(230), std::round(2 * s_));
            } else if (hot) {
                GraphicsPath p;
                gfx::AddRoundRect(p, chip, chip.Height / 2);
                SolidBrush tint(Accent(26));
                g.FillPath(&tint, &p);
            }
            std::wstring keys = !capturing ? HotkeyName(HotkeyOf(i))
                                : liveMods_ ? ModifiersName(liveMods_) + L"…"
                                            : L"Pulsa el atajo…";
            text_.Add(keys, F_GROUP, capturing ? kAccentInk : kInk, RectF(chip.X + 8 * s_, chip.Y, chip.Width - 16 * s_, chip.Height),
                      DWRITE_TEXT_ALIGNMENT_CENTER);
            float tx = chip.X + chip.Width + 12 * s_;
            float right = CanReset(i) ? ResetButton(i).X - 8 * s_ : row.X + row.Width - 14 * s_;
            text_.Add(kShortcutUse[i], F_SMALL, kLabel, RectF(tx, row.Y, right - tx, row.Height), DWRITE_TEXT_ALIGNMENT_LEADING,
                      true);
            if (CanReset(i))
                glass::DrawButton(g, text_, F_SMALL, ResetButton(i), L"Restablecer", false, true, hover_ == BTN_RESET + i,
                                  pressed_ == BTN_RESET + i, s_);
        }

        Color tone = tone_ == Tone::Good ? kGood : tone_ == Tone::Bad ? kBad : kMuted;
        text_.Add(status_, F_SMALL, tone, statusRect_);
        for (const auto& b : buttons_)
            glass::DrawButton(g, text_, F_BUTTON, b.r, b.label, b.primary, ButtonEnabled(b.id), hover_ == b.id,
                              pressed_ == b.id, s_);
    }

    void DrawCard(Graphics& g, const RectF& card, int rows, float rowH) {
        Shadow(g, card, 12 * s_, s_);
        FillGlass(g, card, 12 * s_, 205, 175, 230, s_);
        SolidBrush sep(Color(18, 20, 30, 50));
        for (int i = 1; i < rows; ++i)
            g.FillRectangle(&sep, card.X + 12 * s_, std::round(card.Y + i * rowH), card.Width - 24 * s_, 1.0f);
    }

    void DrawSegmented(Graphics& g, const RectF& r, const std::vector<std::wstring>& labels, int selected, int hitBase) {
        FillGlass(g, r, r.Height / 2, 200, 170, 230, s_);
        for (int i = 0; i < (int)labels.size(); ++i) {
            RectF seg = Segment(r, (int)labels.size(), i);
            bool sel = i == selected, hot = hover_ == hitBase + i;
            if (sel) {
                FillAccent(g, seg, seg.Height / 2, s_);
            } else if (hot) {
                GraphicsPath p;
                gfx::AddRoundRect(p, seg, seg.Height / 2);
                SolidBrush tint(Accent(pressed_ == hitBase + i ? 46 : 26));
                g.FillPath(&tint, &p);
            }
            text_.Add(labels[i], F_BUTTON, sel ? kWhite : kInk, RectF(seg.X + 4 * s_, seg.Y, seg.Width - 8 * s_, seg.Height),
                      DWRITE_TEXT_ALIGNMENT_CENTER);
        }
    }

    void DrawToggleRow(Graphics& g, const RectF& row, const std::wstring& label, bool on, int hit) {
        if (hover_ == hit) {
            GraphicsPath p;
            gfx::AddRoundRect(p, RectF(row.X + 4 * s_, row.Y + 4 * s_, row.Width - 8 * s_, row.Height - 8 * s_), 8 * s_);
            SolidBrush tint(Accent(18));
            g.FillPath(&tint, &p);
        }
        RectF t = Toggle(row);
        text_.Add(label, F_BODY, kInk, RectF(row.X + 14 * s_, row.Y, t.X - row.X - 24 * s_, row.Height));
        if (on) {
            FillAccent(g, t, t.Height / 2, s_);
        } else {
            GraphicsPath p;
            gfx::AddRoundRect(p, t, t.Height / 2);
            SolidBrush track(Color(70, 90, 96, 110));
            g.FillPath(&track, &p);
        }
        float d = t.Height - 6 * s_;
        RectF knob(on ? t.X + t.Width - 3 * s_ - d : t.X + 3 * s_, t.Y + 3 * s_, d, d);
        SolidBrush white(kWhite);
        g.FillEllipse(&white, knob);
    }

    // --- hit testing -----------------------------------------------------------------

    static bool In(const RectF& r, POINT p) {
        return p.x >= r.X && p.x < r.X + r.Width && p.y >= r.Y && p.y < r.Y + r.Height;
    }

    bool ButtonEnabled(int id) const { return id != BTN_EXIF || !checkingExif_; }

    int Hit(POINT p) const {
        for (const auto& b : buttons_)
            if (In(b.r, p)) return ButtonEnabled(b.id) ? b.id : HIT_NONE;
        for (int i = 0; i < (int)std::size(kOutputs); ++i)
            if (In(Segment(outRect_, (int)std::size(kOutputs), i), p)) return HIT_OUT + i;
        for (int i = 0; i < (int)std::size(kQualities); ++i)
            if (In(Segment(qualityRect_, (int)std::size(kQualities), i), p)) return HIT_QUALITY + i;
        if (In(Row(generalCard_, 0, RowH()), p)) return HIT_STARTUP;
        if (In(Row(generalCard_, 1, RowH()), p)) return HIT_NOTIFY;
        for (int i = 0; i < kShortcuts; ++i) {
            if (CanReset(i) && In(ResetButton(i), p)) return BTN_RESET + i;
            if (In(KeyChip(i), p)) return HIT_KEY + i;
        }
        return HIT_NONE;
    }

    // --- shortcuts -------------------------------------------------------------------

    void StartCapture(int i) {
        if (capturing_ == i) {
            EndCapture(0);
            return;
        }
        // Unregister the current shortcuts so that pressing one of them reaches this window.
        if (capturing_ < 0 && actions_.suspendHotkeys) actions_.suspendHotkeys();
        capturing_ = i;
        liveMods_ = 0;
        SetFocus(hwnd_);
        SetStatus(L"Pulsa la nueva combinación, con Ctrl o Alt (Esc para cancelar)", Tone::Normal);
    }

    UINT HeldModifiers() const {
        return (GetKeyState(VK_CONTROL) < 0 ? MOD_CONTROL : 0) | (GetKeyState(VK_MENU) < 0 ? MOD_ALT : 0) |
               (GetKeyState(VK_SHIFT) < 0 ? MOD_SHIFT : 0);
    }

    // Key pressed while waiting for a shortcut.
    void OnCaptureKey(UINT vk) {
        UINT mods = HeldModifiers();
        switch (vk) {
            case VK_CONTROL:
            case VK_MENU:
            case VK_SHIFT:
            case VK_LWIN:
            case VK_RWIN:
                liveMods_ = mods;
                Invalidate();
                return;
            case VK_ESCAPE:
                if (!mods) {
                    EndCapture(0);
                    return;
                }
                break;
        }
        if (!(mods & (MOD_CONTROL | MOD_ALT))) {
            SetStatus(L"El atajo debe llevar Ctrl o Alt además de otra tecla (Esc para cancelar)", Tone::Bad);
            return;
        }
        EndCapture(MakeHotkey(mods, vk));
    }

    // 0 cancels. Registers the new shortcut, or keeps the old one when Windows refuses it.
    void EndCapture(DWORD hotkey) {
        int i = capturing_;
        capturing_ = -1;
        liveMods_ = 0;
        if (i < 0) return;
        if (!hotkey) {
            std::wstring error = actions_.applyHotkeys ? actions_.applyHotkeys() : L"";
            SetStatus(error.empty() ? L"Sin cambios" : error, error.empty() ? Tone::Normal : Tone::Bad);
            return;
        }
        if (int other = ShortcutUsing(hotkey, i); other >= 0) {
            if (actions_.applyHotkeys) actions_.applyHotkeys();
            SetStatus(HotkeyName(hotkey) + L" ya se usa para: " + kShortcutUse[other], Tone::Bad);
            return;
        }
        SetHotkey(i, hotkey);
    }

    void SetHotkey(int i, DWORD hotkey) {
        DWORD old = HotkeyOf(i);
        HotkeyOf(i) = hotkey;
        std::wstring error = actions_.applyHotkeys ? actions_.applyHotkeys() : L"";
        if (error.find(HotkeyName(hotkey)) != std::wstring::npos) {
            HotkeyOf(i) = old;
            if (actions_.applyHotkeys) actions_.applyHotkeys();
            SetStatus(HotkeyName(hotkey) + L" no está disponible: lo usa otra aplicación o Windows", Tone::Bad);
            return;
        }
        SaveSettings();
        if (!error.empty()) SetStatus(error, Tone::Bad);
        else SetStatus(L"✓ " + std::wstring(kShortcutUse[i]) + L": " + HotkeyName(hotkey), Tone::Good);
    }

    // --- actions ---------------------------------------------------------------------

    void SetStatus(const std::wstring& text, Tone tone) {
        status_ = text;
        tone_ = tone;
        Invalidate();
    }

    void OnClick(int id) {
        Settings& st = AppSettings();
        if (id >= HIT_KEY && id < HIT_KEY + kShortcuts) {
            StartCapture(id - HIT_KEY);
            Invalidate();
            return;
        }
        if (capturing_ >= 0) EndCapture(0);  // clicking elsewhere cancels
        if (id >= BTN_RESET && id < BTN_RESET + kShortcuts) {
            int i = id - BTN_RESET;
            if (int other = ShortcutUsing(kDefaultHotkeys[i], i); other >= 0) {
                SetStatus(HotkeyName(kDefaultHotkeys[i]) + L" ya se usa para: " + kShortcutUse[other], Tone::Bad);
            } else {
                SetHotkey(i, kDefaultHotkeys[i]);
            }
        } else if (id >= HIT_OUT && id < HIT_OUT + (int)std::size(kOutputs)) {
            st.outputMode = DWORD(id - HIT_OUT);
            SaveSettings();
            SetStatus(L"✓ Los resultados se guardarán en: " + std::wstring(kOutputs[st.outputMode]), Tone::Good);
        } else if (id >= HIT_QUALITY && id < HIT_QUALITY + (int)std::size(kQualities)) {
            st.quality = kQualities[id - HIT_QUALITY];
            SaveSettings();
            SetStatus(st.quality >= 100 ? L"✓ Calidad máxima: WEBP y AVIF sin pérdida, JPG y HEIC al máximo"
                                         : L"✓ Calidad: " + std::to_wstring(st.quality) + L" %",
                      Tone::Good);
        } else if (id == HIT_STARTUP) {
            st.startWithWindows = !st.startWithWindows;
            ApplyStartWithWindows(st.startWithWindows);
            SaveSettings();
            SetStatus(st.startWithWindows ? L"✓ FileTools se abrirá al iniciar Windows" : L"✓ Ya no se abrirá al iniciar Windows",
                      Tone::Good);
        } else if (id == HIT_NOTIFY) {
            st.notifications = !st.notifications;
            SaveSettings();
            SetStatus(st.notifications ? L"✓ Avisos activados" : L"✓ Avisos desactivados (los errores se siguen mostrando)",
                      Tone::Good);
        } else if (id == BTN_EXIF) {
            PickExifTool();
        } else if (id == BTN_FLOATS) {
            CloseAllFloatShelves();
            SetStatus(L"✓ Tarjetas flotantes cerradas", Tone::Good);
        } else if (id == BTN_QUIT) {
            auto quit = actions_.quit;  // the app closes this window too
            DestroyWindow(hwnd_);
            if (quit) quit();
            return;
        } else if (id == BTN_CLOSE) {
            PostMessageW(hwnd_, WM_CLOSE, 0, 0);
        }
        Invalidate();
    }

    void CheckComponents() {
        std::thread([hwnd = hwnd_] {
            auto* c = new Components;
            c->ffmpeg = !FfmpegPath().empty();
            fs::path exif = ExifToolPath();
            if (!exif.empty()) c->exifVersion = L"ExifTool " + ExifToolVersion(exif);
            if (c->exifVersion == L"ExifTool ") c->exifVersion.clear();
            if (!PostMessageW(hwnd, WM_COMPONENTS, 0, (LPARAM)c)) delete c;
        }).detach();
    }

    void PickExifTool() {
        ComPtr<IFileOpenDialog> dlg;
        if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) return;
        COMDLG_FILTERSPEC types[] = {{L"ExifTool", L"exiftool*.exe"}, {L"Programas", L"*.exe"}};
        dlg->SetFileTypes(2, types);
        dlg->SetTitle(L"Selecciona exiftool.exe");
        ComPtr<IShellItem> item;
        PWSTR raw = nullptr;
        if (FAILED(dlg->Show(hwnd_)) || FAILED(dlg->GetResult(&item)) ||
            FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &raw)))
            return;
        fs::path picked = raw;
        CoTaskMemFree(raw);
        checkingExif_ = true;
        SetStatus(L"Comprobando ExifTool…", Tone::Normal);
        std::thread([hwnd = hwnd_, picked] {
            auto* c = new ExifCheck{picked, ExifToolVersion(picked)};
            if (!PostMessageW(hwnd, WM_EXIF_CHECKED, 0, (LPARAM)c)) delete c;
        }).detach();
    }

    void OnExifChecked(ExifCheck* raw) {
        std::unique_ptr<ExifCheck> c(raw);
        checkingExif_ = false;
        if (c->version.empty()) {
            SetStatus(c->path.filename().wstring() + L" no responde como ExifTool", Tone::Bad);
            return;
        }
        SetConfiguredExifTool(c->path);
        if (!components_) components_ = std::make_unique<Components>();
        components_->exifVersion = L"ExifTool " + c->version;
        SetStatus(L"✓ ExifTool " + c->version + L" conectado", Tone::Good);
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
            if (g_panel.get() == self) g_panel.reset();  // deletes `self`
            return DefWindowProcW(hwnd, msg, wp, lp);
        }
        return self->Handle(msg, wp, lp);
    }

    LRESULT Handle(UINT msg, WPARAM wp, LPARAM lp) {
        switch (msg) {
            case WM_ERASEBKGND: return 1;
            case WM_PAINT:
                PaintBuffered(hwnd_, text_, [this](Graphics& g, float W, float H) { Draw(g, W, H); });
                return 0;
            case WM_SIZE: {
                RECT rc;
                GetClientRect(hwnd_, &rc);
                if (rc.right > 0) Layout(float(rc.right));
                Invalidate();
                return 0;
            }
            case WM_DPICHANGED: {
                s_ = HIWORD(wp) / 96.0f;
                MakeFonts();
                float W = std::round(500 * s_);
                Layout(W);
                RECT r{0, 0, LONG(W), LONG(clientH_)};
                AdjustWindowRectExForDpi(&r, kStyle, FALSE, 0, HIWORD(wp));
                const RECT* suggested = reinterpret_cast<const RECT*>(lp);
                SetWindowPos(hwnd_, nullptr, suggested->left, suggested->top, r.right - r.left, r.bottom - r.top,
                             SWP_NOZORDER | SWP_NOACTIVATE);
                return 0;
            }
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
                    hover_ = HIT_NONE;
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
                pressed_ = HIT_NONE;
                if (pressed) ReleaseCapture();
                if (pressed && Hit({GET_X_LPARAM(lp), GET_Y_LPARAM(lp)}) == pressed) {
                    OnClick(pressed);  // may destroy the window
                    return 0;
                }
                Invalidate();
                return 0;
            }
            case WM_CAPTURECHANGED: pressed_ = HIT_NONE; return 0;
            case WM_KEYDOWN:
            case WM_SYSKEYDOWN:  // Alt combinations arrive here
                if (capturing_ >= 0) {
                    OnCaptureKey(UINT(wp));
                    return 0;
                }
                if (msg == WM_SYSKEYDOWN) break;
                if (wp == VK_ESCAPE) PostMessageW(hwnd_, WM_CLOSE, 0, 0);
                return 0;
            case WM_KEYUP:
            case WM_SYSKEYUP:
                if (capturing_ >= 0) {
                    liveMods_ = HeldModifiers();
                    Invalidate();
                    return 0;
                }
                break;
            case WM_SYSCHAR:  // no beep or menu while typing a shortcut
                if (capturing_ >= 0) return 0;
                break;
            case WM_SYSCOMMAND:
                if (capturing_ >= 0 && (wp & 0xFFF0) == SC_KEYMENU) return 0;
                break;
            case WM_ACTIVATE:
                if (LOWORD(wp) == WA_INACTIVE && capturing_ >= 0) EndCapture(0);
                break;
            case WM_COMPONENTS:
                components_.reset(reinterpret_cast<Components*>(lp));
                Invalidate();
                return 0;
            case WM_EXIF_CHECKED: OnExifChecked(reinterpret_cast<ExifCheck*>(lp)); return 0;
            case WM_CLOSE: DestroyWindow(hwnd_); return 0;
        }
        return DefWindowProcW(hwnd_, msg, wp, lp);
    }

    SettingsActions actions_;
    int capturing_ = -1;  // shortcut waiting for a new key combination
    UINT liveMods_ = 0;   // modifiers held meanwhile
    HWND hwnd_ = nullptr;
    float s_ = 1, clientH_ = 0;
    bool glass_ = false, tracking_ = false, checkingExif_ = false;
    TextQueue text_;
    RectF outLabel_, outRect_, qualityLabel_, qualityRect_, generalLabel_, generalCard_, componentsLabel_,
        componentsCard_, shortcutsLabel_, shortcutsCard_, statusRect_;
    Button exifButton_{};
    std::vector<Button> buttons_;
    int hover_ = HIT_NONE, pressed_ = HIT_NONE;
    std::unique_ptr<Components> components_;
    std::wstring status_;
    Tone tone_ = Tone::Normal;
};

}  // namespace

void OpenSettingsPanel(POINT anchor, SettingsActions actions) {
    if (g_panel) {
        g_panel->Activate();
        return;
    }
    auto panel = std::make_unique<Panel>(anchor, std::move(actions));
    if (panel->hwnd()) g_panel = std::move(panel);
}

void CloseSettingsPanel() {
    if (g_panel) DestroyWindow(g_panel->hwnd());  // WM_NCDESTROY releases it
    g_panel.reset();
}
