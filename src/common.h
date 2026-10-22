#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace fs = std::filesystem;

constexpr wchar_t kAppName[] = L"FileTools";

// Error carrying a user-facing message.
struct AppError {
    std::wstring message;
};
[[noreturn]] void Fail(std::wstring message);
void CheckHr(HRESULT hr, const wchar_t* what);

std::wstring Utf8ToWide(std::string_view s);
std::string WideToUtf8(std::wstring_view s);
std::wstring ToLower(std::wstring s);
std::wstring ToUpper(std::wstring s);
std::wstring ExtOf(const fs::path& p);       // lower-case, no dot ("tar.gz" -> "tgz")
std::wstring BaseNameOf(const fs::path& p);  // file name without extension(s)
std::wstring Quote(const fs::path& p);
std::wstring FormatSize(uint64_t bytes);
uint64_t SizeOnDisk(const fs::path& p);

std::vector<uint8_t> ReadFileBytes(const fs::path& p);
void WriteFileBytes(const fs::path& p, const void* data, size_t size);
inline void WriteFileBytes(const fs::path& p, const std::vector<uint8_t>& d) { WriteFileBytes(p, d.data(), d.size()); }
inline void WriteFileBytes(const fs::path& p, const std::string& d) { WriteFileBytes(p, d.data(), d.size()); }

// "dir\base.ext", "dir\base (2).ext", ... whichever does not exist yet.
fs::path UniquePath(const fs::path& dir, const std::wstring& base, std::wstring_view ext);
fs::path OutputDir(const fs::path& source);
// Output file for `source`: "<base>[ suffix].<newExt>" in the configured output folder.
fs::path OutPath(const fs::path& source, std::wstring_view newExt, std::wstring_view suffix = {});
fs::path MakeTempDir();

struct ProcessResult {
    DWORD exitCode = 0;
    std::string output;  // stdout + stderr (tail when longer than maxOutput)
};
ProcessResult RunProcess(const fs::path& exe, const std::wstring& args, const fs::path& cwd = {},
                         size_t maxOutput = 65536);
fs::path FindProgram(const wchar_t* name);
fs::path ExeDir();

// Global shortcut: RegisterHotKey modifiers (MOD_CONTROL, MOD_ALT, MOD_SHIFT) in the high word, virtual key
// in the low word.
constexpr DWORD MakeHotkey(UINT mods, UINT vk) { return DWORD(mods) << 16 | (vk & 0xFFFF); }
constexpr UINT HotkeyMods(DWORD hotkey) { return hotkey >> 16; }
constexpr UINT HotkeyVk(DWORD hotkey) { return hotkey & 0xFFFF; }
constexpr DWORD kDefaultWheelHotkey = MakeHotkey(MOD_CONTROL | MOD_ALT, VK_SPACE);
constexpr DWORD kDefaultSettingsHotkey = MakeHotkey(MOD_CONTROL | MOD_ALT, 'S');
// Ctrl+Shift with no key: pressed and released on their own (detected by the keyboard hook, since
// RegisterHotKey needs a key).
constexpr DWORD kLauncherTapHotkey = MakeHotkey(MOD_CONTROL | MOD_SHIFT, 0);
constexpr DWORD kDefaultLauncherHotkey = kLauncherTapHotkey;
std::wstring HotkeyName(DWORD hotkey);  // "Ctrl+Alt+Espacio"

struct Settings {
    DWORD outputMode = 0;  // 0 = next to original, 1 = Downloads, 2 = Desktop
    DWORD quality = 100;   // lossy encoders (JPG, WEBP, AVIF, HEIC); 100 = lossless where the format allows it
    DWORD startWithWindows = 0;
    DWORD welcomed = 0;
    DWORD notifications = 1;  // balloon when a background job finishes
    DWORD wheelHotkey = kDefaultWheelHotkey;        // wheel on the Explorer selection
    DWORD settingsHotkey = kDefaultSettingsHotkey;  // settings window
    DWORD launcherHotkey = kDefaultLauncherHotkey;  // launcher wheel (captures and programs)
};
Settings& AppSettings();
void LoadSettings();
void SaveSettings();
// String values under HKCU\Software\FileTools (empty if missing).
std::wstring ReadSettingString(const wchar_t* name);
void WriteSettingString(const wchar_t* name, const std::wstring& value);
void ApplyStartWithWindows(bool enable);
