#include "qrcode.h"

#include "common.h"

#include <algorithm>
#include <cstdlib>

// Follows ISO/IEC 18004 the same way as Project Nayuki's reference encoder (MIT).

namespace {

constexpr int kMaxVersion = 10;
// Level M, indexed by version.
constexpr int kEccPerBlock[kMaxVersion + 1] = {0, 10, 16, 26, 18, 24, 16, 18, 22, 22, 26};
constexpr int kBlocks[kMaxVersion + 1] = {0, 1, 1, 1, 2, 2, 4, 4, 4, 5, 5};
constexpr int kFormatBitsM = 0;  // L = 1, M = 0, Q = 3, H = 2

int RawDataModules(int ver) {
    int result = (16 * ver + 128) * ver + 64;
    if (ver >= 2) {
        int align = ver / 7 + 2;
        result -= (25 * align - 10) * align - 55;
        if (ver >= 7) result -= 36;
    }
    return result;
}

int DataCodewords(int ver) { return RawDataModules(ver) / 8 - kEccPerBlock[ver] * kBlocks[ver]; }

uint8_t GfMul(uint8_t x, uint8_t y) {
    int z = 0;
    for (int i = 7; i >= 0; --i) {
        z = (z << 1) ^ ((z >> 7) * 0x11D);
        z ^= ((y >> i) & 1) * x;
    }
    return uint8_t(z);
}

std::vector<uint8_t> RsDivisor(int degree) {
    std::vector<uint8_t> result(degree, 0);
    result[degree - 1] = 1;
    uint8_t root = 1;
    for (int i = 0; i < degree; ++i) {
        for (int j = 0; j < degree; ++j) {
            result[j] = GfMul(result[j], root);
            if (j + 1 < degree) result[j] ^= result[j + 1];
        }
        root = GfMul(root, 0x02);
    }
    return result;
}

std::vector<uint8_t> RsRemainder(const std::vector<uint8_t>& data, const std::vector<uint8_t>& divisor) {
    std::vector<uint8_t> result(divisor.size(), 0);
    for (uint8_t b : data) {
        uint8_t factor = b ^ result[0];
        result.erase(result.begin());
        result.push_back(0);
        for (size_t i = 0; i < result.size(); ++i) result[i] ^= GfMul(divisor[i], factor);
    }
    return result;
}

std::vector<uint8_t> AddEccAndInterleave(const std::vector<uint8_t>& data, int ver) {
    int blocks = kBlocks[ver], eccLen = kEccPerBlock[ver], raw = RawDataModules(ver) / 8;
    int shortBlocks = blocks - raw % blocks, shortLen = raw / blocks;
    auto divisor = RsDivisor(eccLen);
    std::vector<std::vector<uint8_t>> all;
    size_t k = 0;
    for (int i = 0; i < blocks; ++i) {
        size_t len = shortLen - eccLen + (i < shortBlocks ? 0 : 1);
        std::vector<uint8_t> block(data.begin() + k, data.begin() + k + len);
        k += len;
        auto ecc = RsRemainder(block, divisor);
        if (i < shortBlocks) block.push_back(0);
        block.insert(block.end(), ecc.begin(), ecc.end());
        all.push_back(std::move(block));
    }
    std::vector<uint8_t> result;
    for (size_t i = 0; i < all[0].size(); ++i)
        for (int j = 0; j < blocks; ++j)
            if (i != size_t(shortLen - eccLen) || j >= shortBlocks) result.push_back(all[j][i]);
    return result;
}

class Builder {
public:
    explicit Builder(int ver) : ver_(ver), size_(ver * 4 + 17), dark_(size_ * size_), func_(size_ * size_) {}

    QrCode Build(const std::vector<uint8_t>& codewords) {
        DrawFunctionPatterns();
        DrawCodewords(codewords);
        int best = 0;
        long bestScore = -1;
        for (int mask = 0; mask < 8; ++mask) {
            ApplyMask(mask);
            DrawFormatBits(mask);
            long score = Penalty();
            if (bestScore < 0 || score < bestScore) {
                best = mask;
                bestScore = score;
            }
            ApplyMask(mask);  // XOR again to undo
        }
        ApplyMask(best);
        DrawFormatBits(best);
        QrCode qr;
        qr.size = size_;
        qr.modules.assign(dark_.begin(), dark_.end());
        return qr;
    }

private:
    bool Get(int x, int y) const { return dark_[size_t(y) * size_ + x] != 0; }
    void Set(int x, int y, bool dark) { dark_[size_t(y) * size_ + x] = dark; }
    void SetFunction(int x, int y, bool dark) {
        Set(x, y, dark);
        func_[size_t(y) * size_ + x] = 1;
    }

    std::vector<int> AlignmentPositions() const {
        if (ver_ == 1) return {};
        int count = ver_ / 7 + 2;
        int step = (ver_ * 4 + count * 2 + 1) / (count * 2 - 2) * 2;
        std::vector<int> result{6};
        for (int i = 0, pos = size_ - 7; i < count - 1; ++i, pos -= step) result.insert(result.begin() + 1, pos);
        return result;
    }

    void DrawFinder(int x, int y) {
        for (int dy = -4; dy <= 4; ++dy)
            for (int dx = -4; dx <= 4; ++dx) {
                int dist = std::max(std::abs(dx), std::abs(dy)), xx = x + dx, yy = y + dy;
                if (xx >= 0 && xx < size_ && yy >= 0 && yy < size_) SetFunction(xx, yy, dist != 2 && dist != 4);
            }
    }

    void DrawAlignment(int x, int y) {
        for (int dy = -2; dy <= 2; ++dy)
            for (int dx = -2; dx <= 2; ++dx) SetFunction(x + dx, y + dy, std::max(std::abs(dx), std::abs(dy)) != 1);
    }

    void DrawFunctionPatterns() {
        for (int i = 0; i < size_; ++i) {
            SetFunction(6, i, i % 2 == 0);
            SetFunction(i, 6, i % 2 == 0);
        }
        DrawFinder(3, 3);
        DrawFinder(size_ - 4, 3);
        DrawFinder(3, size_ - 4);
        auto pos = AlignmentPositions();
        int n = (int)pos.size();
        for (int i = 0; i < n; ++i)
            for (int j = 0; j < n; ++j)
                if (!((i == 0 && j == 0) || (i == 0 && j == n - 1) || (i == n - 1 && j == 0)))
                    DrawAlignment(pos[i], pos[j]);
        DrawFormatBits(0);  // reserves the area; redrawn with the real mask
        DrawVersion();
    }

    void DrawFormatBits(int mask) {
        int data = kFormatBitsM << 3 | mask, rem = data;
        for (int i = 0; i < 10; ++i) rem = (rem << 1) ^ ((rem >> 9) * 0x537);
        int bits = (data << 10 | rem) ^ 0x5412;
        auto bit = [bits](int i) { return ((bits >> i) & 1) != 0; };
        for (int i = 0; i <= 5; ++i) SetFunction(8, i, bit(i));
        SetFunction(8, 7, bit(6));
        SetFunction(8, 8, bit(7));
        SetFunction(7, 8, bit(8));
        for (int i = 9; i < 15; ++i) SetFunction(14 - i, 8, bit(i));
        for (int i = 0; i < 8; ++i) SetFunction(size_ - 1 - i, 8, bit(i));
        for (int i = 8; i < 15; ++i) SetFunction(8, size_ - 15 + i, bit(i));
        SetFunction(8, size_ - 8, true);  // always dark
    }

    void DrawVersion() {
        if (ver_ < 7) return;
        int rem = ver_;
        for (int i = 0; i < 12; ++i) rem = (rem << 1) ^ ((rem >> 11) * 0x1F25);
        long bits = long(ver_) << 12 | rem;
        for (int i = 0; i < 18; ++i) {
            bool b = ((bits >> i) & 1) != 0;
            int a = size_ - 11 + i % 3, c = i / 3;
            SetFunction(a, c, b);
            SetFunction(c, a, b);
        }
    }

    void DrawCodewords(const std::vector<uint8_t>& data) {
        size_t i = 0, total = data.size() * 8;
        for (int right = size_ - 1; right >= 1; right -= 2) {
            if (right == 6) right = 5;
            for (int vert = 0; vert < size_; ++vert)
                for (int j = 0; j < 2; ++j) {
                    int x = right - j;
                    bool upward = ((right + 1) & 2) == 0;
                    int y = upward ? size_ - 1 - vert : vert;
                    if (!func_[size_t(y) * size_ + x] && i < total) {
                        Set(x, y, ((data[i >> 3] >> (7 - (i & 7))) & 1) != 0);
                        ++i;
                    }
                }
        }
    }

    void ApplyMask(int mask) {
        for (int y = 0; y < size_; ++y)
            for (int x = 0; x < size_; ++x) {
                bool invert = false;
                switch (mask) {
                    case 0: invert = (x + y) % 2 == 0; break;
                    case 1: invert = y % 2 == 0; break;
                    case 2: invert = x % 3 == 0; break;
                    case 3: invert = (x + y) % 3 == 0; break;
                    case 4: invert = (x / 3 + y / 2) % 2 == 0; break;
                    case 5: invert = x * y % 2 + x * y % 3 == 0; break;
                    case 6: invert = (x * y % 2 + x * y % 3) % 2 == 0; break;
                    case 7: invert = ((x + y) % 2 + x * y % 3) % 2 == 0; break;
                }
                if (invert && !func_[size_t(y) * size_ + x]) Set(x, y, !Get(x, y));
            }
    }

    // Lower is better: long runs, 2x2 blocks, finder-like patterns and an unbalanced dark ratio.
    long Penalty() const {
        long score = 0;
        auto line = [&](bool rows) {
            for (int a = 0; a < size_; ++a) {
                int run = 0;
                bool last = false;
                uint32_t window = 0;  // last 11 modules, newest in bit 0
                for (int b = 0; b < size_; ++b) {
                    bool d = rows ? Get(b, a) : Get(a, b);
                    if (b > 0 && d == last) ++run;
                    else {
                        if (run >= 5) score += 3 + (run - 5);
                        run = 1;
                        last = d;
                    }
                    window = ((window << 1) | (d ? 1 : 0)) & 0x7FF;
                    if (b >= 10 && (window == 0x5D0 || window == 0x05D)) score += 40;  // 1011101 0000 / 0000 1011101
                }
                if (run >= 5) score += 3 + (run - 5);
            }
        };
        line(true);
        line(false);
        for (int y = 0; y + 1 < size_; ++y)
            for (int x = 0; x + 1 < size_; ++x) {
                bool d = Get(x, y);
                if (d == Get(x + 1, y) && d == Get(x, y + 1) && d == Get(x + 1, y + 1)) score += 3;
            }
        long dark = long(std::count(dark_.begin(), dark_.end(), uint8_t(1))), total = long(size_) * size_;
        long k = (std::labs(dark * 20 - total * 10) + total - 1) / total - 1;
        score += k * 10;
        return score;
    }

    int ver_, size_;
    std::vector<uint8_t> dark_, func_;
};

}  // namespace

QrCode EncodeQr(std::string_view text) {
    int ver = 1;
    for (; ver <= kMaxVersion; ++ver) {
        int countBits = ver <= 9 ? 8 : 16;
        if (4 + countBits + text.size() * 8 <= size_t(DataCodewords(ver)) * 8) break;
    }
    if (ver > kMaxVersion) Fail(L"El enlace es demasiado largo para un código QR");

    std::vector<bool> bits;
    auto append = [&](uint32_t value, int count) {
        for (int i = count - 1; i >= 0; --i) bits.push_back(((value >> i) & 1) != 0);
    };
    append(0x4, 4);  // byte mode
    append((uint32_t)text.size(), ver <= 9 ? 8 : 16);
    for (char c : text) append(uint8_t(c), 8);
    size_t capacity = size_t(DataCodewords(ver)) * 8;
    append(0, (int)std::min<size_t>(4, capacity - bits.size()));
    append(0, int((8 - bits.size() % 8) % 8));
    for (uint8_t pad = 0xEC; bits.size() < capacity; pad ^= 0xEC ^ 0x11) append(pad, 8);

    std::vector<uint8_t> data(bits.size() / 8, 0);
    for (size_t i = 0; i < bits.size(); ++i)
        if (bits[i]) data[i >> 3] |= uint8_t(1 << (7 - (i & 7)));
    return Builder(ver).Build(AddEccAndInterleave(data, ver));
}
