#include "vectorize.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <random>

namespace {

struct Rgb {
    float r, g, b;
};

float Dist2(const Rgb& a, const Rgb& b) {
    float dr = a.r - b.r, dg = a.g - b.g, db = a.b - b.b;
    return dr * dr + dg * dg + db * db;
}

// --- palette ---------------------------------------------------------------------------

// k-means++ seeding plus a few Lloyd iterations over a sample of the opaque pixels, then
// near-duplicate colours are merged so flat artwork keeps only the colours it really has.
std::vector<Rgb> BuildPalette(const std::vector<Rgb>& samples, int k) {
    std::mt19937 rng(12345);
    std::vector<Rgb> centers;
    if (samples.empty()) return centers;
    centers.push_back(samples[rng() % samples.size()]);
    std::vector<float> d(samples.size(), 1e30f);
    while ((int)centers.size() < k) {
        double total = 0;
        for (size_t i = 0; i < samples.size(); ++i) {
            d[i] = std::min(d[i], Dist2(samples[i], centers.back()));
            total += d[i];
        }
        if (total <= 0) break;
        double pick = std::uniform_real_distribution<double>(0, total)(rng);
        size_t i = 0;
        for (; i + 1 < samples.size() && (pick -= d[i]) > 0; ++i) {}
        centers.push_back(samples[i]);
    }
    for (int iter = 0; iter < 8; ++iter) {
        std::vector<double> sr(centers.size()), sg(centers.size()), sb(centers.size());
        std::vector<size_t> n(centers.size());
        for (const auto& s : samples) {
            size_t best = 0;
            float bd = 1e30f;
            for (size_t c = 0; c < centers.size(); ++c)
                if (float dd = Dist2(s, centers[c]); dd < bd) {
                    bd = dd;
                    best = c;
                }
            sr[best] += s.r;
            sg[best] += s.g;
            sb[best] += s.b;
            ++n[best];
        }
        std::vector<Rgb> next;
        for (size_t c = 0; c < centers.size(); ++c)
            if (n[c]) next.push_back({float(sr[c] / n[c]), float(sg[c] / n[c]), float(sb[c] / n[c])});
        centers.swap(next);
    }
    // Merge colours closer than ~14 levels: anti-aliasing and compression noise, not real colours.
    for (bool merged = true; merged;) {
        merged = false;
        for (size_t i = 0; i < centers.size() && !merged; ++i)
            for (size_t j = i + 1; j < centers.size() && !merged; ++j)
                if (Dist2(centers[i], centers[j]) < 14 * 14) {
                    centers[i] = {(centers[i].r + centers[j].r) / 2, (centers[i].g + centers[j].g) / 2,
                                  (centers[i].b + centers[j].b) / 2};
                    centers.erase(centers.begin() + j);
                    merged = true;
                }
    }
    return centers;
}

// --- outlines --------------------------------------------------------------------------

struct Pt {
    double x, y;
};

// Outlines of the set pixels of `mask`, walking pixel edges with the inside on the right. Outer
// boundaries come out clockwise and holes counter-clockwise, so even-odd filling is correct.
// Returns only the corners where the direction changes.
std::vector<std::vector<Pt>> Trace(const std::vector<uint8_t>& mask, int w, int h, double minArea) {
    static const int dx[4] = {1, 0, -1, 0}, dy[4] = {0, 1, 0, -1};  // E S W N (y down)
    int cw = w + 1;
    auto in = [&](int x, int y) { return x >= 0 && y >= 0 && x < w && y < h && mask[size_t(y) * w + x]; };
    std::vector<int32_t> out0(size_t(cw) * (h + 1), -1), out1(out0.size(), -1);
    std::vector<int32_t> from;
    std::vector<uint8_t> dir;
    auto add = [&](int cx, int cy, uint8_t d) {
        int32_t id = (int32_t)from.size();
        int32_t c = cy * cw + cx;
        from.push_back(c);
        dir.push_back(d);
        (out0[c] < 0 ? out0[c] : out1[c]) = id;
    };
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            if (!mask[size_t(y) * w + x]) continue;
            if (!in(x, y - 1)) add(x, y, 0);
            if (!in(x + 1, y)) add(x + 1, y, 1);
            if (!in(x, y + 1)) add(x + 1, y + 1, 2);
            if (!in(x - 1, y)) add(x, y + 1, 3);
        }
    std::vector<uint8_t> used(from.size(), 0);
    std::vector<std::vector<Pt>> loops;
    for (size_t start = 0; start < from.size(); ++start) {
        if (used[start]) continue;
        std::vector<Pt> pts;
        int32_t e = (int32_t)start;
        int prevDir = -1;
        while (e >= 0 && !used[e]) {
            used[e] = 1;
            int c = from[e], d = dir[e];
            if (d != prevDir) pts.push_back({double(c % cw), double(c / cw)});
            prevDir = d;
            int next = c + dy[d] * cw + dx[d];
            // At a corner shared by two diagonal pixels prefer turning right: diagonal neighbours
            // become separate shapes and outlines never cross.
            int32_t pick = -1;
            for (int turn : {1, 0, 3}) {
                int want = (d + turn) % 4;
                for (int32_t cand : {out0[next], out1[next]})
                    if (cand >= 0 && !used[cand] && dir[cand] == want) {
                        pick = cand;
                        break;
                    }
                if (pick >= 0) break;
            }
            e = pick;
        }
        if (pts.size() > 1 && dir[start] == prevDir) pts.erase(pts.begin());  // first corner was mid-run
        if (pts.size() < 3) continue;
        double area = 0;
        for (size_t i = 0; i < pts.size(); ++i) {
            const Pt &a = pts[i], &b = pts[(i + 1) % pts.size()];
            area += a.x * b.y - b.x * a.y;
        }
        if (std::abs(area) / 2 >= minArea) loops.push_back(std::move(pts));
    }
    return loops;
}

// Douglas–Peucker on a closed polygon: turns pixel staircases into straight or curving runs.
std::vector<Pt> Simplify(const std::vector<Pt>& p, double tol) {
    size_t n = p.size();
    if (n < 5) return p;
    size_t split = 0;
    double best = -1;
    for (size_t i = 1; i < n; ++i) {
        double d = (p[i].x - p[0].x) * (p[i].x - p[0].x) + (p[i].y - p[0].y) * (p[i].y - p[0].y);
        if (d > best) {
            best = d;
            split = i;
        }
    }
    std::vector<uint8_t> keep(n, 0);
    keep[0] = keep[split] = 1;
    std::vector<std::pair<size_t, size_t>> stack = {{0, split}, {split, n}};  // index n means p[0]
    auto at = [&](size_t i) -> const Pt& { return p[i % n]; };
    while (!stack.empty()) {
        auto [a, b] = stack.back();
        stack.pop_back();
        if (b <= a + 1) continue;
        const Pt &A = at(a), &B = at(b);
        double vx = B.x - A.x, vy = B.y - A.y, len = std::hypot(vx, vy);
        size_t idx = 0;
        double dmax = -1;
        for (size_t i = a + 1; i < b; ++i) {
            const Pt& P = at(i);
            double d = len > 0 ? std::abs(vx * (A.y - P.y) - vy * (A.x - P.x)) / len : std::hypot(P.x - A.x, P.y - A.y);
            if (d > dmax) {
                dmax = d;
                idx = i;
            }
        }
        if (dmax > tol) {
            keep[idx % n] = 1;
            stack.push_back({a, idx});
            stack.push_back({idx, b});
        }
    }
    std::vector<Pt> out;
    for (size_t i = 0; i < n; ++i)
        if (keep[i]) out.push_back(p[i]);
    return out;
}

void Num(std::string& s, double v) {
    char buf[32];
    snprintf(buf, sizeof(buf), "%.1f", v);
    size_t len = strlen(buf);
    if (len > 2 && buf[len - 1] == '0' && buf[len - 2] == '.') buf[len - 2] = 0;
    s += buf;
}

void PointTo(std::string& s, char cmd, const Pt& p) {
    s += cmd;
    Num(s, p.x);
    s += ',';
    Num(s, p.y);
}

// Sharp turns stay corners; gentle ones become quadratic curves through the segment midpoints.
void AppendPath(std::string& d, const std::vector<Pt>& p) {
    size_t n = p.size();
    auto mid = [](const Pt& a, const Pt& b) { return Pt{(a.x + b.x) / 2, (a.y + b.y) / 2}; };
    PointTo(d, 'M', mid(p[n - 1], p[0]));
    for (size_t i = 0; i < n; ++i) {
        const Pt &prev = p[(i + n - 1) % n], &cur = p[i], &next = p[(i + 1) % n];
        double ax = cur.x - prev.x, ay = cur.y - prev.y, bx = next.x - cur.x, by = next.y - cur.y;
        double la = std::hypot(ax, ay), lb = std::hypot(bx, by);
        double cosTurn = la > 0 && lb > 0 ? (ax * bx + ay * by) / (la * lb) : 1;
        bool corner = cosTurn < 0.42;  // turning more than ~65°
        Pt m = mid(cur, next);
        if (corner) {
            PointTo(d, 'L', cur);
            PointTo(d, 'L', m);
        } else {
            PointTo(d, 'Q', cur);
            d += ' ';
            Num(d, m.x);
            d += ',';
            Num(d, m.y);
        }
    }
    d += 'Z';
}

}  // namespace

std::string VectorizeToSvg(const Image& src, int maxColors) {
    if (src.w == 0 || src.h == 0) Fail(L"Imagen vacía");
    // Tracing cost and file size grow with the pixel count; 1600 px keeps detail and stays fast.
    // Small images are enlarged first: their anti-aliased edges then trace as smooth sub-pixel
    // outlines instead of pixel staircases.
    uint32_t side = std::max(src.w, src.h);
    Image img = side > 1600 ? FitWithin(src, 1600)
              : side < 1000 ? Resize(src, uint32_t(src.w * std::min(4.0, 1200.0 / side)),
                                     uint32_t(src.h * std::min(4.0, 1200.0 / side)))
                            : src;
    int w = (int)img.w, h = (int)img.h;
    size_t count = size_t(w) * h;

    // Light 3x3 blur before quantizing so noise does not become thousands of specks.
    std::vector<Rgb> color(count);
    std::vector<uint8_t> opaque(count);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            float r = 0, g = 0, b = 0, n = 0;
            for (int yy = std::max(0, y - 1); yy <= std::min(h - 1, y + 1); ++yy)
                for (int xx = std::max(0, x - 1); xx <= std::min(w - 1, x + 1); ++xx) {
                    const uint8_t* px = img.Row(yy) + size_t(xx) * 4;
                    float wgt = (xx == x && yy == y) ? 4.0f : 1.0f;
                    b += px[0] * wgt;
                    g += px[1] * wgt;
                    r += px[2] * wgt;
                    n += wgt;
                }
            color[size_t(y) * w + x] = {r / n, g / n, b / n};
            opaque[size_t(y) * w + x] = img.Row(y)[size_t(x) * 4 + 3] >= 128;
        }

    std::vector<Rgb> samples;
    size_t step = std::max<size_t>(1, count / 120000);
    for (size_t i = 0; i < count; i += step)
        if (opaque[i]) samples.push_back(color[i]);
    if (samples.empty()) Fail(L"La imagen es completamente transparente");
    std::vector<Rgb> palette = BuildPalette(samples, std::max(2, maxColors));

    constexpr uint8_t kNone = 255;
    std::vector<uint8_t> idx(count, kNone);
    std::vector<size_t> pixels(palette.size(), 0);
    for (size_t i = 0; i < count; ++i) {
        if (!opaque[i]) continue;
        uint8_t best = 0;
        float bd = 1e30f;
        for (size_t c = 0; c < palette.size(); ++c)
            if (float d = Dist2(color[i], palette[c]); d < bd) {
                bd = d;
                best = (uint8_t)c;
            }
        idx[i] = best;
        ++pixels[best];
    }
    // Colours covering a sliver of the image are mostly the blends along edges: give their pixels
    // to the nearest remaining colour, so edges do not turn into thin ragged bands.
    size_t opaqueCount = 0;
    for (size_t n : pixels) opaqueCount += n;
    std::vector<uint8_t> remap(palette.size());
    std::vector<size_t> kept;
    for (size_t c = 0; c < palette.size(); ++c)
        if (pixels[c] >= opaqueCount / 250) kept.push_back(c);
    if (kept.size() < 2) {
        kept.clear();
        for (size_t c = 0; c < palette.size(); ++c) kept.push_back(c);
    }
    for (size_t c = 0; c < palette.size(); ++c) {
        size_t best = kept[0];
        for (size_t k : kept)
            if (Dist2(palette[c], palette[k]) < Dist2(palette[c], palette[best])) best = k;
        remap[c] = (uint8_t)best;
    }
    for (auto& v : idx)
        if (v != kNone) v = remap[v];
    std::fill(pixels.begin(), pixels.end(), 0);
    // Majority filter: isolated pixels take the colour around them.
    std::vector<uint8_t> cleaned = idx;
    for (int y = 1; y + 1 < h; ++y)
        for (int x = 1; x + 1 < w; ++x) {
            size_t i = size_t(y) * w + x;
            if (idx[i] == kNone) continue;
            uint8_t votes[256] = {};
            uint8_t top = idx[i], topVotes = 0;
            for (int yy = -1; yy <= 1; ++yy)
                for (int xx = -1; xx <= 1; ++xx) {
                    uint8_t v = idx[i + yy * w + xx];
                    if (v == kNone) continue;
                    if (++votes[v] > topVotes) {
                        topVotes = votes[v];
                        top = v;
                    }
                }
            if (topVotes >= 5) cleaned[i] = top;
        }
    idx.swap(cleaned);
    for (uint8_t v : idx)
        if (v != kNone) ++pixels[v];

    // Stacked layers, biggest colour first. Each layer also covers every later (smaller) layer's
    // pixels, so the shapes on top always sit on something: no hairline gaps between colours.
    std::vector<int> order(palette.size());
    for (size_t i = 0; i < order.size(); ++i) order[i] = (int)i;
    std::sort(order.begin(), order.end(), [&](int a, int b) { return pixels[a] > pixels[b]; });
    std::vector<int> rank(palette.size());
    for (size_t r = 0; r < order.size(); ++r) rank[order[r]] = (int)r;

    double minArea = std::max(12.0, count * 0.00002);
    std::string body;
    std::vector<uint8_t> mask(count);
    for (size_t layer = 0; layer < order.size(); ++layer) {
        if (!pixels[order[layer]]) continue;
        for (size_t i = 0; i < count; ++i) mask[i] = idx[i] != kNone && rank[idx[i]] >= (int)layer;
        std::string d;
        for (const auto& loop : Trace(mask, w, h, minArea)) {
            auto simple = Simplify(loop, 0.9);
            if (simple.size() >= 3) AppendPath(d, simple);
        }
        if (d.empty()) continue;
        const Rgb& c = palette[order[layer]];
        char fill[16];
        snprintf(fill, sizeof(fill), "#%02x%02x%02x", int(std::lround(std::clamp(c.r, 0.0f, 255.0f))),
                 int(std::lround(std::clamp(c.g, 0.0f, 255.0f))), int(std::lround(std::clamp(c.b, 0.0f, 255.0f))));
        body += "<path fill=\"" + std::string(fill) + "\" fill-rule=\"evenodd\" d=\"" + d + "\"/>\n";
    }
    if (body.empty()) Fail(L"No se pudo trazar ninguna forma");

    char head[256];
    snprintf(head, sizeof(head),
             "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"%u\" height=\"%u\" viewBox=\"0 0 %d %d\">\n", src.w,
             src.h, w, h);
    return std::string(head) + body + "</svg>\n";
}
