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

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

namespace inputleap {

namespace {

void append_png_data(void* context, void* data, int size)
{
    auto* output = static_cast<std::string*>(context);
    output->append(static_cast<const char*>(data), static_cast<std::size_t>(size));
}

} // namespace

bool encode_png(const Screenshot& screenshot, std::string& png, std::string& error)
{
    png.clear();
    error.clear();

    if (!screenshot.valid()) {
        error = "screenshot has invalid dimensions or pixel data";
        return false;
    }

    const int written = stbi_write_png_to_func(
        append_png_data, &png, screenshot.width, screenshot.height, 3,
        screenshot.rgb.data(), screenshot.width * 3);
    if (written == 0 || png.empty()) {
        png.clear();
        error = "failed to encode the screenshot as PNG";
        return false;
    }

    return true;
}

} // namespace inputleap
