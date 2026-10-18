#include "image.h"

#include "gfx.h"

#include <propvarutil.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <cmath>
#include <cstring>

#ifdef FT_HAVE_WEBP
#include <webp/decode.h>
#include <webp/encode.h>
#endif
#ifdef FT_HAVE_AVIF
#include <avif/avif.h>
#endif

using Microsoft::WRL::ComPtr;

namespace {

IWICImagingFactory* Wic() {
    thread_local ComPtr<IWICImagingFactory> factory;
    if (!factory) {
        CheckHr(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory)),
                L"No se pudo iniciar Windows Imaging Component");
    }
    return factory.Get();
}

WICBitmapTransformOptions OrientationTransform(UINT16 o) {
    switch (o) {
        case 2: return WICBitmapTransformFlipHorizontal;
        case 3: return WICBitmapTransformRotate180;
        case 4: return WICBitmapTransformFlipVertical;
        case 5: return WICBitmapTransformOptions(WICBitmapTransformRotate270 | WICBitmapTransformFlipHorizontal);
        case 6: return WICBitmapTransformRotate90;
        case 7: return WICBitmapTransformOptions(WICBitmapTransformRotate90 | WICBitmapTransformFlipHorizontal);
        case 8: return WICBitmapTransformRotate270;
        default: return WICBitmapTransformRotate0;
    }
}

UINT16 ReadOrientation(IWICBitmapFrameDecode* frame) {
    ComPtr<IWICMetadataQueryReader> reader;
    if (FAILED(frame->GetMetadataQueryReader(&reader))) return 1;
    UINT16 result = 1;
    for (const wchar_t* q : {L"System.Photo.Orientation", L"/app1/ifd/{ushort=274}", L"/ifd/{ushort=274}"}) {
        PROPVARIANT v;
        PropVariantInit(&v);
        if (SUCCEEDED(reader->GetMetadataByName(q, &v))) {
            if (v.vt == VT_UI2) result = v.uiVal;
            PropVariantClear(&v);
            if (result >= 1 && result <= 8) break;
        }
    }
    return (result >= 1 && result <= 8) ? result : 1;
}

Image DecodeWith(IWICBitmapDecoder* decoder) {
    ComPtr<IWICBitmapFrameDecode> frame;
    CheckHr(decoder->GetFrame(0, &frame), L"Imagen sin fotogramas");
    ComPtr<IWICBitmapSource> source = frame;
    UINT16 orientation = ReadOrientation(frame.Get());
    if (orientation != 1) {
        ComPtr<IWICBitmapFlipRotator> rot;
        if (SUCCEEDED(Wic()->CreateBitmapFlipRotator(&rot)) &&
            SUCCEEDED(rot->Initialize(frame.Get(), OrientationTransform(orientation))))
            source = rot;
    }
    ComPtr<IWICFormatConverter> conv;
    CheckHr(Wic()->CreateFormatConverter(&conv), L"WIC");
    CheckHr(conv->Initialize(source.Get(), GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr, 0,
                             WICBitmapPaletteTypeMedianCut),
            L"Formato de píxel no soportado");
    Image img;
    CheckHr(conv->GetSize(&img.w, &img.h), L"WIC");
    img.px.resize(size_t(img.w) * img.h * 4);
    CheckHr(conv->CopyPixels(nullptr, img.w * 4, (UINT)img.px.size(), img.px.data()), L"No se pudo decodificar");
    return img;
}

bool IsWebp(const uint8_t* d, size_t n) { return n > 12 && !memcmp(d, "RIFF", 4) && !memcmp(d + 8, "WEBP", 4); }
bool IsAvif(const uint8_t* d, size_t n) {
    return n > 12 && !memcmp(d + 4, "ftyp", 4) && (!memcmp(d + 8, "avif", 4) || !memcmp(d + 8, "avis", 4));
}
bool IsHeif(const uint8_t* d, size_t n) {
    return n > 12 && !memcmp(d + 4, "ftyp", 4) &&
           (!memcmp(d + 8, "heic", 4) || !memcmp(d + 8, "heix", 4) || !memcmp(d + 8, "mif1", 4) ||
            !memcmp(d + 8, "msf1", 4) || !memcmp(d + 8, "hevc", 4));
}

#ifdef FT_HAVE_WEBP
bool DecodeWebp(const uint8_t* d, size_t n, Image& out) {
    int w = 0, h = 0;
    if (!WebPGetInfo(d, n, &w, &h)) return false;
    out = Image(w, h);
    return WebPDecodeBGRAInto(d, n, out.px.data(), out.px.size(), w * 4) != nullptr;
}
#endif

#ifdef FT_HAVE_AVIF
bool DecodeAvif(const uint8_t* d, size_t n, Image& out) {
    avifDecoder* dec = avifDecoderCreate();
    if (!dec) return false;
    bool ok = false;
    if (avifDecoderSetIOMemory(dec, d, n) == AVIF_RESULT_OK && avifDecoderParse(dec) == AVIF_RESULT_OK &&
        avifDecoderNextImage(dec) == AVIF_RESULT_OK) {
        avifRGBImage rgb;
        avifRGBImageSetDefaults(&rgb, dec->image);
        rgb.format = AVIF_RGB_FORMAT_BGRA;
        rgb.depth = 8;
        out = Image(dec->image->width, dec->image->height);
        rgb.pixels = out.px.data();
        rgb.rowBytes = out.w * 4;
        ok = avifImageYUVToRGB(dec->image, &rgb) == AVIF_RESULT_OK;
    }
    avifDecoderDestroy(dec);
    return ok;
}
#endif

Image DecodeFallback(const uint8_t* d, size_t n, HRESULT wicError) {
    Image img;
#ifdef FT_HAVE_WEBP
    if (IsWebp(d, n) && DecodeWebp(d, n, img)) return img;
#endif
#ifdef FT_HAVE_AVIF
    if (IsAvif(d, n) && DecodeAvif(d, n, img)) return img;
#endif
    if (IsHeif(d, n))
        Fail(L"No se puede abrir HEIC: instala \"Extensiones de imagen HEIF\" y \"Extensiones de vídeo HEVC\" desde "
             L"Microsoft Store");
    if (IsAvif(d, n)) Fail(L"No se puede abrir AVIF: instala \"Extensión de vídeo AV1\" desde Microsoft Store");
    CheckHr(wicError, L"Formato de imagen no reconocido");
    Fail(L"Formato de imagen no reconocido");
}

GUID ContainerFor(ImgFmt f) {
    switch (f) {
        case ImgFmt::Jpg: return GUID_ContainerFormatJpeg;
        case ImgFmt::Png: return GUID_ContainerFormatPng;
        case ImgFmt::Tiff: return GUID_ContainerFormatTiff;
        case ImgFmt::Bmp: return GUID_ContainerFormatBmp;
        case ImgFmt::Gif: return GUID_ContainerFormatGif;
        case ImgFmt::Heic: return GUID_ContainerFormatHeif;
        case ImgFmt::Webp: return GUID_ContainerFormatWebp;
        case ImgFmt::Avif: return GUID_NULL;  // Windows ships no AVIF encoder
    }
    return GUID_ContainerFormatPng;
}

bool HasPartialAlpha(const Image& img) {
    for (size_t i = 3; i < img.px.size(); i += 4)
        if (img.px[i] != 0 && img.px[i] != 255) return true;
    return false;
}

std::vector<uint8_t> EncodeWic(const Image& src, ImgFmt fmt, const EncodeOpts& opts) {
    if (fmt == ImgFmt::Webp || fmt == ImgFmt::Avif)
        Fail(L"Esta compilación no incluye el codificador " + ToUpper(ImgFmtExt(fmt)));
    IWICImagingFactory* wic = Wic();
    bool alpha = src.HasAlpha();
    const Image* img = &src;
    Image flat;
    if (alpha && (fmt == ImgFmt::Jpg || fmt == ImgFmt::Bmp || fmt == ImgFmt::Heic)) {
        flat = Flatten(src, 255, 255, 255);
        img = &flat;
        alpha = false;
    }
    bool quantize = fmt == ImgFmt::Gif || (opts.quantize && fmt == ImgFmt::Png && !HasPartialAlpha(*img));

    ComPtr<IStream> stream;
    CheckHr(CreateStreamOnHGlobal(nullptr, TRUE, &stream), L"Memoria insuficiente");
    ComPtr<IWICBitmapEncoder> enc;
    HRESULT hr = wic->CreateEncoder(ContainerFor(fmt), nullptr, &enc);
    if (FAILED(hr)) {
        if (fmt == ImgFmt::Heic)
            Fail(L"Codificador HEIC no disponible: instala \"Extensiones de imagen HEIF\" y \"Extensiones de vídeo "
                 L"HEVC\" desde Microsoft Store");
        Fail(L"Codificador " + std::wstring(ImgFmtExt(fmt)) + L" no disponible en este equipo");
    }
    CheckHr(enc->Initialize(stream.Get(), WICBitmapEncoderNoCache), L"Codificador");
    ComPtr<IWICBitmapFrameEncode> frame;
    ComPtr<IPropertyBag2> props;
    CheckHr(enc->CreateNewFrame(&frame, &props), L"Codificador");
    if (props) {
        if (fmt == ImgFmt::Jpg || fmt == ImgFmt::Heic) {
            PROPBAG2 opt{};
            opt.pstrName = const_cast<LPOLESTR>(L"ImageQuality");
            VARIANT v;
            VariantInit(&v);
            v.vt = VT_R4;
            v.fltVal = std::clamp(opts.quality, 1, 100) / 100.0f;
            props->Write(1, &opt, &v);
        } else if (fmt == ImgFmt::Tiff) {
            PROPBAG2 opt{};
            opt.pstrName = const_cast<LPOLESTR>(L"TiffCompressionMethod");
            VARIANT v;
            VariantInit(&v);
            v.vt = VT_UI1;
            v.bVal = WICTiffCompressionZIP;
            props->Write(1, &opt, &v);
        }
    }
    CheckHr(frame->Initialize(props.Get()), L"Codificador");
    CheckHr(frame->SetSize(img->w, img->h), L"Codificador");

    ComPtr<IWICBitmap> bmp;
    CheckHr(wic->CreateBitmapFromMemory(img->w, img->h, GUID_WICPixelFormat32bppBGRA, img->w * 4,
                                        (UINT)img->px.size(), const_cast<BYTE*>(img->px.data()), &bmp),
            L"Memoria insuficiente");
    ComPtr<IWICBitmapSource> source = bmp;
    WICPixelFormatGUID pf;
    if (quantize) {
        ComPtr<IWICPalette> palette;
        CheckHr(wic->CreatePalette(&palette), L"Paleta");
        CheckHr(palette->InitializeFromBitmap(bmp.Get(), 256, alpha), L"Paleta");
        ComPtr<IWICFormatConverter> conv;
        CheckHr(wic->CreateFormatConverter(&conv), L"WIC");
        CheckHr(conv->Initialize(bmp.Get(), GUID_WICPixelFormat8bppIndexed, WICBitmapDitherTypeErrorDiffusion,
                                 palette.Get(), alpha ? 50.0 : 0.0, WICBitmapPaletteTypeCustom),
                L"Paleta");
        source = conv;
        pf = GUID_WICPixelFormat8bppIndexed;
        CheckHr(frame->SetPixelFormat(&pf), L"Codificador");
        frame->SetPalette(palette.Get());
    } else {
        pf = alpha ? GUID_WICPixelFormat32bppBGRA : GUID_WICPixelFormat24bppBGR;
        CheckHr(frame->SetPixelFormat(&pf), L"Codificador");
        if (pf != GUID_WICPixelFormat32bppBGRA) {
            ComPtr<IWICFormatConverter> conv;
            CheckHr(wic->CreateFormatConverter(&conv), L"WIC");
            CheckHr(conv->Initialize(bmp.Get(), pf, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeMedianCut),
                    L"Conversión de formato de píxel");
            source = conv;
        }
    }
    CheckHr(frame->WriteSource(source.Get(), nullptr), L"No se pudo codificar la imagen");
    CheckHr(frame->Commit(), L"Codificador");
    CheckHr(enc->Commit(), L"Codificador");

    HGLOBAL hg = nullptr;
    CheckHr(GetHGlobalFromStream(stream.Get(), &hg), L"Memoria");
    STATSTG st{};
    stream->Stat(&st, STATFLAG_NONAME);
    std::vector<uint8_t> out((size_t)st.cbSize.QuadPart);
    void* p = GlobalLock(hg);
    memcpy(out.data(), p, out.size());
    GlobalUnlock(hg);
    return out;
}

#ifdef FT_HAVE_AVIF
std::vector<uint8_t> EncodeAvif(const Image& img, int quality) {
    avifImage* image = avifImageCreate(img.w, img.h, 8, AVIF_PIXEL_FORMAT_YUV420);
    avifRGBImage rgb;
    avifRGBImageSetDefaults(&rgb, image);
    rgb.format = AVIF_RGB_FORMAT_BGRA;
    rgb.depth = 8;
    rgb.pixels = const_cast<uint8_t*>(img.px.data());
    rgb.rowBytes = img.w * 4;
    if (!img.HasAlpha()) rgb.ignoreAlpha = AVIF_TRUE;
    avifRWData output = AVIF_DATA_EMPTY;
    avifEncoder* encoder = nullptr;
    std::vector<uint8_t> out;
    if (avifImageRGBToYUV(image, &rgb) == AVIF_RESULT_OK && (encoder = avifEncoderCreate())) {
        encoder->quality = std::clamp(quality - 15, 1, 100);  // AVIF looks good at lower nominal quality
        encoder->qualityAlpha = 90;
        encoder->speed = 7;
        encoder->maxThreads = 8;
        if (avifEncoderWrite(encoder, image, &output) == AVIF_RESULT_OK) out.assign(output.data, output.data + output.size);
    }
    if (encoder) avifEncoderDestroy(encoder);
    avifRWDataFree(&output);
    avifImageDestroy(image);
    if (out.empty()) Fail(L"No se pudo codificar AVIF");
    return out;
}
#endif

}  // namespace

bool Image::HasAlpha() const {
    for (size_t i = 3; i < px.size(); i += 4)
        if (px[i] != 255) return true;
    return false;
}

bool ParseImgFmt(const std::wstring& ext, ImgFmt& out) {
    static const struct {
        const wchar_t* e;
        ImgFmt f;
    } map[] = {{L"jpg", ImgFmt::Jpg},  {L"jpeg", ImgFmt::Jpg},  {L"jfif", ImgFmt::Jpg}, {L"png", ImgFmt::Png},
               {L"webp", ImgFmt::Webp}, {L"heic", ImgFmt::Heic}, {L"heif", ImgFmt::Heic}, {L"tif", ImgFmt::Tiff},
               {L"tiff", ImgFmt::Tiff}, {L"avif", ImgFmt::Avif}, {L"bmp", ImgFmt::Bmp},  {L"gif", ImgFmt::Gif}};
    for (auto& m : map)
        if (ext == m.e) {
            out = m.f;
            return true;
        }
    return false;
}

const wchar_t* ImgFmtExt(ImgFmt f) {
    switch (f) {
        case ImgFmt::Jpg: return L"jpg";
        case ImgFmt::Png: return L"png";
        case ImgFmt::Webp: return L"webp";
        case ImgFmt::Heic: return L"heic";
        case ImgFmt::Tiff: return L"tiff";
        case ImgFmt::Avif: return L"avif";
        case ImgFmt::Bmp: return L"bmp";
        case ImgFmt::Gif: return L"gif";
    }
    return L"png";
}

bool CanEncode(ImgFmt f) {
#ifdef FT_HAVE_WEBP
    if (f == ImgFmt::Webp) return true;
#endif
#ifdef FT_HAVE_AVIF
    if (f == ImgFmt::Avif) return true;
#endif
    if (f == ImgFmt::Webp || f == ImgFmt::Avif) return false;
    ComPtr<IWICBitmapEncoder> enc;
    return SUCCEEDED(Wic()->CreateEncoder(ContainerFor(f), nullptr, &enc));
}

Image LoadImageMemory(const uint8_t* data, size_t size) {
    ComPtr<IWICStream> stream;
    ComPtr<IWICBitmapDecoder> decoder;
    HRESULT hr = Wic()->CreateStream(&stream);
    if (SUCCEEDED(hr)) hr = stream->InitializeFromMemory(const_cast<BYTE*>(data), (DWORD)size);
    if (SUCCEEDED(hr))
        hr = Wic()->CreateDecoderFromStream(stream.Get(), nullptr, WICDecodeMetadataCacheOnDemand, &decoder);
    if (SUCCEEDED(hr)) {
        try {
            return DecodeWith(decoder.Get());
        } catch (const AppError&) {
            hr = E_FAIL;
        }
    }
    return DecodeFallback(data, size, hr);
}

Image LoadImageFile(const fs::path& path) {
    std::vector<uint8_t> bytes = ReadFileBytes(path);
    return LoadImageMemory(bytes.data(), bytes.size());
}

std::vector<uint8_t> EncodeImage(const Image& img, ImgFmt fmt, const EncodeOpts& opts) {
    if (img.w == 0 || img.h == 0) Fail(L"Imagen vacía");
#ifdef FT_HAVE_WEBP
    if (fmt == ImgFmt::Webp) {
        uint8_t* out = nullptr;
        size_t n = WebPEncodeBGRA(img.px.data(), img.w, img.h, img.w * 4, (float)opts.quality, &out);
        if (!n) Fail(L"No se pudo codificar WebP");
        std::vector<uint8_t> v(out, out + n);
        WebPFree(out);
        return v;
    }
#endif
#ifdef FT_HAVE_AVIF
    if (fmt == ImgFmt::Avif) return EncodeAvif(img, opts.quality);
#endif
    return EncodeWic(img, fmt, opts);
}

void SaveImage(const Image& img, const fs::path& path, ImgFmt fmt, const EncodeOpts& opts) {
    WriteFileBytes(path, EncodeImage(img, fmt, opts));
}

Image Flatten(const Image& img, uint8_t r, uint8_t g, uint8_t b) {
    Image out = img;
    for (size_t i = 0; i < out.px.size(); i += 4) {
        uint32_t a = out.px[i + 3];
        if (a == 255) continue;
        out.px[i + 0] = uint8_t((out.px[i + 0] * a + b * (255 - a)) / 255);
        out.px[i + 1] = uint8_t((out.px[i + 1] * a + g * (255 - a)) / 255);
        out.px[i + 2] = uint8_t((out.px[i + 2] * a + r * (255 - a)) / 255);
        out.px[i + 3] = 255;
    }
    return out;
}

Image Resize(const Image& img, uint32_t w, uint32_t h) {
    w = std::max<uint32_t>(1, w);
    h = std::max<uint32_t>(1, h);
    // Scale premultiplied pixels to avoid dark fringes around transparent edges.
    std::vector<uint8_t> pm = img.px;
    for (size_t i = 0; i < pm.size(); i += 4) {
        uint32_t a = pm[i + 3];
        for (int c = 0; c < 3; ++c) pm[i + c] = uint8_t(pm[i + c] * a / 255);
    }
    ComPtr<IWICBitmap> bmp;
    CheckHr(Wic()->CreateBitmapFromMemory(img.w, img.h, GUID_WICPixelFormat32bppPBGRA, img.w * 4, (UINT)pm.size(),
                                          pm.data(), &bmp),
            L"Memoria insuficiente");
    ComPtr<IWICBitmapScaler> scaler;
    CheckHr(Wic()->CreateBitmapScaler(&scaler), L"WIC");
    CheckHr(scaler->Initialize(bmp.Get(), w, h, WICBitmapInterpolationModeHighQualityCubic), L"Escalado");
    Image out(w, h);
    CheckHr(scaler->CopyPixels(nullptr, w * 4, (UINT)out.px.size(), out.px.data()), L"Escalado");
    for (size_t i = 0; i < out.px.size(); i += 4) {
        uint32_t a = out.px[i + 3];
        if (a == 0 || a == 255) continue;
        for (int c = 0; c < 3; ++c) out.px[i + c] = uint8_t(std::min<uint32_t>(255, out.px[i + c] * 255 / a));
    }
    return out;
}

Image FitWithin(const Image& img, uint32_t maxDim) {
    if (img.w <= maxDim && img.h <= maxDim) return img;
    double s = double(maxDim) / std::max(img.w, img.h);
    return Resize(img, uint32_t(std::lround(img.w * s)), uint32_t(std::lround(img.h * s)));
}

Image Rotate90(const Image& img) {
    Image out(img.h, img.w);
    for (uint32_t y = 0; y < img.h; ++y) {
        const uint32_t* src = reinterpret_cast<const uint32_t*>(img.Row(y));
        for (uint32_t x = 0; x < img.w; ++x) {
            // (x, y) -> (h-1-y, x)
            reinterpret_cast<uint32_t*>(out.Row(x))[img.h - 1 - y] = src[x];
        }
    }
    return out;
}

bool AutoCropRect(const Image& img, uint32_t& x, uint32_t& y, uint32_t& w, uint32_t& h) {
    if (img.w < 3 || img.h < 3) return false;
    const uint8_t* ref = img.Row(0);
    auto same = [&](const uint8_t* p) {
        if (ref[3] < 16 && p[3] < 16) return true;  // both transparent
        int d = std::abs(p[0] - ref[0]) + std::abs(p[1] - ref[1]) + std::abs(p[2] - ref[2]) + std::abs(p[3] - ref[3]);
        return d <= 40;
    };
    auto rowUniform = [&](uint32_t y) {
        const uint8_t* r = img.Row(y);
        for (uint32_t x = 0; x < img.w; ++x)
            if (!same(r + x * 4)) return false;
        return true;
    };
    auto colUniform = [&](uint32_t x, uint32_t y0, uint32_t y1) {
        for (uint32_t y = y0; y < y1; ++y)
            if (!same(img.Row(y) + x * 4)) return false;
        return true;
    };
    uint32_t top = 0, bottom = img.h, left = 0, right = img.w;
    while (top < bottom && rowUniform(top)) ++top;
    while (bottom > top && rowUniform(bottom - 1)) --bottom;
    if (top >= bottom) return false;
    while (left < right && colUniform(left, top, bottom)) ++left;
    while (right > left && colUniform(right - 1, top, bottom)) --right;
    if (top == 0 && left == 0 && bottom == img.h && right == img.w) return false;
    // Keep a small margin so text isn't glued to the edge.
    uint32_t m = std::max<uint32_t>(2, std::min(img.w, img.h) / 100);
    top = top > m ? top - m : 0;
    left = left > m ? left - m : 0;
    bottom = std::min(img.h, bottom + m);
    right = std::min(img.w, right + m);
    x = left;
    y = top;
    w = right - left;
    h = bottom - top;
    return true;
}

bool AutoCrop(const Image& img, Image& out) {
    uint32_t x, y, w, h;
    if (!AutoCropRect(img, x, y, w, h)) return false;
    out = Image(w, h);
    for (uint32_t r = 0; r < h; ++r) memcpy(out.Row(r), img.Row(y + r) + x * 4, size_t(w) * 4);
    return true;
}

Image AddBackdrop(const Image& img) {
    using namespace Gdiplus;
    int pad = std::max(16, int(std::max(img.w, img.h) * 0.08));
    int W = img.w + pad * 2, H = img.h + pad * 2;
    Bitmap canvas(W, H, PixelFormat32bppARGB);
    {
        Graphics g(&canvas);
        g.SetSmoothingMode(SmoothingModeAntiAlias);
        g.SetPixelOffsetMode(PixelOffsetModeHalf);
        LinearGradientBrush bg(Point(0, 0), Point(W, H), Color(255, 255, 170, 64), Color(255, 240, 84, 110));
        g.FillRectangle(&bg, 0, 0, W, H);

        float radius = std::max(6.0f, std::min(img.w, img.h) * 0.025f);
        RectF card((REAL)pad, (REAL)pad, (REAL)img.w, (REAL)img.h);
        // Soft shadow made of stacked translucent rounded rects.
        float spread = pad * 0.45f;
        for (int i = 12; i >= 1; --i) {
            float grow = spread * i / 12.0f;
            RectF r(card.X - grow, card.Y - grow + spread * 0.35f, card.Width + grow * 2, card.Height + grow * 2);
            GraphicsPath p;
            gfx::AddRoundRect(p, r, radius + grow);
            SolidBrush b(Color(BYTE(10 - i * 0.6), 60, 20, 0));
            g.FillPath(&b, &p);
        }
        Bitmap src(img.w, img.h, img.w * 4, PixelFormat32bppARGB, const_cast<BYTE*>(img.px.data()));
        // Paint white under the card so transparent screenshots stay readable.
        GraphicsPath cardPath;
        gfx::AddRoundRect(cardPath, card, radius);
        SolidBrush white(Color(255, 255, 255, 255));
        g.FillPath(&white, &cardPath);
        TextureBrush tex(&src, WrapModeClamp);
        tex.TranslateTransform((REAL)pad, (REAL)pad);
        g.FillPath(&tex, &cardPath);
    }
    ::Image out(W, H);
    BitmapData data;
    Rect rect(0, 0, W, H);
    canvas.LockBits(&rect, ImageLockModeRead, PixelFormat32bppARGB, &data);
    for (int y = 0; y < H; ++y)
        memcpy(out.Row(y), static_cast<uint8_t*>(data.Scan0) + size_t(y) * data.Stride, size_t(W) * 4);
    canvas.UnlockBits(&data);
    return out;
}

void FillRect(Image& img, int x, int y, int w, int h, uint32_t bgra) {
    int x0 = std::clamp(x, 0, (int)img.w), x1 = std::clamp(x + w, 0, (int)img.w);
    int y0 = std::clamp(y, 0, (int)img.h), y1 = std::clamp(y + h, 0, (int)img.h);
    for (int yy = y0; yy < y1; ++yy) {
        uint32_t* row = reinterpret_cast<uint32_t*>(img.Row(yy));
        for (int xx = x0; xx < x1; ++xx) row[xx] = bgra;
    }
}

bool JpegPassthroughInfo(const fs::path& path, uint32_t& w, uint32_t& h, bool& gray) {
    ComPtr<IWICBitmapDecoder> decoder;
    if (FAILED(Wic()->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand,
                                                &decoder)))
        return false;
    GUID container;
    if (FAILED(decoder->GetContainerFormat(&container)) || container != GUID_ContainerFormatJpeg) return false;
    ComPtr<IWICBitmapFrameDecode> frame;
    if (FAILED(decoder->GetFrame(0, &frame))) return false;
    if (ReadOrientation(frame.Get()) != 1) return false;
    WICPixelFormatGUID pf;
    frame->GetPixelFormat(&pf);
    if (pf != GUID_WICPixelFormat24bppBGR && pf != GUID_WICPixelFormat8bppGray) return false;
    gray = pf == GUID_WICPixelFormat8bppGray;
    return SUCCEEDED(frame->GetSize(&w, &h));
}

bool StripMetadataLossless(const fs::path& src, const fs::path& dst) {
    std::vector<uint8_t> d = ReadFileBytes(src);
    std::string out;
    if (d.size() > 4 && d[0] == 0xFF && d[1] == 0xD8) {
        // JPEG: bail out when an orientation flag exists, removing it would rotate the photo.
        {
            ComPtr<IWICBitmapDecoder> decoder;
            ComPtr<IWICBitmapFrameDecode> frame;
            if (SUCCEEDED(Wic()->CreateDecoderFromFilename(src.c_str(), nullptr, GENERIC_READ,
                                                           WICDecodeMetadataCacheOnDemand, &decoder)) &&
                SUCCEEDED(decoder->GetFrame(0, &frame)) && ReadOrientation(frame.Get()) != 1)
                return false;
        }
        out.append("\xFF\xD8", 2);
        size_t i = 2;
        while (i + 4 <= d.size()) {
            if (d[i] != 0xFF) return false;
            uint8_t marker = d[i + 1];
            if (marker == 0xD9) break;
            if (marker == 0xDA) {  // start of scan: copy the rest verbatim
                out.append(reinterpret_cast<const char*>(d.data() + i), d.size() - i);
                i = d.size();
                break;
            }
            size_t len = (size_t(d[i + 2]) << 8) | d[i + 3];
            if (i + 2 + len > d.size()) return false;
            bool isIcc = marker == 0xE2 && len > 14 && !memcmp(d.data() + i + 4, "ICC_PROFILE", 11);
            bool drop = (marker >= 0xE1 && marker <= 0xEF && !isIcc) || marker == 0xFE;
            if (!drop) out.append(reinterpret_cast<const char*>(d.data() + i), len + 2);
            i += 2 + len;
        }
    } else if (d.size() > 8 && !memcmp(d.data(), "\x89PNG\r\n\x1a\n", 8)) {
        out.append(reinterpret_cast<const char*>(d.data()), 8);
        size_t i = 8;
        while (i + 12 <= d.size()) {
            size_t len = (size_t(d[i]) << 24) | (size_t(d[i + 1]) << 16) | (size_t(d[i + 2]) << 8) | d[i + 3];
            if (i + 12 + len > d.size()) return false;
            std::string type(reinterpret_cast<const char*>(d.data() + i + 4), 4);
            bool drop = type == "tEXt" || type == "zTXt" || type == "iTXt" || type == "eXIf" || type == "tIME";
            if (!drop) out.append(reinterpret_cast<const char*>(d.data() + i), len + 12);
            i += len + 12;
            if (type == "IEND") break;
        }
    } else {
        return false;
    }
    WriteFileBytes(dst, out);
    return true;
}
