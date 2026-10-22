/*
 * InputLeap -- mouse and keyboard sharing utility
 * Copyright (C) 2012-2016 Symless Ltd.
 * Copyright (C) 2002 Chris Schoeneman
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

#include "server/ClientProxy1_6.h"
#include "ClientConnectionByStream.h"

#include "inputleap/ProtocolUtil.h"
#include "inputleap/ClipboardChunk.h"
#include "inputleap/Exceptions.h"
#include "inputleap/FileChunk.h"
#include "inputleap/StreamChunker.h"
#include "server/Server.h"
#include "io/IStream.h"
#include "base/Log.h"
#include "base/IEventQueue.h"
#include "base/EventQueueTimer.h"
#include "base/Time.h"

#include <array>
#include <cstring>

namespace inputleap {

ClientProxy1_6::ClientProxy1_6(const std::string& name,
                               std::unique_ptr<IClientConnection> backend,
                               Server* server, IEventQueue* events) :
    ClientProxy(name, std::move(backend)),
    m_heartbeatTimer(nullptr),
    m_parser(&ClientProxy1_6::parseHandshakeMessage),
    m_events(events),
    m_keepAliveRate(kKeepAliveRate),
    m_keepAliveTimer(nullptr),
    m_server{server},
    m_motionRate(250.0),
    m_lastMotionSendTime(-1000.0),
    m_hasPendingMotion(false),
    m_pendingMotionRelative(false),
    m_pendingX(0),
    m_pendingY(0),
    m_motionTimer(nullptr),
    m_fastPathSession(0),
    m_disconnected(false)
{
    m_motionRate = m_server->motion_rate();
    // install event handlers
    m_events->add_handler(EventType::STREAM_INPUT_READY, get_conn().get_event_target(),
                          [this](const auto& e){ handle_data(); });
    m_events->add_handler(EventType::STREAM_OUTPUT_ERROR, get_conn().get_event_target(),
                          [this](const auto& e){ handle_write_error(); });
    m_events->add_handler(EventType::STREAM_INPUT_SHUTDOWN, get_conn().get_event_target(),
                          [this](const auto& e){ handle_disconnect(); });
    m_events->add_handler(EventType::STREAM_INPUT_FORMAT_ERROR, get_conn().get_event_target(),
                          [this](const auto& e){ handle_disconnect(); });
    m_events->add_handler(EventType::STREAM_OUTPUT_SHUTDOWN, get_conn().get_event_target(),
                          [this](const auto& e){ handle_write_error(); });
    m_events->add_handler(EventType::FILE_KEEPALIVE, this,
                          [this](const auto& e){ keepAlive(); });
    m_events->add_handler(EventType::CLIPBOARD_SENDING, this,
                          [this](const auto& e){ handle_clipboard_sending_event(e); });
    m_events->add_handler(EventType::TIMER, this,
                          [this](const auto& e){ handle_flatline(); });

    setHeartbeatRate(kHeartRate, kHeartRate * kHeartBeatsUntilDeath);

    get_conn().send_query_info_1_6();

    setHeartbeatRate(kKeepAliveRate, kKeepAliveRate * kKeepAlivesUntilDeath);
}

ClientProxy1_6::~ClientProxy1_6()
{
    remove_handlers();
}

IStream* ClientProxy1_6::getStream() const
{
    return get_conn().get_stream();
}

void ClientProxy1_6::disconnect()
{
    remove_handlers();
    get_conn().close();
    m_events->add_event(EventType::CLIENT_PROXY_DISCONNECTED, get_event_target());
}

void ClientProxy1_6::remove_handlers()
{
    m_disconnected = true;

    if (m_fastPathSession != 0) {
        m_server->remove_fast_path_session(m_fastPathSession);
        m_fastPathSession = 0;
    }

    // uninstall event handlers
    m_events->remove_handler(EventType::STREAM_INPUT_READY, get_conn().get_event_target());
    m_events->remove_handler(EventType::STREAM_OUTPUT_ERROR, get_conn().get_event_target());
    m_events->remove_handler(EventType::STREAM_INPUT_SHUTDOWN, get_conn().get_event_target());
    m_events->remove_handler(EventType::STREAM_OUTPUT_SHUTDOWN, get_conn().get_event_target());
    m_events->remove_handler(EventType::STREAM_INPUT_FORMAT_ERROR, get_conn().get_event_target());
    m_events->remove_handler(EventType::FILE_KEEPALIVE, this);
    m_events->remove_handler(EventType::CLIPBOARD_SENDING, this);
    m_events->remove_handler(EventType::TIMER, this);

    // remove pending mouse motion timer
    if (m_motionTimer != nullptr) {
        m_events->remove_handler(EventType::TIMER, m_motionTimer);
        m_events->deleteTimer(m_motionTimer);
        m_motionTimer = nullptr;
    }
    m_hasPendingMotion = false;

    // remove timer
    removeHeartbeatTimer();
}

void ClientProxy1_6::addHeartbeatTimer()
{
    if (m_keepAliveRate > 0.0) {
        m_keepAliveTimer = m_events->newTimer(m_keepAliveRate, nullptr);
        m_events->add_handler(EventType::TIMER, m_keepAliveTimer,
                              [this](const auto& e){ keepAlive(); });
    }

    if (m_heartbeatAlarm > 0.0) {
        m_heartbeatTimer = m_events->newOneShotTimer(m_heartbeatAlarm, this);
    }
}

void ClientProxy1_6::removeHeartbeatTimer()
{
    if (m_keepAliveTimer != nullptr) {
        m_events->remove_handler(EventType::TIMER, m_keepAliveTimer);
        m_events->deleteTimer(m_keepAliveTimer);
        m_keepAliveTimer = nullptr;
    }

    if (m_heartbeatTimer != nullptr) {
        m_events->deleteTimer(m_heartbeatTimer);
        m_heartbeatTimer = nullptr;
    }
}

void ClientProxy1_6::resetHeartbeatTimer()
{
    // reset the alarm but not the keep alive timer
    if (m_heartbeatTimer != nullptr) {
        m_events->deleteTimer(m_heartbeatTimer);
        m_heartbeatTimer = nullptr;
    }

    if (m_heartbeatAlarm > 0.0) {
        m_heartbeatTimer = m_events->newOneShotTimer(m_heartbeatAlarm, this);
    }
}

void ClientProxy1_6::resetHeartbeatRate()
{
    setHeartbeatRate(kKeepAliveRate, kKeepAliveRate * kKeepAlivesUntilDeath);
}

bool ClientProxy1_6::motion_budget_available() const
{
    if (m_motionRate <= 0.0) {
        return true;
    }
    return (current_time_seconds() - m_lastMotionSendTime) >= (1.0 / m_motionRate);
}

void ClientProxy1_6::send_motion_now(std::int32_t x, std::int32_t y, bool relative)
{
    if (m_fastPathSession != 0 &&
        m_server->send_fast_path_motion(m_fastPathSession, x, y, relative)) {
        m_lastMotionSendTime = current_time_seconds();
        return;
    }

    if (relative) {
        get_conn().send_mouse_relative_move_1_6(x, y);
    }
    else {
        get_conn().send_mouse_move_1_6(x, y);
    }
    m_lastMotionSendTime = current_time_seconds();
}

void ClientProxy1_6::schedule_motion_flush()
{
    if (m_motionTimer != nullptr) {
        return;
    }

    double delay = (1.0 / m_motionRate) - (current_time_seconds() - m_lastMotionSendTime);
    if (delay < 0.001) {
        delay = 0.001;
    }

    m_motionTimer = m_events->newOneShotTimer(delay, nullptr);
    m_events->add_handler(EventType::TIMER, m_motionTimer,
                          [this](const auto&){ flush_pending_motion(); });
}

void ClientProxy1_6::flush_pending_motion()
{
    if (m_motionTimer != nullptr) {
        m_events->remove_handler(EventType::TIMER, m_motionTimer);
        m_events->deleteTimer(m_motionTimer);
        m_motionTimer = nullptr;
    }

    if (m_disconnected || !m_hasPendingMotion) {
        return;
    }

    const std::int32_t x = m_pendingX;
    const std::int32_t y = m_pendingY;
    const bool relative = m_pendingMotionRelative;
    m_hasPendingMotion = false;
    m_pendingX = 0;
    m_pendingY = 0;
    send_motion_now(x, y, relative);
}

void ClientProxy1_6::setHeartbeatRate(double rate, double alarm)
{
    m_keepAliveRate = rate;
    m_heartbeatAlarm = alarm;
}

void ClientProxy1_6::handle_data()
{
    // handle messages until there are no more.  first read message code.
    std::uint8_t code[4];
    std::uint32_t n = getStream()->read(code, 4);
    while (n != 0) {
        // verify we got an entire code
        if (n != 4) {
            LOG_ERR("incomplete message from \"%s\": %d bytes", getName().c_str(), n);
            disconnect();
            return;
        }

        // parse message
        try {
            LOG_DEBUG2("msg from \"%s\": %c%c%c%c", getName().c_str(), code[0], code[1], code[2], code[3]);
            if (!(this->*m_parser)(code)) {
                LOG_ERR("invalid message from client \"%s\": %c%c%c%c", getName().c_str(), code[0], code[1], code[2], code[3]);
                disconnect();
                return;
            }
        } catch (const XBadClient& e) {
            // TODO: disconnect handling is currently dispersed across both parseMessage() and
            // handleData() functions, we should collect that to a single place

            LOG_ERR("protocol error from client: %s", e.what());
            disconnect();
            return;
        }

        // next message
        n = getStream()->read(code, 4);
    }

    // restart heartbeat timer
    resetHeartbeatTimer();
}

bool ClientProxy1_6::parseHandshakeMessage(const std::uint8_t* code)
{
    if (memcmp(code, kMsgCNoop, 4) == 0) {
        // discard no-ops
        LOG_DEBUG2("no-op from %s", getName().c_str());
        return true;
    }
    else if (memcmp(code, kMsgDInfo, 4) == 0) {
        // future messages get parsed by parseMessage
        // NOTE: we're taking address of virtual function here,
        // not ClientProxy1_3 implementation of it.
        m_parser = &ClientProxy1_6::parseMessage;
        if (recvInfo()) {
            m_events->add_event(EventType::CLIENT_PROXY_READY, get_event_target());
            addHeartbeatTimer();
            return true;
        }
    }
    return false;
}

bool ClientProxy1_6::parseMessage(const std::uint8_t* code)
{
    if (memcmp(code, kMsgDFileTransfer, 4) == 0) {
        fileChunkReceived();
        return true;
    } else if (memcmp(code, kMsgDDragInfo, 4) == 0) {
        dragInfoReceived();
        return true;
    } else if (memcmp(code, kMsgCKeepAlive, 4) == 0) {
        // reset alarm
        resetHeartbeatTimer();
        return true;
    } else if (memcmp(code, kMsgDInfo, 4) == 0) {
        if (recvInfo()) {
            m_events->add_event(EventType::SCREEN_SHAPE_CHANGED, get_event_target());
            return true;
        }
        return false;
    }
    else if (memcmp(code, kMsgCNoop, 4) == 0) {
        // discard no-ops
        LOG_DEBUG2("no-op from %s", getName().c_str());
        return true;
    }
    else if (memcmp(code, kMsgCClipboard, 4) == 0) {
        return recvGrabClipboard();
    }
    else if (memcmp(code, kMsgDClipboard, 4) == 0) {
        return recvClipboard();
    }
    else if (memcmp(code, kMsgDFastPath, 4) == 0) {
        return recv_fast_path_request();
    }
    return false;
}

void ClientProxy1_6::handle_disconnect()
{
    LOG_NOTE("client \"%s\" has disconnected", getName().c_str());
    disconnect();
}

void ClientProxy1_6::handle_write_error()
{
    LOG_WARN("error writing to client \"%s\"", getName().c_str());
    disconnect();
}

void ClientProxy1_6::handle_flatline()
{
    // didn't get a heartbeat fast enough.  assume client is dead.
    LOG_NOTE("client \"%s\" is dead", getName().c_str());
    disconnect();
}

void ClientProxy1_6::handle_clipboard_sending_event(const Event& event)
{
    get_conn().send_clipboard_chunk_1_6(event.get_data_as<ClipboardChunk>());
}

bool ClientProxy1_6::getClipboard(ClipboardID id, IClipboard* clipboard) const
{
    Clipboard::copy(clipboard, &m_clipboard[id].m_clipboard);
    return true;
}

void ClientProxy1_6::getShape(std::int32_t& x, std::int32_t& y, std::int32_t& w,
                              std::int32_t& h) const
{
    x = m_info.m_x;
    y = m_info.m_y;
    w = m_info.m_w;
    h = m_info.m_h;
}

void ClientProxy1_6::getCursorPos(std::int32_t& x, std::int32_t& y) const
{
    // note -- this returns the cursor pos from when we last got client info
    x = m_info.m_mx;
    y = m_info.m_my;
}

void ClientProxy1_6::enter(std::int32_t xAbs, std::int32_t yAbs, std::uint32_t seqNum,
                           KeyModifierMask mask, bool)
{
    flush_pending_motion();
    get_conn().send_enter_1_6(xAbs, yAbs, seqNum, mask);
}

bool ClientProxy1_6::leave()
{
    flush_pending_motion();
    get_conn().send_leave_1_6();
    // we can never prevent the user from leaving
    return true;
}

void ClientProxy1_6::setClipboard(ClipboardID id, const IClipboard* clipboard)
{
    flush_pending_motion();

    // ignore if this clipboard is already clean
    if (m_clipboard[id].m_dirty) {
        // this clipboard is now clean
        m_clipboard[id].m_dirty = false;
        Clipboard::copy(&m_clipboard[id].m_clipboard, clipboard);

        std::string data = m_clipboard[id].m_clipboard.marshall();

        size_t size = data.size();
        LOG_DEBUG("sending clipboard %d to \"%s\"", id, getName().c_str());

        StreamChunker::sendClipboard(data, size, id, 0, m_events, this);
    }
}

void ClientProxy1_6::grabClipboard(ClipboardID id)
{
    flush_pending_motion();
    get_conn().send_grab_clipboard(id);

    // this clipboard is now dirty
    m_clipboard[id].m_dirty = true;
}

void ClientProxy1_6::setClipboardDirty(ClipboardID id, bool dirty)
{
    m_clipboard[id].m_dirty = dirty;
}

void ClientProxy1_6::keyDown(KeyID key, KeyModifierMask mask, KeyButton button)
{
    flush_pending_motion();
    get_conn().send_key_down_1_6(key, mask, button);
}

void ClientProxy1_6::keyRepeat(KeyID key, KeyModifierMask mask, std::int32_t count,
                               KeyButton button)
{
    flush_pending_motion();
    get_conn().send_key_repeat_1_6(key, mask, count, button);
}

void ClientProxy1_6::keyUp(KeyID key, KeyModifierMask mask, KeyButton button)
{
    flush_pending_motion();
    get_conn().send_key_up_1_6(key, mask, button);
}

void ClientProxy1_6::mouseDown(ButtonID button)
{
    flush_pending_motion();
    get_conn().send_mouse_down_1_6(button);
}

void ClientProxy1_6::mouseUp(ButtonID button)
{
    flush_pending_motion();
    get_conn().send_mouse_up_1_6(button);
}

void ClientProxy1_6::mouseMove(std::int32_t xAbs, std::int32_t yAbs)
{
    if (m_disconnected) {
        return;
    }

    if (m_motionRate <= 0.0) {
        send_motion_now(xAbs, yAbs, false);
        return;
    }

    // Absolute and relative motion cannot be mixed in one pending packet.
    if (m_hasPendingMotion && m_pendingMotionRelative) {
        flush_pending_motion();
    }

    if (!m_hasPendingMotion && motion_budget_available()) {
        send_motion_now(xAbs, yAbs, false);
        return;
    }

    // Keep only the latest absolute position. Intermediate positions are
    // useless once a newer one is available.
    m_hasPendingMotion = true;
    m_pendingMotionRelative = false;
    m_pendingX = xAbs;
    m_pendingY = yAbs;
    schedule_motion_flush();
}

void ClientProxy1_6::mouseRelativeMove(std::int32_t xRel, std::int32_t yRel)
{
    if (m_disconnected) {
        return;
    }

    if (m_motionRate <= 0.0) {
        send_motion_now(xRel, yRel, true);
        return;
    }

    if (m_hasPendingMotion && !m_pendingMotionRelative) {
        flush_pending_motion();
    }

    if (!m_hasPendingMotion && motion_budget_available()) {
        send_motion_now(xRel, yRel, true);
        return;
    }

    // Relative deltas must be accumulated, not replaced, otherwise motion
    // is lost.
    if (!m_hasPendingMotion) {
        m_pendingX = 0;
        m_pendingY = 0;
    }
    m_hasPendingMotion = true;
    m_pendingMotionRelative = true;
    m_pendingX += xRel;
    m_pendingY += yRel;
    schedule_motion_flush();
}

void ClientProxy1_6::mouseWheel(std::int32_t xDelta, std::int32_t yDelta)
{
    flush_pending_motion();
    get_conn().send_mouse_wheel_1_6(xDelta, yDelta);
}

void ClientProxy1_6::sendDragInfo(std::uint32_t fileCount, const char* info, size_t size)
{
    flush_pending_motion();
    std::string data(info, size);
    get_conn().send_drag_info_1_6(fileCount, data);
}

void ClientProxy1_6::file_chunk_sending(const FileChunk& chunk)
{
    flush_pending_motion();
    get_conn().send_file_chunk_1_6(chunk);
}

void ClientProxy1_6::screensaver(bool on)
{
    flush_pending_motion();
    get_conn().send_screensaver_1_6(on);
}

void ClientProxy1_6::resetOptions()
{
    flush_pending_motion();
    get_conn().send_reset_options_1_6();
    // reset heart rate and death
    resetHeartbeatRate();
    removeHeartbeatTimer();
    addHeartbeatTimer();
}

void ClientProxy1_6::setOptions(const OptionsList& options)
{
    flush_pending_motion();
    get_conn().send_set_options_1_6(options);

    // check options
    for (std::uint32_t i = 0, n = static_cast<std::uint32_t>(options.size()); i < n; i += 2) {
        if (options[i] == kOptionHeartbeat) {
            double rate = 1.0e-3 * static_cast<double>(options[i + 1]);
            if (rate <= 0.0) {
                rate = -1.0;
            }
            setHeartbeatRate(rate, rate * kHeartBeatsUntilDeath);
            removeHeartbeatTimer();
            addHeartbeatTimer();
        }
    }
}

bool ClientProxy1_6::recvInfo()
{
    // parse the message
    std::int16_t x, y, w, h, dummy1, mx, my;
    if (!ProtocolUtil::readf(getStream(), kMsgDInfo + 4,
                            &x, &y, &w, &h, &dummy1, &mx, &my)) {
        return false;
    }
    LOG_DEBUG("received client \"%s\" info shape=%d,%d %dx%d at %d,%d", getName().c_str(), x, y, w, h, mx, my);

    // validate
    if (w <= 0 || h <= 0) {
        return false;
    }
    if (mx < x || mx >= x + w || my < y || my >= y + h) {
        mx = x + w / 2;
        my = y + h / 2;
    }

    // save
    m_info.m_x  = x;
    m_info.m_y  = y;
    m_info.m_w  = w;
    m_info.m_h  = h;
    m_info.m_mx = mx;
    m_info.m_my = my;

    // acknowledge receipt
    get_conn().send_info_ack_1_6();
    return true;
}

bool ClientProxy1_6::recvClipboard()
{
    // parse message
    static std::string dataCached;
    ClipboardID id;
    std::uint32_t seq;

    int r = ClipboardChunk::assemble(getStream(), dataCached, id, seq);

    if (r == kStart) {
        size_t size = ClipboardChunk::getExpectedSize();
        LOG_DEBUG("receiving clipboard %d size=%zd", id, size);
    } else if (r == kFinish) {
        LOG_DEBUG("received client \"%s\" clipboard %d seqnum=%d, size=%zd",
                getName().c_str(), id, seq, dataCached.size());
        // save clipboard
        m_clipboard[id].m_clipboard.unmarshall(dataCached, 0);
        m_clipboard[id].m_sequenceNumber = seq;

        // notify
        ClipboardInfo info;
        info.m_id = id;
        info.m_sequenceNumber = seq;
        m_events->add_event(EventType::CLIPBOARD_CHANGED, get_event_target(),
                            create_event_data<ClipboardInfo>(info));
    }

    return true;
}

bool ClientProxy1_6::recvGrabClipboard()
{
    // parse message
    ClipboardID id;
    std::uint32_t seqNum;
    if (!ProtocolUtil::readf(getStream(), kMsgCClipboard + 4, &id, &seqNum)) {
        return false;
    }
    LOG_DEBUG("received client \"%s\" grabbed clipboard %d seqnum=%d", getName().c_str(), id, seqNum);

    // validate
    if (id >= kClipboardEnd) {
        return false;
    }

    // notify
    ClipboardInfo info;
    info.m_id = id;
    info.m_sequenceNumber = seqNum;
    m_events->add_event(EventType::CLIPBOARD_GRABBED, get_event_target(),
                        create_event_data<ClipboardInfo>(info));

    return true;
}

bool ClientProxy1_6::recv_fast_path_request()
{
    std::uint8_t version = 0;
    if (!ProtocolUtil::readf(getStream(), kMsgDFastPath + 4, &version)) {
        return false;
    }

    if (version != 1 || !m_server->udp_enabled()) {
        return true;
    }

    std::uint32_t session_id = 0;
    std::array<std::uint8_t, 16> key{};
    if (!m_server->create_fast_path_session(session_id, key)) {
        return true;
    }

    if (m_fastPathSession != 0) {
        m_server->remove_fast_path_session(m_fastPathSession);
    }
    m_fastPathSession = session_id;

    std::string key_string(reinterpret_cast<const char*>(key.data()), key.size());
    ProtocolUtil::writef(getStream(), kMsgCFastPath,
                         static_cast<std::uint8_t>(1),
                         static_cast<std::uint16_t>(m_server->udp_port()),
                         session_id, &key_string);
    LOG_DEBUG1("negotiated UDP fast path session %u on port %d",
               session_id, m_server->udp_port());
    return true;
}

void ClientProxy1_6::keepAlive()
{
    get_conn().send_keep_alive_1_6();
}

void ClientProxy1_6::fileChunkReceived()
{
    Server* server = getServer();
    int result = FileChunk::assemble(getStream(), server->getReceivedFileData(),
                                     server->getExpectedFileSize());

    if (result == kFinish) {
        m_events->add_event(EventType::FILE_RECEIVE_COMPLETED, server);
    } else if (result == kStart) {
        if (server->getFakeDragFileList().size() > 0) {
            std::string filename = server->getFakeDragFileList().at(0).getFilename();
            LOG_DEBUG("start receiving %s", filename.c_str());
        }
    }
}

void ClientProxy1_6::dragInfoReceived()
{
    // parse
    std::uint32_t fileNum = 0;
    std::string content;
    ProtocolUtil::readf(getStream(), kMsgDDragInfo + 4, &fileNum, &content);

    m_server->dragInfoReceived(fileNum, content);
}

ClientProxy1_6::ClientClipboard::ClientClipboard() :
    m_clipboard(),
    m_sequenceNumber(0),
    m_dirty(true)
{
    // do nothing
}

} // namespace inputleap
