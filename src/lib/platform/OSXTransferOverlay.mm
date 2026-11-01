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

#include "platform/OSXTransferOverlay.h"

#include "inputleap/FileTransfer.h"
#include "base/Event.h"
#include "base/IEventQueue.h"
#include "base/Log.h"

#import <Cocoa/Cocoa.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <sstream>
#include <utility>

namespace inputleap {

namespace {

constexpr int kDropZone = 64;
constexpr int kLineWidth = 12;
constexpr int kGlowWidth = 20;
constexpr int kProgressWidth = 460;
constexpr int kProgressHeight = 82;

const NSWindowStyleMask kOverlayStyle = NSWindowStyleMaskBorderless;
const NSBackingStoreType kOverlayBacking = NSBackingStoreBuffered;

std::string utf8(NSString* value)
{
    if (value == nil) {
        return {};
    }
    const char* text = [value UTF8String];
    return text == nullptr ? std::string() : std::string(text);
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

} // namespace inputleap

using inputleap::EDirection;
using inputleap::OSXTransferOverlay;

@interface ILTransferLineView : NSView <NSDraggingDestination>
{
    OSXTransferOverlay* overlay_;
    EDirection direction_;
    BOOL glowing_;
}
- (void)setOverlay:(OSXTransferOverlay*)overlay;
- (void)setDirection:(EDirection)direction;
- (void)setGlowing:(BOOL)glowing;
@end

@interface ILTransferProgressView : NSView
{
    std::string name_;
    std::uint64_t done_;
    std::uint64_t total_;
    BOOL sending_;
}
- (void)setName:(const std::string&)name
           done:(std::uint64_t)done
          total:(std::uint64_t)total
        sending:(BOOL)sending;
@end

@implementation ILTransferLineView

- (void)setOverlay:(OSXTransferOverlay*)overlay
{
    overlay_ = overlay;
}

- (void)setDirection:(EDirection)direction
{
    direction_ = direction;
}

- (void)setGlowing:(BOOL)glowing
{
    if (glowing_ == glowing) {
        return;
    }
    glowing_ = glowing;
    [self setNeedsDisplay:YES];
}

- (NSDragOperation)draggingEntered:(id<NSDraggingInfo>)sender
{
    NSPasteboard* pasteboard = [sender draggingPasteboard];
    if ([pasteboard availableTypeFromArray:@[NSFilenamesPboardType]] == nil) {
        return NSDragOperationNone;
    }
    glowing_ = YES;
    [self setNeedsDisplay:YES];
    return NSDragOperationCopy;
}

- (NSDragOperation)draggingUpdated:(id<NSDraggingInfo>)sender
{
    return [self draggingEntered:sender];
}

- (void)draggingExited:(id<NSDraggingInfo>)sender
{
    (void)sender;
    glowing_ = NO;
    [self setNeedsDisplay:YES];
}

- (BOOL)prepareForDragOperation:(id<NSDraggingInfo>)sender
{
    (void)sender;
    return YES;
}

- (BOOL)performDragOperation:(id<NSDraggingInfo>)sender
{
    NSPasteboard* pasteboard = [sender draggingPasteboard];
    NSArray* files = [pasteboard propertyListForType:NSFilenamesPboardType];
    if (files == nil) {
        return NO;
    }

    std::vector<std::string> paths;
    for (NSString* file in files) {
        const std::string path = inputleap::utf8(file);
        if (!path.empty()) {
            paths.push_back(path);
        }
    }

    glowing_ = NO;
    [self setNeedsDisplay:YES];
    if (overlay_ != nullptr && !paths.empty()) {
        overlay_->drop_files(direction_, std::move(paths));
    }
    return YES;
}

- (void)drawRect:(NSRect)dirtyRect
{
    (void)dirtyRect;
    const NSRect bounds = [self bounds];
    const CGFloat line = glowing_ ? inputleap::kGlowWidth : inputleap::kLineWidth;

    NSRect lineRect = bounds;
    switch (direction_) {
    case inputleap::kLeft:
        lineRect = NSMakeRect(0, 0, line, bounds.size.height);
        break;
    case inputleap::kRight:
        lineRect = NSMakeRect(bounds.size.width - line, 0, line,
                              bounds.size.height);
        break;
    case inputleap::kTop:
        lineRect = NSMakeRect(0, bounds.size.height - line,
                              bounds.size.width, line);
        break;
    case inputleap::kBottom:
        lineRect = NSMakeRect(0, 0, bounds.size.width, line);
        break;
    default:
        return;
    }

    NSColor* colour = glowing_
        ? [NSColor colorWithCalibratedRed:0.545 green:0.863 blue:1.0 alpha:1.0]
        : [NSColor colorWithCalibratedRed:0.184 green:0.435 blue:0.929 alpha:1.0];
    [colour setFill];
    NSRectFill(lineRect);
}

@end

@implementation ILTransferProgressView

- (void)setName:(const std::string&)name
           done:(std::uint64_t)done
          total:(std::uint64_t)total
        sending:(BOOL)sending
{
    name_ = name;
    done_ = done;
    total_ = total;
    sending_ = sending;
    [self setNeedsDisplay:YES];
}

- (void)drawRect:(NSRect)dirtyRect
{
    (void)dirtyRect;
    const NSRect bounds = [self bounds];

    [[NSColor colorWithCalibratedRed:0.063 green:0.082 blue:0.110 alpha:1.0]
        setFill];
    NSRectFill(bounds);

    [[NSColor colorWithCalibratedRed:0.298 green:0.616 blue:1.0 alpha:1.0]
        setStroke];
    NSFrameRect(bounds);

    NSMutableParagraphStyle* paragraph =
        [[NSMutableParagraphStyle alloc] init];
    [paragraph setLineBreakMode:NSLineBreakByTruncatingTail];

    NSDictionary* titleAttributes = @{
        NSFontAttributeName: [NSFont systemFontOfSize:13],
        NSForegroundColorAttributeName:
            [NSColor colorWithCalibratedWhite:0.95 alpha:1.0],
        NSParagraphStyleAttributeName: paragraph
    };

    NSString* title = [NSString stringWithFormat:@"%@%s",
        sending_ ? @"Sending " : @"Receiving ", name_.c_str()];
    [title drawInRect:NSMakeRect(14, 50, bounds.size.width - 28, 20)
      withAttributes:titleAttributes];

    const int percent = total_ == 0
        ? 100
        : static_cast<int>((done_ * 100) / total_);
    NSString* status = [NSString stringWithFormat:@"%d%%  %s / %s",
        percent, inputleap::format_bytes(done_).c_str(), inputleap::format_bytes(total_).c_str()];
    [status drawInRect:NSMakeRect(14, 30, bounds.size.width - 28, 18)
       withAttributes:titleAttributes];

    const NSRect bar = NSMakeRect(14, 14, bounds.size.width - 28, 10);
    [[NSColor colorWithCalibratedRed:0.161 green:0.196 blue:0.255 alpha:1.0]
        setFill];
    NSRectFill(bar);

    NSRect filled = bar;
    if (total_ > 0) {
        filled.size.width = bar.size.width *
            std::min<std::uint64_t>(done_, total_) / total_;
    }
    [[NSColor colorWithCalibratedRed:0.298 green:0.616 blue:1.0 alpha:1.0]
        setFill];
    NSRectFill(filled);

    [paragraph release];
}

@end

namespace inputleap {

struct OSXTransferOverlay::Impl {
    IEventQueue* events = nullptr;
    const EventTarget* target = nullptr;

    NSWindow* lineWindows[4] = {nullptr, nullptr, nullptr, nullptr};
    ILTransferLineView* lineViews[4] = {nullptr, nullptr, nullptr, nullptr};
    NSWindow* progressWindow = nullptr;
    ILTransferProgressView* progressView = nullptr;

    std::vector<EDirection> peers;
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;

    bool buttonDown = false;
    bool dragging = false;
    int startX = 0;
    int startY = 0;
    int lastX = 0;
    int lastY = 0;

    ~Impl()
    {
        for (int i = 0; i < 4; ++i) {
            if (lineWindows[i] != nullptr) {
                [lineWindows[i] orderOut:nil];
                [lineWindows[i] release];
                lineWindows[i] = nullptr;
            }
        }
        if (progressWindow != nullptr) {
            [progressWindow orderOut:nil];
            [progressWindow release];
            progressWindow = nullptr;
        }
    }

    int index_of(EDirection direction) const
    {
        switch (direction) {
        case kLeft: return 0;
        case kRight: return 1;
        case kTop: return 2;
        case kBottom: return 3;
        default: return -1;
        }
    }

    void position_windows()
    {
        if (width <= 0 || height <= 0) {
            return;
        }

        NSArray* screens = [NSScreen screens];
        CGFloat maxY = 0;
        for (NSScreen* screen in screens) {
            const NSRect frame = [screen frame];
            maxY = std::max(maxY, frame.origin.y + frame.size.height);
        }

        const CGFloat appkitX = static_cast<CGFloat>(x);
        const CGFloat appkitBottom = maxY - y - height;

        const NSRect frames[4] = {
            NSMakeRect(appkitX, appkitBottom, inputleap::kDropZone, height),
            NSMakeRect(appkitX + width - inputleap::kDropZone, appkitBottom,
                       inputleap::kDropZone, height),
            NSMakeRect(appkitX, appkitBottom + height - inputleap::kDropZone,
                       width, inputleap::kDropZone),
            NSMakeRect(appkitX, appkitBottom, width, inputleap::kDropZone)
        };

        for (int i = 0; i < 4; ++i) {
            if (lineWindows[i] != nullptr) {
                [lineWindows[i] setFrame:frames[i] display:YES];
            }
        }

        if (progressWindow != nullptr) {
            [progressWindow setFrame:NSMakeRect(
                appkitX + (width - inputleap::kProgressWidth) / 2,
                appkitBottom + height - std::max(48, height / 8) -
                    inputleap::kProgressHeight,
                inputleap::kProgressWidth, inputleap::kProgressHeight)
                display:YES];
        }
    }

    void update_visible()
    {
        for (int i = 0; i < 4; ++i) {
            if (lineWindows[i] == nullptr) {
                continue;
            }
            EDirection direction = i == 0 ? kLeft : i == 1 ? kRight
                : i == 2 ? kTop : kBottom;
            const bool peer = std::find(peers.begin(), peers.end(),
                                        direction) != peers.end();
            if (dragging && peer) {
                [lineWindows[i] orderFrontRegardless];
            }
            else {
                [lineViews[i] setGlowing:NO];
                [lineWindows[i] orderOut:nil];
            }
        }
    }
};

OSXTransferOverlay::OSXTransferOverlay(IEventQueue* events,
                                       const EventTarget* target) :
    impl_(std::make_unique<Impl>())
{
    impl_->events = events;
    impl_->target = target;

    // Make sure AppKit is initialized before creating the overlay windows.
    (void)[NSApplication sharedApplication];

    for (int i = 0; i < 4; ++i) {
        EDirection direction = i == 0 ? kLeft : i == 1 ? kRight
            : i == 2 ? kTop : kBottom;

        NSWindow* window = [[NSWindow alloc]
            initWithContentRect:NSMakeRect(0, 0, 1, 1)
                      styleMask:inputleap::kOverlayStyle
                        backing:inputleap::kOverlayBacking
                          defer:NO];
        [window setOpaque:NO];
        [window setBackgroundColor:[NSColor clearColor]];
        [window setHasShadow:NO];
        [window setLevel:NSFloatingWindowLevel];
        [window setIgnoresMouseEvents:NO];
        [window setCollectionBehavior:(NSWindowCollectionBehavior)(
            NSWindowCollectionBehaviorCanJoinAllSpaces |
            NSWindowCollectionBehaviorFullScreenAuxiliary |
            NSWindowCollectionBehaviorStationary)];

        ILTransferLineView* view =
            [[ILTransferLineView alloc] initWithFrame:NSMakeRect(0, 0, 1, 1)];
        [view setAutoresizingMask:NSViewWidthSizable | NSViewHeightSizable];
        [view setOverlay:this];
        [view setDirection:direction];
        [view registerForDraggedTypes:@[NSFilenamesPboardType]];
        [window setContentView:view];
        [view release];
        [window orderOut:nil];

        impl_->lineWindows[i] = window;
        impl_->lineViews[i] = view;
    }

    NSWindow* progress = [[NSWindow alloc]
        initWithContentRect:NSMakeRect(0, 0, inputleap::kProgressWidth,
                                      inputleap::kProgressHeight)
                  styleMask:inputleap::kOverlayStyle
                    backing:inputleap::kOverlayBacking
                      defer:NO];
    [progress setOpaque:NO];
    [progress setBackgroundColor:[NSColor clearColor]];
    [progress setHasShadow:NO];
    [progress setLevel:NSFloatingWindowLevel];
    [progress setIgnoresMouseEvents:YES];
    [progress setCollectionBehavior:(NSWindowCollectionBehavior)(
        NSWindowCollectionBehaviorCanJoinAllSpaces |
        NSWindowCollectionBehaviorFullScreenAuxiliary |
        NSWindowCollectionBehaviorStationary)];

    ILTransferProgressView* progressView = [[ILTransferProgressView alloc]
        initWithFrame:NSMakeRect(0, 0, inputleap::kProgressWidth,
                                 inputleap::kProgressHeight)];
    [progressView setAutoresizingMask:NSViewWidthSizable | NSViewHeightSizable];
    [progress setContentView:progressView];
    [progressView release];
    [progress orderOut:nil];

    impl_->progressWindow = progress;
    impl_->progressView = progressView;
}

OSXTransferOverlay::~OSXTransferOverlay() = default;

void OSXTransferOverlay::set_geometry(int x, int y, int width, int height)
{
    impl_->x = x;
    impl_->y = y;
    impl_->width = width;
    impl_->height = height;
    impl_->position_windows();
}

void OSXTransferOverlay::set_peers(const std::vector<EDirection>& peers)
{
    impl_->peers = peers;
    impl_->update_visible();
}

void OSXTransferOverlay::button_down(int x, int y)
{
    impl_->buttonDown = true;
    impl_->dragging = false;
    impl_->startX = x;
    impl_->startY = y;
    impl_->lastX = x;
    impl_->lastY = y;
}

void OSXTransferOverlay::motion(int x, int y)
{
    impl_->lastX = x;
    impl_->lastY = y;

    if (!impl_->buttonDown) {
        return;
    }

    if (!impl_->dragging &&
        std::abs(x - impl_->startX) + std::abs(y - impl_->startY) >= 4) {
        impl_->dragging = true;
        impl_->update_visible();
    }
    else if (impl_->dragging) {
        for (EDirection direction : impl_->peers) {
            bool near = false;
            switch (direction) {
            case kLeft:
                near = x - impl_->x <= kDropZone;
                break;
            case kRight:
                near = (impl_->x + impl_->width - 1) - x <= kDropZone;
                break;
            case kTop:
                near = y - impl_->y <= kDropZone;
                break;
            case kBottom:
                near = (impl_->y + impl_->height - 1) - y <= kDropZone;
                break;
            default:
                break;
            }
            set_glow(direction, near);
        }
    }
}

void OSXTransferOverlay::button_up()
{
    impl_->buttonDown = false;
    impl_->dragging = false;
    impl_->update_visible();
}

void OSXTransferOverlay::hide_lines()
{
    impl_->buttonDown = false;
    impl_->dragging = false;
    impl_->update_visible();
}

void OSXTransferOverlay::set_glow(EDirection direction, bool glowing)
{
    const int index = impl_->index_of(direction);
    if (index < 0 || impl_->lineViews[index] == nil) {
        return;
    }
    [impl_->lineViews[index] setGlowing:glowing ? YES : NO];
}

void OSXTransferOverlay::drop_files(EDirection direction,
                                    std::vector<std::string> files)
{
    TransferDrop drop;
    drop.edge = direction;
    drop.files = std::move(files);
    impl_->events->add_event(EventType::TRANSFER_FILES_DROPPED, impl_->target,
                             create_event_data<TransferDrop>(drop));
    impl_->dragging = false;
    impl_->buttonDown = false;
    impl_->update_visible();
}

void OSXTransferOverlay::show_progress(const std::string& name,
                                       std::uint64_t done,
                                       std::uint64_t total,
                                       bool sending)
{
    [impl_->progressView setName:name done:done total:total
                       sending:sending ? YES : NO];
    [impl_->progressWindow orderFrontRegardless];
}

void OSXTransferOverlay::finish_progress(const std::string& message,
                                         bool success)
{
    LOG_DEBUG("transfer overlay finished: success=%d message=%s",
              success, message.c_str());
    [impl_->progressWindow orderOut:nil];
}

} // namespace inputleap
