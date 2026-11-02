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

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

#include <cstdint>

namespace inputleap {

namespace {

void put_u32(std::string& out, std::uint32_t value)
{
    out.push_back(static_cast<char>(value & 0xff));
    out.push_back(static_cast<char>((value >> 8) & 0xff));
    out.push_back(static_cast<char>((value >> 16) & 0xff));
    out.push_back(static_cast<char>((value >> 24) & 0xff));
}

} // namespace

bool decode_png_to_clipboard_bitmap(const std::string& png, std::string& bitmap,
                                    std::string& error)
{
    bitmap.clear();
    error.clear();

    if (png.empty()) {
        error = "empty PNG data";
        return false;
    }

    int width = 0;
    int height = 0;
    int channels = 0;
    // stbi returns tightly packed RGBA, rows from top to bottom.
    unsigned char* pixels = stbi_load_from_memory(
        reinterpret_cast<const unsigned char*>(png.data()),
        static_cast<int>(png.size()), &width, &height, &channels, 4);
    if (pixels == nullptr || width <= 0 || height <= 0) {
        error = stbi_failure_reason() != nullptr ? stbi_failure_reason()
                                                 : "could not decode PNG";
        if (pixels != nullptr) {
            stbi_image_free(pixels);
        }
        return false;
    }

    // The internal clipboard bitmap format is a BMP without the 14 byte
    // BITMAPFILEHEADER: a 40 byte BITMAPINFOHEADER followed by the pixels.
    // Pixels are 32bpp BI_RGB, stored bottom-up as BGRA.
    const std::uint32_t stride = static_cast<std::uint32_t>(width) * 4u;
    const std::uint32_t image_size = stride * static_cast<std::uint32_t>(height);

    bitmap.reserve(40u + image_size);
    put_u32(bitmap, 40);                                   // biSize
    put_u32(bitmap, static_cast<std::uint32_t>(width));    // biWidth
    put_u32(bitmap, static_cast<std::uint32_t>(height));   // biHeight (bottom-up)
    put_u32(bitmap, 1);                                    // biPlanes
    put_u32(bitmap, 32);                                   // biBitCount
    put_u32(bitmap, 0);                                    // biCompression (BI_RGB)
    put_u32(bitmap, image_size);                           // biSizeImage
    put_u32(bitmap, 0);                                    // biXPelsPerMeter
    put_u32(bitmap, 0);                                    // biYPelsPerMeter
    put_u32(bitmap, 0);                                    // biClrUsed
    put_u32(bitmap, 0);                                    // biClrImportant

    for (int y = height - 1; y >= 0; --y) {
        const unsigned char* row = pixels + static_cast<std::size_t>(y) * stride;
        for (int x = 0; x < width; ++x) {
            const unsigned char* rgba = row + static_cast<std::size_t>(x) * 4u;
            bitmap.push_back(static_cast<char>(rgba[2]));  // B
            bitmap.push_back(static_cast<char>(rgba[1]));  // G
            bitmap.push_back(static_cast<char>(rgba[0]));  // R
            bitmap.push_back(static_cast<char>(0xff));     // A (opaque)
        }
    }

    stbi_image_free(pixels);
    return true;
}

} // namespace inputleap
