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
//   watch                   QueryConnect: keep the connection open (the
//                           vncconfig prompt). The server sends
//                             query\t<id>\t<timeout>\t<address>\t<user>
//                           per connection to accept; the client answers
//                             approve <id> 1|0
//                           on the same connection. Like
//                           XVncExtSelectInput(VncExtQueryConnectMask).

#ifndef __CONTROLSOCKET_H__
#define __CONTROLSOCKET_H__

#include <functional>
#include <map>
#include <string>
#include <vector>

struct pollfd;

namespace rfb { class VNCServerST; }

class ControlSocket {
public:
  ControlSocket(const std::string& path, rfb::VNCServerST* server);
  ~ControlSocket();

  int getFd() const { return fd; }
  // Call when the fd is readable
  void accept();

  const std::string& getPath() const { return path; }

  // QueryConnect prompts ("watch" connections)
  void addWatcherFds(std::vector<struct pollfd>* fds) const;
  void processWatchers(const struct pollfd* fds, size_t count);
  // Sends a query to every prompt; returns how many got it
  int notifyQueryConnect(uint32_t id, int timeout, const char* address,
                         const char* user);
  std::function<void(uint32_t id, bool accept)> onApprove;

private:
  std::string handle(const std::string& request);
  void readWatcher(int s);
  void dropWatcher(int s);

  std::string path;
  rfb::VNCServerST* server;
  int fd;
  std::map<int, std::string> watchers;   // fd -> partial input line
};

#endif
