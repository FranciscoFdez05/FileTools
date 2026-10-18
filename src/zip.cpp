#include "zip.h"

#include <cstring>
#include <stdexcept>

uint32_t Crc32(const void* data, size_t size) {
    static uint32_t table[256] = {};
    static bool init = false;
    if (!init) {
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            table[i] = c;
        }
        init = true;
    }
    uint32_t crc = 0xFFFFFFFFu;
    auto* p = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < size; ++i) crc = table[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
    return crc ^ 0xFFFFFFFFu;
}

namespace {

void Put16(std::string& s, uint16_t v) {
    s += char(v & 0xFF);
    s += char(v >> 8);
}
void Put32(std::string& s, uint32_t v) {
    Put16(s, uint16_t(v & 0xFFFF));
    Put16(s, uint16_t(v >> 16));
}

// DEFLATE decoder after Mark Adler's "puff".
class Inflater {
public:
    Inflater(const uint8_t* in, size_t n, std::vector<uint8_t>& out) : in_(in), n_(n), out_(out) {}

    void Run() {
        int last;
        do {
            last = Bits(1);
            switch (Bits(2)) {
                case 0: Stored(); break;
                case 1: Fixed(); break;
                case 2: Dynamic(); break;
                default: throw std::runtime_error("bad block");
            }
        } while (!last);
    }

private:
    struct Huffman {
        short count[16];
        short symbol[320];
    };

    int Bits(int need) {
        uint32_t val = bitbuf_;
        while (bitcnt_ < need) {
            if (pos_ >= n_) throw std::runtime_error("eof");
            val |= uint32_t(in_[pos_++]) << bitcnt_;
            bitcnt_ += 8;
        }
        bitbuf_ = val >> need;
        bitcnt_ -= need;
        return int(val & ((1u << need) - 1));
    }

    void Stored() {
        bitbuf_ = 0;
        bitcnt_ = 0;
        if (pos_ + 4 > n_) throw std::runtime_error("eof");
        unsigned len = in_[pos_] | (in_[pos_ + 1] << 8);
        unsigned nlen = in_[pos_ + 2] | (in_[pos_ + 3] << 8);
        pos_ += 4;
        if (len != (~nlen & 0xFFFF)) throw std::runtime_error("bad stored");
        if (pos_ + len > n_) throw std::runtime_error("eof");
        out_.insert(out_.end(), in_ + pos_, in_ + pos_ + len);
        pos_ += len;
    }

    int Decode(const Huffman& h) {
        int code = 0, first = 0, index = 0;
        for (int len = 1; len <= 15; ++len) {
            code |= Bits(1);
            int count = h.count[len];
            if (code - count < first) return h.symbol[index + (code - first)];
            index += count;
            first += count;
            first <<= 1;
            code <<= 1;
        }
        throw std::runtime_error("bad code");
    }

    static int Construct(Huffman& h, const short* length, int n) {
        short offs[16];
        for (int len = 0; len <= 15; ++len) h.count[len] = 0;
        for (int sym = 0; sym < n; ++sym) h.count[length[sym]]++;
        if (h.count[0] == n) return 0;
        int left = 1;
        for (int len = 1; len <= 15; ++len) {
            left <<= 1;
            left -= h.count[len];
            if (left < 0) return left;
        }
        offs[1] = 0;
        for (int len = 1; len < 15; ++len) offs[len + 1] = short(offs[len] + h.count[len]);
        for (int sym = 0; sym < n; ++sym)
            if (length[sym] != 0) h.symbol[offs[length[sym]]++] = short(sym);
        return left;
    }

    void Codes(const Huffman& lencode, const Huffman& distcode) {
        static const short lbase[29] = {3,  4,  5,  6,  7,  8,  9,  10, 11,  13,  15,  17,  19,  23, 27,
                                        31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
        static const short lext[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2,
                                       2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
        static const short dbase[30] = {1,   2,   3,   4,   5,   7,    9,    13,   17,   25,   33,   49,   65,    97,    129,
                                        193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
        static const short dext[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6,
                                       6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};
        for (;;) {
            int sym = Decode(lencode);
            if (sym < 256) {
                out_.push_back(uint8_t(sym));
            } else if (sym == 256) {
                return;
            } else {
                sym -= 257;
                if (sym >= 29) throw std::runtime_error("bad length");
                int len = lbase[sym] + Bits(lext[sym]);
                int ds = Decode(distcode);
                if (ds >= 30) throw std::runtime_error("bad distance");
                size_t dist = size_t(dbase[ds]) + Bits(dext[ds]);
                if (dist > out_.size()) throw std::runtime_error("distance too far");
                size_t start = out_.size() - dist;
                for (int i = 0; i < len; ++i) out_.push_back(out_[start + i]);
            }
        }
    }

    void Fixed() {
        static Huffman lencode, distcode;
        static bool built = false;
        if (!built) {
            short lengths[288];
            int sym = 0;
            for (; sym < 144; ++sym) lengths[sym] = 8;
            for (; sym < 256; ++sym) lengths[sym] = 9;
            for (; sym < 280; ++sym) lengths[sym] = 7;
            for (; sym < 288; ++sym) lengths[sym] = 8;
            Construct(lencode, lengths, 288);
            for (sym = 0; sym < 30; ++sym) lengths[sym] = 5;
            Construct(distcode, lengths, 30);
            built = true;
        }
        Codes(lencode, distcode);
    }

    void Dynamic() {
        static const short order[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
        short lengths[320];
        Huffman lencode, distcode;
        int nlen = Bits(5) + 257, ndist = Bits(5) + 1, ncode = Bits(4) + 4;
        if (nlen > 286 || ndist > 30) throw std::runtime_error("bad counts");
        int index = 0;
        for (; index < ncode; ++index) lengths[order[index]] = short(Bits(3));
        for (; index < 19; ++index) lengths[order[index]] = 0;
        if (Construct(lencode, lengths, 19) != 0) throw std::runtime_error("bad code lengths");
        index = 0;
        while (index < nlen + ndist) {
            int sym = Decode(lencode);
            if (sym < 16) {
                lengths[index++] = short(sym);
            } else {
                short len = 0;
                if (sym == 16) {
                    if (index == 0) throw std::runtime_error("repeat without length");
                    len = lengths[index - 1];
                    sym = 3 + Bits(2);
                } else if (sym == 17) {
                    sym = 3 + Bits(3);
                } else {
                    sym = 11 + Bits(7);
                }
                if (index + sym > nlen + ndist) throw std::runtime_error("too many lengths");
                while (sym--) lengths[index++] = len;
            }
        }
        if (lengths[256] == 0) throw std::runtime_error("no end code");
        int err = Construct(lencode, lengths, nlen);
        if (err && (err < 0 || nlen != lencode.count[0] + lencode.count[1])) throw std::runtime_error("bad lencode");
        err = Construct(distcode, lengths + nlen, ndist);
        if (err && (err < 0 || ndist != distcode.count[0] + distcode.count[1]))
            throw std::runtime_error("bad distcode");
        Codes(lencode, distcode);
    }

    const uint8_t* in_;
    size_t n_;
    size_t pos_ = 0;
    uint32_t bitbuf_ = 0;
    int bitcnt_ = 0;
    std::vector<uint8_t>& out_;
};

uint16_t Le16(const std::vector<uint8_t>& d, size_t at) {
    if (at + 2 > d.size()) Fail(L"ZIP dañado");
    return uint16_t(d[at] | (d[at + 1] << 8));
}
uint32_t Le32(const std::vector<uint8_t>& d, size_t at) {
    if (at + 4 > d.size()) Fail(L"ZIP dañado");
    return uint32_t(d[at]) | (uint32_t(d[at + 1]) << 8) | (uint32_t(d[at + 2]) << 16) | (uint32_t(d[at + 3]) << 24);
}

}  // namespace

std::vector<uint8_t> Inflate(const uint8_t* data, size_t size, size_t expected) {
    std::vector<uint8_t> out;
    out.reserve(expected ? expected : size * 4);
    try {
        Inflater(data, size, out).Run();
    } catch (const std::exception&) {
        Fail(L"Datos comprimidos dañados");
    }
    return out;
}

void ZipWriter::Add(const std::string& name, const void* data, size_t size) {
    Entry e{name, Crc32(data, size), uint32_t(size), uint32_t(buf_.size())};
    Put32(buf_, 0x04034b50);
    Put16(buf_, 20);
    Put16(buf_, 0x0800);  // UTF-8 names
    Put16(buf_, 0);       // stored
    Put16(buf_, 0);       // time
    Put16(buf_, 0x5A21);  // date: 2025-01-01
    Put32(buf_, e.crc);
    Put32(buf_, e.size);
    Put32(buf_, e.size);
    Put16(buf_, uint16_t(name.size()));
    Put16(buf_, 0);
    buf_ += name;
    buf_.append(static_cast<const char*>(data), size);
    entries_.push_back(std::move(e));
}

std::string ZipWriter::Finish() {
    size_t cdStart = buf_.size();
    for (const auto& e : entries_) {
        Put32(buf_, 0x02014b50);
        Put16(buf_, 20);
        Put16(buf_, 20);
        Put16(buf_, 0x0800);
        Put16(buf_, 0);
        Put16(buf_, 0);
        Put16(buf_, 0x5A21);
        Put32(buf_, e.crc);
        Put32(buf_, e.size);
        Put32(buf_, e.size);
        Put16(buf_, uint16_t(e.name.size()));
        Put16(buf_, 0);
        Put16(buf_, 0);
        Put16(buf_, 0);
        Put16(buf_, 0);
        Put32(buf_, 0);
        Put32(buf_, e.offset);
        buf_ += e.name;
    }
    size_t cdSize = buf_.size() - cdStart;
    Put32(buf_, 0x06054b50);
    Put16(buf_, 0);
    Put16(buf_, 0);
    Put16(buf_, uint16_t(entries_.size()));
    Put16(buf_, uint16_t(entries_.size()));
    Put32(buf_, uint32_t(cdSize));
    Put32(buf_, uint32_t(cdStart));
    Put16(buf_, 0);
    return std::move(buf_);
}

ZipReader::ZipReader(const fs::path& path) : data_(ReadFileBytes(path)) {
    if (data_.size() < 22) Fail(L"No es un archivo ZIP/DOCX válido");
    size_t eocd = std::string::npos;
    size_t stop = data_.size() > 65557 ? data_.size() - 65557 : 0;
    for (size_t i = data_.size() - 22 + 1; i-- > stop;)
        if (Le32(data_, i) == 0x06054b50) {
            eocd = i;
            break;
        }
    if (eocd == std::string::npos) Fail(L"No es un archivo ZIP/DOCX válido");
    uint16_t count = Le16(data_, eocd + 10);
    size_t at = Le32(data_, eocd + 16);
    for (uint16_t i = 0; i < count; ++i) {
        if (Le32(data_, at) != 0x02014b50) Fail(L"ZIP dañado");
        Entry e;
        e.method = Le16(data_, at + 10);
        e.csize = Le32(data_, at + 20);
        e.usize = Le32(data_, at + 24);
        uint16_t nameLen = Le16(data_, at + 28), extraLen = Le16(data_, at + 30), commentLen = Le16(data_, at + 32);
        e.localOffset = Le32(data_, at + 42);
        if (at + 46 + nameLen > data_.size()) Fail(L"ZIP dañado");
        e.name.assign(reinterpret_cast<const char*>(data_.data() + at + 46), nameLen);
        entries_.push_back(std::move(e));
        at += 46 + nameLen + extraLen + commentLen;
    }
}

const ZipReader::Entry* ZipReader::Find(const std::string& name) const {
    for (const auto& e : entries_)
        if (e.name == name) return &e;
    return nullptr;
}

bool ZipReader::Has(const std::string& name) const { return Find(name) != nullptr; }

std::vector<std::string> ZipReader::Names() const {
    std::vector<std::string> names;
    for (const auto& e : entries_) names.push_back(e.name);
    return names;
}

std::vector<uint8_t> ZipReader::Read(const std::string& name) const {
    const Entry* e = Find(name);
    if (!e) Fail(L"Falta " + Utf8ToWide(name) + L" en el archivo");
    size_t lh = e->localOffset;
    if (Le32(data_, lh) != 0x04034b50) Fail(L"ZIP dañado");
    size_t start = lh + 30 + Le16(data_, lh + 26) + Le16(data_, lh + 28);
    if (start + e->csize > data_.size()) Fail(L"ZIP dañado");
    const uint8_t* p = data_.data() + start;
    if (e->method == 0) return std::vector<uint8_t>(p, p + e->csize);
    if (e->method == 8) return Inflate(p, e->csize, e->usize);
    Fail(L"Método de compresión ZIP no soportado");
}
