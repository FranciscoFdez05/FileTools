#pragma once
#include "common.h"

// Programs the user pins to the launcher wheel (.exe files or Start menu shortcuts).
struct LauncherApp {
    fs::path path;
    std::wstring name;  // label on the wheel
};

// The wheel fits about eleven segments; three captures and "Añadir" leave room for seven programs.
constexpr size_t kMaxLauncherApps = 7;

std::vector<LauncherApp> LauncherApps();
// Asks for a program and pins it. Returns false if the user cancelled or the list is full.
bool AddLauncherApp(HWND owner);
void RemoveLauncherApp(size_t index);
// Starts the pinned program. Throws AppError if it is gone or Windows refuses to run it.
void RunLauncherApp(size_t index);
