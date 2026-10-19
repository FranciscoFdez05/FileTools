#pragma once
#include <cstdint>
#include <string_view>
#include <vector>

// QR code (byte mode, error correction level M, versions 1-10: up to 213 bytes, plenty for a URL).
struct QrCode {
    int size = 0;                  // modules per side, without the quiet zone
    std::vector<uint8_t> modules;  // size * size, row-major, 1 = dark

    bool Dark(int x, int y) const { return modules[size_t(y) * size + x] != 0; }
};

// Throws AppError when the text does not fit.
QrCode EncodeQr(std::string_view text);
