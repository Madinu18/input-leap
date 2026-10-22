/*
 * InputLeap -- mouse and keyboard sharing utility
 * Copyright (C) 2012-2016 Symless Ltd.
 * Copyright (C) 2012 Nick Bolton
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

#include "io/filesystem.h"

namespace inputleap {

class ArgsBase {
public:
    ArgsBase();
    virtual ~ArgsBase();

public:
    bool m_daemon;
    bool m_backend;
    bool m_restartable;
    bool m_noHooks;
    std::string m_exename;
    const char* m_logFilter;
    const char* m_logFile;
    const char* m_display;
    std::string m_name;
    bool m_disableTray;
    bool m_enableIpc;
    bool m_enableDragDrop;
    std::string m_dropTarget;
#if SYSAPI_WIN32
    bool m_debugServiceWait;
    bool m_pauseOnExit;
    bool m_stopOnDeskSwitch;
#endif
    bool m_shouldExit;
    std::string network_address;
    bool m_enableCrypto;
    // Maximum rate (Hz) at which mouse motion is forwarded to clients.
    // 0 disables rate limiting. The default keeps WiFi airtime low while
    // adding at most 4 ms of latency during continuous motion.
    double m_motionRate;
    // Use an encrypted UDP fast path for mouse motion when both sides
    // support it. TCP remains the control and fallback channel.
    bool m_enableUdp;
    int m_udpPort;
    // Legacy behaviour: treat the Scroll Lock toggle as "lock cursor to
    // screen". Off by default because a stray Scroll Lock state otherwise
    // prevents screen switching.
    bool m_scrollLockLocksCursor;
    inputleap::fs::path m_profileDirectory;
    inputleap::fs::path m_pluginDirectory;
    bool use_x11 = false;
    bool use_ei = false;
    bool use_portal = true; // use the XDG portals for ei
};

} // namespace inputleap
