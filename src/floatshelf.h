#pragma once
#include "common.h"

// A small always-on-top card holding files: drag them out later, drop more onto it.
void OpenFloatShelf(const std::vector<fs::path>& files, POINT anchor);
void CloseAllFloatShelves();
