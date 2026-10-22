// zip.h - minimal ZIP writer (stored entries, CRC-32) for the log collection.
#pragma once
#include <string>
#include <vector>
#include <cstdint>

namespace fc2 {
class ZipWriter {
public:
    bool Open(const std::wstring& path);
    bool Add(const std::string& name, const void* data, size_t size);
    bool Close();
    ~ZipWriter();
private:
    struct Entry { std::string name; uint32_t crc, size, offset; uint16_t time, date; };
    void* f_ = nullptr; uint32_t pos_ = 0; std::vector<Entry> entries_; bool ok_ = true;
    void Put(const void* p, size_t n);
};
uint32_t Crc32(const void* data, size_t n, uint32_t crc = 0);
}
