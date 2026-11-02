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
#include "stb_image_write.h"

#include <algorithm>
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

void put_u16(std::string& out, std::uint16_t value)
{
    out.push_back(static_cast<char>(value & 0xff));
    out.push_back(static_cast<char>((value >> 8) & 0xff));
}

void append_png_data(void* context, void* data, int size)
{
    auto* output = static_cast<std::string*>(context);
    output->append(static_cast<const char*>(data), static_cast<std::size_t>(size));
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
    put_u16(bitmap, 1);                                    // biPlanes
    put_u16(bitmap, 32);                                   // biBitCount
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

bool bmp_to_png(const std::string& bitmap, std::string& png, std::string& error)
{
    png.clear();
    error.clear();

    if (bitmap.size() < 40) {
        error = "bitmap is too small";
        return false;
    }

    auto read_u16 = [&](std::size_t offset) {
        return static_cast<std::uint32_t>(
                   static_cast<unsigned char>(bitmap[offset])) |
               (static_cast<std::uint32_t>(
                    static_cast<unsigned char>(bitmap[offset + 1])) << 8);
    };
    auto read_u32 = [&](std::size_t offset) {
        return static_cast<std::uint32_t>(
                   static_cast<unsigned char>(bitmap[offset])) |
               (static_cast<std::uint32_t>(
                    static_cast<unsigned char>(bitmap[offset + 1])) << 8) |
               (static_cast<std::uint32_t>(
                    static_cast<unsigned char>(bitmap[offset + 2])) << 16) |
               (static_cast<std::uint32_t>(
                    static_cast<unsigned char>(bitmap[offset + 3])) << 24);
    };
    auto write_u32 = [](std::string& out, std::size_t offset,
                        std::uint32_t value) {
        out[offset] = static_cast<char>(value & 0xff);
        out[offset + 1] = static_cast<char>((value >> 8) & 0xff);
        out[offset + 2] = static_cast<char>((value >> 16) & 0xff);
        out[offset + 3] = static_cast<char>((value >> 24) & 0xff);
    };

    const std::uint32_t header_size = read_u32(0);
    const std::uint32_t compression = read_u32(16);
    const std::uint32_t colours_used = read_u32(32);
    const std::uint32_t bit_count = read_u16(14);

    std::size_t header_bytes = header_size;
    if (header_size == 40 && compression == 3 /* BI_BITFIELDS */) {
        header_bytes += 12; // three colour masks follow the header
    }

    std::size_t palette_bytes = 0;
    if (bit_count <= 8) {
        const std::uint32_t entries =
            colours_used != 0 ? colours_used : (1u << bit_count);
        palette_bytes = static_cast<std::size_t>(entries) * 4;
    }

    const std::size_t pixel_offset = 14 + header_bytes + palette_bytes;
    const std::size_t file_size = 14 + bitmap.size();
    if (pixel_offset > file_size) {
        error = "bitmap header is larger than the data";
        return false;
    }

    std::string file(file_size, '\0');
    file[0] = 'B';
    file[1] = 'M';
    write_u32(file, 2, static_cast<std::uint32_t>(file_size));
    write_u32(file, 10, static_cast<std::uint32_t>(pixel_offset));
    std::copy(bitmap.begin(), bitmap.end(), file.begin() + 14);

    int width = 0;
    int height = 0;
    int channels = 0;
    unsigned char* pixels = stbi_load_from_memory(
        reinterpret_cast<const unsigned char*>(file.data()),
        static_cast<int>(file.size()), &width, &height, &channels, 3);
    if (pixels == nullptr || width <= 0 || height <= 0) {
        error = stbi_failure_reason() != nullptr ? stbi_failure_reason()
                                                 : "could not decode the bitmap";
        if (pixels != nullptr) {
            stbi_image_free(pixels);
        }
        return false;
    }

    const int written = stbi_write_png_to_func(
        append_png_data, &png, width, height, 3, pixels, width * 3);
    stbi_image_free(pixels);
    if (written == 0 || png.empty()) {
        png.clear();
        error = "could not encode the bitmap as PNG";
        return false;
    }

    return true;
}

} // namespace inputleap
