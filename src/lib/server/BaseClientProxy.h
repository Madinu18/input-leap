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

#pragma once

#include "base/EventTarget.h"
#include "inputleap/Fwd.h"
#include "inputleap/IClient.h"
#include "inputleap/protocol_types.h"

namespace inputleap {

class IClientConnection;
class IStream;

//! Generic proxy for client or primary
class BaseClientProxy : public IClient, public EventTarget {
public:
    /*!
    \c name is the name of the client.
    */
    BaseClientProxy(const std::string& name);
    ~BaseClientProxy();

    //! @name manipulators
    //@{

    //! Save cursor position
    /*!
    Save the position of the cursor when jumping from client.
    */
    void setJumpCursorPos(std::int32_t x, std::int32_t y);

    //@}
    //! @name accessors
    //@{

    //! Get cursor position
    /*!
    Get the position of the cursor when last jumping from client.
    */
    void getJumpCursorPos(std::int32_t& x, std::int32_t& y) const;

    //! Get cursor position
    /*!
    Return if this proxy is for client or primary.
    */
    virtual bool isPrimary() const { return false; }

    //! Return true if the client can capture screenshots.
    virtual bool supports_screenshots() const { return false; }

    //! Ask the client to capture a screenshot of its screen.
    virtual void request_screenshot() {}

    //! Return true if the client understands file transfer messages.
    virtual bool supports_file_transfer() const { return false; }

    //! Tell the client which screen edge faces the server.
    virtual void send_file_transfer_edge(EDirection) {}

    virtual void send_file_offer(std::uint32_t, std::uint32_t,
                                 const std::string&) {}
    virtual void send_file_data(std::uint32_t, std::uint8_t,
                                const std::string&) {}
    virtual void send_file_accept(std::uint32_t) {}
    virtual void send_file_reject(std::uint32_t, const std::string&) {}
    virtual void send_file_done(std::uint32_t, bool, const std::string&) {}

    //@}

    // IClient overrides
    virtual void sendDragInfo(std::uint32_t fileCount, const char* info, size_t size) = 0;
    virtual void file_chunk_sending(const FileChunk& chunk) = 0;
    std::string getName() const override;
    virtual IClientConnection& get_conn() const = 0;

private:
    std::string m_name;
    std::int32_t m_x, m_y;
};

} // namespace inputleap
