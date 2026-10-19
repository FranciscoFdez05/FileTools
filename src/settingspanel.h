#pragma once
#include "common.h"

// What the settings window needs from the app.
struct SettingsActions {
    std::function<void()> quit;
    std::function<void()> suspendHotkeys;        // while the user types a new shortcut
    std::function<std::wstring()> applyHotkeys;  // registers AppSettings()'s shortcuts; error text if one failed
};

// Settings window: where results are saved, compression quality, start with Windows, notifications,
// optional components (ffmpeg, ExifTool), keyboard shortcuts (editable), and closing the app.
// Only one is open at a time; opening it again brings it to the front.
void OpenSettingsPanel(POINT anchor, SettingsActions actions);
void CloseSettingsPanel();
