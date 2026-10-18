#pragma once
#include "image.h"

// Traces a bitmap into an SVG document: the colours are reduced to a small palette, each colour
// region is outlined and the outlines are simplified into lines and smooth curves. Transparent
// pixels stay transparent. Best for logos, icons, drawings and screenshots; photos come out posterized.
std::string VectorizeToSvg(const Image& img, int maxColors = 16);
