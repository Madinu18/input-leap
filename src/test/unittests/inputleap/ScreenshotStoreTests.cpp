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

#include "inputleap/ScreenshotStore.h"
#include "io/filesystem.h"

#include <gtest/gtest.h>

#include <chrono>
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
                     ("inputleap-screenshot-test-" + std::to_string(counter++));
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

std::size_t count_png_files(const fs::path& directory)
{
    std::size_t count = 0;
    for (const fs::directory_entry& entry : fs::directory_iterator(directory)) {
        if (entry.is_regular_file() && entry.path().extension() == ".png") {
            ++count;
        }
    }
    return count;
}

void write_file(const fs::path& path, const std::string& contents)
{
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file.write(contents.data(), static_cast<std::streamsize>(contents.size()));
}

} // namespace

TEST(ScreenshotStoreTests, save_writes_png_inside_the_store)
{
    TempDirectory temp;
    ScreenshotStore store(temp.path());

    const std::string path = store.save("png-bytes", "client");
    ASSERT_FALSE(path.empty());
    EXPECT_TRUE(fs::exists(fs::u8path(path)));
    EXPECT_EQ(".png", fs::u8path(path).extension().u8string());

    std::ifstream file(fs::u8path(path), std::ios::binary);
    const std::string contents((std::istreambuf_iterator<char>(file)),
                               std::istreambuf_iterator<char>());
    EXPECT_EQ("png-bytes", contents);
}

TEST(ScreenshotStoreTests, device_name_cannot_escape_the_directory)
{
    TempDirectory temp;
    ScreenshotStore store(temp.path());

    const std::string path = store.save("data", "../../evil/name");
    ASSERT_FALSE(path.empty());

    const fs::path saved = fs::u8path(path);
    EXPECT_EQ(temp.path(), saved.parent_path());
    EXPECT_EQ(std::string::npos, saved.filename().u8string().find('/'));
    EXPECT_NE('.', saved.filename().u8string().front());
}

TEST(ScreenshotStoreTests, repeated_saves_get_unique_names)
{
    TempDirectory temp;
    ScreenshotStore store(temp.path());

    const std::string first = store.save("one", "client");
    const std::string second = store.save("two", "client");
    ASSERT_FALSE(first.empty());
    ASSERT_FALSE(second.empty());
    EXPECT_NE(first, second);
    EXPECT_EQ(2u, count_png_files(temp.path()));
}

TEST(ScreenshotStoreTests, old_files_are_pruned_by_count)
{
    TempDirectory temp;
    for (unsigned i = 0; i < 60; ++i) {
        write_file(temp.path() / ("old-" + std::to_string(i) + ".png"), "old");
    }

    ScreenshotStore store(temp.path());
    ASSERT_FALSE(store.save("new", "client").empty());
    EXPECT_LE(count_png_files(temp.path()), 50u);
}

TEST(ScreenshotStoreTests, old_files_are_pruned_by_age)
{
    TempDirectory temp;
    const fs::path old_file = temp.path() / "ancient.png";
    write_file(old_file, "old");

    const auto old_time = fs::file_time_type::clock::now() - std::chrono::hours(24 * 8);
    fs::last_write_time(old_file, old_time);

    ScreenshotStore store(temp.path());
    ASSERT_FALSE(store.save("new", "client").empty());
    EXPECT_FALSE(fs::exists(old_file));
}

TEST(ScreenshotStoreTests, empty_payload_is_rejected)
{
    TempDirectory temp;
    ScreenshotStore store(temp.path());

    EXPECT_TRUE(store.save("", "client").empty());
    EXPECT_EQ(0u, count_png_files(temp.path()));
}

} // namespace inputleap
