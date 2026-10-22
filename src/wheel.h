#pragma once
#include "actions.h"

namespace wheel {

using Notifier = std::function<void(const JobResult& result)>;

void Create(HINSTANCE instance, Notifier onBackgroundResult);
void Destroy();

// Shows the wheel under the cursor while a drag is in progress.
void ShowForDrag(POINT pt, Mode mode);
// Shows the wheel for already-known files (keyboard shortcut on an Explorer selection).
void ShowForFiles(POINT pt, std::vector<fs::path> files);
// Shows the launcher wheel (screen captures and pinned programs) under the cursor.
void ShowLauncher(POINT pt);
void SetMode(Mode mode);
void Hide();

void OnDragEnded();
void OnGlobalMouseDown(POINT pt);
void OnNavKey(UINT vk);

bool IsVisible();
bool KeyboardActive();
bool ContainsPoint(POINT pt);
// Debug: renders the wheel offscreen to a PNG.
void Preview(std::vector<fs::path> files, Mode mode, int hover, std::vector<fs::path> outputs, const fs::path& png);

}  // namespace wheel
