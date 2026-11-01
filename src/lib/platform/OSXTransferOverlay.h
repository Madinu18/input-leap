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

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace inputleap {

class IEventQueue;
class EventTarget;

//! macOS transfer lines and the transfer progress window.
class OSXTransferOverlay {
public:
    OSXTransferOverlay(IEventQueue* events, const EventTarget* target);
    ~OSXTransferOverlay();

    void set_geometry(int x, int y, int width, int height);
    void set_peers(const std::vector<EDirection>& peers);

    void button_down(int x, int y);
    void motion(int x, int y);
    void button_up();
    void hide_lines();

    void show_progress(const std::string& name, std::uint64_t done,
                       std::uint64_t total, bool sending);
    void finish_progress(const std::string& message, bool success);

    void set_glow(EDirection direction, bool glowing);
    void drop_files(EDirection direction, std::vector<std::string> files);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace inputleap
