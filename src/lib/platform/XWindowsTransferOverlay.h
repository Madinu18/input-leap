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

#include <X11/Xlib.h>

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace inputleap {

class IEventQueue;
class EventTarget;

//! X11 transfer lines and the local transfer progress window.
/*!
Each configured screen edge gets a thin, normally hidden overlay window. While
the user drags the pointer, the lines for edges that face a transfer peer are
mapped. Dropping a file on a line starts a transfer. The overlay also receives
XDND events, so it glows while a file is dragged over it.
*/
class XWindowsTransferOverlay {
public:
    XWindowsTransferOverlay(Display* display, Window root, IEventQueue* events,
                            const EventTarget* target);
    ~XWindowsTransferOverlay();

    void set_geometry(int x, int y, int width, int height);
    void set_peers(const std::vector<EDirection>& peers);

    void button_down(int x, int y);
    void motion(int x, int y);
    void button_up();
    void hide_lines();

    void show_progress(const std::string& name, std::uint64_t done,
                       std::uint64_t total, bool sending);
    void finish_progress(const std::string& message, bool success);

    void handle_event(XEvent* event);

private:
    struct EdgeWindow {
        EDirection direction = kNoDirection;
        Window window = None;
        bool visible = false;
        bool glowing = false;
    };

    void create_windows();
    void destroy_windows();
    void update_visible();
    void update_glow();
    void position_window(EdgeWindow& edge);
    void shape_window(EdgeWindow& edge);
    void set_edge_color(EdgeWindow& edge, bool glowing);
    void create_progress_window();
    void draw_progress();
    void hide_progress();

    EdgeWindow* window_to_edge(Window window);
    EdgeWindow* direction_to_edge(EDirection direction);
    void send_xdnd_status(Window source, Window window, bool accept);
    void send_xdnd_finished(Window source, Window window, bool success);
    bool has_uri_list(Window source, long* types);
    void handle_client_message(XClientMessageEvent& message);
    void handle_selection_notify(XSelectionEvent& selection);
    bool parse_uri_list(const std::string& data, EDirection edge);

    Display* display_;
    Window root_;
    IEventQueue* events_;
    const EventTarget* target_;

    int x_ = 0;
    int y_ = 0;
    int width_ = 0;
    int height_ = 0;

    std::array<EdgeWindow, 4> edges_;
    std::vector<EDirection> peers_;

    bool button_down_ = false;
    bool dragging_ = false;
    bool xdnd_active_ = false;
    EDirection xdnd_edge_ = kNoDirection;
    int start_x_ = 0;
    int start_y_ = 0;
    int last_x_ = 0;
    int last_y_ = 0;

    Window xdnd_source_ = None;
    Time xdnd_time_ = CurrentTime;
    EDirection drop_edge_ = kNoDirection;

    Atom xdnd_aware_ = None;
    Atom xdnd_enter_ = None;
    Atom xdnd_position_ = None;
    Atom xdnd_status_ = None;
    Atom xdnd_drop_ = None;
    Atom xdnd_finished_ = None;
    Atom xdnd_leave_ = None;
    Atom xdnd_selection_ = None;
    Atom xdnd_action_copy_ = None;
    Atom xdnd_type_list_ = None;
    Atom text_uri_list_ = None;
    Atom drop_property_ = None;

    Colormap colormap_ = None;
    unsigned long normal_color_ = 0;
    unsigned long glow_color_ = 0;

    Window progress_window_ = None;
    GC progress_gc_ = nullptr;
    XFontStruct* progress_font_ = nullptr;
    unsigned long progress_bg_ = 0;
    unsigned long progress_fg_ = 0;
    unsigned long progress_bar_bg_ = 0;
    unsigned long progress_bar_ = 0;
    unsigned long progress_ok_ = 0;
    unsigned long progress_error_ = 0;
    bool progress_visible_ = false;
    std::string progress_name_;
    std::uint64_t progress_done_ = 0;
    std::uint64_t progress_total_ = 0;
    bool progress_sending_ = false;
    bool progress_finished_ = false;
    bool progress_success_ = false;
    std::string progress_message_;

    static constexpr int kDropZone = 64;
    static constexpr int kLineWidth = 12;
    static constexpr int kGlowWidth = 20;
    static constexpr int kProgressWidth = 460;
    static constexpr int kProgressHeight = 82;
};

} // namespace inputleap
