#pragma once
#include "common.h"

// Window that sends files to another device: a local web server reachable through a link or a QR code
// (same Wi-Fi or network), the Bluetooth transfer wizard and the Windows share sheet (Nearby Sharing).
// The server runs until the window is closed.
void OpenSendPanel(const std::vector<fs::path>& files, POINT anchor);
void CloseAllSendPanels();
bool SendPanelsOpen();
