#pragma once
#include "image.h"

struct OcrBox {
    float x = 0, y = 0, w = 0, h = 0;
};
struct OcrWord {
    std::wstring text;
    OcrBox box;
};
struct OcrLine {
    std::wstring text;
    std::vector<OcrWord> words;
    OcrBox box;
};

// Windows.Media.Ocr using the user's profile languages. Must run on an MTA thread.
std::vector<OcrLine> RunOcr(const Image& img);
// Groups recognised lines into paragraphs.
std::vector<std::wstring> OcrParagraphs(const std::vector<OcrLine>& lines);
// Black-boxes e-mails, phone numbers, IDs, IBAN/card numbers. Returns number of redactions.
int RedactSensitive(Image& img, const std::vector<OcrLine>& lines);
