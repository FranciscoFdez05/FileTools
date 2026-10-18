#pragma once
#include "image.h"

#include <string>

// Renders every page of a PDF (Windows.Data.Pdf). Must run on an MTA thread.
uint32_t PdfPageCount(const fs::path& path);
void PdfRenderPages(const fs::path& path, double dpi,
                    const std::function<void(uint32_t index, uint32_t count, Image&& page)>& onPage);

// Minimal PDF 1.4 writer.
class PdfWriter {
public:
    PdfWriter();
    int AddObject(std::string body);
    int AddStream(const std::string& dict, const std::string& data);
    void AddPage(double widthPt, double heightPt, const std::string& resources, int contentsId);
    // Adds a page holding one JPEG image stretched to the page.
    void AddJpegPage(const std::vector<uint8_t>& jpeg, uint32_t wpx, uint32_t hpx, double widthPt, double heightPt,
                     bool gray = false);
    // Encodes `img` as JPEG and adds it as a page sized for `dpi`.
    void AddImagePage(const Image& img, double dpi, int quality);
    size_t PageCount() const { return pages_.size(); }
    std::string Build() const;
    void Save(const fs::path& path) const;

private:
    std::vector<std::string> objects_;  // object N lives at index N-1
    std::vector<int> pages_;
};

// Picks a DPI so the longest side of a page is at most A4's long side (11.69in).
double PageDpiFor(uint32_t wpx, uint32_t hpx);
