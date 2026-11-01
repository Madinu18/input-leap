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

#include <gtest/gtest.h>

#include <algorithm>
#include <cstring>
#include <string>

namespace inputleap {

namespace {

class MemoryStream : public IStream {
public:
    void close() override {}
    void flush() override {}
    void shutdownInput() override {}
    void shutdownOutput() override {}
    const EventTarget* get_event_target() const override { return nullptr; }
    bool isReady() const override { return position_ < data_.size(); }
    std::uint32_t getSize() const override
    {
        return static_cast<std::uint32_t>(data_.size() - position_);
    }

    std::uint32_t read(void* buffer, std::uint32_t n) override
    {
        const std::size_t available = data_.size() - position_;
        const std::size_t count = std::min<std::size_t>(n, available);
        if (buffer != nullptr && count > 0) {
            std::memcpy(buffer, data_.data() + position_, count);
        }
        position_ += count;
        return static_cast<std::uint32_t>(count);
    }

    void write(const void* buffer, std::uint32_t n) override
    {
        data_.append(static_cast<const char*>(buffer), n);
    }

    void write_chunk(const ScreenshotChunk& chunk)
    {
        ScreenshotChunk copy = chunk;
        ProtocolUtil::writef(this, kMsgDScreenshot, copy.requestId_, copy.mark_,
                             &copy.data_);
    }

    void skip(std::size_t n)
    {
        position_ = std::min(position_ + n, data_.size());
    }

private:
    std::string data_;
    std::size_t position_ = 0;
};

int assemble_next(MemoryStream& stream, std::uint32_t& requestId,
                  std::size_t& expectedSize, std::string& data)
{
    // The message code is consumed by the caller before a chunk is parsed,
    // just like Client::handle_data() does for real connections.
    stream.skip(4);
    return ScreenshotChunk::assemble(&stream, requestId, expectedSize, data);
}

} // namespace

TEST(ScreenshotChunkTests, transfer_round_trip)
{
    MemoryStream stream;
    stream.write_chunk(ScreenshotChunk::start(7, 11));
    stream.write_chunk(ScreenshotChunk::data(7, "hello "));
    stream.write_chunk(ScreenshotChunk::data(7, "world"));
    stream.write_chunk(ScreenshotChunk::end(7));

    std::uint32_t requestId = 0;
    std::size_t expectedSize = 0;
    std::string data;

    ASSERT_EQ(kStart, assemble_next(stream, requestId, expectedSize, data));
    EXPECT_EQ(7u, requestId);
    EXPECT_EQ(11u, expectedSize);
    EXPECT_TRUE(data.empty());

    ASSERT_EQ(kNotFinish, assemble_next(stream, requestId, expectedSize, data));
    EXPECT_EQ("hello ", data);

    ASSERT_EQ(kNotFinish, assemble_next(stream, requestId, expectedSize, data));
    EXPECT_EQ("hello world", data);

    ASSERT_EQ(kFinish, assemble_next(stream, requestId, expectedSize, data));
    EXPECT_EQ("hello world", data);
}

TEST(ScreenshotChunkTests, failure_carries_error_message)
{
    MemoryStream stream;
    stream.write_chunk(ScreenshotChunk::start(3, 9));
    stream.write_chunk(ScreenshotChunk::failure(3, "screen capture is unavailable"));

    std::uint32_t requestId = 0;
    std::size_t expectedSize = 0;
    std::string data;

    ASSERT_EQ(kStart, assemble_next(stream, requestId, expectedSize, data));
    ASSERT_EQ(kError, assemble_next(stream, requestId, expectedSize, data));
    EXPECT_EQ("screen capture is unavailable", data);
}

TEST(ScreenshotChunkTests, truncated_transfer_is_rejected)
{
    MemoryStream stream;
    stream.write_chunk(ScreenshotChunk::start(1, 20));
    stream.write_chunk(ScreenshotChunk::data(1, "short"));
    stream.write_chunk(ScreenshotChunk::end(1));

    std::uint32_t requestId = 0;
    std::size_t expectedSize = 0;
    std::string data;

    ASSERT_EQ(kStart, assemble_next(stream, requestId, expectedSize, data));
    ASSERT_EQ(kNotFinish, assemble_next(stream, requestId, expectedSize, data));
    EXPECT_EQ(kError, assemble_next(stream, requestId, expectedSize, data));
}

TEST(ScreenshotChunkTests, zero_size_transfer_is_rejected)
{
    MemoryStream stream;
    stream.write_chunk(ScreenshotChunk::start(1, 0));

    std::uint32_t requestId = 0;
    std::size_t expectedSize = 0;
    std::string data;

    EXPECT_EQ(kError, assemble_next(stream, requestId, expectedSize, data));
}

TEST(ScreenshotChunkTests, oversized_transfer_is_rejected)
{
    MemoryStream stream;
    stream.write_chunk(ScreenshotChunk::start(1, kMaxScreenshotSize + 1));

    std::uint32_t requestId = 0;
    std::size_t expectedSize = 0;
    std::string data;

    EXPECT_EQ(kError, assemble_next(stream, requestId, expectedSize, data));
}

} // namespace inputleap
