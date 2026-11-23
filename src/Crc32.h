// SPDX-License-Identifier: Apache-2.0
//
// Plain CRC-32 (IEEE 802.3, reflected, poly 0xEDB88320), identical to
// Python's zlib.crc32(). Used to verify serial file transfers.
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace madpico {

// Incremental use: crc = crc32_update(crc, ...) starting from 0
inline uint32_t crc32_update(uint32_t crc, const uint8_t* data, size_t len) {
	crc = ~crc;
	for (size_t i = 0; i < len; ++i) {
		crc ^= data[i];
		for (int k = 0; k < 8; ++k) {
			crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
		}
	}
	return ~crc;
}

} // namespace madpico
