#include "actions.h"

#include "bgremove.h"
#include "capture.h"
#include "docs.h"
#include "editor.h"
#include "fileinfo.h"
#include "floatshelf.h"
#include "image.h"
#include "launcher.h"
#include "media.h"
#include "ocr.h"
#include "pdf.h"
#include "sendpanel.h"
#include "shellutil.h"
#include "vectorize.h"

#include <shellapi.h>
#include <shlwapi.h>
#include <unknwn.h>
#include <winrt/base.h>

#include <algorithm>
#include <cwctype>
#include <map>

namespace {

// Segoe Fluent Icons / MDL2 glyphs.
constexpr wchar_t kGlyphCompress = 0xE73F;
constexpr wchar_t kGlyphSliders = 0xE9E9;
constexpr wchar_t kGlyphPen = 0xE70F;
constexpr wchar_t kGlyphPicture = 0xEB9F;
constexpr wchar_t kGlyphCrop = 0xE7A8;
constexpr wchar_t kGlyphHide = 0xED1A;
constexpr wchar_t kGlyphRotate = 0xE7AD;
constexpr wchar_t kGlyphText = 0xE8D2;
constexpr wchar_t kGlyphCut = 0xE8C6;
constexpr wchar_t kGlyphLink = 0xE71B;
constexpr wchar_t kGlyphMute = 0xE74F;
constexpr wchar_t kGlyphMusic = 0xE8D6;
constexpr wchar_t kGlyphSlideshow = 0xE786;
constexpr wchar_t kGlyphCamera = 0xE722;
constexpr wchar_t kGlyphVolume = 0xE995;
constexpr wchar_t kGlyphFolderOpen = 0xE838;
constexpr wchar_t kGlyphCopy = 0xE8C8;
constexpr wchar_t kGlyphMove = 0xE8DE;
constexpr wchar_t kGlyphZip = 0xF012;
constexpr wchar_t kGlyphTerminal = 0xE756;
constexpr wchar_t kGlyphPin = 0xE840;
constexpr wchar_t kGlyphShare = 0xE72D;
constexpr wchar_t kGlyphOpenWith = 0xE7AC;
constexpr wchar_t kGlyphSpeaker = 0xE7F5;
constexpr wchar_t kGlyphFingerprint = 0xE928;
constexpr wchar_t kGlyphCount = 0xE8EF;
constexpr wchar_t kGlyphInfo = 0xE946;
constexpr wchar_t kGlyphErase = 0xE75C;
constexpr wchar_t kGlyphWand = 0xE771;
constexpr wchar_t kGlyphQr = 0xED14;
constexpr wchar_t kGlyphResize = 0xE740;
constexpr wchar_t kGlyphScreenshot = 0xE7C4;
constexpr wchar_t kGlyphWindow = 0xE737;
constexpr wchar_t kGlyphDesktop = 0xE7F4;
constexpr wchar_t kGlyphAdd = 0xE710;

std::wstring CanonicalExt(const fs::path& p) {
    std::wstring e = ExtOf(p);
    if (e == L"jpeg" || e == L"jfif") return L"jpg";
    if (e == L"tif") return L"tiff";
    if (e == L"heif") return L"heic";
    if (e == L"htm") return L"html";
    if (e == L"markdown") return L"md";
    if (e == L"m4v") return L"mp4";
    if (e == L"aif") return L"aiff";
    if (e == L"oga") return L"ogg";
    return e;
}

std::vector<ActionDef> Formats(std::initializer_list<const wchar_t*> exts, const std::wstring& own) {
    std::vector<ActionDef> out;
    for (const wchar_t* e : exts) {
        if (own == e) continue;
        std::wstring label = e == std::wstring(L"tgz") ? L"TAR.GZ" : ToUpper(e);
        out.push_back({"to:" + WideToUtf8(e), label});
    }
    return out;
}

bool CanEncodeCached(ImgFmt f) {
    static std::map<ImgFmt, bool> cache;
    auto it = cache.find(f);
    if (it != cache.end()) return it->second;
    bool ok = false;
    try {
        ok = CanEncode(f);
    } catch (...) {
    }
    return cache[f] = ok;
}

int Quality() { return (int)AppSettings().quality; }

// Runs `fn` on every file, collecting outputs and errors.
JobResult ForEachFile(const std::vector<fs::path>& files, const Progress& progress,
                      const std::function<std::vector<fs::path>(const fs::path&)>& fn) {
    JobResult r;
    int failed = 0;
    std::wstring firstError;
    for (size_t i = 0; i < files.size(); ++i) {
        if (files.size() > 1) progress((int)i, (int)files.size());
        try {
            auto outs = fn(files[i]);
            r.outputs.insert(r.outputs.end(), outs.begin(), outs.end());
        } catch (const AppError& e) {
            ++failed;
            if (firstError.empty()) firstError = e.message;
        } catch (const winrt::hresult_error& e) {
            ++failed;
            if (firstError.empty()) firstError = e.message().c_str();
        } catch (const std::exception& e) {
            ++failed;
            if (firstError.empty()) firstError = Utf8ToWide(e.what());
        }
    }
    if (r.outputs.empty()) {
        r.ok = false;
        r.message = firstError.empty() ? L"No se generó ningún archivo" : firstError;
    } else if (failed) {
        r.message = std::to_wstring(failed) + L" con error: " + firstError;
    }
    return r;
}

std::wstring SizeChange(uint64_t before, uint64_t after) {
    int pct = before ? int(std::lround(100.0 * (double(before) - double(after)) / double(before))) : 0;
    return FormatSize(before) + L" → " + FormatSize(after) + L" (−" + std::to_wstring(pct) + L"%)";
}

// Format to save an edited image in: keep the original when we can encode it.
ImgFmt SameFormat(const fs::path& f) {
    ImgFmt fmt;
    if (ParseImgFmt(ExtOf(f), fmt) && CanEncodeCached(fmt)) return fmt;
    return ImgFmt::Png;
}

fs::path SaveEdited(const Image& img, const fs::path& src, const wchar_t* suffix) {
    ImgFmt fmt = SameFormat(src);
    fs::path out = OutPath(src, ImgFmtExt(fmt), suffix);
    EncodeOpts o;
    o.quality = std::max(Quality(), 92);
    SaveImage(img, out, fmt, o);
    return out;
}

std::wstring Pad3(uint32_t n) {
    wchar_t buf[16];
    swprintf_s(buf, L"%03u", n);
    return buf;
}

// --- Images -------------------------------------------------------------------

void AddImageToPdf(PdfWriter& pdf, const fs::path& f) {
    uint32_t w, h;
    bool gray;
    if (JpegPassthroughInfo(f, w, h, gray)) {
        double dpi = PageDpiFor(w, h);
        pdf.AddJpegPage(ReadFileBytes(f), w, h, w * 72.0 / dpi, h * 72.0 / dpi, gray);
        return;
    }
    Image img = Flatten(LoadImageFile(f), 255, 255, 255);
    pdf.AddImagePage(img, PageDpiFor(img.w, img.h), 92);
}

void AddPdfToPdf(PdfWriter& pdf, const fs::path& f, double dpi, int quality, const Progress& progress,
                 const std::function<void(Image&)>& transform = nullptr) {
    PdfRenderPages(f, dpi, [&](uint32_t i, uint32_t count, Image&& page) {
        progress((int)i, (int)count);
        if (transform) transform(page);
        pdf.AddImagePage(page, dpi, quality);
    });
}

JobResult ImagesToPdf(const std::vector<fs::path>& files, const Progress& progress) {
    PdfWriter pdf;
    auto r = ForEachFile(files, progress, [&](const fs::path& f) {
        AddImageToPdf(pdf, f);
        return std::vector<fs::path>{f};
    });
    if (pdf.PageCount() == 0) return r;
    fs::path out = OutPath(files[0], L"pdf");
    pdf.Save(out);
    r.outputs = {out};
    return r;
}

JobResult ImagesToDocx(const std::vector<fs::path>& files, const Progress& progress) {
    Doc doc;
    auto r = ForEachFile(files, progress, [&](const fs::path& f) {
        Image img = LoadImageFile(f);
        DocBlock b;
        b.kind = DocBlock::Picture;
        b.w = img.w;
        b.h = img.h;
        uint32_t w, h;
        bool gray;
        if (JpegPassthroughInfo(f, w, h, gray)) {
            b.image = ReadFileBytes(f);
            b.imageExt = "jpeg";
        } else if (CanonicalExt(f) == L"png") {
            b.image = ReadFileBytes(f);
            b.imageExt = "png";
        } else {
            b.image = EncodeImage(img, ImgFmt::Png);
            b.imageExt = "png";
        }
        doc.push_back(std::move(b));
        return std::vector<fs::path>{f};
    });
    if (doc.empty()) return r;
    fs::path out = OutPath(files[0], L"docx");
    WriteDocument(out, doc);
    r.outputs = {out};
    return r;
}

std::vector<fs::path> ConvertImage(const fs::path& f, const std::wstring& target) {
    if (target == L"svg") {
        fs::path out = OutPath(f, L"svg");
        WriteFileBytes(out, VectorizeToSvg(LoadImageFile(f)));
        return {out};
    }
    ImgFmt fmt;
    if (!ParseImgFmt(target, fmt)) Fail(L"Formato no soportado");
    Image img = LoadImageFile(f);
    fs::path out = OutPath(f, ImgFmtExt(fmt));
    EncodeOpts o;
    o.quality = Quality();
    SaveImage(img, out, fmt, o);
    return {out};
}

JobResult CompressImages(const std::vector<fs::path>& files, const Progress& progress) {
    uint64_t before = 0, after = 0;
    auto r = ForEachFile(files, progress, [&](const fs::path& f) {
        uint64_t size = SizeOnDisk(f);
        Image img = FitWithin(LoadImageFile(f), 2560);
        ImgFmt fmt;
        if (!ParseImgFmt(ExtOf(f), fmt) || !CanEncodeCached(fmt) || fmt == ImgFmt::Bmp || fmt == ImgFmt::Tiff ||
            fmt == ImgFmt::Gif)
            fmt = img.HasAlpha() ? ImgFmt::Png : ImgFmt::Jpg;
        EncodeOpts o;
        o.quality = 72;
        o.quantize = fmt == ImgFmt::Png;
        auto data = EncodeImage(img, fmt, o);
        if (data.size() >= size) Fail(L"Ya está optimizada: no se pudo reducir más");
        fs::path out = OutPath(f, ImgFmtExt(fmt), L"comprimido");
        WriteFileBytes(out, data);
        before += size;
        after += data.size();
        return std::vector<fs::path>{out};
    });
    if (r.ok && r.message.empty()) r.message = SizeChange(before, after);
    return r;
}

std::vector<fs::path> ImageOcr(const fs::path& f, std::wstring* text) {
    auto paras = OcrParagraphs(RunOcr(LoadImageFile(f)));
    if (paras.empty()) Fail(L"No se encontró texto en la imagen");
    std::wstring all;
    for (auto& p : paras) all += p + L"\r\n\r\n";
    fs::path out = OutPath(f, L"txt");
    WriteTextFile(out, all);
    if (text) *text += all;
    return {out};
}

// --- PDF ----------------------------------------------------------------------

std::vector<fs::path> PdfToImages(const fs::path& f, const std::wstring& target, const Progress& progress) {
    ImgFmt fmt;
    ParseImgFmt(target, fmt);
    std::vector<fs::path> outs;
    fs::path folder;
    EncodeOpts o;
    o.quality = Quality();
    uint32_t total = PdfPageCount(f);
    if (total > 1) {
        folder = UniquePath(OutputDir(f), BaseNameOf(f) + L" (" + ToUpper(target) + L")", L"");
        fs::create_directories(folder);
    }
    PdfRenderPages(f, 300, [&](uint32_t i, uint32_t count, Image&& page) {
        progress((int)i, (int)count);
        fs::path out = count == 1 ? OutPath(f, ImgFmtExt(fmt))
                                  : folder / (BaseNameOf(f) + L" - " + Pad3(i + 1) + L"." + ImgFmtExt(fmt));
        SaveImage(page, out, fmt, o);
        if (count == 1) outs.push_back(out);
    });
    if (!folder.empty()) outs.push_back(folder);
    return outs;
}

Doc PdfOcrDoc(const fs::path& f, const Progress& progress) {
    Doc doc;
    bool anyText = false;
    PdfRenderPages(f, 250, [&](uint32_t i, uint32_t count, Image&& page) {
        progress((int)i, (int)count);
        for (auto& p : OcrParagraphs(RunOcr(page))) {
            doc.push_back(DocBlock::Paragraph(p));
            anyText = true;
        }
        if (i + 1 < count) {
            DocBlock pb;
            pb.kind = DocBlock::PageBreak;
            doc.push_back(pb);
        }
    });
    if (!anyText) Fail(L"No se encontró texto en el PDF");
    return doc;
}

JobResult PdfRewrite(const std::vector<fs::path>& files, const Progress& progress, const wchar_t* suffix, double dpi,
                     int quality, const std::function<void(Image&)>& transform, bool mustShrink) {
    uint64_t before = 0, after = 0;
    auto r = ForEachFile(files, progress, [&](const fs::path& f) {
        PdfWriter pdf;
        AddPdfToPdf(pdf, f, dpi, quality, progress, transform);
        std::string bytes = pdf.Build();
        uint64_t size = SizeOnDisk(f);
        if (mustShrink && bytes.size() >= size) Fail(L"Ya está optimizado: no se pudo reducir más");
        fs::path out = OutPath(f, L"pdf", suffix);
        WriteFileBytes(out, bytes);
        before += size;
        after += bytes.size();
        return std::vector<fs::path>{out};
    });
    if (mustShrink && r.ok && r.message.empty()) r.message = SizeChange(before, after);
    return r;
}

JobResult SplitPdfs(const std::vector<fs::path>& files, const Progress& progress) {
    return ForEachFile(files, progress, [&](const fs::path& f) {
        fs::path folder = UniquePath(OutputDir(f), BaseNameOf(f) + L" (páginas)", L"");
        fs::create_directories(folder);
        PdfRenderPages(f, 200, [&](uint32_t i, uint32_t count, Image&& page) {
            progress((int)i, (int)count);
            PdfWriter pdf;
            pdf.AddImagePage(page, 200, 85);
            pdf.Save(folder / (BaseNameOf(f) + L" - " + Pad3(i + 1) + L".pdf"));
        });
        return std::vector<fs::path>{folder};
    });
}

JobResult MergeToPdf(const std::vector<fs::path>& files, const Progress& progress) {
    if (files.size() < 2) Fail(L"Suelta varios PDF o imágenes para unirlos");
    std::vector<fs::path> sorted = files;
    std::sort(sorted.begin(), sorted.end(), [](const fs::path& a, const fs::path& b) {
        return StrCmpLogicalW(a.filename().c_str(), b.filename().c_str()) < 0;
    });
    PdfWriter pdf;
    auto r = ForEachFile(sorted, progress, [&](const fs::path& f) {
        if (KindOf(f) == Kind::Pdf) AddPdfToPdf(pdf, f, 200, 85, [](int, int) {});
        else if (KindOf(f) == Kind::Image) AddImageToPdf(pdf, f);
        else Fail(L"Sólo se pueden unir PDF e imágenes");
        return std::vector<fs::path>{f};
    });
    if (pdf.PageCount() == 0) return r;
    fs::path out = OutPath(sorted[0], L"pdf", L"unido");
    pdf.Save(out);
    r.outputs = {out};
    r.message = std::to_wstring(pdf.PageCount()) + L" páginas";
    return r;
}

// --- Archives -----------------------------------------------------------------

std::wstring ArchiveExt(const std::wstring& target) { return target == L"tgz" ? L"tar.gz" : target; }

fs::path ArchiveFromItems(const std::vector<fs::path>& items, const std::wstring& target) {
    fs::path parent = items[0].parent_path();
    std::vector<std::wstring> names;
    for (const auto& f : items)
        if (f.parent_path() == parent) names.push_back(f.filename().wstring());
    std::wstring base = items.size() == 1 ? (fs::is_directory(items[0]) ? items[0].filename().wstring()
                                                                        : BaseNameOf(items[0]))
                                          : (parent.filename().empty() ? L"Archivo" : parent.filename().wstring());
    fs::path out = UniquePath(OutputDir(items[0]), base, ArchiveExt(target));
    CreateArchive(out, parent, names);
    return out;
}

std::vector<fs::path> ConvertArchive(const fs::path& f, const std::wstring& target) {
    fs::path tmp = MakeTempDir();
    std::vector<fs::path> outs;
    try {
        ExtractArchive(f, tmp);
        std::vector<std::wstring> names;
        for (auto& e : fs::directory_iterator(tmp)) names.push_back(e.path().filename().wstring());
        if (names.empty()) Fail(L"El archivo está vacío");
        fs::path out = UniquePath(OutputDir(f), BaseNameOf(f), ArchiveExt(target));
        CreateArchive(out, tmp, names);
        outs.push_back(out);
    } catch (...) {
        std::error_code ec;
        fs::remove_all(tmp, ec);
        throw;
    }
    std::error_code ec;
    fs::remove_all(tmp, ec);
    return outs;
}

// --- Media --------------------------------------------------------------------

std::vector<fs::path> FfmpegTool(const fs::path& f, const std::wstring& args, const std::wstring& ext,
                                 const wchar_t* suffix) {
    fs::path out = OutPath(f, ext, suffix);
    Ffmpeg(f, args, out);
    return {out};
}

JobResult MediaCompress(const std::vector<fs::path>& files, const Progress& progress) {
    uint64_t before = 0, after = 0;
    auto r = ForEachFile(files, progress, [&](const fs::path& f) {
        bool video = KindOf(f) == Kind::Video;
        auto outs = video ? FfmpegTool(f,
                                       L"-c:v libx264 -preset medium -crf 28 -vf \"scale='min(1920,iw)':-2\" "
                                       L"-pix_fmt yuv420p -c:a aac -b:a 128k -movflags +faststart",
                                       L"mp4", L"comprimido")
                          : FfmpegTool(f, L"-vn -c:a libmp3lame -b:a 128k", L"mp3", L"comprimido");
        before += SizeOnDisk(f);
        after += SizeOnDisk(outs[0]);
        return outs;
    });
    if (r.ok && r.message.empty() && after < before) r.message = SizeChange(before, after);
    return r;
}

JobResult Convert(const std::wstring& target, const std::vector<fs::path>& files, const Progress& progress) {
    Kind kind = KindOf(files[0]);
    if (kind == Kind::Image && target == L"pdf") return ImagesToPdf(files, progress);
    if (kind == Kind::Image && target == L"docx") return ImagesToDocx(files, progress);
    if (kind == Kind::Folder || (kind == Kind::Other && (target == L"zip" || target == L"tar" || target == L"tgz"))) {
        JobResult r;
        r.outputs = {ArchiveFromItems(files, target)};
        return r;
    }
    return ForEachFile(files, progress, [&](const fs::path& f) -> std::vector<fs::path> {
        switch (KindOf(f)) {
            case Kind::Image: return ConvertImage(f, target);
            case Kind::Pdf:
                if (target == L"jpg" || target == L"png") return PdfToImages(f, target, progress);
                {
                    fs::path out = OutPath(f, target);
                    WriteDocument(out, PdfOcrDoc(f, progress));
                    return {out};
                }
            case Kind::Document: {
                fs::path out = OutPath(f, target);
                WriteDocument(out, ReadDocument(f));
                return {out};
            }
            case Kind::Audio:
            case Kind::Video: {
                fs::path out = OutPath(f, target);
                Ffmpeg(f, FfmpegArgsFor(target, KindOf(f) == Kind::Video), out);
                return {out};
            }
            case Kind::Archive: return ConvertArchive(f, target);
            case Kind::Folder: return {ArchiveFromItems({f}, target)};
            default: Fail(L"No hay conversión para este tipo de archivo");
        }
    });
}

std::wstring Thousands(uint64_t v) {
    std::wstring s = std::to_wstring(v);
    for (int i = (int)s.size() - 3; i > 0; i -= 3) s.insert(i, L".");
    return s;
}

// --- Hash and metadata (any file) ---------------------------------------------

JobResult HashSheet(const std::vector<fs::path>& files, const Progress& progress) {
    // Folders contribute every file inside them, named relative to the folder.
    std::vector<std::pair<fs::path, std::wstring>> items;
    constexpr size_t kMaxFiles = 2000;
    for (const auto& f : files) {
        std::error_code ec;
        if (!fs::is_directory(f, ec)) {
            items.push_back({f, f.filename().wstring()});
            continue;
        }
        for (auto it = fs::recursive_directory_iterator(f, fs::directory_options::skip_permission_denied, ec);
             it != fs::recursive_directory_iterator() && items.size() < kMaxFiles; it.increment(ec)) {
            if (ec) break;
            if (it->is_regular_file(ec))
                items.push_back({it->path(), (f.filename() / fs::relative(it->path(), f, ec)).wstring()});
        }
    }
    if (items.empty()) Fail(L"No hay archivos de los que calcular el hash");
    auto sheet = std::make_shared<InfoSheet>();
    sheet->type = InfoSheet::Hash;
    sheet->files = files;
    int failed = 0;
    for (size_t i = 0; i < items.size(); ++i) {
        progress((int)i, (int)items.size());
        try {
            auto rows = FileHashes(items[i].first, items[i].second);
            sheet->rows.insert(sheet->rows.end(), rows.begin(), rows.end());
        } catch (const AppError& e) {
            ++failed;
            sheet->rows.push_back({items[i].second, L"Error", e.message});
        }
    }
    std::wstring what = items.size() == 1 ? items[0].second : std::to_wstring(items.size()) + L" archivos";
    sheet->title = L"Hash · " + what;
    sheet->summary = L"MD5 · SHA-1 · SHA-256 · SHA-512" +
                     (items.size() > 1 ? L" · " + std::to_wstring(items.size()) + L" archivos" : std::wstring()) +
                     (items.size() >= kMaxFiles ? L" (límite alcanzado)" : L"") +
                     (failed ? L" · " + std::to_wstring(failed) + L" sin leer" : std::wstring());
    sheet->note = L"Doble clic en una fila para copiar su valor";
    JobResult r;
    r.sheet = sheet;
    r.message = L"Hash calculado";
    return r;
}

JobResult MetadataSheet(const std::vector<fs::path>& files, const Progress& progress) {
    constexpr size_t kMaxFiles = 20;
    auto sheet = std::make_shared<InfoSheet>();
    sheet->type = InfoSheet::Metadata;
    size_t count = std::min(files.size(), kMaxFiles);
    sheet->files.assign(files.begin(), files.begin() + count);
    int exifTags = 0;
    bool exifTool = true;
    for (size_t i = 0; i < count; ++i) {
        progress((int)i, (int)count);
        std::wstring prefix = count > 1 ? files[i].filename().wstring() + L" · " : L"";
        int n = CollectMetadata(files[i], prefix, sheet->rows);
        if (n < 0) exifTool = false;
        else exifTags += n;
        std::error_code ec;
        if (!fs::is_directory(files[i], ec)) sheet->canStrip = true;
    }
    std::wstring what = count == 1 ? files[0].filename().wstring() : std::to_wstring(count) + L" archivos";
    sheet->title = L"Metadatos · " + what;
    sheet->summary = std::to_wstring(sheet->rows.size()) + L" propiedades" +
                     (exifTool ? L" · ExifTool: " + std::to_wstring(exifTags) + L" etiquetas" : std::wstring()) +
                     (files.size() > count ? L" · sólo los primeros " + std::to_wstring(count) + L" archivos"
                                           : std::wstring());
    sheet->note = exifTool ? L"Doble clic en una fila para copiar su valor"
                           : L"Conecta ExifTool (botón de arriba) para ver todos los metadatos";
    JobResult r;
    r.sheet = sheet;
    r.message = L"Metadatos leídos";
    return r;
}

// Writes "<name> sin metadatos.<ext>"; `note` explains anything the user should know about the copy.
std::vector<fs::path> StripFile(const fs::path& f, const Progress& progress, std::wstring& note) {
    Kind kind = KindOf(f);
    std::wstring ext = CanonicalExt(f);
    std::wstring exifError;
    if (kind == Kind::Folder) Fail(L"Las carpetas no tienen metadatos que quitar");
    if (IsOfficeExt(ext)) {
        fs::path out = OutPath(f, ext, L"sin metadatos");
        StripOfficeMetadata(f, out);
        note = L"Autor, empresa, fechas y propiedades borrados";
        return {out};
    }
    if (kind == Kind::Image) {
        ImgFmt fmt = SameFormat(f);
        fs::path out = OutPath(f, ImgFmtExt(fmt), L"sin metadatos");
        if (CanonicalExt(f) == ImgFmtExt(fmt) && StripMetadataLossless(f, out)) return {out};
        fs::path exifOut = OutPath(f, ExtOf(f), L"sin metadatos");
        if (StripWithExifTool(f, exifOut, true, exifError)) return {exifOut};
        EncodeOpts o;
        o.quality = 95;
        SaveImage(LoadImageFile(f), out, fmt, o);
        return {out};
    }
    if (kind == Kind::Pdf) {
        // ExifTool only hides PDF metadata (the old revision stays in the file), so rebuild the pages instead.
        PdfWriter pdf;
        AddPdfToPdf(pdf, f, 200, 88, progress);
        fs::path out = OutPath(f, L"pdf", L"sin metadatos");
        pdf.Save(out);
        note = L"Páginas rasterizadas a 200 ppp";
        return {out};
    }
    if (kind == Kind::Audio || kind == Kind::Video) {
        if (!FfmpegPath().empty())
            return FfmpegTool(f,
                              L"-map 0:v? -map 0:a? -map 0:s? -c copy -map_metadata -1 -map_chapters -1 "
                              L"-fflags +bitexact",
                              ext, L"sin metadatos");
        fs::path out = OutPath(f, ExtOf(f), L"sin metadatos");
        if (StripWithExifTool(f, out, false, exifError)) return {out};
        Fail(L"Para quitar metadatos de audio y vídeo instala ffmpeg (winget install Gyan.FFmpeg) o ExifTool");
    }
    fs::path out = OutPath(f, ExtOf(f), L"sin metadatos");
    if (StripWithExifTool(f, out, false, exifError)) return {out};
    if (HasAlternateStreams(f)) {
        // Nothing embedded, but copying the bytes drops NTFS streams such as where it was downloaded from.
        WriteFileBytes(out, ReadFileBytes(f));
        note = L"Quitado el origen de descarga";
        return {out};
    }
    if (ExifToolPath().empty())
        Fail(L"No se reconocen metadatos en este archivo. Para más formatos instala ExifTool: winget install "
             L"OliverBetz.ExifTool");
    Fail(L"Este archivo no tiene metadatos que se puedan quitar (" + exifError + L")");
}

JobResult Tool(const std::string& tool, const std::vector<fs::path>& files, const Progress& progress) {
    Kind kind = KindOf(files[0]);
    if (tool == "hash") return HashSheet(files, progress);
    if (tool == "info") return MetadataSheet(files, progress);
    if (tool == "metadata") {
        std::wstring note;
        auto r = ForEachFile(files, progress, [&](const fs::path& f) { return StripFile(f, progress, note); });
        if (r.ok && r.message.empty()) r.message = note;
        return r;
    }
    if (tool == "count" && kind == Kind::Folder) {
        uint64_t count = 0, bytes = 0;
        for (const auto& f : files) {
            std::error_code ec;
            for (auto it = fs::recursive_directory_iterator(f, fs::directory_options::skip_permission_denied, ec);
                 it != fs::recursive_directory_iterator(); it.increment(ec)) {
                if (ec) break;
                if (it->is_regular_file(ec)) {
                    ++count;
                    bytes += it->file_size(ec);
                }
            }
        }
        return JobResult{true, {}, Thousands(count) + L" archivos · " + FormatSize(bytes)};
    }
    if (kind == Kind::Document) {
        if (tool == "count") {
            uint64_t words = 0, chars = 0, paras = 0;
            for (const auto& f : files) {
                for (const auto& b : ReadDocument(f)) {
                    if (b.kind == DocBlock::PageBreak || b.kind == DocBlock::Picture) continue;
                    ++paras;
                    bool inWord = false;
                    for (wchar_t c : b.text) {
                        if (b.markup && (c == L'*' || c == L'`' || c == L'\\')) continue;
                        ++chars;
                        bool space = iswspace(c) != 0;
                        if (!space && !inWord) ++words;
                        inWord = !space;
                    }
                }
            }
            return JobResult{true, {}, Thousands(words) + L" palabras · " + Thousands(chars) + L" caracteres · " +
                                           Thousands(paras) + L" párrafos"};
        }
        if (tool == "merge") {
            if (files.size() < 2) Fail(L"Suelta varios documentos para unirlos");
            std::vector<fs::path> sorted = files;
            std::sort(sorted.begin(), sorted.end(), [](const fs::path& a, const fs::path& b) {
                return StrCmpLogicalW(a.filename().c_str(), b.filename().c_str()) < 0;
            });
            Doc all;
            for (size_t i = 0; i < sorted.size(); ++i) {
                progress((int)i, (int)sorted.size());
                if (i) {
                    DocBlock pb;
                    pb.kind = DocBlock::PageBreak;
                    all.push_back(pb);
                }
                Doc d = ReadDocument(sorted[i]);
                all.insert(all.end(), d.begin(), d.end());
            }
            fs::path out = OutPath(sorted[0], L"docx", L"unido");
            WriteDocument(out, all);
            return JobResult{true, {out}, std::to_wstring(sorted.size()) + L" documentos"};
        }
    }
    if (kind == Kind::Image) {
        if (tool == "compress") return CompressImages(files, progress);
        if (tool == "nobg")
            return ForEachFile(files, progress, [](const fs::path& f) {
                fs::path out = OutPath(f, L"png", L"sin fondo");
                SaveImage(RemoveBackground(LoadImageFile(f)), out, ImgFmt::Png);
                return std::vector<fs::path>{out};
            });
        if (tool == "background")
            return ForEachFile(files, progress, [](const fs::path& f) {
                fs::path out = OutPath(f, L"png", L"fondo");
                SaveImage(AddBackdrop(LoadImageFile(f)), out, ImgFmt::Png);
                return std::vector<fs::path>{out};
            });
        if (tool == "crop")
            return ForEachFile(files, progress, [](const fs::path& f) {
                Image cropped;
                if (!AutoCrop(LoadImageFile(f), cropped)) Fail(L"No hay bordes que recortar");
                return std::vector<fs::path>{SaveEdited(cropped, f, L"recortado")};
            });
        if (tool == "redact") {
            int total = 0;
            auto r = ForEachFile(files, progress, [&](const fs::path& f) {
                Image img = LoadImageFile(f);
                int n = RedactSensitive(img, RunOcr(img));
                if (n == 0) Fail(L"No se encontraron datos sensibles (correos, teléfonos, números, IBAN)");
                total += n;
                return std::vector<fs::path>{SaveEdited(img, f, L"censurado")};
            });
            if (r.ok && r.message.empty()) r.message = std::to_wstring(total) + L" elementos ocultados";
            return r;
        }
        if (tool == "rotate")
            return ForEachFile(files, progress, [](const fs::path& f) {
                return std::vector<fs::path>{SaveEdited(Rotate90(LoadImageFile(f)), f, L"rotado")};
            });
        if (tool == "ocr") {
            std::wstring text;
            auto r = ForEachFile(files, progress, [&](const fs::path& f) { return ImageOcr(f, &text); });
            if (r.ok && CopyTextToClipboard(nullptr, text) && r.message.empty()) r.message = L"Texto copiado al portapapeles";
            return r;
        }
    }
    if (kind == Kind::Pdf) {
        if (tool == "compress") return PdfRewrite(files, progress, L"comprimido", 110, 60, nullptr, true);
        if (tool == "split") return SplitPdfs(files, progress);
        if (tool == "merge") return MergeToPdf(files, progress);
        if (tool == "rotate")
            return PdfRewrite(files, progress, L"rotado", 200, 88, [](Image& p) { p = Rotate90(p); }, false);
        if (tool == "redact") {
            int total = 0;
            auto r = PdfRewrite(
                files, progress, L"censurado", 200, 88, [&](Image& p) { total += RedactSensitive(p, RunOcr(p)); },
                false);
            if (r.ok && total == 0) {
                for (auto& o : r.outputs) {
                    std::error_code ec;
                    fs::remove(o, ec);
                }
                r = JobResult{false, {}, L"No se encontraron datos sensibles (correos, teléfonos, números, IBAN)"};
            } else if (r.ok && r.message.empty()) {
                r.message = std::to_wstring(total) + L" elementos ocultados";
            }
            return r;
        }
        if (tool == "ocr")
            return ForEachFile(files, progress, [&](const fs::path& f) {
                fs::path out = OutPath(f, L"txt");
                WriteDocument(out, PdfOcrDoc(f, progress));
                return std::vector<fs::path>{out};
            });
    }
    if (kind == Kind::Video || kind == Kind::Audio) {
        if (tool == "compress") return MediaCompress(files, progress);
        return ForEachFile(files, progress, [&](const fs::path& f) -> std::vector<fs::path> {
            std::wstring ext = CanonicalExt(f);
            if (tool == "mute") return FfmpegTool(f, L"-c:v copy -an", ext, L"sin audio");
            if (tool == "audio") return FfmpegTool(f, L"-vn -c:a aac -b:a 192k", L"m4a", L"");
            if (tool == "gif") return FfmpegTool(f, FfmpegArgsFor(L"gif", true), L"gif", L"");
            if (tool == "rotate")
                return FfmpegTool(f, L"-vf transpose=1 " + FfmpegArgsFor(ext, true), ext, L"rotado");
            if (tool == "frame") return FfmpegTool(f, L"-vf thumbnail -frames:v 1 -q:v 2", L"jpg", L"fotograma");
            if (tool == "normalize")
                return FfmpegTool(f, L"-af loudnorm=I=-16:TP=-1.5:LRA=11 " + FfmpegArgsFor(ext, false), ext,
                                  L"normalizado");
            if (tool == "mono") return FfmpegTool(f, L"-ac 1 " + FfmpegArgsFor(ext, false), ext, L"mono");
            if (tool == "silence")
                return FfmpegTool(f,
                                  L"-af silenceremove=start_periods=1:start_threshold=-50dB:stop_periods=-1:stop_"
                                  L"threshold=-50dB:stop_duration=0.6 " +
                                      FfmpegArgsFor(ext, false),
                                  ext, L"sin silencios");
            Fail(L"Herramienta desconocida");
        });
    }
    if (kind == Kind::Archive && tool == "extract")
        return ForEachFile(files, progress, [](const fs::path& f) {
            fs::path dest = UniquePath(OutputDir(f), BaseNameOf(f), L"");
            ExtractArchive(f, dest);
            return std::vector<fs::path>{dest};
        });
    Fail(L"Esta herramienta no admite este tipo de archivo");
}

}  // namespace

namespace {
// Plain-text sniffing for config files, logs, source code... (no NULs, few control bytes).
bool LooksLikeText(const fs::path& p) {
    HANDLE h = CreateFileW(p.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0,
                           nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    uint8_t buf[4096];
    DWORD n = 0;
    BOOL ok = ReadFile(h, buf, sizeof(buf), &n, nullptr);
    CloseHandle(h);
    if (!ok) return false;
    if (n == 0) return true;
    if (n >= 2 && ((buf[0] == 0xFF && buf[1] == 0xFE) || (buf[0] == 0xFE && buf[1] == 0xFF))) return true;
    int control = 0;
    for (DWORD i = 0; i < n; ++i) {
        if (buf[i] == 0) return false;
        if (buf[i] < 9 || (buf[i] > 13 && buf[i] < 32 && buf[i] != 27)) ++control;
    }
    return control * 100 < int(n);
}
}  // namespace

Kind KindOf(const fs::path& p) {
    std::error_code ec;
    if (fs::is_directory(p, ec)) return Kind::Folder;
    std::wstring ext = ExtOf(p);
    ImgFmt fmt;
    if (ext == L"pdf") return Kind::Pdf;
    if (ParseImgFmt(ext, fmt) || ext == L"ico" || ext == L"svg") return Kind::Image;
    if (IsDocumentExt(ext)) return Kind::Document;
    if (IsAudioExt(ext)) return Kind::Audio;
    if (IsVideoExt(ext)) return Kind::Video;
    if (IsArchiveExt(ext)) return Kind::Archive;
    if (fs::file_size(p, ec) < 64ull * 1024 * 1024 && !ec && LooksLikeText(p)) return Kind::Document;
    return Kind::Other;
}

const wchar_t* ModeName(Mode m) {
    switch (m) {
        case Mode::Convert: return L"CONVERTIR";
        case Mode::Tools: return L"HERRAMIENTAS";
        case Mode::Actions: return L"ACCIONES";
        case Mode::Launcher: return L"LANZADERA";
    }
    return L"";
}

static std::vector<ActionDef> LauncherActions() {
    std::vector<ActionDef> out = {{"cap:region", L"REGIÓN", kGlyphScreenshot, true},
                                  {"cap:window", L"VENTANA", kGlyphWindow, true},
                                  {"cap:screen", L"PANTALLA", kGlyphDesktop, true}};
    std::vector<LauncherApp> apps = LauncherApps();
    for (size_t i = 0; i < apps.size(); ++i)
        out.push_back({"app:" + std::to_string(i), apps[i].name, 0, true, apps[i].path});
    if (apps.size() < kMaxLauncherApps) out.push_back({"app:add", L"AÑADIR", kGlyphAdd, true});
    return out;
}

std::vector<ActionDef> ActionsFor(Mode mode, const std::vector<fs::path>& files) {
    if (mode == Mode::Launcher) return LauncherActions();
    if (files.empty()) return {};
    if (mode == Mode::Actions) {
        return {{"act:copy", L"COPIAR", kGlyphCopy, true},       {"act:path", L"RUTA", kGlyphLink, true},
                {"act:move", L"MOVER", kGlyphMove, true},        {"act:zip", L"ZIP", kGlyphZip, false},
                {"act:terminal", L"TERMINAL", kGlyphTerminal, true}, {"act:float", L"FLOTAR", kGlyphPin, true},
                {"act:share", L"COMPARTIR", kGlyphShare, true},  {"act:openwith", L"ABRIR CON", kGlyphOpenWith, true},
                {"tool:info", L"INFO", kGlyphInfo},               {"tool:hash", L"HASH", kGlyphFingerprint}};
    }
    Kind kind = KindOf(files[0]);
    std::wstring own = CanonicalExt(files[0]);
    if (mode == Mode::Convert) {
        switch (kind) {
            case Kind::Image: {
                std::vector<ActionDef> out;
                for (auto& a : Formats({L"jpg", L"png", L"webp", L"heic", L"tiff", L"avif", L"bmp", L"svg", L"pdf", L"docx"}, own)) {
                    ImgFmt fmt;
                    if (ParseImgFmt(Utf8ToWide(a.id.substr(3)), fmt) && !CanEncodeCached(fmt)) continue;
                    out.push_back(a);
                }
                return out;
            }
            case Kind::Pdf: return Formats({L"docx", L"jpg", L"png", L"txt", L"md"}, own);
            case Kind::Document: return Formats({L"docx", L"txt", L"md", L"html", L"pdf"}, own);
            case Kind::Audio: return Formats({L"mp3", L"m4a", L"wav", L"flac", L"ogg", L"opus", L"aiff", L"wma"}, own);
            case Kind::Video:
                return Formats({L"mp4", L"mov", L"mkv", L"webm", L"avi", L"gif", L"mp3", L"wmv"}, own);
            case Kind::Archive: return Formats({L"zip", L"tar", L"tgz"}, own);
            case Kind::Folder:
            default: return Formats({L"zip", L"tar", L"tgz"}, L"");
        }
    }
    std::vector<ActionDef> tools = [&]() -> std::vector<ActionDef> {
        switch (kind) {
            case Kind::Image:
                return {{"tool:compress", L"COMPRIMIR", kGlyphCompress}, {"tool:metadata", L"LIMPIAR", kGlyphErase},
                        {"tool:edit", L"EDITAR", kGlyphSliders, true},    {"tool:annotate", L"ANOTAR", kGlyphPen, true},
                        {"tool:background", L"FONDO", kGlyphPicture},     {"tool:nobg", L"SIN FONDO", kGlyphWand, true},
                        {"tool:crop", L"RECORTAR", kGlyphCrop, true},     {"tool:resize", L"ESCALAR", kGlyphResize, true},
                        {"tool:redact", L"CENSURAR", kGlyphHide},         {"tool:rotate", L"ROTAR", kGlyphRotate},
                        {"tool:ocr", L"TEXTO", kGlyphText}};
            case Kind::Pdf:
                return {{"tool:compress", L"COMPRIMIR", kGlyphCompress}, {"tool:split", L"DIVIDIR", kGlyphCut},
                        {"tool:merge", L"UNIR", kGlyphLink},              {"tool:rotate", L"ROTAR", kGlyphRotate},
                        {"tool:redact", L"CENSURAR", kGlyphHide},         {"tool:ocr", L"TEXTO", kGlyphText},
                        {"tool:metadata", L"LIMPIAR", kGlyphErase}};
            case Kind::Video:
                return {{"tool:compress", L"COMPRIMIR", kGlyphCompress}, {"tool:mute", L"SILENCIAR", kGlyphMute},
                        {"tool:audio", L"AUDIO", kGlyphMusic},            {"tool:gif", L"GIF", kGlyphSlideshow},
                        {"tool:rotate", L"ROTAR", kGlyphRotate},          {"tool:frame", L"FOTOGRAMA", kGlyphCamera},
                        {"tool:metadata", L"LIMPIAR", kGlyphErase}};
            case Kind::Audio:
                return {{"tool:compress", L"COMPRIMIR", kGlyphCompress}, {"tool:normalize", L"NORMALIZAR", kGlyphVolume},
                        {"tool:mono", L"MONO", kGlyphSpeaker},            {"tool:silence", L"SILENCIOS", kGlyphCut},
                        {"tool:metadata", L"LIMPIAR", kGlyphErase}};
            case Kind::Archive:
                return {{"tool:extract", L"EXTRAER", kGlyphFolderOpen}, {"tool:hash", L"HASH", kGlyphFingerprint}};
            case Kind::Document:
                return {{"tool:merge", L"UNIR", kGlyphLink},
                        {"tool:count", L"CONTAR", kGlyphCount},
                        {"tool:hash", L"HASH", kGlyphFingerprint},
                        {"tool:metadata", L"LIMPIAR", kGlyphErase}};
            case Kind::Folder:
                return {{"tool:count", L"TAMAÑO", kGlyphCount}, {"tool:hash", L"HASH", kGlyphFingerprint}};
            default: return {{"tool:hash", L"HASH", kGlyphFingerprint}, {"tool:metadata", L"LIMPIAR", kGlyphErase}};
        }
    }();
    // Any file or folder can be sent to another device.
    tools.push_back({"tool:send", L"ENVIAR", kGlyphQr, true});
    return tools;
}

JobResult RunAction(const std::string& id, const std::vector<fs::path>& files, const Progress& progress) {
    try {
        if (files.empty()) Fail(L"No hay archivos");
        if (id.starts_with("to:")) return Convert(Utf8ToWide(id.substr(3)), files, progress);
        if (id.starts_with("tool:")) return Tool(id.substr(5), files, progress);
        if (id == "act:zip") {
            JobResult r;
            r.outputs = {ArchiveFromItems(files, L"zip")};
            return r;
        }
        Fail(L"Acción desconocida");
    } catch (const AppError& e) {
        return JobResult{false, {}, e.message};
    } catch (const winrt::hresult_error& e) {
        return JobResult{false, {}, std::wstring(e.message())};
    } catch (const std::exception& e) {
        return JobResult{false, {}, Utf8ToWide(e.what())};
    }
}

JobResult RunUiAction(const std::string& id, const std::vector<fs::path>& files, HWND owner, POINT anchor) {
    JobResult r;
    try {
        if (id == "act:copy") {
            if (!CopyFilesToClipboard(owner, files)) Fail(L"No se pudo acceder al portapapeles");
            r.message = files.size() == 1 ? L"Copiado" : std::to_wstring(files.size()) + L" archivos copiados";
        } else if (id == "act:path") {
            std::wstring text;
            for (const auto& f : files) text += (text.empty() ? L"" : L"\r\n") + f.wstring();
            if (!CopyTextToClipboard(owner, text)) Fail(L"No se pudo acceder al portapapeles");
            r.message = L"Ruta copiada";
        } else if (id == "act:move") {
            fs::path dest;
            if (!PickFolder(owner, dest)) {
                r.hide = true;
                return r;
            }
            if (!MoveFilesTo(owner, files, dest)) Fail(L"No se pudo mover");
            for (const auto& f : files) r.outputs.push_back(dest / f.filename());
            r.message = L"Movido a " + dest.filename().wstring();
        } else if (id == "act:terminal") {
            std::error_code ec;
            OpenTerminalAt(fs::is_directory(files[0], ec) ? files[0] : files[0].parent_path());
            r.hide = true;
        } else if (id == "act:float") {
            OpenFloatShelf(files, anchor);
            r.hide = true;
        } else if (id == "act:share") {
            if (!ShareFiles(owner, files)) Fail(L"No se pudo abrir el panel Compartir");
            r.message = L"Compartiendo…";
        } else if (id == "act:openwith") {
            OpenWithDialog(owner, files[0]);
            r.hide = true;
        } else if (id.starts_with("cap:")) {
            capture::Start(id == "cap:window"   ? capture::Target::Window
                           : id == "cap:screen" ? capture::Target::Screen
                                                : capture::Target::Region,
                           anchor);
            r.hide = true;
        } else if (id == "app:add") {
            AddLauncherApp(owner);
            r.hide = true;
        } else if (id.starts_with("app:")) {
            RunLauncherApp(std::stoul(id.substr(4)));
            r.hide = true;
        } else if (id == "tool:send") {
            OpenSendPanel(files, anchor);
            r.hide = true;
        } else if (id == "tool:nobg") {
            for (const auto& f : files) OpenEditor(f, EditorMode::Background);
            r.hide = true;
        } else if (id == "tool:edit" || id == "tool:annotate" || id == "tool:crop" || id == "tool:resize") {
            EditorMode mode = id == "tool:edit" ? EditorMode::Adjust
                              : id == "tool:annotate" ? EditorMode::Annotate
                              : id == "tool:resize"   ? EditorMode::Resize
                                                      : EditorMode::Crop;
            for (const auto& f : files) OpenEditor(f, mode);
            r.hide = true;
        } else {
            Fail(L"Acción desconocida");
        }
    } catch (const AppError& e) {
        return JobResult{false, {}, e.message};
    } catch (const std::exception& e) {
        return JobResult{false, {}, Utf8ToWide(e.what())};
    }
    return r;
}
