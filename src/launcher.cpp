#include "launcher.h"

#include "shellutil.h"

#include <shellapi.h>

namespace {

constexpr wchar_t kSettingName[] = L"LauncherApps";
// '|' can't appear in a Windows path, so it safely separates the entries in one registry string.
constexpr wchar_t kSeparator = L'|';

std::vector<fs::path> LoadPaths() {
    std::vector<fs::path> paths;
    std::wstring_view rest = ReadSettingString(kSettingName);
    while (!rest.empty()) {
        size_t sep = rest.find(kSeparator);
        if (sep != 0) paths.emplace_back(rest.substr(0, sep));
        if (sep == std::wstring_view::npos) break;
        rest.remove_prefix(sep + 1);
    }
    return paths;
}

void SavePaths(const std::vector<fs::path>& paths) {
    std::wstring value;
    for (const auto& p : paths) value += (value.empty() ? L"" : std::wstring(1, kSeparator)) + p.wstring();
    WriteSettingString(kSettingName, value);
}

}  // namespace

std::vector<LauncherApp> LauncherApps() {
    std::vector<LauncherApp> apps;
    for (auto& p : LoadPaths()) apps.push_back({p, p.stem().wstring()});
    return apps;
}

bool AddLauncherApp(HWND owner) {
    std::vector<fs::path> paths = LoadPaths();
    if (paths.size() >= kMaxLauncherApps) return false;
    fs::path picked;
    if (!PickProgram(owner, picked)) return false;
    for (const auto& p : paths)
        if (ToLower(p.wstring()) == ToLower(picked.wstring())) return true;  // already pinned
    paths.push_back(picked);
    SavePaths(paths);
    return true;
}

void RemoveLauncherApp(size_t index) {
    std::vector<fs::path> paths = LoadPaths();
    if (index >= paths.size()) return;
    paths.erase(paths.begin() + index);
    SavePaths(paths);
}

void RunLauncherApp(size_t index) {
    std::vector<fs::path> paths = LoadPaths();
    if (index >= paths.size()) Fail(L"Ese programa ya no está en la Lanzadera");
    const fs::path& path = paths[index];
    std::error_code ec;
    if (!fs::exists(path, ec)) Fail(L"No se encuentra " + path.filename().wstring() + L". Quítalo con clic derecho.");
    // A bare .exe runs in its own folder, as when started from Explorer; some programs look for files there.
    std::wstring dir = path.parent_path().wstring();
    SHELLEXECUTEINFOW info{sizeof(info)};
    info.fMask = SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI;
    info.lpFile = path.c_str();
    info.lpDirectory = dir.c_str();
    info.nShow = SW_SHOWNORMAL;
    if (!ShellExecuteExW(&info)) {
        // The user turning down the UAC prompt is not an error worth reporting.
        if (GetLastError() == ERROR_CANCELLED) return;
        Fail(L"No se pudo abrir " + path.stem().wstring());
    }
}
