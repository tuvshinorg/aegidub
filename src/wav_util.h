// Copyright (c) 2026, aegidub contributors
//
// Permission to use, copy, modify, and distribute this software for any
// purpose with or without fee is hereby granted, provided that the above
// copyright notice and this permission notice appear in all copies.
//
// THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
// WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
// MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
// ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
// WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
// ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
// OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.

/// @file wav_util.h
/// @brief Writing 16-bit PCM WAV headers

#pragma once

#include <cstdint>
#include <ostream>

namespace wav_util {
	/// Write the 44-byte header of a 16-bit PCM WAV file
	/// @param data_bytes Size of the sample data that follows
	inline void WriteHeader(std::ostream& out, int channels, int rate, uint32_t data_bytes) {
		// WAV is little-endian whatever the machine is
		auto put32 = [&](uint32_t v) {
			const char b[4] = {char(v & 0xFF), char((v >> 8) & 0xFF), char((v >> 16) & 0xFF), char(v >> 24)};
			out.write(b, 4);
		};
		auto put16 = [&](uint16_t v) {
			const char b[2] = {char(v & 0xFF), char(v >> 8)};
			out.write(b, 2);
		};
		out.write("RIFF", 4); put32(36 + data_bytes);
		out.write("WAVEfmt ", 8); put32(16);
		put16(1); // PCM
		put16(static_cast<uint16_t>(channels));
		put32(static_cast<uint32_t>(rate));
		put32(static_cast<uint32_t>(rate * channels * 2));
		put16(static_cast<uint16_t>(channels * 2));
		put16(16);
		out.write("data", 4); put32(data_bytes);
	}
}
