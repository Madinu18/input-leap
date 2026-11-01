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

#include "inputleap/ScreenshotChunk.h"

#include "inputleap/ProtocolUtil.h"
#include "inputleap/protocol_types.h"
#include "io/IStream.h"
#include "base/Log.h"
#include "base/String.h"

namespace inputleap {

ScreenshotChunk ScreenshotChunk::start(std::uint32_t requestId, std::size_t size)
{
    ScreenshotChunk chunk;
    chunk.requestId_ = requestId;
    chunk.mark_ = kDataStart;
    chunk.data_ = std::to_string(size);
    return chunk;
}

ScreenshotChunk ScreenshotChunk::data(std::uint32_t requestId, const std::string& data)
{
    ScreenshotChunk chunk;
    chunk.requestId_ = requestId;
    chunk.mark_ = kDataChunk;
    chunk.data_ = data;
    return chunk;
}

ScreenshotChunk ScreenshotChunk::end(std::uint32_t requestId)
{
    ScreenshotChunk chunk;
    chunk.requestId_ = requestId;
    chunk.mark_ = kDataEnd;
    return chunk;
}

ScreenshotChunk ScreenshotChunk::failure(std::uint32_t requestId, const std::string& error)
{
    ScreenshotChunk chunk;
    chunk.requestId_ = requestId;
    chunk.mark_ = kDataError;
    chunk.data_ = error;
    return chunk;
}

int ScreenshotChunk::assemble(inputleap::IStream* stream, std::uint32_t& requestId,
                              std::size_t& expectedSize, std::string& data)
{
    std::uint8_t mark = 0;
    std::string payload;

    if (!ProtocolUtil::readf(stream, kMsgDScreenshot + 4, &requestId, &mark, &payload)) {
        return kError;
    }

    if (mark == kDataStart) {
        expectedSize = inputleap::string::stringToSizeType(payload);
        data.clear();
        if (expectedSize == 0 || expectedSize > kMaxScreenshotSize) {
            LOG_ERR("invalid screenshot size %zd", expectedSize);
            return kError;
        }
        return kStart;
    }
    else if (mark == kDataChunk) {
        if (data.size() + payload.size() > kMaxScreenshotSize) {
            LOG_ERR("screenshot exceeds the maximum size");
            return kError;
        }
        data.append(payload);
        return kNotFinish;
    }
    else if (mark == kDataEnd) {
        if (expectedSize != data.size()) {
            LOG_ERR("corrupted screenshot data, expected size=%zd actual size=%zd",
                    expectedSize, data.size());
            return kError;
        }
        return kFinish;
    }
    else if (mark == kDataError) {
        data = payload;
        return kError;
    }

    LOG_ERR("screenshot transmission failed: unknown chunk mark %d", mark);
    return kError;
}

} // namespace inputleap
