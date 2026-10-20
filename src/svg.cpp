// ID2D1DeviceContext5 (Direct2D's SVG renderer) is only declared for Windows 10 1703+, while the
// project targets plain Windows 10, so this file alone raises the SDK version it compiles against.
#define NTDDI_VERSION 0x0A000003  // NTDDI_WIN10_RS2
#include "svg.h"

#include "svgtext.h"

#include <d2d1_3.h>
#include <shlwapi.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstring>

using Microsoft::WRL::ComPtr;

namespace {

ID2D1Factory1* D2d() {
    thread_local ComPtr<ID2D1Factory1> factory;
    if (!factory)
        CheckHr(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, factory.GetAddressOf()),
                L"No se pudo iniciar Direct2D");
    return factory.Get();
}

IWICImagingFactory* Wic() {
    thread_local ComPtr<IWICImagingFactory> factory;
    if (!factory)
        CheckHr(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory)),
                L"No se pudo iniciar Windows Imaging Component");
    return factory.Get();
}

// width/height in plain numbers win; percentages or units Direct2D can't parse ("10cm") fall back
// to the viewBox, and a file with neither gets the same default size browsers use.
D2D1_SIZE_F IntrinsicSize(ID2D1SvgElement* root) {
    D2D1_SIZE_F size = D2D1::SizeF(300, 150);
    D2D1_SVG_VIEWBOX vb{};
    bool hasViewBox = SUCCEEDED(root->GetAttributeValue(L"viewBox", D2D1_SVG_ATTRIBUTE_POD_TYPE_VIEWBOX, &vb,
                                                         sizeof(vb))) &&
                      vb.width > 0 && vb.height > 0;
    if (hasViewBox) size = D2D1::SizeF(vb.width, vb.height);
    D2D1_SVG_LENGTH w{}, h{};
    bool hasW = SUCCEEDED(root->GetAttributeValue(L"width", D2D1_SVG_ATTRIBUTE_POD_TYPE_LENGTH, &w, sizeof(w))) &&
                w.units == D2D1_SVG_LENGTH_UNITS_NUMBER && w.value > 0;
    bool hasH = SUCCEEDED(root->GetAttributeValue(L"height", D2D1_SVG_ATTRIBUTE_POD_TYPE_LENGTH, &h, sizeof(h))) &&
                h.units == D2D1_SVG_LENGTH_UNITS_NUMBER && h.value > 0;
    if (hasW && hasH) return D2D1::SizeF(w.value, h.value);
    // Only one side given: keep the viewBox proportions.
    if (hasW) return D2D1::SizeF(w.value, w.value * size.height / size.width);
    if (hasH) return D2D1::SizeF(h.value * size.width / size.height, h.value);
    return size;
}

// Maps the root's user units onto its viewport the way Direct2D does when drawing the document
// (viewBox plus preserveAspectRatio), so text drawn separately lands on the shapes.
D2D1::Matrix3x2F ViewBoxTransform(ID2D1SvgElement* root, D2D1_SIZE_F viewport) {
    D2D1_SVG_VIEWBOX vb{};
    if (FAILED(root->GetAttributeValue(L"viewBox", D2D1_SVG_ATTRIBUTE_POD_TYPE_VIEWBOX, &vb, sizeof(vb))) ||
        vb.width <= 0 || vb.height <= 0)
        return D2D1::Matrix3x2F::Identity();
    D2D1_SVG_PRESERVE_ASPECT_RATIO par{FALSE, D2D1_SVG_ASPECT_ALIGN_X_MID_Y_MID, D2D1_SVG_ASPECT_SCALING_MEET};
    root->GetAttributeValue(L"preserveAspectRatio", D2D1_SVG_ATTRIBUTE_POD_TYPE_PRESERVE_ASPECT_RATIO, &par,
                            sizeof(par));
    float sx = viewport.width / vb.width, sy = viewport.height / vb.height;
    if (par.align == D2D1_SVG_ASPECT_ALIGN_NONE)
        return D2D1::Matrix3x2F::Translation(-vb.x, -vb.y) * D2D1::Matrix3x2F::Scale(sx, sy);
    float s = par.meetOrSlice == D2D1_SVG_ASPECT_SCALING_SLICE ? std::max(sx, sy) : std::min(sx, sy);
    // The align values run xMinYMin, xMidYMin, xMaxYMin, xMinYMid, ... after NONE.
    int align = int(par.align) - 1;
    float fx = (align % 3) / 2.f, fy = (align / 3) / 2.f;
    return D2D1::Matrix3x2F::Translation(-vb.x, -vb.y) * D2D1::Matrix3x2F::Scale(s, s) *
           D2D1::Matrix3x2F::Translation((viewport.width - vb.width * s) * fx, (viewport.height - vb.height * s) * fy);
}

float RenderScale(D2D1_SIZE_F size) {
    float longSide = std::max(size.width, size.height);
    float target = std::clamp(longSide, float(kSvgMinLongSide), float(kSvgMaxLongSide));
    return target / longSide;
}

}  // namespace

bool IsSvgData(const uint8_t* data, size_t size) {
    // The root may follow an XML declaration, comments or a DOCTYPE, so look a little way in.
    std::string_view head(reinterpret_cast<const char*>(data), std::min<size_t>(size, 4096));
    return head.find("<svg") != std::string_view::npos;
}

Image RasterizeSvg(const uint8_t* data, size_t size) {
    ComPtr<IStream> stream;
    stream.Attach(SHCreateMemStream(data, (UINT)size));
    if (!stream) Fail(L"Sin memoria para leer el SVG");

    // Any WIC-backed render target is enough to parse; the real one is created once the size is known.
    ComPtr<IWICBitmap> probeBitmap;
    CheckHr(Wic()->CreateBitmap(1, 1, GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnLoad, &probeBitmap), L"WIC");
    auto props = D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_SOFTWARE,
                                              D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
    ComPtr<ID2D1RenderTarget> probeTarget;
    CheckHr(D2d()->CreateWicBitmapRenderTarget(probeBitmap.Get(), props, &probeTarget), L"Direct2D");
    ComPtr<ID2D1DeviceContext5> probeDc;
    if (FAILED(probeTarget.As(&probeDc))) Fail(L"Abrir SVG requiere Windows 10 1703 o posterior");
    ComPtr<ID2D1SvgDocument> doc;
    CheckHr(probeDc->CreateSvgDocument(stream.Get(), D2D1::SizeF(1, 1), &doc), L"SVG no válido o no soportado");
    ComPtr<ID2D1SvgElement> root;
    doc->GetRoot(&root);
    if (!root) Fail(L"SVG vacío");

    D2D1_SIZE_F intrinsic = IntrinsicSize(root.Get());
    float scale = RenderScale(intrinsic);
    uint32_t w = std::max(1u, (uint32_t)std::lround(intrinsic.width * scale));
    uint32_t h = std::max(1u, (uint32_t)std::lround(intrinsic.height * scale));
    CheckHr(doc->SetViewportSize(intrinsic), L"SVG");

    ComPtr<IWICBitmap> bitmap;
    CheckHr(Wic()->CreateBitmap(w, h, GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnLoad, &bitmap),
            L"SVG demasiado grande");
    ComPtr<ID2D1RenderTarget> target;
    CheckHr(D2d()->CreateWicBitmapRenderTarget(bitmap.Get(), props, &target), L"Direct2D");
    ComPtr<ID2D1DeviceContext5> dc;
    CheckHr(target.As(&dc), L"Direct2D");
    dc->BeginDraw();
    dc->Clear(D2D1::ColorF(0, 0, 0, 0));
    dc->SetTransform(D2D1::Matrix3x2F::Scale(scale, scale));
    dc->DrawSvgDocument(doc.Get());
    DrawSvgText(dc.Get(), data, size, ViewBoxTransform(root.Get(), intrinsic) * D2D1::Matrix3x2F::Scale(scale, scale));
    CheckHr(dc->EndDraw(), L"No se pudo dibujar el SVG");

    // Image holds straight alpha; the converter un-premultiplies.
    ComPtr<IWICFormatConverter> conv;
    CheckHr(Wic()->CreateFormatConverter(&conv), L"WIC");
    CheckHr(conv->Initialize(bitmap.Get(), GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr, 0,
                             WICBitmapPaletteTypeCustom),
            L"WIC");
    Image img(w, h);
    CheckHr(conv->CopyPixels(nullptr, w * 4, (UINT)img.px.size(), img.px.data()), L"No se pudo leer el SVG");
    return img;
}
