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

#ifndef __XVNC_WAYLAND_PARAMETERS_H__
#define __XVNC_WAYLAND_PARAMETERS_H__

#include <core/Configuration.h>

// Same names, defaults and meaning as Xvnc (unix/xserver/hw/vnc)
extern core::IntParameter rfbport;
extern core::StringParameter rfbunixpath;
extern core::IntParameter rfbunixmode;
extern core::StringParameter desktopName;
extern core::BoolParameter localhostOnly;
extern core::StringParameter interface;
extern core::BoolParameter avoidShiftNumLock;
extern core::StringListParameter allowOverride;
extern core::BoolParameter setPrimary;
extern core::BoolParameter sendPrimary;
extern core::BoolParameter rawKeyboard;

// Wayland mode only
extern core::StringParameter waylandDisplay;
extern core::BoolParameter xwaylandEnabled;

#endif
