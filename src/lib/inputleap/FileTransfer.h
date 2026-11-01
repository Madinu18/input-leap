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

#include "inputleap/protocol_types.h"
#include "io/filesystem.h"

#include <cstdint>
#include <deque>
#include <fstream>
#include <string>
#include <vector>

namespace inputleap {

class IEventQueue;
class EventTarget;

// Protocol limits. The size is sent as a 32-bit integer, so 4 GiB is the
// largest file that can be offered.
constexpr std::uint64_t kMaxFileTransferSize = 0xFFFFFFFFull;
constexpr std::size_t kFileTransferChunkSize = 32 * 1024;

struct FileTransferOffer {
    std::uint32_t id = 0;
    std::uint32_t size = 0;
    std::string name;
};

struct FileTransferChunk {
    std::uint32_t id = 0;
    std::uint8_t mark = 0;
    std::string data;
};

struct FileTransferProgress {
    std::string name;
    std::uint64_t done = 0;
    std::uint64_t total = 0;
    bool sending = false;
};

struct FileTransferResult {
    std::uint32_t id = 0;
    bool success = false;
    std::string message;
};

struct TransferDrop {
    EDirection edge = kNoDirection;
    std::vector<std::string> files;
};

//! Sends queued files to the peer one at a time.
/*!
The owner must forward TRANSFER_SEND_OFFER and TRANSFER_SEND_CHUNK events to
the stream and call on_chunk_sent() after each chunk has been written. The
actual data is read on the event loop, so a transfer never blocks the input
thread for long.
*/
class FileTransferSender {
public:
    FileTransferSender(IEventQueue* events, EventTarget* target);

    void enqueue_files(const std::vector<std::string>& paths);
    void on_accept(std::uint32_t id);
    void on_reject(std::uint32_t id, const std::string& reason);
    void on_done(std::uint32_t id, bool success, const std::string& message);
    void on_chunk_sent();
    void cancel();

    bool busy() const { return state_ != State::Idle; }
    std::uint32_t current_id() const { return current_id_; }

private:
    struct Job {
        std::string path;
        std::string name;
        std::uint64_t size = 0;
    };

    enum class State { Idle, WaitingAccept, Sending, WaitingDone };

    void offer_next();
    void finish_current(bool success, const std::string& message);
    void post_progress(bool force = false);
    void send_chunk_result(std::uint8_t mark, const std::string& data);

    IEventQueue* events_;
    EventTarget* target_;
    std::deque<Job> queue_;
    Job current_;
    std::ifstream file_;
    std::uint32_t next_id_ = 0;
    std::uint32_t current_id_ = 0;
    std::uint64_t sent_ = 0;
    double last_progress_time_ = 0.0;
    State state_ = State::Idle;
};

//! Receives one file into the Downloads directory at a time.
class FileTransferReceiver {
public:
    explicit FileTransferReceiver(const fs::path& directory = {});

    bool offer(std::uint32_t id, std::uint32_t size, const std::string& name,
               std::string& error);
    enum class Result { InProgress, Finished, Error };
    Result data(std::uint32_t id, std::uint8_t mark, const std::string& data,
                std::string& message);
    void abort();

    bool active() const { return active_; }
    const std::string& name() const { return name_; }
    std::uint64_t done() const { return done_; }
    std::uint64_t total() const { return total_; }
    std::string final_path() const { return final_path_.u8string(); }

    //! Returns true when the overlay should be updated (throttles events).
    bool take_progress();

private:
    fs::path directory_;
    fs::path final_path_;
    fs::path part_path_;
    std::ofstream file_;
    std::uint32_t id_ = 0;
    std::uint64_t total_ = 0;
    std::uint64_t done_ = 0;
    std::string name_;
    bool active_ = false;
    double last_progress_time_ = 0.0;
    std::uint64_t last_progress_done_ = 0;
};

std::string sanitize_transfer_name(const std::string& name);
fs::path unique_transfer_path(const fs::path& directory, const std::string& name);

} // namespace inputleap
