#include "common.h"

#include <shlobj.h>

#include <algorithm>
#include <cwctype>
#include <fstream>
#include <random>

void Fail(std::wstring message) { throw AppError{std::move(message)}; }

void CheckHr(HRESULT hr, const wchar_t* what) {
    if (SUCCEEDED(hr)) return;
    wchar_t buf[64];
    swprintf_s(buf, L" (0x%08X)", static_cast<unsigned>(hr));
    Fail(std::wstring(what) + buf);
}

std::wstring Utf8ToWide(std::string_view s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
    return w;
}

std::string WideToUtf8(std::wstring_view s) {
    if (s.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0, nullptr, nullptr);
    std::string r(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.data(), (int)s.size(), r.data(), n, nullptr, nullptr);
    return r;
}

std::wstring ToLower(std::wstring s) {
    std::transform(s.begin(), s.end(), s.begin(), [](wchar_t c) { return (wchar_t)std::towlower(c); });
    return s;
}

std::wstring ToUpper(std::wstring s) {
    std::transform(s.begin(), s.end(), s.begin(), [](wchar_t c) { return (wchar_t)std::towupper(c); });
    return s;
}

std::wstring ExtOf(const fs::path& p) {
    std::wstring name = ToLower(p.filename().wstring());
    if (name.size() > 7 && name.ends_with(L".tar.gz")) return L"tgz";
    std::wstring e = ToLower(p.extension().wstring());
    if (!e.empty() && e[0] == L'.') e.erase(0, 1);
    return e;
}

std::wstring BaseNameOf(const fs::path& p) {
    std::wstring name = p.filename().wstring();
    if (ToLower(name).ends_with(L".tar.gz")) return name.substr(0, name.size() - 7);
    return p.stem().wstring();
}

std::wstring Quote(const fs::path& p) { return L"\"" + p.wstring() + L"\""; }

std::wstring FormatSize(uint64_t bytes) {
    wchar_t buf[64];
    if (bytes < 1024) swprintf_s(buf, L"%llu B", (unsigned long long)bytes);
    else if (bytes < 1024ull * 1024) swprintf_s(buf, L"%.0f KB", bytes / 1024.0);
    else if (bytes < 1024ull * 1024 * 1024) swprintf_s(buf, L"%.1f MB", bytes / (1024.0 * 1024));
    else swprintf_s(buf, L"%.2f GB", bytes / (1024.0 * 1024 * 1024));
    std::wstring s = buf;
    std::replace(s.begin(), s.end(), L'.', L',');
    return s;
}

uint64_t SizeOnDisk(const fs::path& p) {
    std::error_code ec;
    if (fs::is_directory(p, ec)) {
        uint64_t total = 0;
        for (auto it = fs::recursive_directory_iterator(p, fs::directory_options::skip_permission_denied, ec);
             it != fs::recursive_directory_iterator(); it.increment(ec)) {
            if (ec) break;
            if (it->is_regular_file(ec)) total += it->file_size(ec);
        }
        return total;
    }
    auto s = fs::file_size(p, ec);
    return ec ? 0 : s;
}

std::vector<uint8_t> ReadFileBytes(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    if (!f) Fail(L"No se pudo leer " + p.filename().wstring());
    f.seekg(0, std::ios::end);
    std::vector<uint8_t> data((size_t)f.tellg());
    f.seekg(0);
    f.read(reinterpret_cast<char*>(data.data()), data.size());
    return data;
}

void WriteFileBytes(const fs::path& p, const void* data, size_t size) {
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    if (!f) Fail(L"No se pudo escribir " + p.filename().wstring());
    f.write(static_cast<const char*>(data), size);
    if (!f) Fail(L"Error al escribir " + p.filename().wstring());
}

fs::path UniquePath(const fs::path& dir, const std::wstring& base, std::wstring_view ext) {
    std::wstring dotExt = ext.empty() ? L"" : L"." + std::wstring(ext);
    fs::path candidate = dir / (base + dotExt);
    std::error_code ec;
    for (int n = 2; fs::exists(candidate, ec); ++n)
        candidate = dir / (base + L" (" + std::to_wstring(n) + L")" + dotExt);
    return candidate;
}

static fs::path KnownFolder(REFKNOWNFOLDERID id) {
    PWSTR raw = nullptr;
    fs::path result;
    if (SUCCEEDED(SHGetKnownFolderPath(id, 0, nullptr, &raw))) result = raw;
    CoTaskMemFree(raw);
    return result;
}

fs::path OutputDir(const fs::path& source) {
    fs::path dir;
    switch (AppSettings().outputMode) {
        case 1: dir = KnownFolder(FOLDERID_Downloads); break;
        case 2: dir = KnownFolder(FOLDERID_Desktop); break;
    }
    if (dir.empty()) dir = source.parent_path();
    return dir;
}

fs::path OutPath(const fs::path& source, std::wstring_view newExt, std::wstring_view suffix) {
    std::wstring base = BaseNameOf(source);
    if (!suffix.empty()) base += L" " + std::wstring(suffix);
    return UniquePath(OutputDir(source), base, newExt);
}

fs::path MakeTempDir() {
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    std::random_device rd;
    fs::path dir = fs::path(tmp) / L"FileTools" / std::to_wstring(rd());
    fs::create_directories(dir);
    return dir;
}

ProcessResult RunProcess(const fs::path& exe, const std::wstring& args, const fs::path& cwd, size_t maxOutput) {
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE rd = nullptr, wr = nullptr;
    if (!CreatePipe(&rd, &wr, &sa, 0)) Fail(L"CreatePipe falló");
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    HANDLE nul = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr);

    STARTUPINFOW si{sizeof(si)};
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdInput = nul;
    si.hStdOutput = wr;
    si.hStdError = wr;
    std::wstring cmd = Quote(exe) + L" " + args;
    PROCESS_INFORMATION pi{};
    BOOL ok = CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr,
                             cwd.empty() ? nullptr : cwd.c_str(), &si, &pi);
    CloseHandle(wr);
    if (nul != INVALID_HANDLE_VALUE) CloseHandle(nul);
    if (!ok) {
        CloseHandle(rd);
        Fail(L"No se pudo ejecutar " + exe.filename().wstring());
    }

    ProcessResult result;
    char buf[4096];
    DWORD n = 0;
    while (ReadFile(rd, buf, sizeof(buf), &n, nullptr) && n) {
        result.output.append(buf, n);
        if (result.output.size() > maxOutput) result.output.erase(0, result.output.size() - maxOutput / 2);
    }
    CloseHandle(rd);
    WaitForSingleObject(pi.hProcess, INFINITE);
    GetExitCodeProcess(pi.hProcess, &result.exitCode);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return result;
}

fs::path ExeDir() {
    wchar_t buf[MAX_PATH];
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    return fs::path(buf).parent_path();
}

fs::path FindProgram(const wchar_t* name) {
    std::error_code ec;
    std::vector<fs::path> candidates = {ExeDir() / name, ExeDir() / L"bin" / name};
    wchar_t found[MAX_PATH];
    if (SearchPathW(nullptr, name, nullptr, MAX_PATH, found, nullptr)) candidates.emplace_back(found);
    wchar_t buf[MAX_PATH];
    if (GetEnvironmentVariableW(L"LOCALAPPDATA", buf, MAX_PATH))
        candidates.push_back(fs::path(buf) / L"Microsoft" / L"WinGet" / L"Links" / name);
    if (GetEnvironmentVariableW(L"ProgramFiles", buf, MAX_PATH))
        candidates.push_back(fs::path(buf) / L"ffmpeg" / L"bin" / name);
    candidates.push_back(fs::path(L"C:\\ffmpeg\\bin") / name);
    for (auto& c : candidates)
        if (fs::exists(c, ec)) return c;
    return {};
}

// ---------------------------------------------------------------------------
// Settings (HKCU\Software\FileTools)

static constexpr wchar_t kRegKey[] = L"Software\\FileTools";

Settings& AppSettings() {
    static Settings s;
    return s;
}

std::wstring HotkeyName(DWORD hotkey) {
    UINT mods = HotkeyMods(hotkey), vk = HotkeyVk(hotkey);
    std::wstring name;
    if (mods & MOD_CONTROL) name += L"Ctrl+";
    if (mods & MOD_ALT) name += L"Alt+";
    if (mods & MOD_SHIFT) name += L"Mayús+";
    if (mods & MOD_WIN) name += L"Win+";
    if (!vk) return name.empty() ? name : name.substr(0, name.size() - 1);  // modifiers alone: "Ctrl+Mayús"
    if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9')) return name + wchar_t(vk);
    if (vk >= VK_F1 && vk <= VK_F24) return name + L"F" + std::to_wstring(vk - VK_F1 + 1);
    switch (vk) {
        case VK_SPACE: return name + L"Espacio";
        case VK_RETURN: return name + L"Intro";
        case VK_TAB: return name + L"Tab";
        case VK_BACK: return name + L"Retroceso";
        case VK_DELETE: return name + L"Supr";
        case VK_INSERT: return name + L"Insert";
        case VK_HOME: return name + L"Inicio";
        case VK_END: return name + L"Fin";
        case VK_PRIOR: return name + L"RePág";
        case VK_NEXT: return name + L"AvPág";
        case VK_LEFT: return name + L"←";
        case VK_RIGHT: return name + L"→";
        case VK_UP: return name + L"↑";
        case VK_DOWN: return name + L"↓";
    }
    if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9) return name + L"Num " + std::to_wstring(vk - VK_NUMPAD0);
    // Punctuation and anything else: the character the key types on the current layout, else its name.
    if (UINT ch = MapVirtualKeyW(vk, MAPVK_VK_TO_CHAR) & 0x7FFFFFFF; ch > 32)
        return name + std::wstring(1, wchar_t(towupper(wchar_t(ch))));
    wchar_t key[64] = {};
    if (GetKeyNameTextW(LONG(MapVirtualKeyW(vk, MAPVK_VK_TO_VSC)) << 16, key, 64) > 0) return name + key;
    return name + L"Tecla " + std::to_wstring(vk);
}

static void ReadDword(const wchar_t* name, DWORD& v) {
    DWORD size = sizeof(DWORD);
    RegGetValueW(HKEY_CURRENT_USER, kRegKey, name, RRF_RT_REG_DWORD, nullptr, &v, &size);
}

void LoadSettings() {
    Settings& s = AppSettings();
    ReadDword(L"OutputMode", s.outputMode);
    ReadDword(L"Quality", s.quality);
    ReadDword(L"StartWithWindows", s.startWithWindows);
    ReadDword(L"Welcomed", s.welcomed);
    ReadDword(L"Notifications", s.notifications);
    ReadDword(L"WheelHotkey", s.wheelHotkey);
    ReadDword(L"SettingsHotkey", s.settingsHotkey);
    ReadDword(L"LauncherHotkey", s.launcherHotkey);
    if (!HotkeyVk(s.wheelHotkey)) s.wheelHotkey = kDefaultWheelHotkey;
    if (!HotkeyVk(s.settingsHotkey)) s.settingsHotkey = kDefaultSettingsHotkey;
    if (!HotkeyVk(s.launcherHotkey)) s.launcherHotkey = kDefaultLauncherHotkey;
    s.quality = std::clamp<DWORD>(s.quality, 30, 100);
}

void SaveSettings() {
    HKEY key;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kRegKey, 0, nullptr, 0, KEY_WRITE, nullptr, &key, nullptr) != ERROR_SUCCESS)
        return;
    const Settings& s = AppSettings();
    auto put = [&](const wchar_t* n, DWORD v) { RegSetValueExW(key, n, 0, REG_DWORD, (const BYTE*)&v, sizeof(v)); };
    put(L"OutputMode", s.outputMode);
    put(L"Quality", s.quality);
    put(L"StartWithWindows", s.startWithWindows);
    put(L"Welcomed", s.welcomed);
    put(L"Notifications", s.notifications);
    put(L"WheelHotkey", s.wheelHotkey);
    put(L"SettingsHotkey", s.settingsHotkey);
    put(L"LauncherHotkey", s.launcherHotkey);
    RegCloseKey(key);
}

std::wstring ReadSettingString(const wchar_t* name) {
    DWORD size = 0;
    if (RegGetValueW(HKEY_CURRENT_USER, kRegKey, name, RRF_RT_REG_SZ, nullptr, nullptr, &size) != ERROR_SUCCESS ||
        size < sizeof(wchar_t))
        return {};
    std::wstring value(size / sizeof(wchar_t), L'\0');
    if (RegGetValueW(HKEY_CURRENT_USER, kRegKey, name, RRF_RT_REG_SZ, nullptr, value.data(), &size) != ERROR_SUCCESS)
        return {};
    value.resize(wcslen(value.c_str()));
    return value;
}

void WriteSettingString(const wchar_t* name, const std::wstring& value) {
    HKEY key;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kRegKey, 0, nullptr, 0, KEY_WRITE, nullptr, &key, nullptr) != ERROR_SUCCESS)
        return;
    if (value.empty()) RegDeleteValueW(key, name);
    else RegSetValueExW(key, name, 0, REG_SZ, (const BYTE*)value.c_str(), DWORD((value.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(key);
}

void ApplyStartWithWindows(bool enable) {
    HKEY key;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0, KEY_WRITE, &key) !=
        ERROR_SUCCESS)
        return;
    if (enable) {
        wchar_t exe[MAX_PATH];
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        std::wstring v = L"\"" + std::wstring(exe) + L"\"";
        RegSetValueExW(key, kAppName, 0, REG_SZ, (const BYTE*)v.c_str(), DWORD((v.size() + 1) * sizeof(wchar_t)));
    } else {
        RegDeleteValueW(key, kAppName);
    }
    RegCloseKey(key);
}
