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

#include <cstdint>
#include <string>
#include <vector>

namespace inputleap {

//! A platform independent screenshot.
/*!
Pixels are stored as tightly packed 8-bit RGB, rows from top to bottom.
*/
struct Screenshot {
    std::int32_t width = 0;
    std::int32_t height = 0;
    std::vector<std::uint8_t> rgb;
    std::string error;

    bool valid() const
    {
        return width > 0 && height > 0 &&
               rgb.size() == static_cast<std::size_t>(width) *
                                 static_cast<std::size_t>(height) * 3;
    }
};

//! Encode a screenshot as PNG.
bool encode_png(const Screenshot& screenshot, std::string& png, std::string& error);

//! Decode a PNG into the internal clipboard bitmap format.
/*!
The result is a BMP without the 14 byte file header (a 40 byte
BITMAPINFOHEADER followed by 32bpp BI_RGB pixels), which is the layout
\c IClipboard::kBitmap expects.
*/
bool decode_png_to_clipboard_bitmap(const std::string& png, std::string& bitmap,
                                    std::string& error);

//! Encode the internal clipboard bitmap format as PNG.
bool bmp_to_png(const std::string& bitmap, std::string& png, std::string& error);

} // namespace inputleap
