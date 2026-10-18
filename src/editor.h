#pragma once
#include "common.h"

enum class EditorMode { Adjust, Annotate, Crop, Background };

// Opens the built-in image editor (adjustments, annotations, crop, background removal) on the UI thread.
void OpenEditor(const fs::path& file, EditorMode mode);
