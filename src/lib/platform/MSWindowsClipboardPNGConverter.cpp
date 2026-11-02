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

#include "platform/MSWindowsClipboardPNGConverter.h"

#include "inputleap/Screenshot.h"
#include "base/Log.h"

namespace inputleap {

MSWindowsClipboardPNGConverter::MSWindowsClipboardPNGConverter()
{
}

MSWindowsClipboardPNGConverter::~MSWindowsClipboardPNGConverter()
{
}

IClipboard::EFormat
MSWindowsClipboardPNGConverter::getFormat() const
{
    return IClipboard::kPNG;
}

UINT
MSWindowsClipboardPNGConverter::getWin32Format() const
{
    // Windows applications paste images as CF_DIB.
    return CF_DIB;
}

HANDLE MSWindowsClipboardPNGConverter::fromIClipboard(const std::string& data) const
{
    std::string bitmap;
    std::string error;
    if (!decode_png_to_clipboard_bitmap(data, bitmap, error)) {
        LOG_ERR("could not convert a PNG clipboard to a bitmap: %s",
                error.c_str());
        return nullptr;
    }

    HGLOBAL gData = GlobalAlloc(GMEM_MOVEABLE | GMEM_DDESHARE, bitmap.size());
    if (gData == nullptr) {
        return nullptr;
    }

    char* dst = static_cast<char*>(GlobalLock(gData));
    if (dst == nullptr) {
        GlobalFree(gData);
        return nullptr;
    }

    memcpy(dst, bitmap.data(), bitmap.size());
    GlobalUnlock(gData);
    return gData;
}

std::string MSWindowsClipboardPNGConverter::toIClipboard(HANDLE) const
{
    // Windows does not provide PNG data on the clipboard, so there is
    // nothing to convert in this direction.
    return {};
}

} // namespace inputleap
