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

#include "platform/XWindowsTransferOverlay.h"

#include "inputleap/FileTransfer.h"
#include "base/Event.h"
#include "base/IEventQueue.h"
#include "base/Log.h"

#include <X11/Xatom.h>
#include <X11/extensions/shape.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>

namespace inputleap {

namespace {

unsigned long alloc_color(Display* display, Colormap colormap, const char* name)
{
    XColor color;
    if (XParseColor(display, colormap, name, &color) == 0 ||
        XAllocColor(display, colormap, &color) == 0) {
        return BlackPixel(display, DefaultScreen(display));
    }
    return color.pixel;
}

std::string percent_decode(const std::string& value)
{
    std::string result;
    result.reserve(value.size());

    for (std::size_t i = 0; i < value.size(); ++i) {
        if (value[i] == '%' && i + 2 < value.size() &&
            std::isxdigit(static_cast<unsigned char>(value[i + 1])) &&
            std::isxdigit(static_cast<unsigned char>(value[i + 2]))) {
            const std::string hex = value.substr(i + 1, 2);
            result.push_back(static_cast<char>(std::strtol(hex.c_str(), nullptr, 16)));
            i += 2;
        }
        else {
            result.push_back(value[i]);
        }
    }
    return result;
}

std::string format_bytes(std::uint64_t bytes)
{
    char buffer[64];
    if (bytes < 1024) {
        std::snprintf(buffer, sizeof(buffer), "%llu B",
                      static_cast<unsigned long long>(bytes));
    }
    else if (bytes < 1024 * 1024) {
        std::snprintf(buffer, sizeof(buffer), "%.1f KB", bytes / 1024.0);
    }
    else if (bytes < 1024ull * 1024 * 1024) {
        std::snprintf(buffer, sizeof(buffer), "%.1f MB", bytes / (1024.0 * 1024.0));
    }
    else {
        std::snprintf(buffer, sizeof(buffer), "%.2f GB",
                      bytes / (1024.0 * 1024.0 * 1024.0));
    }
    return buffer;
}

std::string shorten(const std::string& value, std::size_t max_length)
{
    if (value.size() <= max_length) {
        return value;
    }
    return value.substr(0, max_length - 3) + "...";
}

} // namespace

XWindowsTransferOverlay::XWindowsTransferOverlay(Display* display, Window root,
                                                 IEventQueue* events,
                                                 const EventTarget* target) :
    display_(display),
    root_(root),
    events_(events),
    target_(target)
{
    edges_[0].direction = kLeft;
    edges_[1].direction = kRight;
    edges_[2].direction = kTop;
    edges_[3].direction = kBottom;

    xdnd_aware_ = XInternAtom(display_, "XdndAware", False);
    xdnd_enter_ = XInternAtom(display_, "XdndEnter", False);
    xdnd_position_ = XInternAtom(display_, "XdndPosition", False);
    xdnd_status_ = XInternAtom(display_, "XdndStatus", False);
    xdnd_drop_ = XInternAtom(display_, "XdndDrop", False);
    xdnd_finished_ = XInternAtom(display_, "XdndFinished", False);
    xdnd_leave_ = XInternAtom(display_, "XdndLeave", False);
    xdnd_selection_ = XInternAtom(display_, "XdndSelection", False);
    xdnd_action_copy_ = XInternAtom(display_, "XdndActionCopy", False);
    xdnd_type_list_ = XInternAtom(display_, "XdndTypeList", False);
    text_uri_list_ = XInternAtom(display_, "text/uri-list", False);
    drop_property_ = XInternAtom(display_, "INPUTLEAP_XDND_DROP", False);

    colormap_ = DefaultColormap(display_, DefaultScreen(display_));
    normal_color_ = alloc_color(display_, colormap_, "#2f6fed");
    glow_color_ = alloc_color(display_, colormap_, "#8bdcff");

    create_windows();
}

XWindowsTransferOverlay::~XWindowsTransferOverlay()
{
    hide_progress();
    destroy_windows();

    if (progress_gc_ != nullptr) {
        XFreeGC(display_, progress_gc_);
        progress_gc_ = nullptr;
    }
    if (progress_font_ != nullptr) {
        XFreeFont(display_, progress_font_);
        progress_font_ = nullptr;
    }
    if (progress_window_ != None) {
        XDestroyWindow(display_, progress_window_);
        progress_window_ = None;
    }
}

void XWindowsTransferOverlay::create_windows()
{
    XSetWindowAttributes attributes{};
    attributes.override_redirect = True;
    attributes.background_pixel = normal_color_;
    attributes.event_mask = ExposureMask;

    const long mask = CWOverrideRedirect | CWBackPixel | CWEventMask;

    for (EdgeWindow& edge : edges_) {
        edge.window = XCreateWindow(
            display_, root_, 0, 0, 1, 1, 0,
            CopyFromParent, InputOutput, CopyFromParent, mask, &attributes);
        if (edge.window == None) {
            continue;
        }

        long version = 5;
        XChangeProperty(display_, edge.window, xdnd_aware_, XA_ATOM, 32,
                        PropModeReplace,
                        reinterpret_cast<unsigned char*>(&version), 1);
        XUnmapWindow(display_, edge.window);
    }
}

void XWindowsTransferOverlay::destroy_windows()
{
    for (EdgeWindow& edge : edges_) {
        if (edge.window != None) {
            XDestroyWindow(display_, edge.window);
            edge.window = None;
        }
    }
}

void XWindowsTransferOverlay::set_geometry(int x, int y, int width, int height)
{
    x_ = x;
    y_ = y;
    width_ = width;
    height_ = height;

    for (EdgeWindow& edge : edges_) {
        if (edge.window != None) {
            position_window(edge);
        }
    }

    if (progress_window_ != None && progress_visible_) {
        XMoveWindow(display_, progress_window_,
                    x_ + (width_ - kProgressWidth) / 2,
                    y_ + std::max(48, height_ / 8));
        XRaiseWindow(display_, progress_window_);
    }
}

void XWindowsTransferOverlay::set_peers(const std::vector<EDirection>& peers)
{
    peers_ = peers;
    update_visible();
}

void XWindowsTransferOverlay::position_window(EdgeWindow& edge)
{
    if (edge.window == None || width_ <= 0 || height_ <= 0) {
        return;
    }

    int x = x_;
    int y = y_;
    int width = kDropZone;
    int height = height_;

    switch (edge.direction) {
    case kLeft:
        break;
    case kRight:
        x = x_ + width_ - kDropZone;
        break;
    case kTop:
        width = width_;
        height = kDropZone;
        break;
    case kBottom:
        y = y_ + height_ - kDropZone;
        width = width_;
        height = kDropZone;
        break;
    default:
        return;
    }

    XMoveResizeWindow(display_, edge.window, x, y, width, height);
    shape_window(edge);
    set_edge_color(edge, edge.glowing);
}

void XWindowsTransferOverlay::shape_window(EdgeWindow& edge)
{
    if (edge.window == None) {
        return;
    }

    XWindowAttributes attributes;
    if (XGetWindowAttributes(display_, edge.window, &attributes) == 0 ||
        attributes.width <= 0 || attributes.height <= 0) {
        return;
    }

    const int line = edge.glowing ? kGlowWidth : kLineWidth;
    XRectangle line_rect{0, 0, 0, 0};

    switch (edge.direction) {
    case kLeft:
        line_rect = {0, 0, static_cast<unsigned short>(line),
                     static_cast<unsigned short>(attributes.height)};
        break;
    case kRight:
        line_rect = {static_cast<short>(attributes.width - line), 0,
                     static_cast<unsigned short>(line),
                     static_cast<unsigned short>(attributes.height)};
        break;
    case kTop:
        line_rect = {0, 0, static_cast<unsigned short>(attributes.width),
                     static_cast<unsigned short>(line)};
        break;
    case kBottom:
        line_rect = {0, static_cast<short>(attributes.height - line),
                     static_cast<unsigned short>(attributes.width),
                     static_cast<unsigned short>(line)};
        break;
    default:
        return;
    }

    XRectangle input_rect = {
        0, 0, static_cast<unsigned short>(attributes.width),
        static_cast<unsigned short>(attributes.height)};

    XShapeCombineRectangles(display_, edge.window, ShapeBounding, 0, 0,
                            &line_rect, 1, ShapeSet, Unsorted);
    XShapeCombineRectangles(display_, edge.window, ShapeInput, 0, 0,
                            &input_rect, 1, ShapeSet, Unsorted);
    XClearWindow(display_, edge.window);
}

void XWindowsTransferOverlay::set_edge_color(EdgeWindow& edge, bool glowing)
{
    if (edge.window == None) {
        return;
    }
    XSetWindowBackground(display_, edge.window,
                         glowing ? glow_color_ : normal_color_);
    XClearWindow(display_, edge.window);
}

void XWindowsTransferOverlay::update_visible()
{
    for (EdgeWindow& edge : edges_) {
        const bool peer = std::find(peers_.begin(), peers_.end(),
                                    edge.direction) != peers_.end();
        const bool should_show = dragging_ && peer;
        if (should_show == edge.visible) {
            continue;
        }

        edge.visible = should_show;
        if (should_show) {
            XMapRaised(display_, edge.window);
        }
        else {
            edge.glowing = false;
            set_edge_color(edge, false);
            shape_window(edge);
            XUnmapWindow(display_, edge.window);
        }
    }
    XFlush(display_);
    update_glow();
}

void XWindowsTransferOverlay::update_glow()
{
    if (!dragging_) {
        for (EdgeWindow& edge : edges_) {
            if (edge.glowing) {
                edge.glowing = false;
                shape_window(edge);
                set_edge_color(edge, false);
            }
        }
        XFlush(display_);
        return;
    }

    for (EdgeWindow& edge : edges_) {
        bool near = false;
        switch (edge.direction) {
        case kLeft:
            near = last_x_ - x_ <= kDropZone;
            break;
        case kRight:
            near = (x_ + width_ - 1) - last_x_ <= kDropZone;
            break;
        case kTop:
            near = last_y_ - y_ <= kDropZone;
            break;
        case kBottom:
            near = (y_ + height_ - 1) - last_y_ <= kDropZone;
            break;
        default:
            break;
        }

        const bool glowing = edge.visible &&
            (near || (xdnd_active_ && xdnd_edge_ == edge.direction));
        if (glowing == edge.glowing) {
            continue;
        }

        edge.glowing = glowing;
        shape_window(edge);
        set_edge_color(edge, glowing);
    }
    XFlush(display_);
}

void XWindowsTransferOverlay::button_down(int x, int y)
{
    button_down_ = true;
    dragging_ = false;
    start_x_ = x;
    start_y_ = y;
    last_x_ = x;
    last_y_ = y;
}

void XWindowsTransferOverlay::motion(int x, int y)
{
    last_x_ = x;
    last_y_ = y;

    if (!button_down_) {
        return;
    }

    if (!dragging_ &&
        std::abs(x - start_x_) + std::abs(y - start_y_) >= 4) {
        dragging_ = true;
        update_visible();
    }
    else if (dragging_) {
        update_glow();
    }
}

void XWindowsTransferOverlay::button_up()
{
    button_down_ = false;
    dragging_ = false;
    update_visible();
}

void XWindowsTransferOverlay::hide_lines()
{
    button_down_ = false;
    dragging_ = false;
    xdnd_active_ = false;
    xdnd_edge_ = kNoDirection;
    update_visible();
}

void XWindowsTransferOverlay::handle_event(XEvent* event)
{
    if (event == nullptr) {
        return;
    }

    if (event->type == ClientMessage) {
        XClientMessageEvent& message = event->xclient;
        if (message.message_type == xdnd_enter_ ||
            message.message_type == xdnd_position_ ||
            message.message_type == xdnd_drop_ ||
            message.message_type == xdnd_leave_) {
            handle_client_message(message);
        }
    }
    else if (event->type == SelectionNotify) {
        handle_selection_notify(event->xselection);
    }
    else if (event->type == Expose) {
        if (event->xexpose.window == progress_window_) {
            draw_progress();
        }
        else {
            for (EdgeWindow& edge : edges_) {
                if (edge.window == event->xexpose.window) {
                    XClearWindow(display_, edge.window);
                    break;
                }
            }
        }
    }
}

XWindowsTransferOverlay::EdgeWindow*
XWindowsTransferOverlay::window_to_edge(Window window)
{
    for (EdgeWindow& edge : edges_) {
        if (edge.window == window) {
            return &edge;
        }
    }
    return nullptr;
}

XWindowsTransferOverlay::EdgeWindow*
XWindowsTransferOverlay::direction_to_edge(EDirection direction)
{
    for (EdgeWindow& edge : edges_) {
        if (edge.direction == direction) {
            return &edge;
        }
    }
    return nullptr;
}

bool XWindowsTransferOverlay::has_uri_list(Window source, long* types)
{
    if ((types[0] & 1) != 0) {
        Atom actual = None;
        int format = 0;
        unsigned long count = 0;
        unsigned long remaining = 0;
        unsigned char* data = nullptr;

        if (XGetWindowProperty(display_, source, xdnd_type_list_, 0, 32, False,
                               XA_ATOM, &actual, &format, &count, &remaining,
                               &data) == Success &&
            data != nullptr && format == 32) {
            auto* atoms = reinterpret_cast<Atom*>(data);
            for (unsigned long i = 0; i < count; ++i) {
                if (atoms[i] == text_uri_list_) {
                    XFree(data);
                    return true;
                }
            }
            XFree(data);
        }
        return false;
    }

    for (int i = 1; i <= 3; ++i) {
        if (static_cast<Atom>(types[i]) == text_uri_list_) {
            return true;
        }
    }
    return false;
}

void XWindowsTransferOverlay::send_xdnd_status(Window source, Window window,
                                               bool accept)
{
    XEvent event{};
    event.xclient.type = ClientMessage;
    event.xclient.window = source;
    event.xclient.message_type = xdnd_status_;
    event.xclient.format = 32;
    event.xclient.data.l[0] = static_cast<long>(window);
    event.xclient.data.l[1] = accept ? 1 : 0;
    event.xclient.data.l[2] = 0;
    event.xclient.data.l[3] = 0;
    event.xclient.data.l[4] = static_cast<long>(xdnd_action_copy_);
    XSendEvent(display_, source, False, NoEventMask, &event);
    XFlush(display_);
}

void XWindowsTransferOverlay::send_xdnd_finished(Window source, Window window,
                                                 bool success)
{
    if (source == None) {
        return;
    }

    XEvent event{};
    event.xclient.type = ClientMessage;
    event.xclient.window = source;
    event.xclient.message_type = xdnd_finished_;
    event.xclient.format = 32;
    event.xclient.data.l[0] = static_cast<long>(window);
    event.xclient.data.l[1] = success ? 1 : 0;
    event.xclient.data.l[2] = success ? static_cast<long>(xdnd_action_copy_) : 0;
    XSendEvent(display_, source, False, NoEventMask, &event);
    XFlush(display_);
}

void XWindowsTransferOverlay::handle_client_message(XClientMessageEvent& message)
{
    EdgeWindow* edge = window_to_edge(message.window);
    if (edge == nullptr) {
        return;
    }

    if (message.message_type == xdnd_enter_) {
        const Window source = static_cast<Window>(message.data.l[0]);
        long types[4] = {
            message.data.l[1], message.data.l[2],
            message.data.l[3], message.data.l[4]};

        if (!has_uri_list(source, types)) {
            send_xdnd_status(source, message.window, false);
            return;
        }

        xdnd_source_ = source;
        xdnd_active_ = true;
        xdnd_edge_ = edge->direction;
        drop_edge_ = edge->direction;
        send_xdnd_status(source, message.window, true);
        update_glow();
        return;
    }

    if (message.message_type == xdnd_position_) {
        const Window source = static_cast<Window>(message.data.l[0]);
        if (source != xdnd_source_) {
            xdnd_source_ = source;
        }
        xdnd_time_ = static_cast<Time>(message.data.l[4]);
        last_x_ = static_cast<int>(message.data.l[2]);
        last_y_ = static_cast<int>(message.data.l[3]);
        xdnd_active_ = true;
        xdnd_edge_ = edge->direction;
        send_xdnd_status(source, message.window, true);
        update_glow();
        return;
    }

    if (message.message_type == xdnd_drop_) {
        const Window source = static_cast<Window>(message.data.l[0]);
        if (source == None) {
            return;
        }
        xdnd_source_ = source;
        xdnd_time_ = static_cast<Time>(message.data.l[2]);
        drop_edge_ = edge->direction;
        XConvertSelection(display_, xdnd_selection_, text_uri_list_,
                          drop_property_, message.window, xdnd_time_);
        XFlush(display_);
        return;
    }

    if (message.message_type == xdnd_leave_) {
        xdnd_active_ = false;
        xdnd_edge_ = kNoDirection;
        xdnd_source_ = None;
        update_glow();
    }
}

void XWindowsTransferOverlay::handle_selection_notify(XSelectionEvent& selection)
{
    if (selection.property != drop_property_) {
        return;
    }

    Atom actual = None;
    int format = 0;
    unsigned long count = 0;
    unsigned long remaining = 0;
    unsigned char* data = nullptr;

    bool success = false;
    if (XGetWindowProperty(display_, selection.requestor, drop_property_,
                           0, 0x1fffffff, True, AnyPropertyType,
                           &actual, &format, &count, &remaining,
                           &data) == Success &&
        data != nullptr && format == 8) {
        const std::string uri_list(reinterpret_cast<char*>(data), count);
        success = parse_uri_list(uri_list, drop_edge_);
    }

    if (data != nullptr) {
        XFree(data);
    }

    send_xdnd_finished(xdnd_source_, selection.requestor, success);
    xdnd_active_ = false;
    xdnd_edge_ = kNoDirection;
    xdnd_source_ = None;
    drop_edge_ = kNoDirection;
    button_down_ = false;
    dragging_ = false;
    update_visible();
}

bool XWindowsTransferOverlay::parse_uri_list(const std::string& data,
                                             EDirection edge)
{
    std::vector<std::string> files;
    std::size_t position = 0;

    while (position < data.size()) {
        std::size_t end = data.find('\n', position);
        if (end == std::string::npos) {
            end = data.size();
        }

        std::string line = data.substr(position, end - position);
        position = end + 1;
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (line.empty() || line[0] == '#') {
            continue;
        }

        const std::string prefix = "file://";
        if (line.rfind(prefix, 0) != 0) {
            continue;
        }

        std::string rest = line.substr(prefix.size());
        if (rest.rfind("localhost/", 0) == 0) {
            rest = rest.substr(9);
        }
        else if (!rest.empty() && rest[0] != '/') {
            const std::size_t slash = rest.find('/');
            if (slash == std::string::npos) {
                continue;
            }
            rest = rest.substr(slash);
        }

        const std::string path = percent_decode(rest);
        if (!path.empty()) {
            files.push_back(path);
        }
    }

    if (files.empty()) {
        return false;
    }

    TransferDrop drop;
    drop.edge = edge;
    drop.files = files;
    events_->add_event(EventType::TRANSFER_FILES_DROPPED, target_,
                       create_event_data<TransferDrop>(drop));
    return true;
}

void XWindowsTransferOverlay::create_progress_window()
{
    if (progress_window_ != None) {
        return;
    }

    progress_bg_ = alloc_color(display_, colormap_, "#10151c");
    progress_fg_ = alloc_color(display_, colormap_, "#f2f6fb");
    progress_bar_bg_ = alloc_color(display_, colormap_, "#293241");
    progress_bar_ = alloc_color(display_, colormap_, "#4c9dff");
    progress_ok_ = alloc_color(display_, colormap_, "#6ee7a0");
    progress_error_ = alloc_color(display_, colormap_, "#ff7b7b");

    XSetWindowAttributes attributes{};
    attributes.override_redirect = True;
    attributes.background_pixel = progress_bg_;
    attributes.event_mask = ExposureMask;

    progress_window_ = XCreateWindow(
        display_, root_, 0, 0, kProgressWidth, kProgressHeight, 1,
        CopyFromParent, InputOutput, CopyFromParent,
        CWOverrideRedirect | CWBackPixel | CWEventMask, &attributes);
    if (progress_window_ == None) {
        return;
    }

    XShapeCombineRectangles(display_, progress_window_, ShapeInput, 0, 0,
                            nullptr, 0, ShapeSet, Unsorted);

    progress_gc_ = XCreateGC(display_, progress_window_, 0, nullptr);
    progress_font_ = XLoadQueryFont(display_, "9x15");
    if (progress_font_ == nullptr) {
        progress_font_ = XLoadQueryFont(display_, "fixed");
    }
    if (progress_font_ != nullptr) {
        XSetFont(display_, progress_gc_, progress_font_->fid);
    }
}

void XWindowsTransferOverlay::show_progress(const std::string& name,
                                            std::uint64_t done,
                                            std::uint64_t total,
                                            bool sending)
{
    create_progress_window();
    if (progress_window_ == None) {
        return;
    }

    progress_name_ = name;
    progress_done_ = done;
    progress_total_ = total;
    progress_sending_ = sending;
    progress_finished_ = false;
    progress_visible_ = true;

    XMoveWindow(display_, progress_window_,
                x_ + (width_ - kProgressWidth) / 2,
                y_ + std::max(48, height_ / 8));
    XMapRaised(display_, progress_window_);
    draw_progress();
}

void XWindowsTransferOverlay::finish_progress(const std::string& message,
                                              bool success)
{
    LOG_DEBUG("transfer overlay finished: success=%d message=%s",
              success, message.c_str());
    hide_progress();
}

void XWindowsTransferOverlay::hide_progress()
{
    if (progress_window_ != None && progress_visible_) {
        XUnmapWindow(display_, progress_window_);
        XFlush(display_);
    }
    progress_visible_ = false;
}

void XWindowsTransferOverlay::draw_progress()
{
    if (!progress_visible_ || progress_window_ == None ||
        progress_gc_ == nullptr) {
        return;
    }

    const int bar_x = 16;
    const int bar_y = 58;
    const int bar_width = kProgressWidth - 32;
    const int bar_height = 12;

    XSetForeground(display_, progress_gc_, progress_bg_);
    XFillRectangle(display_, progress_window_, progress_gc_, 0, 0,
                   kProgressWidth, kProgressHeight);

    XSetForeground(display_, progress_gc_, progress_bar_);
    XDrawRectangle(display_, progress_window_, progress_gc_, 0, 0,
                   kProgressWidth - 1, kProgressHeight - 1);

    const std::string title = progress_sending_ ? "Sending " : "Receiving ";
    const std::string filename = shorten(progress_name_, 42);
    const int percent = progress_total_ == 0
        ? 100
        : static_cast<int>((progress_done_ * 100) / progress_total_);

    std::ostringstream status;
    status << percent << "%  " << format_bytes(progress_done_)
           << " / " << format_bytes(progress_total_);

    XSetForeground(display_, progress_gc_, progress_fg_);
    const int baseline = progress_font_ != nullptr
        ? progress_font_->ascent + 8 : 18;
    XDrawString(display_, progress_window_, progress_gc_,
                16, baseline, (title + filename).c_str(),
                static_cast<int>((title + filename).size()));

    const std::string status_text = status.str();
    XDrawString(display_, progress_window_, progress_gc_,
                16, baseline + 24, status_text.c_str(),
                static_cast<int>(status_text.size()));

    XSetForeground(display_, progress_gc_, progress_bar_bg_);
    XFillRectangle(display_, progress_window_, progress_gc_,
                   bar_x, bar_y, bar_width, bar_height);

    int filled = 0;
    if (progress_total_ > 0) {
        filled = static_cast<int>(
            std::min<std::uint64_t>(progress_done_, progress_total_) *
            bar_width / progress_total_);
    }
    XSetForeground(display_, progress_gc_, progress_bar_);
    XFillRectangle(display_, progress_window_, progress_gc_,
                   bar_x, bar_y, filled, bar_height);
    XFlush(display_);
}

} // namespace inputleap
