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

#include "net/FastPath.h"

#include "arch/Arch.h"
#include "arch/XArch.h"
#include "base/Event.h"
#include "base/EventTypes.h"
#include "base/IEventQueue.h"
#include "base/Log.h"
#include "base/XBase.h"
#include "base/Time.h"
#include "mt/Thread.h"

#include <openssl/evp.h>
#include <openssl/rand.h>

#include <cstring>
#include <vector>

namespace inputleap {

namespace {

constexpr std::uint8_t kMagic[4] = {'I', 'L', 'F', 'P'};
constexpr std::uint8_t kVersion = 1;

constexpr std::uint8_t kTypeHeartbeat = 1;
constexpr std::uint8_t kTypeMotionAbs = 2;
constexpr std::uint8_t kTypeMotionRel = 3;

constexpr std::size_t kHeaderSize = 20;
constexpr std::size_t kPayloadSize = 8;
constexpr std::size_t kTagSize = 16;
constexpr std::size_t kPacketSize = kHeaderSize + kPayloadSize + kTagSize;

constexpr double kHeartbeatInterval = 0.25;
constexpr double kSessionTimeout = 1.0;

void put_u32(std::uint8_t* p, std::uint32_t v)
{
    p[0] = static_cast<std::uint8_t>((v >> 24) & 0xff);
    p[1] = static_cast<std::uint8_t>((v >> 16) & 0xff);
    p[2] = static_cast<std::uint8_t>((v >> 8) & 0xff);
    p[3] = static_cast<std::uint8_t>(v & 0xff);
}

std::uint32_t get_u32(const std::uint8_t* p)
{
    return (static_cast<std::uint32_t>(p[0]) << 24) |
           (static_cast<std::uint32_t>(p[1]) << 16) |
           (static_cast<std::uint32_t>(p[2]) << 8) |
            static_cast<std::uint32_t>(p[3]);
}

void put_u64(std::uint8_t* p, std::uint64_t v)
{
    put_u32(p, static_cast<std::uint32_t>(v >> 32));
    put_u32(p + 4, static_cast<std::uint32_t>(v & 0xffffffffu));
}

bool encrypt_packet(const std::array<std::uint8_t, 16>& key, std::uint8_t type,
                    std::uint32_t session, std::uint32_t seq,
                    std::int32_t x, std::int32_t y, std::uint8_t* out)
{
    std::memset(out, 0, kPacketSize);
    std::memcpy(out, kMagic, sizeof(kMagic));
    out[4] = kVersion;
    out[5] = type;
    put_u32(out + 8, session);
    put_u32(out + 12, seq);

    std::uint32_t salt = 0;
    if (RAND_bytes(reinterpret_cast<unsigned char*>(&salt), sizeof(salt)) != 1) {
        return false;
    }
    put_u32(out + 16, salt);

    put_u32(out + kHeaderSize, static_cast<std::uint32_t>(x));
    put_u32(out + kHeaderSize + 4, static_cast<std::uint32_t>(y));

    std::uint8_t nonce[12];
    put_u32(nonce, salt);
    put_u64(nonce + 4, static_cast<std::uint64_t>(seq));

    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    if (ctx == nullptr) {
        return false;
    }

    bool ok = false;
    do {
        int len = 0;
        if (EVP_EncryptInit_ex(ctx, EVP_aes_128_gcm(), nullptr, nullptr, nullptr) != 1) {
            break;
        }
        if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN,
                                static_cast<int>(sizeof(nonce)), nullptr) != 1) {
            break;
        }
        if (EVP_EncryptInit_ex(ctx, nullptr, nullptr, key.data(), nonce) != 1) {
            break;
        }
        // Header is authenticated but not encrypted.
        if (EVP_EncryptUpdate(ctx, nullptr, &len, out, kHeaderSize) != 1) {
            break;
        }
        int out_len = 0;
        if (EVP_EncryptUpdate(ctx, out + kHeaderSize, &len,
                              out + kHeaderSize, kPayloadSize) != 1) {
            break;
        }
        out_len += len;
        if (EVP_EncryptFinal_ex(ctx, out + kHeaderSize + out_len, &len) != 1) {
            break;
        }
        out_len += len;
        if (out_len != static_cast<int>(kPayloadSize)) {
            break;
        }
        if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_GET_TAG, kTagSize,
                                out + kHeaderSize + kPayloadSize) != 1) {
            break;
        }
        ok = true;
    } while (false);

    EVP_CIPHER_CTX_free(ctx);
    return ok;
}

bool decrypt_packet(const std::array<std::uint8_t, 16>& key,
                    const std::uint8_t* in, std::size_t len,
                    std::uint8_t* payload)
{
    if (len < kPacketSize || std::memcmp(in, kMagic, sizeof(kMagic)) != 0 ||
        in[4] != kVersion) {
        return false;
    }

    const std::uint32_t salt = get_u32(in + 16);
    const std::uint32_t seq = get_u32(in + 12);

    std::uint8_t nonce[12];
    put_u32(nonce, salt);
    put_u64(nonce + 4, static_cast<std::uint64_t>(seq));

    std::uint8_t tag[kTagSize];
    std::memcpy(tag, in + kHeaderSize + kPayloadSize, kTagSize);

    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    if (ctx == nullptr) {
        return false;
    }

    bool ok = false;
    do {
        int len = 0;
        if (EVP_DecryptInit_ex(ctx, EVP_aes_128_gcm(), nullptr, nullptr, nullptr) != 1) {
            break;
        }
        if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN,
                                static_cast<int>(sizeof(nonce)), nullptr) != 1) {
            break;
        }
        if (EVP_DecryptInit_ex(ctx, nullptr, nullptr, key.data(), nonce) != 1) {
            break;
        }
        if (EVP_DecryptUpdate(ctx, nullptr, &len, in, kHeaderSize) != 1) {
            break;
        }
        int out_len = 0;
        if (EVP_DecryptUpdate(ctx, payload, &len,
                              in + kHeaderSize, kPayloadSize) != 1) {
            break;
        }
        out_len += len;
        if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_TAG, kTagSize, tag) != 1) {
            break;
        }
        int final_len = 0;
        if (EVP_DecryptFinal_ex(ctx, payload + out_len, &final_len) <= 0) {
            break;
        }
        ok = true;
    } while (false);

    EVP_CIPHER_CTX_free(ctx);
    return ok;
}

} // namespace

FastPathServer::FastPathServer(int port) :
    port_(port)
{
    try {
        socket_ = ARCH->newSocket(IArchNetwork::kINET, IArchNetwork::kDGRAM);
        NetworkAddress bind_address(port_);
        ARCH->bindSocket(socket_, bind_address.getAddress());
        try {
            ARCH->setDSCPOnSocket(socket_, 46);
        }
        catch (XArchNetwork&) {
            // optional
        }

        running_ = true;
        thread_ = new Thread([this]() { receive_loop(); });
    }
    catch (XBase& e) {
        LOG_WARN("failed to start UDP fast path on port %d: %s", port_, e.what());
        if (socket_ != nullptr) {
            ARCH->closeSocket(socket_);
            socket_ = nullptr;
        }
    }
    catch (...) {
        if (socket_ != nullptr) {
            ARCH->closeSocket(socket_);
            socket_ = nullptr;
        }
    }
}

FastPathServer::~FastPathServer()
{
    running_ = false;
    if (thread_ != nullptr) {
        thread_->cancel();
        thread_->wait();
        delete thread_;
        thread_ = nullptr;
    }
    if (socket_ != nullptr) {
        ARCH->closeSocket(socket_);
        socket_ = nullptr;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& entry : sessions_) {
        if (entry.second.endpoint != nullptr) {
            ARCH->closeAddr(entry.second.endpoint);
        }
    }
    sessions_.clear();
}

bool FastPathServer::create_session(std::uint32_t& session_id,
                                    std::array<std::uint8_t, 16>& key)
{
    if (!is_valid()) {
        return false;
    }

    std::lock_guard<std::mutex> lock(mutex_);

    std::uint32_t id = 0;
    for (int attempt = 0; attempt < 16; ++attempt) {
        if (RAND_bytes(reinterpret_cast<unsigned char*>(&id), sizeof(id)) != 1) {
            return false;
        }
        if (id != 0 && sessions_.find(id) == sessions_.end()) {
            break;
        }
        id = 0;
    }
    if (id == 0) {
        return false;
    }

    std::array<std::uint8_t, 16> new_key{};
    if (RAND_bytes(new_key.data(), static_cast<int>(new_key.size())) != 1) {
        return false;
    }

    Session session;
    session.key = new_key;
    sessions_[id] = session;

    session_id = id;
    key = new_key;
    return true;
}

void FastPathServer::remove_session(std::uint32_t session_id)
{
    std::lock_guard<std::mutex> lock(mutex_);
    auto index = sessions_.find(session_id);
    if (index == sessions_.end()) {
        return;
    }
    if (index->second.endpoint != nullptr) {
        ARCH->closeAddr(index->second.endpoint);
    }
    sessions_.erase(index);
}

bool FastPathServer::is_active(std::uint32_t session_id) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    auto index = sessions_.find(session_id);
    if (index == sessions_.end() || index->second.endpoint == nullptr) {
        return false;
    }
    return (current_time_seconds() - index->second.last_heartbeat) < kSessionTimeout;
}

bool FastPathServer::send_motion(std::uint32_t session_id, std::int32_t x,
                                 std::int32_t y, bool relative)
{
    std::lock_guard<std::mutex> lock(mutex_);
    auto index = sessions_.find(session_id);
    if (index == sessions_.end() || index->second.endpoint == nullptr) {
        return false;
    }
    if ((current_time_seconds() - index->second.last_heartbeat) >= kSessionTimeout) {
        return false;
    }

    Session& session = index->second;
    ++session.tx_seq;

    std::uint8_t packet[kPacketSize];
    if (!encrypt_packet(session.key, relative ? kTypeMotionRel : kTypeMotionAbs,
                        session_id, session.tx_seq, x, y, packet)) {
        return false;
    }

    try {
        return ARCH->sendTo(socket_, packet, kPacketSize, session.endpoint) == kPacketSize;
    }
    catch (XArchNetwork&) {
        return false;
    }
}

void FastPathServer::receive_loop()
{
    std::vector<std::uint8_t> buffer(2048);

    while (running_) {
        IArchNetwork::PollEntry entry;
        entry.m_socket = socket_;
        entry.m_events = IArchNetwork::kPOLLIN;
        entry.m_revents = 0;

        int ready = 0;
        try {
            ready = ARCH->pollSocket(&entry, 1, 0.2);
        }
        catch (XArchNetwork&) {
            break;
        }

        if (ready <= 0 || (entry.m_revents & IArchNetwork::kPOLLIN) == 0) {
            continue;
        }

        ArchNetAddress from = nullptr;
        std::size_t received = 0;
        try {
            received = ARCH->recvFrom(socket_, buffer.data(), buffer.size(), &from);
        }
        catch (XArchNetwork&) {
            if (from != nullptr) {
                ARCH->closeAddr(from);
            }
            continue;
        }

        if (received > 0 && from != nullptr) {
            handle_packet(buffer.data(), received, from);
        }
        if (from != nullptr) {
            ARCH->closeAddr(from);
        }
    }
}

void FastPathServer::handle_packet(const std::uint8_t* data, std::size_t len,
                                   ArchNetAddress from)
{
    if (len < kPacketSize) {
        return;
    }

    const std::uint32_t session_id = get_u32(data + 8);

    std::lock_guard<std::mutex> lock(mutex_);
    auto index = sessions_.find(session_id);
    if (index == sessions_.end()) {
        return;
    }

    std::uint8_t payload[kPayloadSize];
    if (!decrypt_packet(index->second.key, data, len, payload)) {
        return;
    }

    const std::uint32_t seq = get_u32(data + 12);
    Session& session = index->second;
    if (session.has_seq && static_cast<std::int32_t>(seq - session.last_seq) <= 0) {
        return;
    }
    session.has_seq = true;
    session.last_seq = seq;
    session.last_heartbeat = current_time_seconds();

    if (session.endpoint != nullptr) {
        ARCH->closeAddr(session.endpoint);
    }
    session.endpoint = ARCH->copyAddr(from);
}

FastPathClient::FastPathClient(IEventQueue* events, const EventTarget* target,
                               const NetworkAddress& server, int port,
                               std::uint32_t session_id,
                               const std::array<std::uint8_t, 16>& key) :
    events_(events),
    target_(target),
    server_(server),
    port_(port),
    session_id_(session_id),
    key_(key)
{
    if (server_.getAddress() == nullptr) {
        return;
    }

    try {
        const auto family = ARCH->getAddrFamily(server_.getAddress());
        socket_ = ARCH->newSocket(family, IArchNetwork::kDGRAM);
        NetworkAddress local = NetworkAddress::wildcard(0, family);
        ARCH->bindSocket(socket_, local.getAddress());
        try {
            ARCH->setDSCPOnSocket(socket_, 46);
        }
        catch (XArchNetwork&) {
            // optional
        }

        running_ = true;
        thread_ = new Thread([this]() { receive_loop(); });
    }
    catch (XBase& e) {
        LOG_WARN("failed to start UDP fast path client: %s", e.what());
        if (socket_ != nullptr) {
            ARCH->closeSocket(socket_);
            socket_ = nullptr;
        }
    }
    catch (...) {
        if (socket_ != nullptr) {
            ARCH->closeSocket(socket_);
            socket_ = nullptr;
        }
    }
}

FastPathClient::~FastPathClient()
{
    running_ = false;
    if (thread_ != nullptr) {
        thread_->cancel();
        thread_->wait();
        delete thread_;
        thread_ = nullptr;
    }
    if (socket_ != nullptr) {
        ARCH->closeSocket(socket_);
        socket_ = nullptr;
    }
}

void FastPathClient::send_heartbeat()
{
    std::uint8_t packet[kPacketSize];
    ++seq_;
    if (!encrypt_packet(key_, kTypeHeartbeat, session_id_, seq_, 0, 0, packet)) {
        return;
    }

    ArchNetAddress address = ARCH->copyAddr(server_.getAddress());
    ARCH->setAddrPort(address, port_);
    try {
        ARCH->sendTo(socket_, packet, kPacketSize, address);
    }
    catch (XArchNetwork&) {
        // The next heartbeat will retry.
    }
    ARCH->closeAddr(address);
}

void FastPathClient::receive_loop()
{
    std::vector<std::uint8_t> buffer(2048);
    double next_heartbeat = 0.0;

    while (running_) {
        const double now = current_time_seconds();
        if (now >= next_heartbeat) {
            send_heartbeat();
            next_heartbeat = now + kHeartbeatInterval;
        }

        IArchNetwork::PollEntry entry;
        entry.m_socket = socket_;
        entry.m_events = IArchNetwork::kPOLLIN;
        entry.m_revents = 0;

        int ready = 0;
        try {
            ready = ARCH->pollSocket(&entry, 1, 0.05);
        }
        catch (XArchNetwork&) {
            break;
        }

        if (ready <= 0 || (entry.m_revents & IArchNetwork::kPOLLIN) == 0) {
            continue;
        }

        ArchNetAddress from = nullptr;
        std::size_t received = 0;
        try {
            received = ARCH->recvFrom(socket_, buffer.data(), buffer.size(), &from);
        }
        catch (XArchNetwork&) {
            if (from != nullptr) {
                ARCH->closeAddr(from);
            }
            continue;
        }

        if (received > 0) {
            handle_packet(buffer.data(), received);
        }
        if (from != nullptr) {
            ARCH->closeAddr(from);
        }
    }
}

void FastPathClient::handle_packet(const std::uint8_t* data, std::size_t len)
{
    if (len < kPacketSize || get_u32(data + 8) != session_id_) {
        return;
    }

    std::uint8_t payload[kPayloadSize];
    if (!decrypt_packet(key_, data, len, payload)) {
        return;
    }

    const std::uint32_t seq = get_u32(data + 12);
    if (has_rx_seq_ && static_cast<std::int32_t>(seq - last_rx_seq_) <= 0) {
        return;
    }
    has_rx_seq_ = true;
    last_rx_seq_ = seq;

    const std::uint8_t type = data[5];
    if (type != kTypeMotionAbs && type != kTypeMotionRel) {
        return;
    }

    FastPathMotionInfo info;
    info.x = static_cast<std::int32_t>(get_u32(payload));
    info.y = static_cast<std::int32_t>(get_u32(payload + 4));
    info.relative = (type == kTypeMotionRel);

    events_->add_event(EventType::CLIENT_FAST_PATH_MOTION, target_,
                       create_event_data<FastPathMotionInfo>(info));
}

} // namespace inputleap
