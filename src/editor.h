#pragma once
#include "common.h"

enum class EditorMode { Adjust, Annotate, Crop, Background, Resize };

// Opens the built-in image editor (adjustments, annotations, crop, background removal, resize) on the UI thread.
void OpenEditor(const fs::path& file, EditorMode mode);
