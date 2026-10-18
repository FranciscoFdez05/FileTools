#pragma once
#include "common.h"

#include <string>

struct DocBlock {
    enum Kind { Para, Heading, Bullet, Picture, PageBreak } kind = Para;
    int level = 0;          // heading level 1..6
    std::wstring text;      // may contain **bold**, *italic* and `code` when `markup` is set
    bool markup = false;
    std::vector<uint8_t> image;  // Picture: PNG or JPEG bytes
    std::string imageExt;        // "png" | "jpeg"
    uint32_t w = 0, h = 0;       // Picture size in pixels

    static DocBlock Paragraph(std::wstring t, bool md = false) {
        DocBlock b;
        b.text = std::move(t);
        b.markup = md;
        return b;
    }
};
using Doc = std::vector<DocBlock>;

bool IsDocumentExt(const std::wstring& ext);  // txt, md, html, docx
Doc ReadDocument(const fs::path& path);
// Format chosen from the extension: txt, md, html, docx, pdf.
void WriteDocument(const fs::path& path, const Doc& doc);

std::wstring ReadTextFile(const fs::path& path);
void WriteTextFile(const fs::path& path, const std::wstring& text);
