/*
 * InputLeap -- mouse and keyboard sharing utility
 * Copyright (C) 2026 InputLeap contributors
 *
 * This package is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * found in the file LICENSE that should have accompanied this file.
 *
 * This package is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#pragma once

#include "inputleap/protocol_types.h"

#include <cstddef>
#include <cstdint>
#include <string>

namespace inputleap {

class IStream;

// Hard upper bound for a transferred screenshot. A 4K PNG is usually a
// few megabytes, so this only rejects malformed or malicious transfers.
static constexpr std::size_t kMaxScreenshotSize = 64 * 1024 * 1024;

class ScreenshotChunk {
public:
    static ScreenshotChunk start(std::uint32_t requestId, std::size_t size);
    static ScreenshotChunk data(std::uint32_t requestId, const std::string& data);
    static ScreenshotChunk end(std::uint32_t requestId);
    static ScreenshotChunk failure(std::uint32_t requestId, const std::string& error);

    // Assembles the next chunk from the stream. Returns one of EDataReceived.
    // On kStart the expected size is stored in expectedSize and data is
    // cleared, on kNotFinish the chunk is appended to data and on kFinish
    // data holds the complete payload. On kError data may contain an error
    // message sent by the peer.
    static int assemble(inputleap::IStream* stream, std::uint32_t& requestId,
                        std::size_t& expectedSize, std::string& data);

    std::uint32_t requestId_ = 0;
    std::uint8_t mark_ = 0;
    std::string data_;
};

} // namespace inputleap
