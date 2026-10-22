// zip.cpp - see zip.h
#include "zip.h"
#include <windows.h>
#include <cstring>

namespace fc2 {

uint32_t Crc32(const void* data, size_t n, uint32_t crc) {
    static uint32_t table[256]; static bool init = false;
    if (!init) { for (uint32_t i = 0; i < 256; ++i) { uint32_t c = i; for (int k = 0; k < 8; ++k) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1; table[i] = c; } init = true; }
    const uint8_t* p = (const uint8_t*)data; crc = ~crc;
    for (size_t i = 0; i < n; ++i) crc = table[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

bool ZipWriter::Open(const std::wstring& path) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    f_ = h; pos_ = 0; ok_ = true; entries_.clear(); return true;
}
void ZipWriter::Put(const void* p, size_t n) {
    DWORD w = 0;
    if (!f_ || !WriteFile((HANDLE)f_, p, (DWORD)n, &w, nullptr) || w != n) ok_ = false;
    pos_ += (uint32_t)n;
}
static void W16(std::vector<uint8_t>& b, uint16_t v) { b.push_back(v & 0xFF); b.push_back(v >> 8); }
static void W32(std::vector<uint8_t>& b, uint32_t v) { for (int i = 0; i < 4; ++i) b.push_back((v >> (8 * i)) & 0xFF); }
bool ZipWriter::Add(const std::string& name, const void* data, size_t size) {
    if (!f_) return false;
    SYSTEMTIME st; GetLocalTime(&st);
    Entry e{ name, Crc32(data, size), (uint32_t)size, pos_,
             (uint16_t)((st.wHour << 11) | (st.wMinute << 5) | (st.wSecond / 2)),
             (uint16_t)(((st.wYear - 1980) << 9) | (st.wMonth << 5) | st.wDay) };
    std::vector<uint8_t> h;
    W32(h, 0x04034b50); W16(h, 20); W16(h, 0x0800 /*UTF-8 names*/); W16(h, 0 /*stored*/); W16(h, e.time); W16(h, e.date);
    W32(h, e.crc); W32(h, e.size); W32(h, e.size); W16(h, (uint16_t)name.size()); W16(h, 0);
    Put(h.data(), h.size()); Put(name.data(), name.size()); if (size) Put(data, size);
    entries_.push_back(e);
    return ok_;
}
bool ZipWriter::Close() {
    if (!f_) return false;
    uint32_t cdStart = pos_;
    for (const Entry& e : entries_) {
        std::vector<uint8_t> h;
        W32(h, 0x02014b50); W16(h, 20); W16(h, 20); W16(h, 0x0800); W16(h, 0); W16(h, e.time); W16(h, e.date);
        W32(h, e.crc); W32(h, e.size); W32(h, e.size); W16(h, (uint16_t)e.name.size()); W16(h, 0); W16(h, 0); W16(h, 0); W16(h, 0);
        W32(h, 0); W32(h, e.offset);
        Put(h.data(), h.size()); Put(e.name.data(), e.name.size());
    }
    std::vector<uint8_t> t;
    W32(t, 0x06054b50); W16(t, 0); W16(t, 0); W16(t, (uint16_t)entries_.size()); W16(t, (uint16_t)entries_.size());
    W32(t, pos_ - cdStart); W32(t, cdStart); W16(t, 0);
    Put(t.data(), t.size());
    CloseHandle((HANDLE)f_); f_ = nullptr;
    return ok_;
}
ZipWriter::~ZipWriter() { if (f_) CloseHandle((HANDLE)f_); }

} // namespace fc2
