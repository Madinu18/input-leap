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

#include "inputleap/FileTransfer.h"

#include "common/DataDirectories.h"
#include "base/Event.h"
#include "base/IEventQueue.h"
#include "base/Log.h"
#include "base/Time.h"

#include <algorithm>
#include <cctype>
#include <system_error>

namespace inputleap {

namespace {

constexpr double kProgressInterval = 0.1;

fs::path make_part_path(const fs::path& final_path)
{
    return fs::u8path(final_path.u8string() + ".part");
}

} // namespace

std::string sanitize_transfer_name(const std::string& name)
{
    const auto separator = name.find_last_of("/\\");
    std::string base = separator == std::string::npos ? name : name.substr(separator + 1);

    std::string result;
    result.reserve(base.size());
    for (unsigned char c : base) {
        if (c < 0x20 || c == 0x7f) {
            continue;
        }
        if (c == '<' || c == '>' || c == ':' || c == '"' ||
            c == '|' || c == '?' || c == '*') {
            result.push_back('_');
        }
        else {
            result.push_back(static_cast<char>(c));
        }
    }

    while (!result.empty() &&
           (result.back() == '.' || result.back() == ' ')) {
        result.pop_back();
    }

    if (result.empty() || result == "." || result == "..") {
        result = "file";
    }
    if (result.size() > 200) {
        result.resize(200);
    }

    // Windows refuses to create these device names.
    std::string stem = result;
    const std::size_t dot = stem.find('.');
    if (dot != std::string::npos) {
        stem.resize(dot);
    }
    std::transform(stem.begin(), stem.end(), stem.begin(),
                   [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    static const char* reserved[] = {
        "CON", "PRN", "AUX", "NUL",
        "COM1", "COM2", "COM3", "COM4", "COM5", "COM6", "COM7", "COM8", "COM9",
        "LPT1", "LPT2", "LPT3", "LPT4", "LPT5", "LPT6", "LPT7", "LPT8", "LPT9"};
    for (const char* name : reserved) {
        if (stem == name) {
            result.insert(result.begin(), '_');
            break;
        }
    }

    return result;
}

fs::path unique_transfer_path(const fs::path& directory, const std::string& name)
{
    const std::string safe_name = sanitize_transfer_name(name);
    fs::path candidate = directory / fs::u8path(safe_name);
    if (!fs::exists(candidate)) {
        return candidate;
    }

    const fs::path original = fs::u8path(safe_name);
    const std::string stem = original.stem().u8string();
    const std::string extension = original.extension().u8string();
    for (unsigned i = 1; i < 10000; ++i) {
        candidate = directory / fs::u8path(
            stem + " (" + std::to_string(i) + ")" + extension);
        if (!fs::exists(candidate)) {
            return candidate;
        }
    }
    return directory / fs::u8path(
        stem + " (" + std::to_string(static_cast<unsigned>(current_time_seconds())) +
        ")" + extension);
}

FileTransferSender::FileTransferSender(IEventQueue* events, EventTarget* target) :
    events_(events),
    target_(target)
{
}

void FileTransferSender::enqueue_files(const std::vector<std::string>& paths)
{
    bool added = false;
    for (const std::string& path_string : paths) {
        const fs::path path = fs::u8path(path_string);
        std::error_code error;

        if (!fs::is_regular_file(path, error) || error) {
            LOG_ERR("ignoring transfer of non-regular file %s", path_string.c_str());
            continue;
        }

        const std::uintmax_t size = fs::file_size(path, error);
        if (error || size > kMaxFileTransferSize) {
            LOG_ERR("ignoring transfer of %s: file is too large or unreadable",
                    path_string.c_str());
            continue;
        }

        Job job;
        job.path = path_string;
        job.name = sanitize_transfer_name(path.filename().u8string());
        job.size = static_cast<std::uint64_t>(size);
        queue_.push_back(job);
        added = true;
    }

    if (state_ == State::Idle && !queue_.empty()) {
        offer_next();
    }
    else if (!added && state_ == State::Idle) {
        FileTransferResult result;
        result.success = false;
        result.message = "No regular files to transfer";
        events_->add_event(EventType::TRANSFER_SEND_RESULT, target_,
                           create_event_data<FileTransferResult>(result));
    }
}

void FileTransferSender::offer_next()
{
    if (queue_.empty()) {
        state_ = State::Idle;
        return;
    }

    current_ = queue_.front();
    queue_.pop_front();
    current_id_ = ++next_id_;
    sent_ = 0;
    last_progress_time_ = 0.0;
    state_ = State::WaitingAccept;

    FileTransferOffer offer;
    offer.id = current_id_;
    offer.size = static_cast<std::uint32_t>(current_.size);
    offer.name = current_.name;
    events_->add_event(EventType::TRANSFER_SEND_OFFER, target_,
                       create_event_data<FileTransferOffer>(offer));

    post_progress(true);
}

void FileTransferSender::on_accept(std::uint32_t id)
{
    if (state_ != State::WaitingAccept || id != current_id_) {
        return;
    }

    file_.open(current_.path, std::ios::binary);
    if (!file_) {
        finish_current(false, "Could not open " + current_.name);
        return;
    }

    state_ = State::Sending;
    sent_ = 0;
    post_progress(true);
    on_chunk_sent();
}

void FileTransferSender::on_reject(std::uint32_t id, const std::string& reason)
{
    if (id != current_id_) {
        return;
    }

    queue_.clear();
    finish_current(false, reason.empty() ? "The peer rejected the file" : reason);
}

void FileTransferSender::on_chunk_sent()
{
    if (state_ != State::Sending) {
        return;
    }

    std::string buffer(kFileTransferChunkSize, '\0');
    file_.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
    const std::streamsize read = file_.gcount();

    if (read > 0) {
        buffer.resize(static_cast<std::size_t>(read));
        sent_ += static_cast<std::uint64_t>(read);
        send_chunk_result(kDataChunk, buffer);
        post_progress();
        return;
    }

    if (!file_.eof()) {
        finish_current(false, "Could not read " + current_.name);
        return;
    }

    file_.close();
    send_chunk_result(kDataEnd, std::string());
    state_ = State::WaitingDone;
    post_progress(true);
}

void FileTransferSender::on_done(std::uint32_t id, bool success,
                                 const std::string& message)
{
    if (id != current_id_ || state_ == State::Idle) {
        return;
    }

    finish_current(success, message);
}

void FileTransferSender::cancel()
{
    if (state_ == State::Idle) {
        return;
    }

    queue_.clear();
    finish_current(false, "Transfer cancelled");
}

void FileTransferSender::finish_current(bool success, const std::string& message)
{
    if (file_.is_open()) {
        file_.close();
    }

    FileTransferResult result;
    result.id = current_id_;
    result.success = success;
    result.message = message;
    events_->add_event(EventType::TRANSFER_SEND_RESULT, target_,
                       create_event_data<FileTransferResult>(result));

    state_ = State::Idle;
    if (!queue_.empty()) {
        offer_next();
    }
}

void FileTransferSender::send_chunk_result(std::uint8_t mark,
                                           const std::string& data)
{
    FileTransferChunk chunk;
    chunk.id = current_id_;
    chunk.mark = mark;
    chunk.data = data;
    events_->add_event(EventType::TRANSFER_SEND_CHUNK, target_,
                       create_event_data<FileTransferChunk>(chunk));
}

void FileTransferSender::post_progress(bool force)
{
    const double now = current_time_seconds();
    if (!force && last_progress_time_ != 0.0 &&
        now - last_progress_time_ < kProgressInterval) {
        return;
    }
    last_progress_time_ = now;

    FileTransferProgress progress;
    progress.name = current_.name;
    progress.done = sent_;
    progress.total = current_.size;
    progress.sending = true;
    events_->add_event(EventType::TRANSFER_SEND_PROGRESS, target_,
                       create_event_data<FileTransferProgress>(progress));
}

FileTransferReceiver::FileTransferReceiver(const fs::path& directory) :
    directory_(directory.empty() ? DataDirectories::downloads() : directory)
{
}

bool FileTransferReceiver::offer(std::uint32_t id, std::uint32_t size,
                                 const std::string& name, std::string& error)
{
    if (active_) {
        error = "Another file transfer is already in progress";
        return false;
    }

    if (size > kMaxFileTransferSize) {
        error = "The file is too large";
        return false;
    }

    std::error_code dir_error;
    fs::create_directories(directory_, dir_error);
    if (dir_error) {
        error = "Could not create the Downloads directory: " + dir_error.message();
        return false;
    }

    const std::string safe_name = sanitize_transfer_name(name);
    final_path_ = unique_transfer_path(directory_, safe_name);
    part_path_ = make_part_path(final_path_);

    file_.open(part_path_.u8string(), std::ios::binary | std::ios::trunc);
    if (!file_) {
        error = "Could not create " + final_path_.u8string();
        return false;
    }

    id_ = id;
    name_ = safe_name;
    total_ = size;
    done_ = 0;
    last_progress_time_ = 0.0;
    last_progress_done_ = 0;
    active_ = true;
    return true;
}

bool FileTransferReceiver::take_progress()
{
    if (!active_) {
        return false;
    }

    const double now = current_time_seconds();
    if (done_ == total_ ||
        last_progress_time_ == 0.0 ||
        now - last_progress_time_ >= 0.1 ||
        done_ - last_progress_done_ >= 256 * 1024) {
        last_progress_time_ = now;
        last_progress_done_ = done_;
        return true;
    }
    return false;
}

FileTransferReceiver::Result FileTransferReceiver::data(
    std::uint32_t id, std::uint8_t mark, const std::string& data,
    std::string& message)
{
    if (!active_ || id != id_) {
        message = "Unexpected file transfer data";
        return Result::Error;
    }

    if (mark == kDataChunk) {
        if (done_ + data.size() > total_) {
            message = "The sender sent more data than announced";
            abort();
            return Result::Error;
        }

        file_.write(data.data(), static_cast<std::streamsize>(data.size()));
        if (!file_) {
            message = "Could not write the received file";
            abort();
            return Result::Error;
        }
        done_ += data.size();
        return Result::InProgress;
    }

    if (mark == kDataEnd) {
        file_.flush();
        file_.close();

        if (done_ != total_) {
            message = "The received file has the wrong size";
            abort();
            return Result::Error;
        }

        std::error_code rename_error;
        fs::rename(part_path_, final_path_, rename_error);
        if (rename_error) {
            message = "Could not move the file to Downloads: " +
                      rename_error.message();
            abort();
            return Result::Error;
        }

        active_ = false;
        message = final_path_.u8string();
        return Result::Finished;
    }

    if (mark == kDataError) {
        message = data.empty() ? "The sender aborted the transfer" : data;
        abort();
        return Result::Error;
    }

    message = "Unknown file transfer chunk";
    abort();
    return Result::Error;
}

void FileTransferReceiver::abort()
{
    if (file_.is_open()) {
        file_.close();
    }

    std::error_code error;
    if (!part_path_.empty()) {
        fs::remove(part_path_, error);
    }

    active_ = false;
    id_ = 0;
    total_ = 0;
    done_ = 0;
    name_.clear();
    part_path_.clear();
    final_path_.clear();
}

} // namespace inputleap
