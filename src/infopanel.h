#pragma once
#include "actions.h"

// Window listing an InfoSheet (hashes or metadata): filter/compare box, copy and, for metadata, "Quitar metadatos".
void OpenInfoPanel(std::shared_ptr<const InfoSheet> sheet, POINT anchor);
void CloseAllInfoPanels();
bool InfoPanelsOpen();  // any panel of this process still open
