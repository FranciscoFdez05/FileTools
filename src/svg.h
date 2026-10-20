#pragma once
#include "image.h"

// True if the bytes look like an SVG document (an <svg> root near the start of the file).
bool IsSvgData(const uint8_t* data, size_t size);

// Renders an SVG document to a bitmap with Direct2D. Vectors have no fixed resolution, so the
// drawing is scaled up until its longest side reaches kSvgMinLongSide (small icons would otherwise
// come out as 24x24 thumbnails) and capped at kSvgMaxLongSide.
constexpr uint32_t kSvgMinLongSide = 2048;
constexpr uint32_t kSvgMaxLongSide = 8192;
Image RasterizeSvg(const uint8_t* data, size_t size);
