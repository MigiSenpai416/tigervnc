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
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301,
 * USA.
 */

// The Wayland mode's replacement for the VNC X extension: vncconfig
// talks to the server through a UNIX socket next to the Wayland
// socket ($XDG_RUNTIME_DIR/<wayland socket>.vncconfig). Only the
// user running the server can connect.
//
// Protocol: the client sends one request line, the server answers
// with "OK" or "ERR <message>", then any result lines, and closes.
//
//   list                    parameter names
//   get <name>              value (not for Password)
//   desc <name>             description
//   set <name>=<value>      only for parameters in AllowOverride
//   connect [-view-only] <host[:port]>   reverse connection
//   disconnect              disconnect all viewers

#ifndef __CONTROLSOCKET_H__
#define __CONTROLSOCKET_H__

#include <string>

namespace rfb { class VNCServerST; }

class ControlSocket {
public:
  ControlSocket(const std::string& path, rfb::VNCServerST* server);
  ~ControlSocket();

  int getFd() const { return fd; }
  // Call when the fd is readable
  void accept();

  const std::string& getPath() const { return path; }

private:
  std::string handle(const std::string& request);

  std::string path;
  rfb::VNCServerST* server;
  int fd;
};

#endif
