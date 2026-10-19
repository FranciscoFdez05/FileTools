#include "shellutil.h"

#include <exdisp.h>
#include <shlguid.h>
#include <shlobj.h>
#include <shobjidl_core.h>
#include <wrl/client.h>

#include <unknwn.h>
#include <winrt/Windows.ApplicationModel.DataTransfer.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Storage.h>

#include <map>

using Microsoft::WRL::ComPtr;

std::unique_ptr<Gdiplus::Bitmap> ShellThumbnail(const fs::path& path, int sizePx) {
    ComPtr<IShellItemImageFactory> factory;
    if (FAILED(SHCreateItemFromParsingName(path.c_str(), nullptr, IID_PPV_ARGS(&factory)))) return nullptr;
    HBITMAP hbmp = nullptr;
    SIZE size{sizePx, sizePx};
    if (FAILED(factory->GetImage(size, SIIGBF_RESIZETOFIT | SIIGBF_BIGGERSIZEOK, &hbmp)) &&
        FAILED(factory->GetImage(size, SIIGBF_ICONONLY, &hbmp)))
        return nullptr;
    auto bmp = gfx::BitmapFromHBitmap(hbmp);
    DeleteObject(hbmp);
    return bmp;
}

std::vector<fs::path> FilesFromDataObject(IDataObject* data) {
    std::vector<fs::path> files;
    if (!data) return files;
    FORMATETC fe{CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
    STGMEDIUM medium{};
    if (FAILED(data->GetData(&fe, &medium))) return files;
    HDROP drop = static_cast<HDROP>(GlobalLock(medium.hGlobal));
    if (drop) {
        UINT count = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
        for (UINT i = 0; i < count; ++i) {
            UINT len = DragQueryFileW(drop, i, nullptr, 0);
            std::wstring name(len, L'\0');
            DragQueryFileW(drop, i, name.data(), len + 1);
            files.emplace_back(name);
        }
        GlobalUnlock(medium.hGlobal);
    }
    ReleaseStgMedium(&medium);
    return files;
}

static HBITMAP MakeDragImage(const fs::path& file, int size, int count) {
    auto thumb = ShellThumbnail(file, size);
    if (!thumb) return nullptr;
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = size;
    bi.bmiHeader.biHeight = -size;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    void* bits = nullptr;
    HBITMAP hbmp = CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!hbmp) return nullptr;
    memset(bits, 0, size_t(size) * size * 4);
    {
        using namespace Gdiplus;
        Bitmap canvas(size, size, size * 4, PixelFormat32bppPARGB, static_cast<BYTE*>(bits));
        Graphics g(&canvas);
        g.SetSmoothingMode(SmoothingModeAntiAlias);
        g.SetInterpolationMode(InterpolationModeHighQualityBicubic);
        g.SetTextRenderingHint(TextRenderingHintAntiAlias);
        gfx::DrawFitted(g, *thumb, RectF(4, 4, size - 8.0f, size - 8.0f));
        if (count > 1) {
            float d = size * 0.3f;
            RectF badge(size - d - 2, 2, d, d);
            SolidBrush red(Color(255, 232, 72, 40));
            g.FillEllipse(&red, badge);
            Font font(gfx::TextFamily(), d * 0.45f, FontStyleBold, UnitPixel);
            SolidBrush white(Color(255, 255, 255, 255));
            gfx::DrawCentered(g, std::to_wstring(count), font, white, badge);
        }
    }
    return hbmp;
}

// Shell data object for `files` (what Explorer hands to drop targets). Null if none of them can be parsed.
static ComPtr<IDataObject> FilesDataObject(const std::vector<fs::path>& files) {
    std::vector<PIDLIST_ABSOLUTE> pidls;
    for (const auto& f : files) {
        PIDLIST_ABSOLUTE pidl = nullptr;
        if (SUCCEEDED(SHParseDisplayName(f.c_str(), nullptr, &pidl, 0, nullptr))) pidls.push_back(pidl);
    }
    ComPtr<IShellItemArray> items;
    ComPtr<IDataObject> data;
    if (!pidls.empty() &&
        SUCCEEDED(SHCreateShellItemArrayFromIDLists((UINT)pidls.size(), (PCIDLIST_ABSOLUTE_ARRAY)pidls.data(), &items)))
        items->BindToHandler(nullptr, BHID_DataObject, IID_PPV_ARGS(&data));
    for (auto p : pidls) CoTaskMemFree(p);
    return data;
}

DWORD DragOutFiles(HWND hwnd, const std::vector<fs::path>& files) {
    if (files.empty()) return DROPEFFECT_NONE;
    DWORD effect = DROPEFFECT_NONE;
    ComPtr<IDataObject> data = FilesDataObject(files);
    if (!data) return effect;
    ComPtr<IDragSourceHelper> helper;
    if (SUCCEEDED(CoCreateInstance(CLSID_DragDropHelper, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&helper)))) {
        int size = int(96 * gfx::DpiScaleAt(POINT{0, 0}));
        if (HBITMAP img = MakeDragImage(files[0], size, (int)files.size())) {
            SHDRAGIMAGE di{};
            di.sizeDragImage = {size, size};
            di.ptOffset = {size / 2, size / 2};
            di.hbmpDragImage = img;
            di.crColorKey = CLR_NONE;
            if (FAILED(helper->InitializeFromBitmap(&di, data.Get()))) DeleteObject(img);
        }
    }
    SHDoDragDrop(hwnd, data.Get(), nullptr, DROPEFFECT_COPY | DROPEFFECT_MOVE | DROPEFFECT_LINK, &effect);
    return effect;
}

bool SendViaBluetooth(const std::vector<fs::path>& files) {
    // Same as "Send to > Bluetooth device": drop the files on fsquirt.exe, the Bluetooth file transfer wizard.
    wchar_t sys[MAX_PATH] = {};
    GetSystemDirectoryW(sys, MAX_PATH);
    fs::path wizard = fs::path(sys) / L"fsquirt.exe";
    std::error_code ec;
    if (!fs::exists(wizard, ec)) return false;
    ComPtr<IDataObject> data = FilesDataObject(files);
    ComPtr<IShellItem> item;
    ComPtr<IDropTarget> target;
    if (!data || FAILED(SHCreateItemFromParsingName(wizard.c_str(), nullptr, IID_PPV_ARGS(&item))) ||
        FAILED(item->BindToHandler(nullptr, BHID_SFUIObject, IID_PPV_ARGS(&target))))
        return false;
    POINTL pt{0, 0};
    DWORD effect = DROPEFFECT_COPY;
    if (FAILED(target->DragEnter(data.Get(), MK_LBUTTON, pt, &effect)) || effect == DROPEFFECT_NONE) {
        target->DragLeave();
        return false;
    }
    effect = DROPEFFECT_COPY;
    return SUCCEEDED(target->Drop(data.Get(), MK_LBUTTON, pt, &effect));
}

static bool OpenClipboardRetry(HWND hwnd) {
    for (int i = 0; i < 10; ++i) {
        if (OpenClipboard(hwnd)) return true;
        Sleep(20);
    }
    return false;
}

bool CopyFilesToClipboard(HWND hwnd, const std::vector<fs::path>& files) {
    size_t chars = 1;
    for (const auto& f : files) chars += f.wstring().size() + 1;
    HGLOBAL h = GlobalAlloc(GHND, sizeof(DROPFILES) + chars * sizeof(wchar_t));
    if (!h) return false;
    auto* df = static_cast<DROPFILES*>(GlobalLock(h));
    df->pFiles = sizeof(DROPFILES);
    df->fWide = TRUE;
    auto* p = reinterpret_cast<wchar_t*>(reinterpret_cast<BYTE*>(df) + sizeof(DROPFILES));
    for (const auto& f : files) {
        std::wstring s = f.wstring();
        memcpy(p, s.c_str(), (s.size() + 1) * sizeof(wchar_t));
        p += s.size() + 1;
    }
    *p = 0;
    GlobalUnlock(h);

    HGLOBAL eff = GlobalAlloc(GHND, sizeof(DWORD));
    *static_cast<DWORD*>(GlobalLock(eff)) = DROPEFFECT_COPY;
    GlobalUnlock(eff);

    if (!OpenClipboardRetry(hwnd)) {
        GlobalFree(h);
        GlobalFree(eff);
        return false;
    }
    EmptyClipboard();
    SetClipboardData(CF_HDROP, h);
    SetClipboardData(RegisterClipboardFormatW(CFSTR_PREFERREDDROPEFFECT), eff);
    CloseClipboard();
    return true;
}

bool CopyTextToClipboard(HWND hwnd, const std::wstring& text) {
    HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, (text.size() + 1) * sizeof(wchar_t));
    if (!h) return false;
    memcpy(GlobalLock(h), text.c_str(), (text.size() + 1) * sizeof(wchar_t));
    GlobalUnlock(h);
    if (!OpenClipboardRetry(hwnd)) {
        GlobalFree(h);
        return false;
    }
    EmptyClipboard();
    SetClipboardData(CF_UNICODETEXT, h);
    CloseClipboard();
    return true;
}

static std::vector<fs::path> SelectionFromView(IShellBrowser* browser) {
    std::vector<fs::path> files;
    ComPtr<IShellView> view;
    ComPtr<IFolderView2> folderView;
    ComPtr<IShellItemArray> items;
    if (FAILED(browser->QueryActiveShellView(&view)) || FAILED(view.As(&folderView)) ||
        FAILED(folderView->GetSelection(FALSE, &items)) || !items)
        return files;
    DWORD count = 0;
    items->GetCount(&count);
    for (DWORD i = 0; i < count; ++i) {
        ComPtr<IShellItem> item;
        PWSTR path = nullptr;
        if (SUCCEEDED(items->GetItemAt(i, &item)) && SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
            files.emplace_back(path);
            CoTaskMemFree(path);
        }
    }
    return files;
}

std::vector<fs::path> ExplorerSelection() {
    HWND fg = GetForegroundWindow();
    wchar_t cls[64] = {};
    GetClassNameW(fg, cls, 64);
    ComPtr<IShellWindows> windows;
    if (FAILED(CoCreateInstance(CLSID_ShellWindows, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&windows)))) return {};

    if (!wcscmp(cls, L"Progman") || !wcscmp(cls, L"WorkerW")) {
        VARIANT empty{};
        long hwnd = 0;
        ComPtr<IDispatch> disp;
        if (FAILED(windows->FindWindowSW(&empty, &empty, SWC_DESKTOP, &hwnd, SWFO_NEEDDISPATCH, &disp)) || !disp)
            return {};
        ComPtr<IServiceProvider> sp;
        ComPtr<IShellBrowser> browser;
        if (SUCCEEDED(disp.As(&sp)) && SUCCEEDED(sp->QueryService(SID_STopLevelBrowser, IID_PPV_ARGS(&browser))))
            return SelectionFromView(browser.Get());
        return {};
    }
    if (wcscmp(cls, L"CabinetWClass") && wcscmp(cls, L"ExploreWClass")) return {};

    long count = 0;
    windows->get_Count(&count);
    std::vector<fs::path> fallback;
    for (long i = 0; i < count; ++i) {
        VARIANT idx{};
        idx.vt = VT_I4;
        idx.lVal = i;
        ComPtr<IDispatch> disp;
        if (FAILED(windows->Item(idx, &disp)) || !disp) continue;
        ComPtr<IWebBrowserApp> app;
        SHANDLE_PTR h = 0;
        if (FAILED(disp.As(&app)) || FAILED(app->get_HWND(&h)) || HWND(h) != fg) continue;
        ComPtr<IServiceProvider> sp;
        ComPtr<IShellBrowser> browser;
        if (FAILED(app.As(&sp)) || FAILED(sp->QueryService(SID_STopLevelBrowser, IID_PPV_ARGS(&browser)))) continue;
        // Windows 11 tabs share the frame HWND: prefer the visible tab.
        HWND tab = nullptr;
        browser->GetWindow(&tab);
        auto sel = SelectionFromView(browser.Get());
        if (tab && IsWindowVisible(tab)) return sel;
        if (fallback.empty()) fallback = std::move(sel);
    }
    return fallback;
}

bool PickFolder(HWND owner, fs::path& out) {
    ComPtr<IFileOpenDialog> dlg;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) return false;
    DWORD opts = 0;
    dlg->GetOptions(&opts);
    dlg->SetOptions(opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
    dlg->SetTitle(L"Mover a…");
    if (FAILED(dlg->Show(owner))) return false;
    ComPtr<IShellItem> item;
    PWSTR path = nullptr;
    if (FAILED(dlg->GetResult(&item)) || FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) return false;
    out = path;
    CoTaskMemFree(path);
    return true;
}

bool MoveFilesTo(HWND owner, const std::vector<fs::path>& files, const fs::path& dest) {
    ComPtr<IFileOperation> op;
    ComPtr<IShellItem> destItem;
    if (FAILED(CoCreateInstance(CLSID_FileOperation, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&op))) ||
        FAILED(SHCreateItemFromParsingName(dest.c_str(), nullptr, IID_PPV_ARGS(&destItem))))
        return false;
    op->SetOwnerWindow(owner);
    op->SetOperationFlags(FOF_ALLOWUNDO | FOF_NOCONFIRMMKDIR);
    for (const auto& f : files) {
        ComPtr<IShellItem> item;
        if (SUCCEEDED(SHCreateItemFromParsingName(f.c_str(), nullptr, IID_PPV_ARGS(&item))))
            op->MoveItem(item.Get(), destItem.Get(), nullptr, nullptr);
    }
    BOOL aborted = FALSE;
    return SUCCEEDED(op->PerformOperations()) && SUCCEEDED(op->GetAnyOperationsAborted(&aborted)) && !aborted;
}

void RevealInExplorer(const std::vector<fs::path>& files) {
    if (files.empty()) return;
    fs::path parent = files[0].parent_path();
    PIDLIST_ABSOLUTE folder = ILCreateFromPathW(parent.c_str());
    if (!folder) return;
    std::vector<PIDLIST_ABSOLUTE> abs;
    std::vector<PCUITEMID_CHILD> children;
    for (const auto& f : files) {
        if (f.parent_path() != parent) continue;
        if (PIDLIST_ABSOLUTE p = ILCreateFromPathW(f.c_str())) {
            abs.push_back(p);
            children.push_back(ILFindLastID(p));
        }
    }
    SHOpenFolderAndSelectItems(folder, (UINT)children.size(), children.data(), 0);
    for (auto p : abs) ILFree(p);
    ILFree(folder);
}

void OpenTerminalAt(const fs::path& dir) {
    std::wstring d = dir.wstring();
    if (!d.empty() && d.back() == L'\\') d += L'.';  // avoid \" escaping the closing quote
    fs::path wt = FindProgram(L"wt.exe");
    if (!wt.empty()) {
        std::wstring args = L"-d \"" + d + L"\"";
        if ((INT_PTR)ShellExecuteW(nullptr, L"open", wt.c_str(), args.c_str(), nullptr, SW_SHOWNORMAL) > 32) return;
    }
    ShellExecuteW(nullptr, L"open", L"cmd.exe", nullptr, dir.c_str(), SW_SHOWNORMAL);
}

void OpenWithDialog(HWND owner, const fs::path& file) {
    OPENASINFO info{};
    info.pcszFile = file.c_str();
    info.oaifInFlags = OAIF_ALLOW_REGISTRATION | OAIF_EXEC;
    SHOpenWithDialog(owner, &info);
}

// --- Windows share sheet ----------------------------------------------------

namespace {
using namespace winrt::Windows::ApplicationModel::DataTransfer;
using winrt::Windows::Storage::IStorageItem;

std::vector<fs::path> g_shareFiles;
std::map<HWND, std::pair<DataTransferManager, winrt::event_token>> g_shareManagers;

winrt::fire_and_forget OnDataRequested(DataRequestedEventArgs args) {
    auto request = args.Request();
    auto deferral = request.GetDeferral();
    std::vector<fs::path> files = g_shareFiles;
    std::vector<IStorageItem> items;
    for (const auto& f : files) {
        try {
            std::error_code ec;
            if (fs::is_directory(f, ec))
                items.push_back(co_await winrt::Windows::Storage::StorageFolder::GetFolderFromPathAsync(f.wstring()));
            else
                items.push_back(co_await winrt::Windows::Storage::StorageFile::GetFileFromPathAsync(f.wstring()));
        } catch (...) {
        }
    }
    request.Data().Properties().Title(files.size() == 1 ? winrt::hstring(files[0].filename().wstring())
                                                        : winrt::hstring(std::to_wstring(files.size()) + L" archivos"));
    request.Data().SetStorageItems(winrt::single_threaded_vector(std::move(items)));
    deferral.Complete();
}
}  // namespace

bool ShareFiles(HWND hwnd, const std::vector<fs::path>& files) {
    try {
        auto interop = winrt::get_activation_factory<DataTransferManager, IDataTransferManagerInterop>();
        auto it = g_shareManagers.find(hwnd);
        if (it == g_shareManagers.end()) {
            DataTransferManager dtm{nullptr};
            winrt::check_hresult(interop->GetForWindow(hwnd, winrt::guid_of<DataTransferManager>(), winrt::put_abi(dtm)));
            auto token = dtm.DataRequested([](const DataTransferManager&, const DataRequestedEventArgs& args) {
                OnDataRequested(args);
            });
            g_shareManagers.emplace(hwnd, std::make_pair(dtm, token));
        }
        g_shareFiles = files;
        return SUCCEEDED(interop->ShowShareUIForWindow(hwnd));
    } catch (...) {
        return false;
    }
}
