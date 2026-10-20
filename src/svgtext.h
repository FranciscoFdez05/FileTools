#pragma once
#include "common.h"

#include <d2d1.h>

// Direct2D's SVG parser throws away <text> elements, so RasterizeSvg would lose every label. This
// reads the text back from the source XML and draws it with DirectWrite on top of the rendered
// drawing. `toPixels` maps the root <svg>'s user units (after its viewBox) to target pixels. Must be
// called between BeginDraw and EndDraw. Supported: <text>/<tspan> with x/y/dx/dy, transforms,
// inherited fill, opacity, font-family/size/weight/style, text-anchor, display/visibility, and
// class or tag rules from <style>. Text is drawn after all shapes, so a shape meant to cover
// a label won't.
void DrawSvgText(ID2D1RenderTarget* target, const uint8_t* data, size_t size, const D2D1_MATRIX_3X2_F& toPixels);
