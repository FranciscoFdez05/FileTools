#pragma once
#include "common.h"

#include <algorithm>
#include <memory>
namespace Gdiplus {
using std::max;
using std::min;
}  // namespace Gdiplus
#include <objidl.h>
#include <gdiplus.h>

namespace gfx {

void AddRoundRect(Gdiplus::GraphicsPath& path, const Gdiplus::RectF& r, float radius);
const Gdiplus::FontFamily* TextFamily();
const Gdiplus::FontFamily* IconFamily();
const Gdiplus::FontFamily* MonoFamily();  // hashes and other fixed-width values

// Draws `text` centered in `r` on a single line.
void DrawCentered(Gdiplus::Graphics& g, const std::wstring& text, const Gdiplus::Font& font,
                  const Gdiplus::Brush& brush, const Gdiplus::RectF& r);
// Draws an icon-font glyph as a filled outline centred in `box`: sharper than DrawString on layered
// surfaces. `weight` (px) thickens the strokes slightly.
void DrawGlyph(Gdiplus::Graphics& g, wchar_t glyph, float sizePx, const Gdiplus::Color& color,
               const Gdiplus::RectF& box, float weight = 0);
float TextWidth(Gdiplus::Graphics& g, const std::wstring& text, const Gdiplus::Font& font);

// Wraps a GDI+ bitmap around HBITMAP pixel data (premultiplied 32bpp).
std::unique_ptr<Gdiplus::Bitmap> BitmapFromHBitmap(HBITMAP hbmp);

// Draws an image scaled to fit inside `box`, keeping aspect ratio. Returns drawn rect.
Gdiplus::RectF DrawFitted(Gdiplus::Graphics& g, Gdiplus::Image& img, const Gdiplus::RectF& box);

float DpiScaleAt(POINT screenPt);

// A premultiplied 32bpp surface presented through UpdateLayeredWindow.
class LayeredSurface {
public:
    LayeredSurface() = default;
    LayeredSurface(const LayeredSurface&) = delete;
    LayeredSurface& operator=(const LayeredSurface&) = delete;
    ~LayeredSurface();

    Gdiplus::Graphics& Begin(int width, int height);
    void Present(HWND hwnd, POINT screenPos, BYTE alpha);
    bool SavePng(const fs::path& path);

private:
    void Release();
    HDC dc_ = nullptr;
    HBITMAP bitmap_ = nullptr;
    HGDIOBJ old_ = nullptr;
    void* bits_ = nullptr;
    int w_ = 0, h_ = 0;
    std::unique_ptr<Gdiplus::Bitmap> canvas_;
    std::unique_ptr<Gdiplus::Graphics> graphics_;
};

}  // namespace gfx
