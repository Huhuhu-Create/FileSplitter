#pragma once
// crc32.h —— ZIP 标准 CRC-32
#include <cstddef>
#include <cstdint>

namespace fsplit {

unsigned int Crc32(const void* data, size_t len);
unsigned int Crc32Update(unsigned int crc, const void* data, size_t len);

}  // namespace fsplit
