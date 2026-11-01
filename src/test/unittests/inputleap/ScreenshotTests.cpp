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

#include "inputleap/Screenshot.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <string>

namespace inputleap {

namespace {

std::uint32_t read_be32(const std::string& data, std::size_t offset)
{
    return (static_cast<std::uint32_t>(static_cast<std::uint8_t>(data[offset])) << 24) |
           (static_cast<std::uint32_t>(static_cast<std::uint8_t>(data[offset + 1])) << 16) |
           (static_cast<std::uint32_t>(static_cast<std::uint8_t>(data[offset + 2])) << 8) |
           static_cast<std::uint32_t>(static_cast<std::uint8_t>(data[offset + 3]));
}

Screenshot make_screenshot()
{
    Screenshot screenshot;
    screenshot.width = 2;
    screenshot.height = 1;
    screenshot.rgb = {255, 0, 0, 0, 255, 0};
    return screenshot;
}

} // namespace

TEST(ScreenshotTests, valid_requires_matching_pixel_data)
{
    Screenshot screenshot = make_screenshot();
    EXPECT_TRUE(screenshot.valid());

    screenshot.rgb.pop_back();
    EXPECT_FALSE(screenshot.valid());

    screenshot = make_screenshot();
    screenshot.width = 0;
    EXPECT_FALSE(screenshot.valid());
}

TEST(ScreenshotTests, encode_png_writes_a_rgb_image)
{
    const Screenshot screenshot = make_screenshot();
    std::string png;
    std::string error;

    ASSERT_TRUE(encode_png(screenshot, png, error)) << error;
    ASSERT_GE(png.size(), 8u + 25u + 12u);

    const std::string signature("\x89PNG\r\n\x1a\n", 8);
    EXPECT_EQ(signature, png.substr(0, 8));

    // IHDR is the first chunk and has a fixed 13 byte payload.
    EXPECT_EQ(13u, read_be32(png, 8));
    EXPECT_EQ("IHDR", png.substr(12, 4));
    EXPECT_EQ(2u, read_be32(png, 16));  // width
    EXPECT_EQ(1u, read_be32(png, 20));  // height
    EXPECT_EQ(8u, static_cast<std::uint8_t>(png[24]));  // bit depth
    EXPECT_EQ(2u, static_cast<std::uint8_t>(png[25]));  // colour type: RGB

    EXPECT_EQ("IEND", png.substr(png.size() - 8, 4));
}

TEST(ScreenshotTests, encode_png_rejects_invalid_images)
{
    Screenshot screenshot = make_screenshot();
    screenshot.width = 3;

    std::string png;
    std::string error;
    EXPECT_FALSE(encode_png(screenshot, png, error));
    EXPECT_FALSE(error.empty());
    EXPECT_TRUE(png.empty());
}

} // namespace inputleap
