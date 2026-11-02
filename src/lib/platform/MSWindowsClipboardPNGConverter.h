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

#include "platform/MSWindowsClipboard.h"

namespace inputleap {

//! Convert PNG clipboard data to a Windows CF_DIB bitmap
/*!
Clients on X11 and macOS usually put screenshots on the clipboard as PNG.
Windows applications expect CF_DIB, so decode the PNG into a device
independent bitmap.
*/
class MSWindowsClipboardPNGConverter : public IMSWindowsClipboardConverter
{
public:
    MSWindowsClipboardPNGConverter();
    virtual ~MSWindowsClipboardPNGConverter();

    // IMSWindowsClipboardConverter overrides
    virtual IClipboard::EFormat getFormat() const;
    virtual UINT getWin32Format() const;
    virtual HANDLE fromIClipboard(const std::string&) const;
    virtual std::string toIClipboard(HANDLE) const;
};

} // namespace inputleap
