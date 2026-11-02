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

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <Windows.h>

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace inputleap {

class IEventQueue;
class EventTarget;
class MSWindowsTransferDropTarget;

//! Windows transfer lines and the transfer progress window.
class MSWindowsTransferOverlay {
public:
    MSWindowsTransferOverlay(HINSTANCE instance, IEventQueue* events,
                             const EventTarget* target);
    ~MSWindowsTransferOverlay();

    void set_geometry();
    void set_peers(const std::vector<EDirection>& peers);

    void button_down(int x, int y);
    void resume_drag();
    void motion(int x, int y);
    void button_up();
    void hide_lines();

    void show_progress(const std::string& name, std::uint64_t done,
                       std::uint64_t total, bool sending);
    void finish_progress(const std::string& message, bool success);

    void set_glow(EDirection direction, bool glowing);
    void drop_files(EDirection direction, std::vector<std::string> files);

    //! A file drag entered or left one of the edge drop zones.
    void set_file_drag(bool active);
    bool has_file_drag() const { return file_drag_; }

    LRESULT handle_message(HWND window, UINT message, WPARAM wparam,
                           LPARAM lparam);

private:
    struct EdgeWindow {
        EDirection direction = kNoDirection;
        HWND window = nullptr;
        int x = 0;
        int y = 0;
        int width = 0;
        int height = 0;
        bool visible = false;
        bool glowing = false;
        std::unique_ptr<MSWindowsTransferDropTarget> drop_target;
    };

    static LRESULT CALLBACK window_proc(HWND window, UINT message,
                                        WPARAM wparam, LPARAM lparam);

    void create_windows();
    void destroy_windows();
    void create_progress_window();
    void paint_progress(HDC dc);
    void position_progress_window();
    void update_visible();
    void update_glow();
    void update_animation();
    void position_edge(EdgeWindow& edge);
    void render_edge(EdgeWindow& edge);

    EdgeWindow* window_to_edge(HWND window);
    EdgeWindow* direction_to_edge(EDirection direction);
    std::wstring wide(const std::string& value) const;

    HINSTANCE instance_;
    IEventQueue* events_;
    const EventTarget* target_;

    ATOM window_class_ = 0;
    HWND progress_window_ = nullptr;
    std::array<EdgeWindow, 4> edges_;
    std::vector<EDirection> peers_;

    int screen_x_ = 0;
    int screen_y_ = 0;
    int screen_width_ = 0;
    int screen_height_ = 0;

    bool button_down_ = false;
    bool dragging_ = false;
    bool file_drag_ = false;
    bool elevated_ = false;
    int start_x_ = 0;
    int start_y_ = 0;
    int last_x_ = 0;
    int last_y_ = 0;

    bool progress_visible_ = false;
    bool progress_sending_ = false;
    std::string progress_name_;
    std::uint64_t progress_done_ = 0;
    std::uint64_t progress_total_ = 0;
    double progress_shown_ = 0.0;
    bool progress_done_state_ = false;
    bool progress_success_ = false;
    std::wstring progress_message_;
    int pulse_phase_ = 0;
    bool animation_timer_ = false;

    static constexpr int kDropZone = 64;
    static constexpr int kLineWidth = 12;
    static constexpr int kGlowWidth = 20;
    // Keep the visible line away from the outer pixel of the screen: touching
    // that pixel switches to the neighbouring machine before the drop lands.
    static constexpr int kLineInset = 20;
    static constexpr int kLineSpanPercent = 25;
    static constexpr int kMinLineSpan = 160;
    static constexpr int kProgressWidth = 520;
    static constexpr int kProgressHeight = 112;
    static constexpr int kPulseRadius = 18;

    static int line_span(int screen_size);
};

} // namespace inputleap
