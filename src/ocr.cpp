#include "ocr.h"

#include <unknwn.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Imaging.h>
#include <winrt/Windows.Media.Ocr.h>
#include <winrt/Windows.Storage.Streams.h>

#include <algorithm>
#include <cstring>
#include <regex>

using namespace winrt;
using namespace winrt::Windows::Graphics::Imaging;
using namespace winrt::Windows::Media::Ocr;

std::vector<OcrLine> RunOcr(const Image& src) {
    OcrEngine engine = OcrEngine::TryCreateFromUserProfileLanguages();
    if (!engine)
        Fail(L"OCR no disponible: añade un idioma con \"Reconocimiento óptico de caracteres\" en Configuración > "
             L"Hora e idioma");
    Image img = Flatten(src, 255, 255, 255);
    double scale = 1.0;
    uint32_t maxDim = OcrEngine::MaxImageDimension();
    if (img.w > maxDim || img.h > maxDim) {
        scale = double(maxDim) / std::max(img.w, img.h);
        img = Resize(img, uint32_t(img.w * scale), uint32_t(img.h * scale));
    } else if (std::max(img.w, img.h) < 800) {
        // Small images (icons, crops) recognise better when upscaled a bit.
        scale = 2.0;
        img = Resize(img, img.w * 2, img.h * 2);
    }
    winrt::Windows::Storage::Streams::Buffer buffer((uint32_t)img.px.size());
    memcpy(buffer.data(), img.px.data(), img.px.size());
    buffer.Length((uint32_t)img.px.size());
    auto bitmap = SoftwareBitmap::CreateCopyFromBuffer(buffer, BitmapPixelFormat::Bgra8, img.w, img.h,
                                                       BitmapAlphaMode::Premultiplied);
    OcrResult result{nullptr};
    try {
        result = engine.RecognizeAsync(bitmap).get();
    } catch (const hresult_error& e) {
        Fail(L"Error de OCR: " + std::wstring(e.message()));
    }
    std::vector<::OcrLine> lines;
    float inv = float(1.0 / scale);
    for (const auto& line : result.Lines()) {
        ::OcrLine l;
        l.text = line.Text();
        float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
        for (const auto& word : line.Words()) {
            auto r = word.BoundingRect();
            ::OcrWord w{std::wstring(word.Text()), {r.X * inv, r.Y * inv, r.Width * inv, r.Height * inv}};
            x0 = std::min(x0, w.box.x);
            y0 = std::min(y0, w.box.y);
            x1 = std::max(x1, w.box.x + w.box.w);
            y1 = std::max(y1, w.box.y + w.box.h);
            l.words.push_back(std::move(w));
        }
        if (l.words.empty()) continue;
        l.box = {x0, y0, x1 - x0, y1 - y0};
        lines.push_back(std::move(l));
    }
    return lines;
}

std::vector<std::wstring> OcrParagraphs(const std::vector<OcrLine>& lines) {
    std::vector<std::wstring> paras;
    if (lines.empty()) return paras;
    std::vector<float> heights;
    for (auto& l : lines) heights.push_back(l.box.h);
    std::nth_element(heights.begin(), heights.begin() + heights.size() / 2, heights.end());
    float median = heights[heights.size() / 2];

    std::wstring cur = lines[0].text;
    for (size_t i = 1; i < lines.size(); ++i) {
        const auto& prev = lines[i - 1].box;
        const auto& box = lines[i].box;
        float gap = box.y - (prev.y + prev.h);
        bool indentJump = std::abs(box.x - prev.x) > median * 4;
        if (gap > median * 0.9f || gap < -median * 2 || indentJump) {
            paras.push_back(cur);
            cur = lines[i].text;
        } else if (!cur.empty() && cur.back() == L'-') {
            cur.pop_back();
            cur += lines[i].text;
        } else {
            cur += L" " + lines[i].text;
        }
    }
    paras.push_back(cur);
    return paras;
}

int RedactSensitive(Image& img, const std::vector<OcrLine>& lines) {
    static const std::wregex patterns[] = {
        std::wregex(LR"([\w.+-]+@[\w-]+(\.[\w-]+)+)"),                       // e-mail
        std::wregex(LR"(\+?\d[\d ().-]{7,}\d)"),                             // phone numbers, long digit runs
        std::wregex(LR"(\b[A-Z]{2}\d{2}(?: ?[A-Z0-9]{4}){3,8}\b)"),          // IBAN
        std::wregex(LR"(\b[XYZ]?\d{7,8}[- ]?[A-Z]\b)", std::regex::icase),   // DNI / NIE
        std::wregex(LR"(\b\d{4,}\b)"),                                       // any 4+ digit number
        std::wregex(LR"(https?://\S+)", std::regex::icase),                  // URLs
    };
    int count = 0;
    for (const auto& line : lines) {
        // Rebuild the line from words to map regex matches back to word boxes.
        std::wstring text;
        std::vector<std::pair<size_t, size_t>> spans;
        for (const auto& w : line.words) {
            if (!text.empty()) text += L' ';
            spans.emplace_back(text.size(), text.size() + w.text.size());
            text += w.text;
        }
        std::vector<bool> hit(line.words.size(), false);
        for (const auto& re : patterns) {
            for (std::wsregex_iterator it(text.begin(), text.end(), re), end; it != end; ++it) {
                size_t a = it->position(), b = a + it->length();
                for (size_t k = 0; k < spans.size(); ++k)
                    if (spans[k].first < b && spans[k].second > a) hit[k] = true;
            }
        }
        for (size_t k = 0; k < hit.size(); ++k) {
            if (!hit[k]) continue;
            const auto& b = line.words[k].box;
            int pad = std::max(2, int(b.h * 0.15f));
            FillRect(img, int(b.x) - pad, int(b.y) - pad, int(b.w) + pad * 2, int(b.h) + pad * 2, 0xFF000000);
            ++count;
        }
    }
    return count;
}
