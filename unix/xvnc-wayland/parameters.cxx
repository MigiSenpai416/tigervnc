/* Copyright 2026 The TigerVNC Wayland-mode fork contributors
 *
 * This is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This software is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this software; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307,
 * USA.
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <pwd.h>
#include <stdio.h>
#include <unistd.h>

#include <vector>

#include <core/i18n.h>

#include "parameters.h"

static const char* defaultDesktopName();

// These mirror unix/xserver/hw/vnc/vncExtInit.cc and XserverDesktop.cc
// so that "Xvnc --wayland" accepts the same VNC options as "Xvnc".

core::IntParameter
  rfbport("rfbport",
          _("TCP port to listen for RFB protocol"), 0, -1, 65535);
core::StringParameter
  rfbunixpath("rfbunixpath",
              _("UNIX socket path to listen for RFB protocol"), "");
core::IntParameter
  rfbunixmode("rfbunixmode",
              _("UNIX socket access mode"), 0600, 0000, 0777);
core::StringParameter
  desktopName("desktop",
              _("Name of VNC desktop"), defaultDesktopName());
core::BoolParameter
  localhostOnly("localhost",
                _("Only allow connections from localhost"), false);
core::StringParameter
  interface("interface",
            _("Listen on the specified network address"), "all");
core::BoolParameter
  avoidShiftNumLock("AvoidShiftNumLock",
                    _("Avoid fake Shift presses for keys affected by "
                      "NumLock"), true);
core::StringListParameter
  allowOverride("AllowOverride",
                _("Comma separated list of parameters that are allowed "
                  "to be modified after startup"),
                {"desktop", "AcceptPointerEvents", "SendCutText",
                 "AcceptCutText", "SendPrimary", "SetPrimary"});
core::BoolParameter
  setPrimary("SetPrimary",
             _("Set the primary selection as well as the clipboard "
               "selection"),
             true);
core::BoolParameter
  sendPrimary("SendPrimary",
              _("Send the primary selection to the client as well as "
                "the clipboard selection"),
              true);
core::BoolParameter
  rawKeyboard("RawKeyboard",
              _("Send keyboard events straight through and avoid "
                "mapping them to the current keyboard layout"),
              false);

core::StringParameter
  waylandDisplay("WaylandDisplay",
                 _("Name of the Wayland socket to create in "
                   "XDG_RUNTIME_DIR (default: wayland-<display number>)"),
                 "");
core::BoolParameter
  xwaylandEnabled("Xwayland",
                  _("Start Xwayland so that X11 applications can run "
                    "on the Wayland display"),
                  true);
core::EnumParameter
  nestedCompositor("NestedCompositor",
                   _("How to treat a session that is itself a Wayland "
                     "compositor (sway, labwc, KDE, ...): Auto detects "
                     "it, On treats every top level window as one, Off "
                     "always manages windows as normal applications"),
                   {"Auto", "On", "Off"}, "Auto");

static const char* defaultDesktopName()
{
  long host_max = sysconf(_SC_HOST_NAME_MAX);
  if (host_max < 0)
    return "";

  std::vector<char> hostname(host_max + 1);
  if (gethostname(hostname.data(), hostname.size()) == -1)
    return "";

  struct passwd* pwent = getpwuid(getuid());
  if (pwent == nullptr)
    return "";

  int len = snprintf(nullptr, 0, "%s@%s", pwent->pw_name, hostname.data());
  if (len < 0)
    return "";

  char* name = new char[len + 1];

  snprintf(name, len + 1, "%s@%s", pwent->pw_name, hostname.data());

  return name;
}
