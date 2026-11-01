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

#include "inputleap/FileTransfer.h"
#include "io/filesystem.h"

#include <gtest/gtest.h>

#include <fstream>
#include <string>

namespace inputleap {

namespace {

class TempDirectory {
public:
    TempDirectory()
    {
        static unsigned counter = 0;
        directory_ = fs::temp_directory_path() /
                     ("inputleap-file-transfer-test-" + std::to_string(counter++));
        fs::remove_all(directory_);
        fs::create_directories(directory_);
    }

    ~TempDirectory()
    {
        std::error_code error;
        fs::remove_all(directory_, error);
    }

    const fs::path& path() const { return directory_; }

private:
    fs::path directory_;
};

std::string read_file(const fs::path& path)
{
    std::ifstream file(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(file)),
                       std::istreambuf_iterator<char>());
}

} // namespace

TEST(FileTransferTests, sanitize_strips_directories_and_control_characters)
{
    EXPECT_EQ("report.pdf", sanitize_transfer_name("../../report.pdf"));
    EXPECT_EQ("report.pdf", sanitize_transfer_name("C:\\tmp\\report.pdf"));
    EXPECT_EQ("file", sanitize_transfer_name(""));
    EXPECT_EQ("file", sanitize_transfer_name(".."));
    EXPECT_EQ("ab", sanitize_transfer_name(std::string("a\0b", 3)));
    EXPECT_EQ("a_b.txt", sanitize_transfer_name("a:b.txt"));
    EXPECT_EQ("trail", sanitize_transfer_name("trail... "));
    EXPECT_EQ("_CON", sanitize_transfer_name("CON"));
}

TEST(FileTransferTests, unique_path_does_not_overwrite)
{
    TempDirectory temp;
    const fs::path first = unique_transfer_path(temp.path(), "photo.png");
    {
        std::ofstream file(first);
        file << "x";
    }

    const fs::path second = unique_transfer_path(temp.path(), "photo.png");
    EXPECT_EQ((temp.path() / "photo (1).png").u8string(), second.u8string());
    EXPECT_FALSE(fs::exists(second));
}

TEST(FileTransferTests, receiver_writes_file_to_downloads_directory)
{
    TempDirectory temp;
    FileTransferReceiver receiver(temp.path());

    std::string error;
    ASSERT_TRUE(receiver.offer(7, 11, "../hello.txt", error)) << error;
    std::string message;
    EXPECT_EQ(FileTransferReceiver::Result::InProgress,
              receiver.data(7, kDataChunk, "hello ", message));
    EXPECT_EQ(FileTransferReceiver::Result::InProgress,
              receiver.data(7, kDataChunk, "world", message));
    EXPECT_EQ(FileTransferReceiver::Result::Finished,
              receiver.data(7, kDataEnd, "", message));

    EXPECT_EQ((temp.path() / "hello.txt").u8string(), message);
    EXPECT_EQ("hello world", read_file(temp.path() / "hello.txt"));
    EXPECT_FALSE(receiver.active());
}

TEST(FileTransferTests, receiver_rejects_a_second_transfer)
{
    TempDirectory temp;
    FileTransferReceiver receiver(temp.path());

    std::string error;
    ASSERT_TRUE(receiver.offer(1, 4, "a.txt", error)) << error;
    EXPECT_FALSE(receiver.offer(2, 4, "b.txt", error));
    EXPECT_FALSE(error.empty());

    receiver.abort();
    EXPECT_TRUE(receiver.offer(3, 0, "empty.txt", error)) << error;
    std::string message;
    EXPECT_EQ(FileTransferReceiver::Result::Finished,
              receiver.data(3, kDataEnd, "", message));
    EXPECT_TRUE(fs::exists(temp.path() / "empty.txt"));
}

TEST(FileTransferTests, receiver_rejects_truncated_and_oversized_data)
{
    TempDirectory temp;
    FileTransferReceiver receiver(temp.path());

    std::string error;
    ASSERT_TRUE(receiver.offer(4, 5, "short.txt", error)) << error;
    std::string message;
    EXPECT_EQ(FileTransferReceiver::Result::Error,
              receiver.data(4, kDataChunk, "toolong", message));
    EXPECT_FALSE(fs::exists(temp.path() / "short.txt"));

    ASSERT_TRUE(receiver.offer(5, 10, "short.txt", error)) << error;
    EXPECT_EQ(FileTransferReceiver::Result::Error,
              receiver.data(5, kDataEnd, "", message));
    EXPECT_FALSE(fs::exists(temp.path() / "short.txt"));
}

TEST(FileTransferTests, receiver_handles_peer_error)
{
    TempDirectory temp;
    FileTransferReceiver receiver(temp.path());

    std::string error;
    ASSERT_TRUE(receiver.offer(6, 100, "broken.bin", error)) << error;
    std::string message;
    EXPECT_EQ(FileTransferReceiver::Result::Error,
              receiver.data(6, kDataError, "sender aborted", message));
    EXPECT_EQ("sender aborted", message);
    EXPECT_FALSE(fs::exists(temp.path() / "broken.bin"));
}


} // namespace inputleap
