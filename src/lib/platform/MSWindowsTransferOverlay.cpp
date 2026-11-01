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

    HRESULT STDMETHODCALLTYPE DragEnter(IDataObject* data, DWORD, POINTL,
                                        DWORD* effect) override
    {
        const bool accept = has_files(data);
        overlay_->set_glow(direction_, accept);
        *effect = accept ? DROPEFFECT_COPY : DROPEFFECT_NONE;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE DragOver(DWORD, POINTL, DWORD* effect) override
    {
        overlay_->set_glow(direction_, true);
        *effect = DROPEFFECT_COPY;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE DragLeave() override
    {
        overlay_->set_glow(direction_, false);
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE Drop(IDataObject* data, DWORD, POINTL,
                                   DWORD* effect) override
    {
        std::vector<std::string> files;
        if (has_files(data)) {
            read_files(data, files);
        }
        overlay_->set_glow(direction_, false);
        const bool accepted = !files.empty();
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
        UnregisterClassW(MAKEINTATOM(window_class_), instance_);
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
    return DefWindowProcW(window, message, wparam, lparam);
}

void MSWindowsTransferOverlay::create_windows()
{
    WNDCLASSEXW window_class{};
    window_class.cbSize = sizeof(window_class);
    window_class.lpfnWndProc = &MSWindowsTransferOverlay::window_proc;
    window_class.hInstance = instance_;
    window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    window_class.lpszClassName = L"InputLeapTransferOverlay";
    window_class_ = RegisterClassExW(&window_class);

    if (window_class_ == 0) {
        LOG_ERR("could not register the transfer overlay window class");
        return;
    }

    const DWORD ex_style = WS_EX_LAYERED | WS_EX_TOOLWINDOW |
                           WS_EX_NOACTIVATE | WS_EX_TOPMOST;

    for (EdgeWindow& edge : edges_) {
        edge.window = CreateWindowExW(
            ex_style, MAKEINTATOM(window_class_), L"InputLeap transfer line",
            WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, instance_, nullptr);
        if (edge.window == nullptr) {
            continue;
        }

        SetWindowLongPtrW(edge.window, GWLP_USERDATA,
                          reinterpret_cast<LONG_PTR>(this));
        edge.drop_target = std::make_unique<MSWindowsTransferDropTarget>(
            this, edge.direction);
        if (RegisterDragDrop(edge.window, edge.drop_target.get()) != S_OK) {
            LOG_WARN("could not register a transfer drop target");
            edge.drop_target.reset();
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

    const int line = edge.glowing ? kGlowWidth : kLineWidth;
    const std::uint32_t color = edge.glowing
        ? 0xFF8BDCFFu   // #8bdcff
        : 0xFF2F6FEDu;  // #2f6fed

    std::vector<std::uint32_t> pixels(
        static_cast<std::size_t>(edge.width) * edge.height, 0);

    auto set_pixel = [&](int x, int y) {
        if (x >= 0 && x < edge.width && y >= 0 && y < edge.height) {
            pixels[static_cast<std::size_t>(y) * edge.width + x] = color;
        }
    };

    switch (edge.direction) {
    case kLeft:
        for (int y = 0; y < edge.height; ++y) {
            for (int x = 0; x < line; ++x) {
                set_pixel(x, y);
            }
        }
        break;
    case kRight:
        for (int y = 0; y < edge.height; ++y) {
            for (int x = 0; x < line; ++x) {
                set_pixel(edge.width - 1 - x, y);
            }
        }
        break;
    case kTop:
        for (int y = 0; y < line; ++y) {
            for (int x = 0; x < edge.width; ++x) {
                set_pixel(x, y);
            }
        }
        break;
    case kBottom:
        for (int y = 0; y < line; ++y) {
            for (int x = 0; x < edge.width; ++x) {
                set_pixel(x, edge.height - 1 - y);
            }
        }
        break;
    default:
        return;
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
        bool near = false;
        switch (edge.direction) {
        case kLeft:
            near = last_x_ - screen_x_ <= kDropZone;
            break;
        case kRight:
            near = (screen_x_ + screen_width_ - 1) - last_x_ <= kDropZone;
            break;
        case kTop:
            near = last_y_ - screen_y_ <= kDropZone;
            break;
        case kBottom:
            near = (screen_y_ + screen_height_ - 1) - last_y_ <= kDropZone;
            break;
        default:
            break;
        }

        const bool glowing = edge.visible && dragging_ && near;
        if (glowing != edge.glowing) {
            edge.glowing = glowing;
            render_edge(edge);
        }
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

void MSWindowsTransferOverlay::button_down(int x, int y)
{
    button_down_ = true;
    dragging_ = false;
    start_x_ = x;
    start_y_ = y;
    last_x_ = x;
    last_y_ = y;
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
    }
    else if (dragging_) {
        update_glow();
    }
}

void MSWindowsTransferOverlay::button_up()
{
    button_down_ = false;
    dragging_ = false;
    update_visible();
}

void MSWindowsTransferOverlay::hide_lines()
{
    button_down_ = false;
    dragging_ = false;
    update_visible();
}

void MSWindowsTransferOverlay::drop_files(EDirection direction,
                                          std::vector<std::string> files)
{
    TransferDrop drop;
    drop.edge = direction;
    drop.files = std::move(files);
    events_->add_event(EventType::TRANSFER_FILES_DROPPED, target_,
                       create_event_data<TransferDrop>(drop));

    dragging_ = false;
    update_visible();
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
        MAKEINTATOM(window_class_), L"InputLeap transfer progress",
        WS_POPUP, 0, 0, kProgressWidth, kProgressHeight,
        nullptr, nullptr, instance_, nullptr);

    if (progress_window_ != nullptr) {
        SetWindowLongPtrW(progress_window_, GWLP_USERDATA,
                          reinterpret_cast<LONG_PTR>(this));
        SetLayeredWindowAttributes(progress_window_, 0, 235, LWA_ALPHA);
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

    position_progress_window();
    ShowWindow(progress_window_, SW_SHOWNOACTIVATE);
    InvalidateRect(progress_window_, nullptr, TRUE);
    UpdateWindow(progress_window_);
}

void MSWindowsTransferOverlay::finish_progress(const std::string& message,
                                               bool success)
{
    LOG_DEBUG("transfer overlay finished: success=%d message=%s",
              success, message.c_str());
    if (progress_window_ != nullptr) {
        ShowWindow(progress_window_, SW_HIDE);
    }
    progress_visible_ = false;
}

void MSWindowsTransferOverlay::paint_progress(HDC dc)
{
    RECT client{};
    GetClientRect(progress_window_, &client);

    HBRUSH background = CreateSolidBrush(RGB(16, 21, 28));
    FillRect(dc, &client, background);
    DeleteObject(background);

    HPEN border = CreatePen(PS_SOLID, 1, RGB(76, 157, 255));
    HGDIOBJ old_pen = SelectObject(dc, border);
    HGDIOBJ old_brush = SelectObject(dc, GetStockObject(NULL_BRUSH));
    Rectangle(dc, 0, 0, client.right, client.bottom);
    SelectObject(dc, old_brush);
    SelectObject(dc, old_pen);
    DeleteObject(border);

    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(242, 246, 251));
    HFONT font = static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
    HGDIOBJ old_font = SelectObject(dc, font);

    const std::wstring title = (progress_sending_ ? L"Sending " : L"Receiving ") +
                               wide(progress_name_);
    RECT title_rect{14, 8, client.right - 14, 30};
    DrawTextW(dc, title.c_str(), -1, &title_rect,
              DT_SINGLELINE | DT_LEFT | DT_VCENTER | DT_END_ELLIPSIS);

    const int percent = progress_total_ == 0
        ? 100
        : static_cast<int>((progress_done_ * 100) / progress_total_);
    const std::string status_text = std::to_string(percent) + "%  " +
        format_bytes(progress_done_) + " / " + format_bytes(progress_total_);
    const std::wstring status = wide(status_text);
    RECT status_rect{14, 30, client.right - 14, 50};
    DrawTextW(dc, status.c_str(), -1, &status_rect,
              DT_SINGLELINE | DT_LEFT | DT_VCENTER);

    RECT bar{14, 58, client.right - 14, 70};
    HBRUSH bar_background = CreateSolidBrush(RGB(41, 50, 65));
    FillRect(dc, &bar, bar_background);
    DeleteObject(bar_background);

    RECT filled = bar;
    if (progress_total_ > 0) {
        filled.right = bar.left + static_cast<LONG>(
            (std::min<std::uint64_t>(progress_done_, progress_total_) *
             (bar.right - bar.left)) / progress_total_);
    }
    else {
        filled.right = bar.right;
    }
    HBRUSH bar_brush = CreateSolidBrush(RGB(76, 157, 255));
    FillRect(dc, &filled, bar_brush);
    DeleteObject(bar_brush);

    SelectObject(dc, old_font);
}

} // namespace inputleap
