#pragma once
#include "common.h"

#include <string>

// Store-only ZIP writer (enough for DOCX packages).
class ZipWriter {
public:
    void Add(const std::string& name, const void* data, size_t size);
    void Add(const std::string& name, const std::string& data) { Add(name, data.data(), data.size()); }
    std::string Finish();

private:
    struct Entry {
        std::string name;
        uint32_t crc, size, offset;
    };
    std::string buf_;
    std::vector<Entry> entries_;
};

// ZIP reader supporting stored and deflated entries.
class ZipReader {
public:
    explicit ZipReader(const fs::path& path);
    bool Has(const std::string& name) const;
    std::vector<std::string> Names() const;  // in archive order
    std::vector<uint8_t> Read(const std::string& name) const;

private:
    struct Entry {
        std::string name;
        uint16_t method;
        uint32_t csize, usize, localOffset;
    };
    const Entry* Find(const std::string& name) const;
    std::vector<uint8_t> data_;
    std::vector<Entry> entries_;
};

std::vector<uint8_t> Inflate(const uint8_t* data, size_t size, size_t expected = 0);
uint32_t Crc32(const void* data, size_t size);
