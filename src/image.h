#pragma once
#include "common.h"

enum class ImgFmt { Jpg, Png, Webp, Heic, Tiff, Avif, Bmp, Gif };

// 32-bit BGRA, straight (non-premultiplied) alpha, top-down rows.
struct Image {
    uint32_t w = 0, h = 0;
    std::vector<uint8_t> px;

    Image() = default;
    Image(uint32_t width, uint32_t height) : w(width), h(height), px(size_t(width) * height * 4, 0) {}
    uint8_t* Row(uint32_t y) { return px.data() + size_t(y) * w * 4; }
    const uint8_t* Row(uint32_t y) const { return px.data() + size_t(y) * w * 4; }
    bool HasAlpha() const;
};

bool ParseImgFmt(const std::wstring& ext, ImgFmt& out);
const wchar_t* ImgFmtExt(ImgFmt f);
bool CanEncode(ImgFmt f);  // false when the codec isn't available on this machine/build

Image LoadImageFile(const fs::path& path);
Image LoadImageMemory(const uint8_t* data, size_t size);

struct EncodeOpts {
    int quality = 90;       // 1..100 for lossy formats
    bool quantize = false;  // PNG: reduce to a 256 colour palette
};
std::vector<uint8_t> EncodeImage(const Image& img, ImgFmt fmt, const EncodeOpts& opts = {});
void SaveImage(const Image& img, const fs::path& path, ImgFmt fmt, const EncodeOpts& opts = {});

Image Flatten(const Image& img, uint8_t r, uint8_t g, uint8_t b);
Image Resize(const Image& img, uint32_t w, uint32_t h);
Image FitWithin(const Image& img, uint32_t maxDim);
Image Rotate90(const Image& img);
bool AutoCrop(const Image& img, Image& out);
bool AutoCropRect(const Image& img, uint32_t& x, uint32_t& y, uint32_t& w, uint32_t& h);
Image AddBackdrop(const Image& img);
void FillRect(Image& img, int x, int y, int w, int h, uint32_t bgra);

// JPEG passthrough for PDF embedding: true if `path` is a baseline RGB/gray JPEG
// with no rotation flag, so its bytes can be embedded as-is.
bool JpegPassthroughInfo(const fs::path& path, uint32_t& w, uint32_t& h, bool& gray);

// Removes EXIF/XMP/comments without re-encoding (JPEG, PNG). Returns false if not possible.
bool StripMetadataLossless(const fs::path& src, const fs::path& dst);
