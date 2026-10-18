#include "pdf.h"

#include <unknwn.h>
#include <winrt/Windows.Data.Pdf.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Imaging.h>
#include <winrt/Windows.Storage.Streams.h>
#include <winrt/Windows.Storage.h>
#include <winrt/Windows.UI.h>

#include <cmath>
#include <cstring>

using namespace winrt;
using namespace winrt::Windows::Data::Pdf;
using namespace winrt::Windows::Graphics::Imaging;
using namespace winrt::Windows::Storage;
using namespace winrt::Windows::Storage::Streams;

namespace {

PdfDocument OpenPdf(const fs::path& path) {
    try {
        auto file = StorageFile::GetFileFromPathAsync(fs::absolute(path).make_preferred().wstring()).get();
        return PdfDocument::LoadFromFileAsync(file).get();
    } catch (const hresult_error& e) {
        if (e.code() == HRESULT_FROM_WIN32(ERROR_WRONG_PASSWORD))
            Fail(L"El PDF está protegido con contraseña");
        Fail(L"No se pudo abrir el PDF: " + std::wstring(e.message()));
    }
}

Image RenderPage(const PdfPage& page, double dpi) {
    auto size = page.Size();  // DIPs (1/96 in)
    double w = std::max(1.0, size.Width * dpi / 96.0), h = std::max(1.0, size.Height * dpi / 96.0);
    double limit = 9000.0;
    if (w > limit || h > limit) {
        double s = limit / std::max(w, h);
        w *= s;
        h *= s;
    }
    InMemoryRandomAccessStream stream;
    PdfPageRenderOptions opts;
    opts.DestinationWidth(uint32_t(std::lround(w)));
    opts.DestinationHeight(uint32_t(std::lround(h)));
    opts.BackgroundColor(winrt::Windows::UI::Color{255, 255, 255, 255});
    page.RenderToStreamAsync(stream, opts).get();
    stream.Seek(0);
    auto decoder = BitmapDecoder::CreateAsync(stream).get();
    auto bitmap = decoder.GetSoftwareBitmapAsync(BitmapPixelFormat::Bgra8, BitmapAlphaMode::Straight).get();
    Image img(bitmap.PixelWidth(), bitmap.PixelHeight());
    Buffer buffer((uint32_t)img.px.size());
    bitmap.CopyToBuffer(buffer);
    memcpy(img.px.data(), buffer.data(), std::min<size_t>(img.px.size(), buffer.Length()));
    return img;
}

std::string Num(double v) {
    char buf[32];
    snprintf(buf, sizeof(buf), "%.2f", v);
    return buf;
}

}  // namespace

uint32_t PdfPageCount(const fs::path& path) { return OpenPdf(path).PageCount(); }

void PdfRenderPages(const fs::path& path, double dpi,
                    const std::function<void(uint32_t, uint32_t, Image&&)>& onPage) {
    auto doc = OpenPdf(path);
    uint32_t count = doc.PageCount();
    for (uint32_t i = 0; i < count; ++i) {
        Image img;
        try {
            img = RenderPage(doc.GetPage(i), dpi);
        } catch (const hresult_error& e) {
            Fail(L"No se pudo renderizar la página " + std::to_wstring(i + 1) + L": " + std::wstring(e.message()));
        }
        onPage(i, count, std::move(img));
    }
}

double PageDpiFor(uint32_t wpx, uint32_t hpx) {
    double longest = std::max(wpx, hpx);
    return std::max(72.0, longest / 11.69);
}

PdfWriter::PdfWriter() { objects_.resize(2); }  // 1 = catalog, 2 = page tree

int PdfWriter::AddObject(std::string body) {
    objects_.push_back(std::move(body));
    return (int)objects_.size();
}

int PdfWriter::AddStream(const std::string& dict, const std::string& data) {
    return AddObject("<< " + dict + " /Length " + std::to_string(data.size()) + " >>\nstream\n" + data +
                     "\nendstream");
}

void PdfWriter::AddPage(double widthPt, double heightPt, const std::string& resources, int contentsId) {
    pages_.push_back(AddObject("<< /Type /Page /Parent 2 0 R /MediaBox [0 0 " + Num(widthPt) + " " + Num(heightPt) +
                               "] /Resources " + resources + " /Contents " + std::to_string(contentsId) + " 0 R >>"));
}

void PdfWriter::AddJpegPage(const std::vector<uint8_t>& jpeg, uint32_t wpx, uint32_t hpx, double widthPt,
                            double heightPt, bool gray) {
    int img = AddStream("/Type /XObject /Subtype /Image /Width " + std::to_string(wpx) + " /Height " +
                            std::to_string(hpx) + (gray ? " /ColorSpace /DeviceGray" : " /ColorSpace /DeviceRGB") +
                            " /BitsPerComponent 8 /Filter /DCTDecode",
                        std::string(jpeg.begin(), jpeg.end()));
    int content = AddStream("", "q " + Num(widthPt) + " 0 0 " + Num(heightPt) + " 0 0 cm /Im0 Do Q");
    AddPage(widthPt, heightPt, "<< /XObject << /Im0 " + std::to_string(img) + " 0 R >> >>", content);
}

void PdfWriter::AddImagePage(const Image& img, double dpi, int quality) {
    EncodeOpts opts;
    opts.quality = quality;
    auto jpeg = EncodeImage(img, ImgFmt::Jpg, opts);
    AddJpegPage(jpeg, img.w, img.h, img.w * 72.0 / dpi, img.h * 72.0 / dpi);
}

std::string PdfWriter::Build() const {
    std::string out = "%PDF-1.4\n%\xE2\xE3\xCF\xD3\n";
    std::vector<size_t> offsets(objects_.size());
    std::string kids;
    for (int p : pages_) kids += std::to_string(p) + " 0 R ";
    for (size_t i = 0; i < objects_.size(); ++i) {
        offsets[i] = out.size();
        out += std::to_string(i + 1) + " 0 obj\n";
        if (i == 0) out += "<< /Type /Catalog /Pages 2 0 R >>";
        else if (i == 1) out += "<< /Type /Pages /Kids [" + kids + "] /Count " + std::to_string(pages_.size()) + " >>";
        else out += objects_[i];
        out += "\nendobj\n";
    }
    size_t xref = out.size();
    out += "xref\n0 " + std::to_string(objects_.size() + 1) + "\n0000000000 65535 f \n";
    char line[32];
    for (size_t off : offsets) {
        snprintf(line, sizeof(line), "%010zu 00000 n \n", off);
        out += line;
    }
    out += "trailer\n<< /Size " + std::to_string(objects_.size() + 1) +
           " /Root 1 0 R /Info << /Producer (FileTools) >> >>\nstartxref\n" + std::to_string(xref) + "\n%%EOF\n";
    return out;
}

void PdfWriter::Save(const fs::path& path) const {
    if (pages_.empty()) Fail(L"El PDF no tiene páginas");
    WriteFileBytes(path, Build());
}
