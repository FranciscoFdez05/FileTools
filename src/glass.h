#pragma once
#include "gfx.h"

#include <d2d1.h>
#include <dwrite.h>
#include <wrl/client.h>

#include <functional>

// Look shared by the hand-drawn windows (info panel, send panel): on Windows 11 the client area sits on
// the system Mica backdrop and every element is painted with alpha on top (shapes with GDI+, text with
// DirectWrite). Without Mica (Windows 10) an opaque frost is used.
namespace glass {

// Palette: dark ink on frosted white, Windows blue as the accent.
inline const Gdiplus::Color kInk(255, 22, 24, 30);
inline const Gdiplus::Color kLabel(255, 70, 74, 86);
inline const Gdiplus::Color kMuted(255, 96, 100, 112);
inline const Gdiplus::Color kGood(255, 16, 124, 16);
inline const Gdiplus::Color kBad(255, 196, 43, 28);
inline const Gdiplus::Color kAccentInk(255, 0, 90, 170);
inline const Gdiplus::Color kWhite(255, 255, 255, 255);
// Accent gradient, left to right.
constexpr BYTE kAccent1[3] = {28, 132, 230}, kAccent2[3] = {0, 96, 196};

Gdiplus::Color Accent(BYTE alpha);

// Brings the window to the front even though the drag left another app in the foreground.
void ForceForeground(HWND hwnd);
// Light frame, round corners and Mica. Returns true when the backdrop is translucent.
bool ApplyBackdrop(HWND hwnd);

// Antialiasing and filtering used for every hand-drawn surface.
void SetQuality(Gdiplus::Graphics& g);
// Rounded outline drawn just inside `r` (`inset` px in), landing on whole pixels.
void StrokeInside(Gdiplus::Graphics& g, const Gdiplus::RectF& r, float radius, float inset, const Gdiplus::Color& color,
                  float width);

// Frosted pane: translucent white, lighter at the top, with a faint dark edge and a bright inner rim.
void FillGlass(Gdiplus::Graphics& g, const Gdiplus::RectF& r, float radius, BYTE top, BYTE bottom, BYTE rim, float s,
               BYTE edge = 26);
// Soft drop shadow under a pane.
void Shadow(Gdiplus::Graphics& g, const Gdiplus::RectF& r, float radius, float s);
// Selection and primary button: blue glass.
void FillAccent(Gdiplus::Graphics& g, const Gdiplus::RectF& r, float radius, float s, BYTE alpha = 240);
// Window background: a light wash over Mica, or an opaque frost.
void FillBackdrop(Gdiplus::Graphics& g, float w, float h, bool translucent);

// DirectWrite draws much sharper text than GDI+: hinted glyphs on whole pixels.
struct TextEngine {
    Microsoft::WRL::ComPtr<ID2D1Factory> d2d;
    Microsoft::WRL::ComPtr<IDWriteFactory> dw;
    Microsoft::WRL::ComPtr<IDWriteRenderingParams> params;
    std::wstring textFamily = L"Segoe UI", displayFamily = L"Segoe UI", monoFamily = L"Consolas";
};
TextEngine& Engine();

D2D1_COLOR_F ToD2D(const Gdiplus::Color& c);
Gdiplus::RectF Snap(const Gdiplus::RectF& r);

// Text queued while GDI+ paints the shapes, then drawn on top with DirectWrite. Fonts are numbered by the
// window that owns the queue.
class TextQueue {
public:
    enum class Family { Text, Display, Mono };  // Display: optical size for titles
    void SetFont(int index, Family family, float px, DWRITE_FONT_WEIGHT weight);  // px already scaled for DPI
    float Measure(const std::wstring& text, int font) const;
    void Add(const std::wstring& text, int font, const Gdiplus::Color& color, const Gdiplus::RectF& r,
             DWRITE_TEXT_ALIGNMENT align = DWRITE_TEXT_ALIGNMENT_LEADING, bool wrap = false);
    void Render(HDC dc, const RECT& rc);

private:
    struct Op {
        std::wstring text;
        int font;
        Gdiplus::Color color;
        Gdiplus::RectF r;
        DWRITE_TEXT_ALIGNMENT align;
        bool wrap;
    };
    Microsoft::WRL::ComPtr<IDWriteTextLayout> Layout(const std::wstring& text, int font, float w, float h, bool wrap) const;
    std::vector<Microsoft::WRL::ComPtr<IDWriteTextFormat>> formats_;
    Microsoft::WRL::ComPtr<ID2D1DCRenderTarget> target_;
    std::vector<Op> ops_;
};

// WM_PAINT for a hand-drawn window: `draw` paints with GDI+ on a cleared premultiplied buffer (client width
// and height given), then the queued text goes on top.
void PaintBuffered(HWND hwnd, TextQueue& text, const std::function<void(Gdiplus::Graphics&, float, float)>& draw);

// Pill button: blue glass when primary, frosted otherwise.
void DrawButton(Gdiplus::Graphics& g, TextQueue& text, int font, const Gdiplus::RectF& r, const std::wstring& label,
                bool primary, bool enabled, bool hot, bool down, float s);

}  // namespace glass
