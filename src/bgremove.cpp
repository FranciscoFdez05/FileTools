#include "bgremove.h"

#include <unknwn.h>
#include <winrt/Windows.AI.MachineLearning.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Storage.Streams.h>

#include <algorithm>
#include <cmath>
#include <mutex>

using namespace winrt::Windows::AI::MachineLearning;
using namespace winrt::Windows::Storage::Streams;

namespace {

constexpr int kModelResource = 201;  // res/u2netp.onnx
constexpr int kSide = 320;           // the model's input and output size

// The model is loaded once and shared; sessions are cheap to create.
LearningModel Model() {
    static std::mutex lock;
    static LearningModel model{nullptr};
    std::lock_guard guard(lock);
    if (model) return model;
    HRSRC res = FindResourceW(nullptr, MAKEINTRESOURCEW(kModelResource), RT_RCDATA);
    HGLOBAL data = res ? LoadResource(nullptr, res) : nullptr;
    auto* bytes = data ? static_cast<const uint8_t*>(LockResource(data)) : nullptr;
    if (!bytes) Fail(L"Falta el modelo para quitar fondos en el ejecutable");
    InMemoryRandomAccessStream stream;
    DataWriter writer(stream);
    writer.WriteBytes(winrt::array_view<const uint8_t>(bytes, bytes + SizeofResource(nullptr, res)));
    writer.StoreAsync().get();
    writer.DetachStream();
    stream.Seek(0);
    model = LearningModel::LoadFromStream(RandomAccessStreamReference::CreateFromStream(stream));
    return model;
}

// Bilinear resampling of a single-channel float map.
std::vector<float> ResizeMap(const std::vector<float>& src, int sw, int sh, int dw, int dh) {
    std::vector<float> out(size_t(dw) * dh);
    for (int y = 0; y < dh; ++y) {
        float fy = std::clamp((y + 0.5f) * sh / dh - 0.5f, 0.0f, float(sh - 1));
        int y0 = int(fy), y1 = std::min(y0 + 1, sh - 1);
        float ty = fy - y0;
        for (int x = 0; x < dw; ++x) {
            float fx = std::clamp((x + 0.5f) * sw / dw - 0.5f, 0.0f, float(sw - 1));
            int x0 = int(fx), x1 = std::min(x0 + 1, sw - 1);
            float tx = fx - x0;
            float a = src[size_t(y0) * sw + x0] * (1 - tx) + src[size_t(y0) * sw + x1] * tx;
            float b = src[size_t(y1) * sw + x0] * (1 - tx) + src[size_t(y1) * sw + x1] * tx;
            out[size_t(y) * dw + x] = a * (1 - ty) + b * ty;
        }
    }
    return out;
}

// Mean over a (2r+1)² window, shrinking at the borders. Separable running sums, O(w·h).
std::vector<float> BoxMean(const std::vector<float>& in, int w, int h, int r) {
    std::vector<float> tmp(in.size()), out(in.size());
    for (int y = 0; y < h; ++y) {
        const float* row = in.data() + size_t(y) * w;
        double sum = 0;
        int lo = 0, hi = -1;
        for (int x = 0; x < w; ++x) {
            while (hi < std::min(w - 1, x + r)) sum += row[++hi];
            while (lo < x - r) sum -= row[lo++];
            tmp[size_t(y) * w + x] = float(sum / (hi - lo + 1));
        }
    }
    for (int x = 0; x < w; ++x) {
        double sum = 0;
        int lo = 0, hi = -1;
        for (int y = 0; y < h; ++y) {
            while (hi < std::min(h - 1, y + r)) sum += tmp[size_t(++hi) * w + x];
            while (lo < y - r) sum -= tmp[size_t(lo++) * w + x];
            out[size_t(y) * w + x] = float(sum / (hi - lo + 1));
        }
    }
    return out;
}

// Guided filter (He et al.): smooths the coarse mask `p` while making its edges follow the
// edges of the guide image `I`, which recovers detail the 320 px prediction cannot see.
std::vector<float> GuidedFilter(const std::vector<float>& I, const std::vector<float>& p, int w, int h, int r,
                                float eps) {
    size_t n = I.size();
    std::vector<float> Ip(n), II(n);
    for (size_t i = 0; i < n; ++i) {
        Ip[i] = I[i] * p[i];
        II[i] = I[i] * I[i];
    }
    auto mI = BoxMean(I, w, h, r), mp = BoxMean(p, w, h, r), mIp = BoxMean(Ip, w, h, r), mII = BoxMean(II, w, h, r);
    std::vector<float> a(n), b(n);
    for (size_t i = 0; i < n; ++i) {
        float var = mII[i] - mI[i] * mI[i], cov = mIp[i] - mI[i] * mp[i];
        a[i] = cov / (var + eps);
        b[i] = mp[i] - a[i] * mI[i];
    }
    auto ma = BoxMean(a, w, h, r), mb = BoxMean(b, w, h, r);
    std::vector<float> q(n);
    for (size_t i = 0; i < n; ++i) q[i] = ma[i] * I[i] + mb[i];
    return q;
}

std::vector<float> PredictMask(const Image& img) {
    Image scaled = Resize(img, kSide, kSide);
    // Same preprocessing as the reference implementation: scale by the brightest value, then
    // ImageNet mean/std, planar RGB.
    uint8_t maxv = 1;
    for (size_t i = 0; i < scaled.px.size(); i += 4) maxv = std::max({maxv, scaled.px[i], scaled.px[i + 1], scaled.px[i + 2]});
    const float mean[3] = {0.485f, 0.456f, 0.406f}, stdv[3] = {0.229f, 0.224f, 0.225f};
    std::vector<float> input(size_t(3) * kSide * kSide);
    for (int i = 0; i < kSide * kSide; ++i) {
        const uint8_t* px = scaled.px.data() + size_t(i) * 4;  // BGRA
        float rgb[3] = {px[2] / float(maxv), px[1] / float(maxv), px[0] / float(maxv)};
        for (int c = 0; c < 3; ++c) input[size_t(c) * kSide * kSide + i] = (rgb[c] - mean[c]) / stdv[c];
    }

    LearningModel model = Model();
    LearningModelSession session(model, LearningModelDevice(LearningModelDeviceKind::Cpu));
    LearningModelBinding binding(session);
    std::vector<int64_t> shape{1, 3, kSide, kSide};
    binding.Bind(model.InputFeatures().GetAt(0).Name(), TensorFloat::CreateFromArray(shape, input));
    auto result = session.Evaluate(binding, L"nobg");
    // The first output is the fused, full-resolution prediction.
    auto tensor = result.Outputs().Lookup(model.OutputFeatures().GetAt(0).Name()).as<TensorFloat>();
    auto view = tensor.GetAsVectorView();
    std::vector<float> mask(view.Size());
    view.GetMany(0, mask);
    if (mask.size() != size_t(kSide) * kSide) Fail(L"El modelo devolvió un resultado inesperado");
    auto [lo, hi] = std::minmax_element(mask.begin(), mask.end());
    float mn = *lo, range = std::max(*hi - *lo, 1e-6f);
    for (float& v : mask) v = (v - mn) / range;
    return mask;
}

}  // namespace

SubjectMask DetectSubject(const Image& img) {
    if (img.w == 0 || img.h == 0) Fail(L"Imagen vacía");
    SubjectMask m;
    try {
        m.coarse = PredictMask(img);
    } catch (const winrt::hresult_error& e) {
        Fail(L"Windows ML no pudo ejecutar el modelo (requiere Windows 10 2004 o superior): " +
             std::wstring(e.message()));
    }
    // Edges are refined at up to 1024 px: enough for the guided filter, and fast to redo.
    uint32_t side = std::max(img.w, img.h);
    float k = side > 1024 ? 1024.0f / side : 1.0f;
    m.gw = std::max(1, int(std::lround(img.w * k)));
    m.gh = std::max(1, int(std::lround(img.h * k)));
    Image guideImg = (m.gw == int(img.w) && m.gh == int(img.h)) ? img : Resize(img, m.gw, m.gh);
    m.guide.resize(size_t(m.gw) * m.gh);
    for (size_t i = 0; i < m.guide.size(); ++i) {
        const uint8_t* px = guideImg.px.data() + i * 4;
        m.guide[i] = (0.114f * px[0] + 0.587f * px[1] + 0.299f * px[2]) / 255.0f;
    }
    return m;
}

void ApplyCutout(Image& img, const SubjectMask& m, const CutoutSettings& set) {
    // Threshold: low-confidence areas (shadows, clutter next to the subject) come out as a faint
    // haze, so they are decided before refining and the guided filter only works on real edges.
    float center = 0.05f + 0.9f * std::clamp(set.threshold, 0, 100) / 100.0f;
    float width = 0.02f + 0.76f * std::clamp(set.softness, 0, 100) / 100.0f;
    std::vector<float> coarse = m.coarse;
    for (float& v : coarse) {
        float t = std::clamp((v - (center - width / 2)) / width, 0.0f, 1.0f);
        v = t * t * (3 - 2 * t);
    }
    auto mask = ResizeMap(coarse, kSide, kSide, m.gw, m.gh);
    int radius = std::max(2, int(std::lround(std::max(m.gw, m.gh) / 128.0)));
    mask = GuidedFilter(m.guide, mask, m.gw, m.gh, radius, 1e-3f);
    if (m.gw != int(img.w) || m.gh != int(img.h)) mask = ResizeMap(mask, m.gw, m.gh, img.w, img.h);
    // Softness also sets how wide the final transition is: 0 is a crisp cut, 100 a long feather.
    float span = 0.3f + 1.0f * std::clamp(set.softness, 0, 100) / 100.0f;
    for (size_t i = 0; i < mask.size(); ++i) {
        float a = std::clamp((mask[i] - (0.5f - span / 2)) / span, 0.0f, 1.0f);
        uint8_t& alpha = img.px[i * 4 + 3];
        alpha = uint8_t(std::lround(alpha * a));
    }
}

Image RemoveBackground(const Image& img) {
    Image out = img;
    ApplyCutout(out, DetectSubject(img), LoadCutoutSettings());
    size_t opaque = 0;
    for (size_t i = 3; i < out.px.size(); i += 4) opaque += out.px[i] > 127;
    if (opaque < size_t(out.w) * out.h / 400) Fail(L"No se distingue ningún sujeto en la imagen");
    return out;
}

CutoutSettings LoadCutoutSettings() {
    CutoutSettings s;
    std::wstring t = ReadSettingString(L"CutoutThreshold"), f = ReadSettingString(L"CutoutSoftness");
    if (!t.empty()) s.threshold = std::clamp(_wtoi(t.c_str()), 0, 100);
    if (!f.empty()) s.softness = std::clamp(_wtoi(f.c_str()), 0, 100);
    return s;
}

void SaveCutoutSettings(const CutoutSettings& s) {
    WriteSettingString(L"CutoutThreshold", std::to_wstring(s.threshold));
    WriteSettingString(L"CutoutSoftness", std::to_wstring(s.softness));
}
