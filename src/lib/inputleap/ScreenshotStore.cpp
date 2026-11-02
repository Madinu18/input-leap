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

#include "common/DataDirectories.h"
#include "base/Log.h"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <system_error>
#include <vector>

namespace inputleap {

namespace {

// Screenshots are convenience artifacts, not documents. Keeping the newest
// few files bounded by both age and size means the cache cannot grow forever
// even on machines that take 4K screenshots all day.
constexpr std::size_t kMaxScreenshotFiles = 50;
constexpr std::uintmax_t kMaxScreenshotBytes = 512ull * 1024 * 1024;
constexpr auto kMaxScreenshotAge = std::chrono::hours(24 * 7);

std::string sanitize_device_name(const std::string& device_name)
{
    std::string result;
    result.reserve(device_name.size());

    for (unsigned char c : device_name) {
        if (std::isalnum(c) != 0 || c == '-' || c == '_' || c == '.') {
            result.push_back(static_cast<char>(c));
        }
        else {
            result.push_back('_');
        }
    }

    // Never produce hidden files or names that can escape the directory.
    while (!result.empty() && (result.front() == '.' || result.front() == '-')) {
        result.erase(result.begin());
    }

    if (result.size() > 40) {
        result.resize(40);
    }

    return result.empty() ? "screenshot" : result;
}

std::string timestamp()
{
    const std::time_t now = std::time(nullptr);
    const std::tm* local = std::localtime(&now);
    if (local == nullptr) {
        return "unknown";
    }

    std::ostringstream stream;
    stream << std::put_time(local, "%Y%m%d-%H%M%S");
    return stream.str();
}

} // namespace

ScreenshotStore::ScreenshotStore() : directory_(default_directory()) {}

ScreenshotStore::ScreenshotStore(const fs::path& directory) : directory_(directory) {}

fs::path ScreenshotStore::default_directory()
{
    return DataDirectories::cache() / "screenshots";
}

std::string ScreenshotStore::save(const std::string& png,
                                  const std::string& device_name)
{
    if (png.empty()) {
        LOG_ERR("refusing to save an empty screenshot");
        return {};
    }

    std::error_code error;
    fs::create_directories(directory_, error);
    if (error) {
        LOG_ERR("could not create screenshot directory %s: %s",
                directory_.u8string().c_str(), error.message().c_str());
        return {};
    }

    const std::string base = sanitize_device_name(device_name) + "-" + timestamp();

    fs::path path = directory_ / (base + ".png");
    for (std::uint32_t suffix = 1; fs::exists(path, error); ++suffix) {
        path = directory_ / (base + "-" + std::to_string(suffix) + ".png");
        if (error) {
            LOG_ERR("could not inspect screenshot path: %s", error.message().c_str());
            return {};
        }
    }

    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file) {
        LOG_ERR("could not open %s for writing", path.u8string().c_str());
        return {};
    }

    file.write(png.data(), static_cast<std::streamsize>(png.size()));
    file.close();
    if (!file) {
        LOG_ERR("could not write screenshot to %s", path.u8string().c_str());
        return {};
    }

    prune();

    LOG_NOTE("screenshot saved to %s", path.u8string().c_str());
    return path.u8string();
}

void ScreenshotStore::prune()
{
    struct Entry {
        fs::path path;
        fs::file_time_type write_time;
        std::uintmax_t size;
    };

    std::error_code error;
    std::vector<Entry> entries;

    for (fs::directory_iterator it(directory_, error), end;
         !error && it != end; it.increment(error)) {
        const fs::directory_entry& item = *it;
        std::error_code item_error;

        if (!item.is_regular_file(item_error) || item_error) {
            continue;
        }
        if (item.path().extension() != ".png") {
            continue;
        }

        const auto write_time = item.last_write_time(item_error);
        if (item_error) {
            continue;
        }

        const auto size = item.file_size(item_error);
        if (item_error) {
            continue;
        }

        entries.push_back({item.path(), write_time, size});
    }

    std::sort(entries.begin(), entries.end(),
              [](const Entry& a, const Entry& b) { return a.write_time < b.write_time; });

    const auto now = fs::file_time_type::clock::now();
    std::size_t remaining = entries.size();
    std::uintmax_t remaining_bytes = 0;
    for (const Entry& entry : entries) {
        remaining_bytes += entry.size;
    }

    for (const Entry& entry : entries) {
        const bool too_old = now - entry.write_time > kMaxScreenshotAge;
        const bool too_many = remaining > kMaxScreenshotFiles;
        const bool too_large = remaining_bytes > kMaxScreenshotBytes;
        if (!too_old && !too_many && !too_large) {
            break;
        }

        std::error_code remove_error;
        if (!fs::remove(entry.path, remove_error) || remove_error) {
            LOG_DEBUG1("could not remove old screenshot %s: %s",
                       entry.path.u8string().c_str(),
                       remove_error.message().c_str());
            continue;
        }

        --remaining;
        remaining_bytes -= (std::min)(remaining_bytes, entry.size);
    }
}

} // namespace inputleap
