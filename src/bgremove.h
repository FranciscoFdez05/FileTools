#pragma once
#include "image.h"

#include <memory>

// Cuts the main subject out of a photo: U²-Net (embedded, run with Windows ML) predicts the subject
// mask, which is refined against the full-resolution image and becomes the alpha channel.
//
// Detection (the model) is the slow part and runs once; the cut-out can then be re-applied with
// different settings almost instantly, which is what the editor's sliders do.
struct SubjectMask {
    std::vector<float> coarse;  // model output, 320x320, 0..1
    std::vector<float> guide;   // luminance of the image at up to 1024 px, for edge refinement
    int gw = 0, gh = 0;
};

// Both settings go from 0 to 100; 50 is the default.
//  threshold: higher removes more of the uncertain areas, lower keeps more of the subject.
//  softness:  0 gives a hard cut, 100 a wide feathered edge.
struct CutoutSettings {
    int threshold = 50, softness = 50;
};

SubjectMask DetectSubject(const Image& img);  // throws AppError
// Multiplies the alpha channel of `img` (any size, same aspect as the detected image) by the subject mask.
void ApplyCutout(Image& img, const SubjectMask& mask, const CutoutSettings& settings);
// Detect + apply in one go, with the remembered settings. Fails if no subject is left.
Image RemoveBackground(const Image& img);

CutoutSettings LoadCutoutSettings();  // last values used in the editor
void SaveCutoutSettings(const CutoutSettings& settings);
