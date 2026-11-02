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

#include "platform/MSWindowsTransferOverlay.h"

#include "inputleap/FileTransfer.h"
#include "base/Event.h"
#include "base/IEventQueue.h"
#include "base/Log.h"

#include <ShellAPI.h>
#include <oleidl.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <utility>

namespace inputleap {

namespace {

std::string utf8_from_wide(const std::wstring& value)
{
    if (value.empty()) {
        return {};
    }
    const int size = WideCharToMultiByte(CP_UTF8, 0, value.c_str(),
                                         static_cast<int>(value.size()),
                                         nullptr, 0, nullptr, nullptr);
    if (size <= 0) {
        return {};
    }
    std::string result(static_cast<std::size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.c_str(),
                        static_cast<int>(value.size()),
                        result.data(), size, nullptr, nullptr);
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
        std::snprintf(buffer, sizeof(buffer), "%.1f MB",
                      bytes / (1024.0 * 1024.0));
    }
    else {
        std::snprintf(buffer, sizeof(buffer), "%.2f GB",
                      bytes / (1024.0 * 1024.0 * 1024.0));
    }
    return buffer;
}

bool is_process_elevated()
{
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
        return false;
    }

    TOKEN_ELEVATION elevation{};
    DWORD size = sizeof(elevation);
    const bool elevated =
        GetTokenInformation(token, TokenElevation, &elevation,
                            sizeof(elevation), &size) != 0 &&
        elevation.TokenIsElevated != 0;
    CloseHandle(token);
    return elevated;
}

} // namespace

class MSWindowsTransferDropTarget : public IDropTarget {
public:
    MSWindowsTransferDropTarget(MSWindowsTransferOverlay* overlay,
                                EDirection direction) :
        overlay_(overlay),
        direction_(direction)
    {
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** object) override
    {
        if (object == nullptr) {
            return E_INVALIDARG;
        }
        if (iid == IID_IUnknown || iid == IID_IDropTarget) {
            AddRef();
            *object = this;
            return S_OK;
        }
        *object = nullptr;
        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() override
    {
        return InterlockedIncrement(&references_);
    }

    ULONG STDMETHODCALLTYPE Release() override
    {
        const LONG count = InterlockedDecrement(&references_);
        if (count == 0) {
            delete this;
        }
        return count;
    }

    HRESULT STDMETHODCALLTYPE DragEnter(IDataObject* data, DWORD, POINTL pt,
                                        DWORD* effect) override
    {
        accepted_ = has_files(data);
        POINT cursor{};
        GetCursorPos(&cursor);
        LOG_DEBUG("OLE drag enter on edge %d, files=%d cursor=%ld,%ld",
                  static_cast<int>(direction_), accepted_ ? 1 : 0,
                  cursor.x, cursor.y);
        overlay_->set_file_drag(accepted_);
        overlay_->set_glow(direction_, accepted_);
        *effect = accepted_ ? DROPEFFECT_COPY : DROPEFFECT_NONE;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE DragOver(DWORD, POINTL, DWORD* effect) override
    {
        overlay_->set_glow(direction_, accepted_);
        *effect = accepted_ ? DROPEFFECT_COPY : DROPEFFECT_NONE;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE DragLeave() override
    {
        POINT cursor{};
        GetCursorPos(&cursor);
        LOG_DEBUG("OLE drag leave on edge %d cursor=%ld,%ld",
                  static_cast<int>(direction_), cursor.x, cursor.y);
        accepted_ = false;
        overlay_->set_file_drag(false);
        overlay_->set_glow(direction_, false);
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE Drop(IDataObject* data, DWORD, POINTL,
                                   DWORD* effect) override
    {
        std::vector<std::string> files;
        const bool had_files = data != nullptr && has_files(data);
        if (had_files) {
            read_files(data, files);
        }
        LOG_DEBUG("OLE drop on edge %d, accepted=%d had_files=%d files=%zd",
                  static_cast<int>(direction_), accepted_ ? 1 : 0,
                  had_files ? 1 : 0, files.size());
        overlay_->set_glow(direction_, false);
        const bool accepted = accepted_ && !files.empty();
        accepted_ = false;
        overlay_->set_file_drag(false);
        if (accepted) {
            overlay_->drop_files(direction_, std::move(files));
        }
        *effect = accepted ? DROPEFFECT_COPY : DROPEFFECT_NONE;
        return S_OK;
    }

private:
    bool has_files(IDataObject* data) const
    {
        FORMATETC format = {CF_HDROP, nullptr, DVASPECT_CONTENT, -1,
                            TYMED_HGLOBAL};
        return data != nullptr && data->QueryGetData(&format) == S_OK;
    }

    void read_files(IDataObject* data, std::vector<std::string>& files) const
    {
        FORMATETC format = {CF_HDROP, nullptr, DVASPECT_CONTENT, -1,
                            TYMED_HGLOBAL};
        STGMEDIUM medium{};
        if (data->GetData(&format, &medium) != S_OK) {
            return;
        }

        HDROP drop = static_cast<HDROP>(GlobalLock(medium.hGlobal));
        if (drop != nullptr) {
            const UINT count = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
            for (UINT i = 0; i < count; ++i) {
                const UINT length = DragQueryFileW(drop, i, nullptr, 0);
                std::wstring name(length + 1, L'\0');
                DragQueryFileW(drop, i, name.data(), length + 1);
                name.resize(length);
                files.push_back(utf8_from_wide(name));
            }
            GlobalUnlock(medium.hGlobal);
        }
        ReleaseStgMedium(&medium);
    }

    LONG references_ = 1;
    bool accepted_ = false;
    MSWindowsTransferOverlay* overlay_;
    EDirection direction_;
};

MSWindowsTransferOverlay::MSWindowsTransferOverlay(HINSTANCE instance,
                                                   IEventQueue* events,
                                                   const EventTarget* target) :
    instance_(instance),
    events_(events),
    target_(target)
{
    edges_[0].direction = kLeft;
    edges_[1].direction = kRight;
    edges_[2].direction = kTop;
    edges_[3].direction = kBottom;

    elevated_ = is_process_elevated();
    create_windows();
    set_geometry();
}

MSWindowsTransferOverlay::~MSWindowsTransferOverlay()
{
    destroy_windows();

    if (progress_window_ != nullptr) {
        DestroyWindow(progress_window_);
        progress_window_ = nullptr;
    }
    if (window_class_ != 0) {
        UnregisterClassW(L"InputLeapTransferOverlay", instance_);
    }
}

LRESULT CALLBACK MSWindowsTransferOverlay::window_proc(HWND window, UINT message,
                                                       WPARAM wparam, LPARAM lparam)
{
    auto* overlay = reinterpret_cast<MSWindowsTransferOverlay*>(
        GetWindowLongPtrW(window, GWLP_USERDATA));
    if (overlay != nullptr) {
        return overlay->handle_message(window, message, wparam, lparam);
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

LRESULT MSWindowsTransferOverlay::handle_message(HWND window, UINT message,
                                                 WPARAM wparam, LPARAM lparam)
{
    if (window == progress_window_) {
        if (message == WM_PAINT) {
            PAINTSTRUCT paint{};
            HDC dc = BeginPaint(window, &paint);
            paint_progress(dc);
            EndPaint(window, &paint);
            return 0;
        }
        if (message == WM_ERASEBKGND) {
            return 1;
        }
    }

    if (message == WM_TIMER && wparam == 1) {
        ++pulse_phase_;
        if (pulse_phase_ > 400) {
            pulse_phase_ = 0;
        }
        if (dragging_ && (file_drag_ || elevated_)) {
            for (EdgeWindow& edge : edges_) {
                if (edge.visible) {
                    render_edge(edge);
                }
            }
        }
        if (progress_visible_) {
            const double target = progress_total_ != 0
                ? 100.0 * static_cast<double>(progress_done_) /
                      static_cast<double>(progress_total_)
                : 100.0;
            const double previous = progress_shown_;
            progress_shown_ += (target - progress_shown_) * 0.22;
            if (std::abs(target - progress_shown_) < 0.2) {
                progress_shown_ = target;
            }
            if (progress_window_ != nullptr &&
                std::abs(progress_shown_ - previous) >= 0.05) {
                InvalidateRect(progress_window_, nullptr, FALSE);
            }
        }
        return 0;
    }

    if (message == WM_TIMER && wparam == 2) {
        KillTimer(window, 2);
        if (progress_window_ != nullptr) {
            ShowWindow(progress_window_, SW_HIDE);
        }
        progress_visible_ = false;
        update_animation();
        return 0;
    }

    if (message == WM_DROPFILES) {
        EdgeWindow* edge = window_to_edge(window);
        auto drop = reinterpret_cast<HDROP>(wparam);
        LOG_DEBUG("WM_DROPFILES on edge %d (elevated=%d)",
                  edge != nullptr ? static_cast<int>(edge->direction) : -1,
                  elevated_ ? 1 : 0);
        if (edge != nullptr && drop != nullptr) {
            std::vector<std::string> files;
            const UINT count = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
            for (UINT i = 0; i < count; ++i) {
                const UINT length = DragQueryFileW(drop, i, nullptr, 0);
                std::wstring name(length + 1, L'\0');
                DragQueryFileW(drop, i, name.data(), length + 1);
                name.resize(length);
                files.push_back(utf8_from_wide(name));
            }
            DragFinish(drop);
            if (!files.empty()) {
                drop_files(edge->direction, std::move(files));
            }
            return 0;
        }
        DragFinish(drop);
        return 0;
    }

    return DefWindowProcW(window, message, wparam, lparam);
}

void MSWindowsTransferOverlay::create_windows()
{
    WNDCLASSEXW window_class{};
    window_class.cbSize = sizeof(window_class);
    window_class.lpfnWndProc = &MSWindowsTransferOverlay::window_proc;
    window_class.hInstance = instance_;
    window_class.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
    window_class.lpszClassName = L"InputLeapTransferOverlay";
    window_class.style = CS_DROPSHADOW;
    window_class_ = RegisterClassExW(&window_class);

    if (window_class_ == 0) {
        LOG_ERR("could not register the transfer overlay window class");
        return;
    }

    const DWORD ex_style = WS_EX_LAYERED | WS_EX_TOOLWINDOW |
                           WS_EX_NOACTIVATE | WS_EX_TOPMOST;

    for (EdgeWindow& edge : edges_) {
        edge.window = CreateWindowExW(
            ex_style, L"InputLeapTransferOverlay", L"InputLeap transfer line",
            WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, instance_, nullptr);
        if (edge.window == nullptr) {
            continue;
        }

        SetWindowLongPtrW(edge.window, GWLP_USERDATA,
                          reinterpret_cast<LONG_PTR>(this));
        if (elevated_) {
            // UIPI does not let a medium integrity drag source (Explorer)
            // talk to an elevated OLE drop target, so fall back to the
            // legacy WM_DROPFILES path, which can be allowed explicitly.
            ChangeWindowMessageFilterEx(edge.window, WM_DROPFILES,
                                        MSGFLT_ALLOW, nullptr);
            ChangeWindowMessageFilterEx(edge.window, 0x0049 /* WM_COPYGLOBALDATA */,
                                        MSGFLT_ALLOW, nullptr);
            ChangeWindowMessageFilterEx(edge.window, WM_COPYDATA,
                                        MSGFLT_ALLOW, nullptr);
            DragAcceptFiles(edge.window, TRUE);
        }
        else {
            edge.drop_target = std::make_unique<MSWindowsTransferDropTarget>(
                this, edge.direction);
            if (RegisterDragDrop(edge.window, edge.drop_target.get()) != S_OK) {
                LOG_WARN("could not register a transfer drop target");
                edge.drop_target.reset();
            }
        }
        ShowWindow(edge.window, SW_HIDE);
    }
}

void MSWindowsTransferOverlay::destroy_windows()
{
    for (EdgeWindow& edge : edges_) {
        if (edge.window != nullptr) {
            if (edge.drop_target) {
                RevokeDragDrop(edge.window);
                edge.drop_target.reset();
            }
            if (elevated_) {
                DragAcceptFiles(edge.window, FALSE);
            }
            DestroyWindow(edge.window);
            edge.window = nullptr;
        }
    }
}

void MSWindowsTransferOverlay::set_geometry()
{
    screen_x_ = GetSystemMetrics(SM_XVIRTUALSCREEN);
    screen_y_ = GetSystemMetrics(SM_YVIRTUALSCREEN);
    screen_width_ = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    screen_height_ = GetSystemMetrics(SM_CYVIRTUALSCREEN);

    for (EdgeWindow& edge : edges_) {
        position_edge(edge);
    }
    position_progress_window();
}

void MSWindowsTransferOverlay::set_peers(const std::vector<EDirection>& peers)
{
    peers_ = peers;
    update_visible();
}

int MSWindowsTransferOverlay::line_span(int screen_size)
{
    const int span = screen_size * kLineSpanPercent / 100;
    return std::max(kMinLineSpan, std::min(span, screen_size));
}

void MSWindowsTransferOverlay::position_edge(EdgeWindow& edge)
{
    if (edge.window == nullptr) {
        return;
    }

    switch (edge.direction) {
    case kLeft:
        edge.x = screen_x_;
        edge.y = screen_y_;
        edge.width = kDropZone;
        edge.height = screen_height_;
        break;
    case kRight:
        edge.x = screen_x_ + screen_width_ - kDropZone;
        edge.y = screen_y_;
        edge.width = kDropZone;
        edge.height = screen_height_;
        break;
    case kTop:
        edge.x = screen_x_;
        edge.y = screen_y_;
        edge.width = screen_width_;
        edge.height = kDropZone;
        break;
    case kBottom:
        edge.x = screen_x_;
        edge.y = screen_y_ + screen_height_ - kDropZone;
        edge.width = screen_width_;
        edge.height = kDropZone;
        break;
    default:
        return;
    }

    SetWindowPos(edge.window, HWND_TOPMOST, edge.x, edge.y,
                 edge.width, edge.height,
                 SWP_NOACTIVATE | SWP_NOOWNERZORDER);
    render_edge(edge);
}

void MSWindowsTransferOverlay::render_edge(EdgeWindow& edge)
{
    if (edge.window == nullptr || edge.width <= 0 || edge.height <= 0) {
        return;
    }

    // Only paint the accent line once a file drag is over the edge, or when
    // the process is elevated and the legacy WM_DROPFILES path gives us no
    // early warning. The rest of the drop zone stays hit-testable with the
    // smallest non-zero alpha.

    // Neon line: a bright core with a soft gradient halo. The pulse
    // animation modulates the intensity while a file drag is active.
    const bool show_line = file_drag_ || elevated_;
    const double pulse = 0.72 + 0.28 * std::sin(pulse_phase_ * 0.16);
    const int thickness = edge.glowing ? 8 : 6;
    const int halo = edge.glowing ? kPulseRadius : 12;

    std::vector<std::uint32_t> pixels(
        static_cast<std::size_t>(edge.width) * edge.height, 0x01000000u);

    auto blend_pixel = [&](int x, int y, double intensity) {
        if (x < 0 || x >= edge.width || y < 0 || y >= edge.height ||
            intensity <= 0.0) {
            return;
        }
        const double clamped = std::min(1.0, intensity) * pulse;
        const int alpha = static_cast<int>(clamped * clamped * 255.0);
        if (alpha <= 1) {
            return;
        }
        const double t = std::min(1.0, clamped);
        const int r = static_cast<int>(47 + (139 - 47) * t);
        const int g = static_cast<int>(111 + (220 - 111) * t);
        const int b = static_cast<int>(237 + (255 - 237) * t);
        const std::uint32_t value =
            (static_cast<std::uint32_t>(alpha) << 24) |
            (static_cast<std::uint32_t>(r * alpha / 255) << 16) |
            (static_cast<std::uint32_t>(g * alpha / 255) << 8) |
            static_cast<std::uint32_t>(b * alpha / 255);
        pixels[static_cast<std::size_t>(y) * edge.width + x] = value;
    };

    const int span_vertical = line_span(edge.height);
    const int start_vertical = (edge.height - span_vertical) / 2;
    const int span_horizontal = line_span(edge.width);
    const int start_horizontal = (edge.width - span_horizontal) / 2;

    if (show_line) {
        auto intensity_for = [&](int distance) {
            if (distance <= thickness / 2) {
                return 1.0;
            }
            const double falloff =
                1.0 - static_cast<double>(distance - thickness / 2) / halo;
            return falloff > 0.0 ? falloff * falloff * 0.9 : 0.0;
        };

        switch (edge.direction) {
        case kLeft:
        case kRight: {
            const int center = kLineInset + thickness / 2;
            for (int y = start_vertical; y < start_vertical + span_vertical; ++y) {
                for (int d = -halo; d <= halo; ++d) {
                    const int x = edge.direction == kLeft
                        ? center + d
                        : edge.width - 1 - center - d;
                    blend_pixel(x, y, intensity_for(std::abs(d)));
                }
            }
            break;
        }
        case kTop:
        case kBottom: {
            const int center = kLineInset + thickness / 2;
            for (int x = start_horizontal; x < start_horizontal + span_horizontal; ++x) {
                for (int d = -halo; d <= halo; ++d) {
                    const int y = edge.direction == kTop
                        ? center + d
                        : edge.height - 1 - center - d;
                    blend_pixel(x, y, intensity_for(std::abs(d)));
                }
            }
            break;
        }
        default:
            return;
        }
    }
    HDC screen = GetDC(nullptr);
    HDC memory = CreateCompatibleDC(screen);

    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = edge.width;
    info.bmiHeader.biHeight = -edge.height;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;

    void* bits = nullptr;
    HBITMAP bitmap = CreateDIBSection(screen, &info, DIB_RGB_COLORS,
                                      &bits, nullptr, 0);
    if (bitmap != nullptr && bits != nullptr) {
        HGDIOBJ old = SelectObject(memory, bitmap);
        std::memcpy(bits, pixels.data(), pixels.size() * sizeof(std::uint32_t));

        BLENDFUNCTION blend{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
        POINT destination{edge.x, edge.y};
        SIZE size{edge.width, edge.height};
        POINT source{0, 0};
        UpdateLayeredWindow(edge.window, screen, &destination, &size,
                            memory, &source, 0, &blend, ULW_ALPHA);

        SelectObject(memory, old);
        DeleteObject(bitmap);
    }

    DeleteDC(memory);
    ReleaseDC(nullptr, screen);

    if (edge.visible) {
        ShowWindow(edge.window, SW_SHOWNOACTIVATE);
        SetWindowPos(edge.window, HWND_TOPMOST, edge.x, edge.y,
                     edge.width, edge.height,
                     SWP_NOACTIVATE | SWP_NOOWNERZORDER);
    }
}

void MSWindowsTransferOverlay::update_visible()
{
    for (EdgeWindow& edge : edges_) {
        const bool peer = std::find(peers_.begin(), peers_.end(),
                                    edge.direction) != peers_.end();
        const bool show = dragging_ && peer;
        if (show == edge.visible) {
            continue;
        }

        edge.visible = show;
        if (show) {
            render_edge(edge);
            ShowWindow(edge.window, SW_SHOWNOACTIVATE);
            SetWindowPos(edge.window, HWND_TOPMOST, edge.x, edge.y,
                         edge.width, edge.height,
                         SWP_NOACTIVATE | SWP_NOOWNERZORDER);
        }
        else {
            edge.glowing = false;
            ShowWindow(edge.window, SW_HIDE);
        }
    }
    update_glow();
}

void MSWindowsTransferOverlay::update_glow()
{
    for (EdgeWindow& edge : edges_) {
        bool is_near = false;
        switch (edge.direction) {
        case kLeft:
            is_near = last_x_ - screen_x_ <= kDropZone;
            break;
        case kRight:
            is_near = (screen_x_ + screen_width_ - 1) - last_x_ <= kDropZone;
            break;
        case kTop:
            is_near = last_y_ - screen_y_ <= kDropZone;
            break;
        case kBottom:
            is_near = (screen_y_ + screen_height_ - 1) - last_y_ <= kDropZone;
            break;
        default:
            break;
        }

        const bool glowing = edge.visible && dragging_ &&
                             (file_drag_ || elevated_) && is_near;
        if (glowing != edge.glowing) {
            edge.glowing = glowing;
            render_edge(edge);
        }
    }
}

void MSWindowsTransferOverlay::update_animation()
{
    HWND host = edges_[0].window;
    if (host == nullptr) {
        return;
    }

    const bool needed = progress_visible_ ||
                        (dragging_ && (file_drag_ || elevated_));
    if (needed && !animation_timer_) {
        SetTimer(host, 1, 33, nullptr);
        animation_timer_ = true;
    }
    else if (!needed && animation_timer_) {
        KillTimer(host, 1);
        animation_timer_ = false;
    }
}

void MSWindowsTransferOverlay::set_glow(EDirection direction, bool glowing)
{
    EdgeWindow* edge = direction_to_edge(direction);
    if (edge == nullptr || !edge->visible || edge->glowing == glowing) {
        return;
    }
    edge->glowing = glowing;
    render_edge(*edge);
}

void MSWindowsTransferOverlay::set_file_drag(bool active)
{
    if (file_drag_ == active) {
        return;
    }

    file_drag_ = active;
    for (EdgeWindow& edge : edges_) {
        if (edge.visible) {
            render_edge(edge);
        }
    }
    update_glow();
    update_animation();
}

void MSWindowsTransferOverlay::button_down(int x, int y)
{
    button_down_ = true;
    dragging_ = false;
    start_x_ = x;
    start_y_ = y;
    last_x_ = x;
    last_y_ = y;
}

void MSWindowsTransferOverlay::resume_drag()
{
    // The pointer left this screen while the button was still held and came
    // back; show the drop zones again so the file can be dropped without
    // pressing the button a second time.
    POINT position{};
    if (!GetCursorPos(&position)) {
        return;
    }

    button_down_ = true;
    dragging_ = true;
    file_drag_ = false;
    start_x_ = position.x;
    start_y_ = position.y;
    last_x_ = position.x;
    last_y_ = position.y;
    update_visible();
    update_animation();
}

void MSWindowsTransferOverlay::motion(int x, int y)
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
        update_animation();
    }
    else if (dragging_) {
        update_glow();
    }
}

void MSWindowsTransferOverlay::button_up()
{
    button_down_ = false;
    dragging_ = false;

    // When a file drag is over the drop zone, keep the edge windows mapped
    // until the drop target receives Drop or DragLeave. Hiding them here
    // would make OLE cancel the drop and send DragLeave instead.
    if (!file_drag_) {
        update_visible();
        update_animation();
    }
}

void MSWindowsTransferOverlay::hide_lines()
{
    button_down_ = false;
    dragging_ = false;
    file_drag_ = false;
    update_visible();
}

void MSWindowsTransferOverlay::drop_files(EDirection direction,
                                          std::vector<std::string> files)
{
    LOG_DEBUG("transfer drop: %zd file(s) toward direction %d",
              files.size(), static_cast<int>(direction));

    TransferDrop drop;
    drop.edge = direction;
    drop.files = std::move(files);
    events_->add_event(EventType::TRANSFER_FILES_DROPPED, target_,
                       create_event_data<TransferDrop>(drop));

    dragging_ = false;
    file_drag_ = false;
    update_visible();
    update_animation();
}

MSWindowsTransferOverlay::EdgeWindow*
MSWindowsTransferOverlay::window_to_edge(HWND window)
{
    for (EdgeWindow& edge : edges_) {
        if (edge.window == window) {
            return &edge;
        }
    }
    return nullptr;
}

MSWindowsTransferOverlay::EdgeWindow*
MSWindowsTransferOverlay::direction_to_edge(EDirection direction)
{
    for (EdgeWindow& edge : edges_) {
        if (edge.direction == direction) {
            return &edge;
        }
    }
    return nullptr;
}

std::wstring MSWindowsTransferOverlay::wide(const std::string& value) const
{
    if (value.empty()) {
        return {};
    }
    const int size = MultiByteToWideChar(CP_UTF8, 0, value.c_str(),
                                         static_cast<int>(value.size()),
                                         nullptr, 0);
    if (size <= 0) {
        return {};
    }
    std::wstring result(static_cast<std::size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value.c_str(),
                        static_cast<int>(value.size()),
                        result.data(), size);
    return result;
}

void MSWindowsTransferOverlay::create_progress_window()
{
    if (progress_window_ != nullptr || window_class_ == 0) {
        return;
    }

    progress_window_ = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW |
            WS_EX_NOACTIVATE | WS_EX_TOPMOST,
        L"InputLeapTransferOverlay", L"InputLeap transfer progress",
        WS_POPUP, 0, 0, kProgressWidth, kProgressHeight,
        nullptr, nullptr, instance_, nullptr);

    if (progress_window_ != nullptr) {
        SetWindowLongPtrW(progress_window_, GWLP_USERDATA,
                          reinterpret_cast<LONG_PTR>(this));
        SetLayeredWindowAttributes(progress_window_, 0, 255, LWA_ALPHA);
        SetWindowRgn(progress_window_,
                     CreateRoundRectRgn(0, 0, kProgressWidth + 1,
                                        kProgressHeight + 1, 20, 20),
                     TRUE);

        // Windows 11 gives the window proper rounded corners and a shadow.
        using DwmSetWindowAttributeFn =
            HRESULT(WINAPI*)(HWND, DWORD, LPCVOID, DWORD);
        HMODULE dwm = LoadLibraryW(L"dwmapi.dll");
        if (dwm != nullptr) {
            auto set_attribute =
                reinterpret_cast<DwmSetWindowAttributeFn>(
                    GetProcAddress(dwm, "DwmSetWindowAttribute"));
            if (set_attribute != nullptr) {
                const DWORD preference = 2; // DWMWCP_ROUND
                set_attribute(progress_window_, 33,
                              &preference, sizeof(preference));
            }
            FreeLibrary(dwm);
        }

        ShowWindow(progress_window_, SW_HIDE);
    }
}

void MSWindowsTransferOverlay::position_progress_window()
{
    if (progress_window_ == nullptr) {
        return;
    }
    const int x = screen_x_ + (screen_width_ - kProgressWidth) / 2;
    const int y = screen_y_ + std::max(48, screen_height_ / 8);
    SetWindowPos(progress_window_, HWND_TOPMOST, x, y,
                 kProgressWidth, kProgressHeight,
                 SWP_NOACTIVATE | SWP_NOOWNERZORDER);
}

void MSWindowsTransferOverlay::show_progress(const std::string& name,
                                             std::uint64_t done,
                                             std::uint64_t total,
                                             bool sending)
{
    create_progress_window();
    if (progress_window_ == nullptr) {
        return;
    }

    progress_name_ = name;
    progress_done_ = done;
    progress_total_ = total;
    progress_sending_ = sending;
    progress_visible_ = true;
    progress_done_state_ = false;
    progress_success_ = false;

    if (progress_shown_ > 100.0) {
        progress_shown_ = 0.0;
    }

    LOG_DEBUG("showing transfer progress: %s %llu/%llu sending=%d",
              name.c_str(), static_cast<unsigned long long>(done),
              static_cast<unsigned long long>(total), sending ? 1 : 0);

    position_progress_window();
    ShowWindow(progress_window_, SW_SHOWNOACTIVATE);
    InvalidateRect(progress_window_, nullptr, FALSE);
    UpdateWindow(progress_window_);
    update_animation();
}

void MSWindowsTransferOverlay::finish_progress(const std::string& message,
                                               bool success)
{
    LOG_DEBUG("transfer overlay finished: success=%d message=%s",
              success, message.c_str());

    if (progress_window_ == nullptr) {
        progress_visible_ = false;
        update_animation();
        return;
    }

    progress_done_state_ = true;
    progress_success_ = success;
    progress_message_ = wide(message);
    progress_shown_ = 100.0;
    progress_visible_ = true;

    ShowWindow(progress_window_, SW_SHOWNOACTIVATE);
    InvalidateRect(progress_window_, nullptr, FALSE);
    UpdateWindow(progress_window_);
    SetTimer(progress_window_, 2, 1700, nullptr);
    update_animation();
}

void MSWindowsTransferOverlay::paint_progress(HDC dc)
{
    RECT client{};
    GetClientRect(progress_window_, &client);
    const int width = client.right;
    const int height = client.bottom;

    // Paint into an off-screen bitmap and blit once, so the animated
    // percentage and progress bar do not flicker.
    HDC target = dc;
    HDC buffer = CreateCompatibleDC(target);
    HBITMAP bitmap = CreateCompatibleBitmap(target, width, height);
    if (buffer == nullptr || bitmap == nullptr) {
        if (bitmap != nullptr) DeleteObject(bitmap);
        if (buffer != nullptr) DeleteDC(buffer);
        return;
    }
    HGDIOBJ old_bitmap = SelectObject(buffer, bitmap);
    dc = buffer;

    const COLORREF accent = progress_done_state_
        ? (progress_success_ ? RGB(49, 196, 141) : RGB(240, 106, 122))
        : RGB(47, 111, 237);

    // Card background and border.
    HBRUSH background = CreateSolidBrush(RGB(15, 20, 27));
    FillRect(dc, &client, background);
    DeleteObject(background);

    HPEN border = CreatePen(PS_SOLID, 1, RGB(38, 50, 67));
    HGDIOBJ old_pen = SelectObject(dc, border);
    HGDIOBJ old_brush = SelectObject(dc, GetStockObject(NULL_BRUSH));
    Rectangle(dc, 0, 0, width, height);
    SelectObject(dc, old_brush);
    SelectObject(dc, old_pen);
    DeleteObject(border);

    // Accent bar along the top edge.
    RECT accent_bar{0, 0, width, 3};
    HBRUSH accent_brush = CreateSolidBrush(accent);
    FillRect(dc, &accent_bar, accent_brush);

    SetBkMode(dc, TRANSPARENT);

    HFONT title_font = CreateFontW(-15, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE,
                                   FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                                   CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                   DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    HFONT big_font = CreateFontW(-26, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                                 DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                                 CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                 DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    HFONT small_font = CreateFontW(-12, 0, 0, 0, FW_NORMAL, FALSE, FALSE,
                                   FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                                   CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                   DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");

    std::wstring title;
    if (progress_done_state_) {
        title = (progress_success_ ? L"Transfer complete  \u2713"
                                   : L"Transfer failed  \u2715");
    }
    else {
        title = (progress_sending_ ? L"Sending  " : L"Receiving  ") +
                wide(progress_name_);
    }

    HGDIOBJ old_font = SelectObject(dc, title_font);
    SetTextColor(dc, RGB(230, 237, 247));
    RECT title_rect{20, 14, width - 110, 38};
    DrawTextW(dc, title.c_str(), -1, &title_rect,
              DT_SINGLELINE | DT_LEFT | DT_VCENTER | DT_END_ELLIPSIS);

    // Large percentage on the right.
    wchar_t percent_text[16];
    swprintf_s(percent_text, L"%d%%", static_cast<int>(progress_shown_ + 0.5));
    SelectObject(dc, big_font);
    SetTextColor(dc, accent);
    RECT percent_rect{width - 104, 10, width - 20, 44};
    DrawTextW(dc, percent_text, -1, &percent_rect,
              DT_SINGLELINE | DT_RIGHT | DT_VCENTER);

    // Status line.
    SelectObject(dc, small_font);
    SetTextColor(dc, RGB(143, 164, 194));
    std::wstring status;
    if (progress_done_state_) {
        status = progress_message_;
    }
    else {
        status = wide(format_bytes(progress_done_) + " / " +
                      format_bytes(progress_total_));
    }
    RECT status_rect{20, 42, width - 20, 62};
    DrawTextW(dc, status.c_str(), -1, &status_rect,
              DT_SINGLELINE | DT_LEFT | DT_VCENTER | DT_END_ELLIPSIS);

    // Progress track.
    const RECT track{20, 74, width - 20, 88};
    HBRUSH track_brush = CreateSolidBrush(RGB(27, 36, 50));
    FillRect(dc, &track, track_brush);
    DeleteObject(track_brush);

    const double fraction = std::max(0.0, std::min(1.0, progress_shown_ / 100.0));
    const int track_width = static_cast<int>(track.right - track.left);
    const int filled_width = static_cast<int>(track_width * fraction);
    if (filled_width > 0) {
        // Horizontal gradient from the accent colour to the light core.
        for (int x = 0; x < filled_width; ++x) {
            const double t = static_cast<double>(x) /
                             std::max(1, track_width);
            const int r = static_cast<int>(47 + (139 - 47) * t);
            const int g = static_cast<int>(111 + (220 - 111) * t);
            const int b = static_cast<int>(237 + (255 - 237) * t);
            RECT column{track.left + x, track.top, track.left + x + 1,
                        track.bottom};
            HBRUSH column_brush = CreateSolidBrush(RGB(r, g, b));
            FillRect(dc, &column, column_brush);
            DeleteObject(column_brush);
        }
    }

    SelectObject(dc, old_font);
    DeleteObject(title_font);
    DeleteObject(big_font);
    DeleteObject(small_font);
    SelectObject(buffer, old_bitmap);
    BitBlt(target, 0, 0, width, height, buffer, 0, 0, SRCCOPY);
    DeleteObject(bitmap);
    DeleteDC(buffer);
}
} // namespace inputleap
