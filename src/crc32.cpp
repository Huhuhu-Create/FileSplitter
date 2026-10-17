// ============================================================================
//  crc32.cpp  ——  标准 CRC-32（IEEE 802.3，ZIP 用的那个）
//  4bit 查表 + 8bit 查表混合，速度够快且代码短。
// ============================================================================
#include "crc32.h"

namespace fsplit {

static unsigned int g_table[256];
static bool g_ready = false;

static void InitTable() {
    for (unsigned int i = 0; i < 256; ++i) {
        unsigned int c = i;
        for (int k = 0; k < 8; ++k) c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        g_table[i] = c;
    }
    g_ready = true;
}

unsigned int Crc32Update(unsigned int crc, const void* data, size_t len) {
    if (!g_ready) InitTable();
    const unsigned char* p = static_cast<const unsigned char*>(data);
    unsigned int c = crc ^ 0xFFFFFFFFu;
    for (size_t i = 0; i < len; ++i) c = g_table[(c ^ p[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

unsigned int Crc32(const void* data, size_t len) {
    return Crc32Update(0u, data, len);
}

}  // namespace fsplit
