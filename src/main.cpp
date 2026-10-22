// FileTools: drag a file, press a modifier key, drop it on the wheel.
#include "capture.h"
#include "common.h"
#include "editor.h"
#include "floatshelf.h"
#include "gfx.h"
#include "infopanel.h"
#include "media.h"
#include "sendpanel.h"
#include "settingspanel.h"
#include "shellutil.h"
#include "wheel.h"

#include <commctrl.h>
#include <shellapi.h>
#include <windowsx.h>

#include <unknwn.h>
#include <winrt/base.h>

namespace {

constexpr wchar_t kMainClass[] = L"FileTools.Main";
constexpr UINT WM_TRAY = WM_APP + 1;
constexpr UINT WM_HOOK_DRAG_END = WM_APP + 2;
constexpr UINT WM_HOOK_MODIFIER = WM_APP + 3;
constexpr UINT WM_HOOK_MOUSEDOWN = WM_APP + 4;
constexpr UINT WM_HOOK_NAVKEY = WM_APP + 5;
constexpr UINT WM_BACKGROUND_RESULT = WM_APP + 6;
constexpr UINT WM_OPEN_SETTINGS = WM_APP + 7;  // sent by a second launch
constexpr UINT WM_HOOK_LAUNCHER = WM_APP + 8;
constexpr int kHotkeyId = 1;
constexpr int kSettingsHotkeyId = 2;
constexpr int kLauncherHotkeyId = 3;

enum MenuId {
    ID_HELP = 1,
    ID_SETTINGS,
    ID_OUT_NEXT = 10,
    ID_OUT_DOWNLOADS,
    ID_OUT_DESKTOP,
    ID_Q_BASE = 20,
    ID_STARTUP = 40,
    ID_CLOSE_FLOATS,
    ID_QUIT = 99,
};
constexpr DWORD kQualities[] = {60, 75, 85, 90, 95, 100};

HINSTANCE g_inst;
HWND g_main;
HHOOK g_mouseHook, g_keyHook;
UINT g_taskbarCreated;
NOTIFYICONDATAW g_nid{};
HICON g_icon;
std::vector<fs::path> g_lastOutputs;

// Drag detection state (hooks run on the UI thread).
bool g_leftDown = false, g_dragging = false;
POINT g_downPt{};
bool g_modDown[3] = {};

// Ctrl+Shift tapped on their own (no other key or click in between, released within a second)
// opens the launcher. Anything else keeps them working as part of other shortcuts.
constexpr ULONGLONG kComboMaxMs = 1000;
bool g_comboArmed = false;
ULONGLONG g_comboAt = 0;

LRESULT CALLBACK MouseHook(int code, WPARAM wp, LPARAM lp) {
    if (code == HC_ACTION) {
        auto* m = reinterpret_cast<MSLLHOOKSTRUCT*>(lp);
        switch (wp) {
            case WM_LBUTTONDOWN:
                g_comboArmed = false;  // Ctrl+Shift+click selects; it isn't a tap
                g_leftDown = true;
                g_dragging = false;
                g_downPt = m->pt;
                PostMessageW(g_main, WM_HOOK_MOUSEDOWN, 0, MAKELPARAM(m->pt.x, m->pt.y));
                break;
            case WM_RBUTTONDOWN:
            case WM_MBUTTONDOWN:
                g_comboArmed = false;
                PostMessageW(g_main, WM_HOOK_MOUSEDOWN, 0, MAKELPARAM(m->pt.x, m->pt.y));
                break;
            case WM_MOUSEMOVE:
                if (g_leftDown && !g_dragging) {
                    int th = std::max(6, GetSystemMetrics(SM_CXDRAG) * 2);
                    if (std::abs(m->pt.x - g_downPt.x) > th || std::abs(m->pt.y - g_downPt.y) > th) g_dragging = true;
                }
                break;
            case WM_LBUTTONUP:
                if (g_dragging) PostMessageW(g_main, WM_HOOK_DRAG_END, 0, 0);
                g_leftDown = g_dragging = false;
                break;
        }
    }
    return CallNextHookEx(g_mouseHook, code, wp, lp);
}

int ModifierIndex(DWORD vk) {
    switch (vk) {
        case VK_SHIFT:
        case VK_LSHIFT:
        case VK_RSHIFT: return 0;
        case VK_CONTROL:
        case VK_LCONTROL:
        case VK_RCONTROL: return 1;
        case VK_MENU:
        case VK_LMENU:
        case VK_RMENU: return 2;
    }
    return -1;
}

LRESULT CALLBACK KeyboardHook(int code, WPARAM wp, LPARAM lp) {
    if (code == HC_ACTION) {
        auto* k = reinterpret_cast<KBDLLHOOKSTRUCT*>(lp);
        bool down = wp == WM_KEYDOWN || wp == WM_SYSKEYDOWN;
        bool up = wp == WM_KEYUP || wp == WM_SYSKEYUP;
        int mod = ModifierIndex(k->vkCode);
        if (down && mod < 0) g_comboArmed = false;  // Ctrl+Shift+T, Win+..., etc.
        if (mod >= 0) {
            // Only fresh presses count, so auto-repeat and keys held before the drag are ignored.
            if (down && !g_modDown[mod]) {
                g_modDown[mod] = true;
                if (g_dragging || wheel::KeyboardActive()) PostMessageW(g_main, WM_HOOK_MODIFIER, mod, 0);
                // During a drag, Shift and Ctrl switch the file wheels instead.
                if (mod == 2) g_comboArmed = false;
                else if (g_modDown[0] && g_modDown[1] && !g_modDown[2] && !g_dragging &&
                         AppSettings().launcherHotkey == kLauncherTapHotkey) {
                    g_comboArmed = true;
                    g_comboAt = GetTickCount64();
                }
            } else if (up) {
                g_modDown[mod] = false;
                if (g_comboArmed && mod != 2) {
                    g_comboArmed = false;
                    if (GetTickCount64() - g_comboAt <= kComboMaxMs) PostMessageW(g_main, WM_HOOK_LAUNCHER, 0, 0);
                }
            }
        } else if (down && wheel::IsVisible()) {
            switch (k->vkCode) {
                case VK_ESCAPE:
                    PostMessageW(g_main, WM_HOOK_NAVKEY, VK_ESCAPE, 0);
                    if (wheel::KeyboardActive()) return 1;
                    break;
                case VK_LEFT:
                case VK_RIGHT:
                case VK_UP:
                case VK_DOWN:
                case VK_RETURN:
                case VK_TAB:
                    if (wheel::KeyboardActive()) {
                        PostMessageW(g_main, WM_HOOK_NAVKEY, k->vkCode, 0);
                        return 1;
                    }
                    break;
            }
        }
    }
    return CallNextHookEx(g_keyHook, code, wp, lp);
}

HICON MakeAppIcon(int size) {
    using namespace Gdiplus;
    Bitmap bmp(size, size, PixelFormat32bppARGB);
    {
        Graphics g(&bmp);
        g.SetSmoothingMode(SmoothingModeAntiAlias);
        g.SetPixelOffsetMode(PixelOffsetModeHalf);
        float s = size / 32.0f;
        LinearGradientBrush disc(PointF(0, 0), PointF((REAL)size, (REAL)size), Color(255, 62, 154, 240),
                                 Color(255, 0, 96, 196));
        g.FillEllipse(&disc, 1 * s, 1 * s, 30 * s, 30 * s);
        Pen seg(Color(255, 222, 238, 255), 2.2f * s);
        for (int i = 0; i < 6; ++i) {
            float a = (-90 + i * 60) * 3.14159265f / 180;
            g.DrawLine(&seg, 16 * s + cosf(a) * 7 * s, 16 * s + sinf(a) * 7 * s, 16 * s + cosf(a) * 15 * s,
                       16 * s + sinf(a) * 15 * s);
        }
        SolidBrush hole(Color(255, 236, 244, 255));
        g.FillEllipse(&hole, 11 * s, 11 * s, 10 * s, 10 * s);
        SolidBrush leaf(Color(255, 76, 160, 60));
        g.FillEllipse(&leaf, 18 * s, 0.5f * s, 9 * s, 5 * s);
    }
    HICON icon = nullptr;
    bmp.GetHICON(&icon);
    return icon;
}

void Balloon(const std::wstring& title, const std::wstring& text, bool error = false) {
    NOTIFYICONDATAW n = g_nid;
    n.uFlags = NIF_INFO;
    wcsncpy_s(n.szInfoTitle, title.c_str(), _TRUNCATE);
    wcsncpy_s(n.szInfo, text.c_str(), _TRUNCATE);
    n.dwInfoFlags = error ? NIIF_ERROR : NIIF_USER | NIIF_LARGE_ICON;
    n.hBalloonIcon = g_icon;
    Shell_NotifyIconW(NIM_MODIFY, &n);
}

void AddTrayIcon() {
    g_nid = {sizeof(g_nid)};
    g_nid.hWnd = g_main;
    g_nid.uID = 1;
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP | NIF_SHOWTIP;
    g_nid.uCallbackMessage = WM_TRAY;
    g_nid.hIcon = g_icon;
    wcscpy_s(g_nid.szTip, L"FileTools — arrastra un archivo y pulsa Mayús/Ctrl/Alt");
    Shell_NotifyIconW(NIM_ADD, &g_nid);
    g_nid.uVersion = NOTIFYICON_VERSION_4;
    Shell_NotifyIconW(NIM_SETVERSION, &g_nid);
}

void UnregisterHotkeys() {
    UnregisterHotKey(g_main, kHotkeyId);
    UnregisterHotKey(g_main, kSettingsHotkeyId);
    UnregisterHotKey(g_main, kLauncherHotkeyId);
}

// (Re)registers the shortcuts chosen in the settings. Returns what could not be registered, if anything.
std::wstring RegisterHotkeys() {
    UnregisterHotkeys();
    std::wstring failed;
    const Settings& s = AppSettings();
    for (auto [id, hotkey] : {std::pair{kHotkeyId, s.wheelHotkey}, std::pair{kSettingsHotkeyId, s.settingsHotkey},
                              std::pair{kLauncherHotkeyId, s.launcherHotkey}}) {
        if (hotkey == kLauncherTapHotkey) continue;  // handled by the keyboard hook
        if (!RegisterHotKey(g_main, id, HotkeyMods(hotkey) | MOD_NOREPEAT, HotkeyVk(hotkey)))
            failed += (failed.empty() ? L"" : L" y ") + HotkeyName(hotkey);
    }
    return failed.empty() ? L"" : L"No se pudo usar " + failed + L": lo usa otra aplicación o Windows.";
}

void OpenSettings() {
    POINT pt;
    GetCursorPos(&pt);
    OpenSettingsPanel(pt, {[] { DestroyWindow(g_main); }, UnregisterHotkeys, RegisterHotkeys});
}

void ShowHelp() {
    const Settings& s = AppSettings();
    std::wstring text =
        L"Arrastra uno o varios archivos y, sin soltar el botón, pulsa:\n\n"
        L"     Mayús  →  Convertir a otro formato\n"
        L"     Ctrl     →  Herramientas (comprimir, recortar, OCR…)\n"
        L"     Alt       →  Acciones (copiar, mover, ZIP, compartir…)\n\n"
        L"Suelta el archivo sobre una opción de la rueda. El resultado aparece en el centro: "
        L"arrástralo a donde quieras o haz clic para verlo en el Explorador. Puedes pulsar otra tecla "
        L"durante el arrastre para cambiar de rueda.\n\n"
        L"Con archivos seleccionados en el Explorador, pulsa " + HotkeyName(s.wheelHotkey) +
        L" para abrir la rueda con el teclado (flechas, Intro, Tab, Esc).\n\n" + HotkeyName(s.launcherHotkey) +
        (s.launcherHotkey == kLauncherTapHotkey ? L" (pulsar y soltar)" : L"") + L" abre la Lanzadera: capturas de pantalla (región, ventana o pantalla completa) y los programas que "
        L"añadas.\n\n" + HotkeyName(s.settingsHotkey) +
        L" (o clic en el icono de la bandeja) abre los ajustes, donde también puedes cambiar estos atajos.\n\n"
        L"Todo se procesa en local. Audio y vídeo requieren ffmpeg.";
    MessageBoxW(nullptr, text.c_str(), L"FileTools", MB_OK | MB_ICONINFORMATION | MB_SETFOREGROUND);
}

void ShowTrayMenu() {
    const Settings& s = AppSettings();
    HMENU out = CreatePopupMenu();
    AppendMenuW(out, MF_STRING | (s.outputMode == 0 ? MF_CHECKED : 0), ID_OUT_NEXT, L"Junto al original");
    AppendMenuW(out, MF_STRING | (s.outputMode == 1 ? MF_CHECKED : 0), ID_OUT_DOWNLOADS, L"Descargas");
    AppendMenuW(out, MF_STRING | (s.outputMode == 2 ? MF_CHECKED : 0), ID_OUT_DESKTOP, L"Escritorio");
    HMENU quality = CreatePopupMenu();
    for (int i = 0; i < (int)std::size(kQualities); ++i)
        AppendMenuW(quality, MF_STRING | (s.quality == kQualities[i] ? MF_CHECKED : 0), ID_Q_BASE + i,
                    (std::to_wstring(kQualities[i]) + L" %").c_str());

    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING | MF_DISABLED, 0, L"FileTools");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING | MF_DEFAULT, ID_SETTINGS, (L"Ajustes…\t" + HotkeyName(s.settingsHotkey)).c_str());
    AppendMenuW(menu, MF_STRING, ID_HELP, L"Cómo se usa…");
    AppendMenuW(menu, MF_POPUP, (UINT_PTR)out, L"Guardar resultados en");
    AppendMenuW(menu, MF_POPUP, (UINT_PTR)quality, L"Calidad al convertir (JPG, WEBP, AVIF, HEIC)");
    AppendMenuW(menu, MF_STRING | (s.startWithWindows ? MF_CHECKED : 0), ID_STARTUP, L"Iniciar con Windows");
    AppendMenuW(menu, MF_STRING, ID_CLOSE_FLOATS, L"Cerrar tarjetas flotantes");
    AppendMenuW(menu, MF_STRING | MF_DISABLED, 0,
                FfmpegPath().empty() ? L"ffmpeg: no encontrado (audio/vídeo desactivados)" : L"ffmpeg: detectado");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, ID_QUIT, L"Salir");

    POINT pt;
    GetCursorPos(&pt);
    SetForegroundWindow(g_main);
    UINT cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY, pt.x, pt.y, 0, g_main, nullptr);
    DestroyMenu(menu);

    Settings& w = AppSettings();
    if (cmd == ID_SETTINGS) OpenSettings();
    else if (cmd == ID_HELP) ShowHelp();
    else if (cmd >= ID_OUT_NEXT && cmd <= ID_OUT_DESKTOP) w.outputMode = cmd - ID_OUT_NEXT;
    else if (cmd >= ID_Q_BASE && cmd < ID_Q_BASE + std::size(kQualities)) w.quality = kQualities[cmd - ID_Q_BASE];
    else if (cmd == ID_STARTUP) {
        w.startWithWindows = !w.startWithWindows;
        ApplyStartWithWindows(w.startWithWindows);
    } else if (cmd == ID_CLOSE_FLOATS) CloseAllFloatShelves();
    else if (cmd == ID_QUIT) DestroyWindow(g_main);
    SaveSettings();
}

LRESULT CALLBACK MainProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == g_taskbarCreated && msg != 0) {
        AddTrayIcon();
        return 0;
    }
    switch (msg) {
        case WM_HOOK_MODIFIER: {
            POINT pt;
            GetCursorPos(&pt);
            Mode mode = Mode(wp);
            if (wheel::KeyboardActive()) wheel::SetMode(mode);
            else if (g_dragging) wheel::ShowForDrag(pt, mode);
            return 0;
        }
        case WM_HOOK_DRAG_END: wheel::OnDragEnded(); return 0;
        case WM_HOOK_MOUSEDOWN: wheel::OnGlobalMouseDown(POINT{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)}); return 0;
        case WM_HOOK_NAVKEY: wheel::OnNavKey((UINT)wp); return 0;
        case WM_OPEN_SETTINGS: OpenSettings(); return 0;
        case WM_HOOK_LAUNCHER: {
            POINT pt;
            GetCursorPos(&pt);
            wheel::ShowLauncher(pt);
            return 0;
        }
        case WM_HOTKEY:
            if (wp == kSettingsHotkeyId) OpenSettings();
            if (wp == kLauncherHotkeyId) {
                POINT pt;
                GetCursorPos(&pt);
                wheel::ShowLauncher(pt);
            }
            if (wp == kHotkeyId) {
                auto files = ExplorerSelection();
                if (files.empty()) {
                    Balloon(L"FileTools", L"Selecciona archivos en el Explorador o en el Escritorio primero.");
                } else {
                    POINT pt;
                    GetCursorPos(&pt);
                    wheel::ShowForFiles(pt, std::move(files));
                }
            }
            return 0;
        case WM_BACKGROUND_RESULT: {
            std::unique_ptr<JobResult> r(reinterpret_cast<JobResult*>(lp));
            if (r->sheet) {
                POINT pt;
                GetCursorPos(&pt);
                OpenInfoPanel(r->sheet, pt);
            } else if (!r->ok) {
                Balloon(L"No se pudo completar", r->message, true);
            } else if (!r->outputs.empty()) {
                g_lastOutputs = r->outputs;
                if (!AppSettings().notifications) return 0;
                std::wstring name = r->outputs.size() == 1 ? r->outputs[0].filename().wstring()
                                                           : std::to_wstring(r->outputs.size()) + L" archivos";
                Balloon(L"Listo", name + (r->message.empty() ? L"" : L"\n" + r->message));
            }
            return 0;
        }
        case WM_TRAY:
            switch (LOWORD(lp)) {
                case WM_CONTEXTMENU:
                case WM_RBUTTONUP: ShowTrayMenu(); break;
                case NIN_SELECT:
                case NIN_KEYSELECT: OpenSettings(); break;
                case NIN_BALLOONUSERCLICK:
                    if (!g_lastOutputs.empty()) RevealInExplorer(g_lastOutputs);
                    break;
            }
            return 0;
        case WM_DESTROY:
            Shell_NotifyIconW(NIM_DELETE, &g_nid);
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// Headless mode used for scripting and tests:
//   FileTools.exe --run <action-id> [--log <file>] <files...>
int RunCli(int argc, wchar_t** argv) {
    std::string id = WideToUtf8(argv[2]);
    fs::path log;
    std::vector<fs::path> files;
    for (int i = 3; i < argc; ++i) {
        if (!wcscmp(argv[i], L"--log") && i + 1 < argc) log = argv[++i];
        else files.push_back(fs::absolute(argv[i]));
    }
    winrt::init_apartment(winrt::apartment_type::multi_threaded);
    ULONG_PTR token;
    Gdiplus::GdiplusStartupInput input;
    Gdiplus::GdiplusStartup(&token, &input, nullptr);
    LoadSettings();
    JobResult r = RunAction(id, files, [](int, int) {});
    std::wstring text = std::wstring(r.ok ? L"OK" : L"ERROR") + L"\t" + r.message + L"\n";
    for (const auto& o : r.outputs) text += o.wstring() + L"\n";
    if (r.sheet)
        for (const auto& row : r.sheet->rows) text += row.group + L"\t" + row.name + L"\t" + row.value + L"\n";
    if (!log.empty()) WriteFileBytes(log, WideToUtf8(text));
    Gdiplus::GdiplusShutdown(token);
    return r.ok ? 0 : 1;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int) {
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argc >= 3 && !wcscmp(argv[1], L"--run")) return RunCli(argc, argv);
    if (argc >= 4 && !wcscmp(argv[1], L"--edit")) {
        // --edit <mode 0-2> <file>: opens the editor standalone.
        SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        OleInitialize(nullptr);
        ULONG_PTR tok;
        Gdiplus::GdiplusStartupInput in;
        Gdiplus::GdiplusStartup(&tok, &in, nullptr);
        OpenEditor(fs::absolute(argv[3]), EditorMode(_wtoi(argv[2])));
        MSG m;
        while (GetMessageW(&m, nullptr, 0, 0) > 0) {
            TranslateMessage(&m);
            DispatchMessageW(&m);
            if (!FindWindowW(L"FileTools.Editor", nullptr) && !FindWindowW(L"FileTools.Float", nullptr)) break;
        }
        CloseAllFloatShelves();
        Gdiplus::GdiplusShutdown(tok);
        return 0;
    }
    if (argc >= 4 && !wcscmp(argv[1], L"--panel")) {
        // --panel <tool:info|tool:hash> <files...>: opens the info popup standalone.
        SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        winrt::init_apartment(winrt::apartment_type::single_threaded);
        OleInitialize(nullptr);
        ULONG_PTR tok;
        Gdiplus::GdiplusStartupInput in;
        Gdiplus::GdiplusStartup(&tok, &in, nullptr);
        LoadSettings();
        std::vector<fs::path> files;
        for (int i = 3; i < argc; ++i) files.push_back(fs::absolute(argv[i]));
        JobResult r = RunAction(WideToUtf8(argv[2]), files, [](int, int) {});
        POINT pt;
        GetCursorPos(&pt);
        if (r.sheet) OpenInfoPanel(r.sheet, pt);
        else MessageBoxW(nullptr, r.message.c_str(), L"FileTools", MB_OK);
        MSG m;
        while (InfoPanelsOpen() && GetMessageW(&m, nullptr, 0, 0) > 0) {
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
        Gdiplus::GdiplusShutdown(tok);
        return 0;
    }
    if (argc >= 3 && !wcscmp(argv[1], L"--send")) {
        // --send <files...>: opens the send window standalone; the server stops when it is closed.
        SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        winrt::init_apartment(winrt::apartment_type::single_threaded);
        OleInitialize(nullptr);
        ULONG_PTR tok;
        Gdiplus::GdiplusStartupInput in;
        Gdiplus::GdiplusStartup(&tok, &in, nullptr);
        LoadSettings();
        std::vector<fs::path> files;
        for (int i = 2; i < argc; ++i) files.push_back(fs::absolute(argv[i]));
        POINT pt;
        GetCursorPos(&pt);
        OpenSendPanel(files, pt);
        MSG m;
        while (SendPanelsOpen() && GetMessageW(&m, nullptr, 0, 0) > 0) {
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
        CloseAllSendPanels();
        Gdiplus::GdiplusShutdown(tok);
        return 0;
    }
    if (argc >= 6 && !wcscmp(argv[1], L"--preview")) {
        // --preview <mode 0-2> <hover> <out.png> <file> [result]
        SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        winrt::init_apartment(winrt::apartment_type::single_threaded);
        OleInitialize(nullptr);
        ULONG_PTR tok;
        Gdiplus::GdiplusStartupInput in;
        Gdiplus::GdiplusStartup(&tok, &in, nullptr);
        wheel::Create(inst, nullptr);
        std::vector<fs::path> outs;
        if (argc >= 7) outs.push_back(fs::absolute(argv[6]));
        wheel::Preview({fs::absolute(argv[5])}, Mode(_wtoi(argv[2])), _wtoi(argv[3]), outs, argv[4]);
        wheel::Destroy();
        Gdiplus::GdiplusShutdown(tok);
        return 0;
    }

    HANDLE mutex = CreateMutexW(nullptr, TRUE, L"Local\\FileTools.SingleInstance");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        // Launching it again (or "FileTools.exe --settings") opens the settings of the running copy.
        if (HWND running = FindWindowW(kMainClass, nullptr)) {
            DWORD pid = 0;
            GetWindowThreadProcessId(running, &pid);
            AllowSetForegroundWindow(pid);
            PostMessageW(running, WM_OPEN_SETTINGS, 0, 0);
        } else {
            MessageBoxW(nullptr, L"FileTools ya se está ejecutando (icono en la bandeja del sistema).", L"FileTools",
                        MB_OK | MB_ICONINFORMATION);
        }
        return 0;
    }
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    winrt::init_apartment(winrt::apartment_type::single_threaded);
    OleInitialize(nullptr);
    ULONG_PTR gdiplusToken;
    Gdiplus::GdiplusStartupInput gdiplusInput;
    Gdiplus::GdiplusStartup(&gdiplusToken, &gdiplusInput, nullptr);
    LoadSettings();

    g_inst = inst;
    // Tray/balloon icon from the embedded .ico; fall back to drawing one.
    LoadIconMetric(inst, MAKEINTRESOURCEW(1), LIM_SMALL, &g_icon);
    if (!g_icon) g_icon = MakeAppIcon(GetSystemMetrics(SM_CXSMICON) * 2);
    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = MainProc;
    wc.hInstance = inst;
    wc.hIcon = g_icon;
    wc.lpszClassName = kMainClass;
    RegisterClassExW(&wc);
    g_main = CreateWindowExW(WS_EX_TOOLWINDOW, kMainClass, L"FileTools", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, inst,
                             nullptr);
    g_taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");

    wheel::Create(inst, [](const JobResult& r) {
        PostMessageW(g_main, WM_BACKGROUND_RESULT, 0, (LPARAM) new JobResult(r));
    });
    capture::Create(inst, [](const JobResult& r) {
        PostMessageW(g_main, WM_BACKGROUND_RESULT, 0, (LPARAM) new JobResult(r));
    });
    AddTrayIcon();
    g_mouseHook = SetWindowsHookExW(WH_MOUSE_LL, MouseHook, inst, 0);
    g_keyHook = SetWindowsHookExW(WH_KEYBOARD_LL, KeyboardHook, inst, 0);
    if (std::wstring error = RegisterHotkeys(); !error.empty())
        Balloon(L"FileTools", error + L" Cámbialo en los ajustes.");
    if (argc >= 2 && !wcscmp(argv[1], L"--settings")) OpenSettings();

    if (!AppSettings().welcomed) {
        AppSettings().welcomed = 1;
        SaveSettings();
        Balloon(L"FileTools está activo",
                L"Arrastra un archivo y pulsa Mayús (convertir), Ctrl (herramientas) o Alt (acciones).");
    }

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    UnregisterHotkeys();
    UnhookWindowsHookEx(g_mouseHook);
    UnhookWindowsHookEx(g_keyHook);
    CloseAllFloatShelves();
    CloseAllInfoPanels();
    CloseAllSendPanels();
    CloseSettingsPanel();
    capture::Destroy();
    wheel::Destroy();
    DestroyIcon(g_icon);
    Gdiplus::GdiplusShutdown(gdiplusToken);
    OleUninitialize();
    ReleaseMutex(mutex);
    return 0;
}
