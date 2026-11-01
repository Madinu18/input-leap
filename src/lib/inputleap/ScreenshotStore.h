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

#include "io/filesystem.h"

#include <string>

namespace inputleap {

//! Stores received screenshots in a temporary, self-pruning directory.
class ScreenshotStore {
public:
    ScreenshotStore();
    explicit ScreenshotStore(const fs::path& directory);

    //! Save a PNG and return its path, or an empty string on failure.
    std::string save(const std::string& png, const std::string& device_name);

    const fs::path& directory() const { return directory_; }

    static fs::path default_directory();

private:
    void prune();

    fs::path directory_;
};

} // namespace inputleap
