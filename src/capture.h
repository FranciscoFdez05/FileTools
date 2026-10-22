#pragma once
#include "actions.h"

namespace capture {

enum class Target { Region, Window, Screen };
using Notifier = std::function<void(const JobResult& result)>;

// Registers the overlay window class. `onSaved` reports saved captures and errors (tray balloon).
void Create(HINSTANCE instance, Notifier onSaved);
void Destroy();

// Freezes the screen and lets the user pick the area, adjust it with handles and then copy, save
// or edit it. `at` is where the launcher was opened: Window and Screen start from the window or
// monitor under it, Region from nothing.
void Start(Target target, POINT at);

}  // namespace capture
