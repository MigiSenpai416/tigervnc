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

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <stdexcept>

#include <core/Configuration.h>
#include <core/LogWriter.h>
#include <core/i18n.h>
#include <network/TcpSocket.h>
#include <rfb/VNCServerST.h>

#include "ControlSocket.h"
#include "parameters.h"

static core::LogWriter vlog("ControlSocket");

ControlSocket::ControlSocket(const std::string& path_,
                             rfb::VNCServerST* server_)
  : path(path_), server(server_), fd(-1)
{
  struct sockaddr_un addr;

  if (path.size() >= sizeof(addr.sun_path))
    throw std::runtime_error(_("Control socket path too long"));

  fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
  if (fd < 0)
    throw std::runtime_error(strerror(errno));

  memset(&addr, 0, sizeof(addr));
  addr.sun_family = AF_UNIX;
  strcpy(addr.sun_path, path.c_str());

  unlink(path.c_str());
  mode_t old = umask(0077);
  int r = bind(fd, (struct sockaddr*)&addr, sizeof(addr));
  umask(old);
  if (r < 0 || listen(fd, 5) < 0) {
    int e = errno;
    close(fd);
    fd = -1;
    throw std::runtime_error(strerror(e));
  }
}

ControlSocket::~ControlSocket()
{
  if (fd >= 0) {
    close(fd);
    unlink(path.c_str());
  }
}

static bool writeAll(int s, const std::string& data)
{
  size_t done = 0;
  while (done < data.size()) {
    ssize_t n = write(s, data.data() + done, data.size() - done);
    if (n < 0 && errno == EINTR)
      continue;
    if (n <= 0)
      return false;
    done += n;
  }
  return true;
}

void ControlSocket::accept()
{
  int s = ::accept4(fd, nullptr, nullptr, SOCK_CLOEXEC);
  if (s < 0)
    return;

  // The socket is private (0600), but check the peer anyway
  struct ucred cred;
  socklen_t len = sizeof(cred);
  if (getsockopt(s, SOL_SOCKET, SO_PEERCRED, &cred, &len) < 0 ||
      cred.uid != getuid()) {
    vlog.error(_("Refusing control connection from uid %d"),
               (int)cred.uid);
    close(s);
    return;
  }

  // One short request line; local clients send it at once
  std::string req;
  char buf[512];
  while (req.find('\n') == std::string::npos && req.size() < 8192) {
    struct pollfd p = { s, POLLIN, 0 };
    if (poll(&p, 1, 2000) <= 0)
      break;
    ssize_t n = read(s, buf, sizeof(buf));
    if (n <= 0)
      break;
    req.append(buf, n);
  }
  size_t nl = req.find('\n');
  if (nl != std::string::npos)
    req.resize(nl);

  std::string reply;
  try {
    reply = handle(req);
  } catch (std::exception& e) {
    reply = std::string("ERR ") + e.what() + "\n";
  }
  writeAll(s, reply);
  close(s);
}

std::string ControlSocket::handle(const std::string& request)
{
  std::string cmd = request, arg;
  size_t sp = request.find(' ');
  if (sp != std::string::npos) {
    cmd = request.substr(0, sp);
    arg = request.substr(sp + 1);
  }

  if (cmd == "list") {
    std::string out = "OK\n";
    for (core::Parameter* p : *core::Configuration::global())
      out += std::string(p->getName()) + "\n";
    return out;
  }

  if (cmd == "get" || cmd == "desc") {
    core::Parameter* p = core::Configuration::getParam(arg.c_str());
    // Like the VNC extension: never hand out the password
    if (!p || (cmd == "get" && strcasecmp(arg.c_str(), "Password") == 0))
      return "ERR " + std::string(_("No such parameter")) + "\n";
    if (cmd == "get")
      return "OK\n" + p->getValueStr() + "\n";
    return "OK\n" + std::string(p->getDescription()) + "\n";
  }

  if (cmd == "set") {
    size_t eq = arg.find('=');
    if (eq == std::string::npos)
      return "ERR " + std::string(_("Invalid parameter syntax")) + "\n";
    std::string name = arg.substr(0, eq), value = arg.substr(eq + 1);
    bool allowed = false;
    for (const char* a : allowOverride)
      if (strcasecmp(a, name.c_str()) == 0)
        allowed = true;
    if (!allowed)
      return "ERR " + std::string(_("Parameter not in AllowOverride")) +
             "\n";
    if (!core::Configuration::setParam(name.c_str(), value.c_str()))
      return "ERR " + std::string(_("Invalid value")) + "\n";
    vlog.info(_("Parameter %s changed by vncconfig"), name.c_str());
    if (strcasecmp(name.c_str(), "desktop") == 0)
      server->setName(desktopName);
    return "OK\n";
  }

  if (cmd == "disconnect") {
    server->closeClients(_("User requested disconnection"));
    return "OK\n";
  }

  if (cmd == "connect") {
    bool viewOnly = false;
    if (arg.compare(0, 11, "-view-only ") == 0) {
      viewOnly = true;
      arg = arg.substr(11);
    }
    if (arg.empty())
      return "ERR " + std::string(_("No host given")) + "\n";
    std::string host;
    int port;
    network::getHostAndPort(arg.c_str(), &host, &port, 5500);
    network::Socket* sock = new network::TcpSocket(host.c_str(), port);
    vlog.info(_("Reverse connection: %s:%d%s"), host.c_str(), port,
              viewOnly ? _(" (view only)") : "");
    if (!server->addSocket(sock, true, viewOnly ? rfb::AccessView :
                                                  rfb::AccessDefault)) {
      delete sock;
      return "ERR " + std::string(_("Failed to add reverse connection "
                                    "socket")) + "\n";
    }
    return "OK\n";
  }

  return "ERR " + std::string(_("Unknown request")) + "\n";
}
