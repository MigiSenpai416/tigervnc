/* Copyright 2026 The TigerVNC Wayland-mode fork contributors
 * Copyright (C) 2002-2005 RealVNC Ltd.  All Rights Reserved.
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

/*
 * Xvnc-wayland: the Wayland mode of Xvnc ("Xvnc --wayland").
 *
 * A persistent virtual Wayland display (a small wlroots compositor)
 * with TigerVNC's VNC server built in, mirroring what Xvnc does for
 * X11. It is normally started through "Xvnc --wayland", which execs
 * this program with the remaining arguments.
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <errno.h>
#include <fcntl.h>
#include <locale.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <list>
#include <stdexcept>
#include <string>
#include <vector>

#include <core/Configuration.h>
#include <core/Exception.h>
#include <core/Logger_stdio.h>
#include <core/Logger_syslog.h>
#include <core/LogWriter.h>
#include <core/Timer.h>
#include <core/i18n.h>

#include <rdr/FdOutStream.h>

#include <rfb/VNCServerST.h>

#include <network/TcpSocket.h>
#include <network/UnixSocket.h>

#include "WaylandServerDesktop.h"
#include "parameters.h"

static core::LogWriter vlog("Main");

static const char* programName;

static int signalPipe[2] = { -1, -1 };
static volatile sig_atomic_t caughtTerm = 0;
static volatile sig_atomic_t caughtChild = 0;

static void termHandler(int /*sig*/)
{
  int saved = errno;
  caughtTerm = 1;
  if (signalPipe[1] != -1)
    (void)!write(signalPipe[1], "t", 1);
  errno = saved;
}

static void childHandler(int /*sig*/)
{
  int saved = errno;
  caughtChild = 1;
  if (signalPipe[1] != -1)
    (void)!write(signalPipe[1], "c", 1);
  errno = saved;
}

static void printVersion(FILE* fp)
{
  fprintf(fp, _("TigerVNC server version %s, built %s\n"),
          PACKAGE_VERSION, BUILD_TIMESTAMP);
  fprintf(fp, _("Wayland mode (wlroots %s)\n"), WLROOTS_VERSION_STR);
}

static void usage()
{
  printVersion(stderr);
  fprintf(stderr,
          _("\n"
            "Usage: %s --wayland [:<display>] [<options>] "
            "[-- <session command> [<args>]]\n"
            "       %s --wayland --version\n"),
          programName, programName);
  fprintf(stderr,
          _("\n"
            ":<display>             display number; selects TCP port "
            "5900+<display> and\n"
            "                       Wayland socket wayland-<display>\n"
            "-geometry WxH          set initial framebuffer size "
            "(default 1024x768)\n"
            "-depth D               set framebuffer depth (only 24 is "
            "supported)\n"
            "-pixelformat fmt       set pixel format (only rgb888 is "
            "supported)\n"
            "-inetd                 has been launched from inetd\n"
            "-- cmd [args]          run cmd as the desktop session once "
            "the display is\n"
            "                       ready; the server exits when it "
            "exits\n"));
  fprintf(stderr,
          _("\n"
            "Parameters can be turned on with -<param> or off with -<param>=0\n"
            "Parameters which take a value can be specified as "
            "-<param> <value>\n"
            "Other valid forms are <param>=<value> -<param>=<value> "
            "--<param>=<value>\n"
            "Parameter names are case-insensitive.  The parameters are:\n\n"));
  core::Configuration::listParams(79, 14);
  exit(1);
}

static bool ensureRuntimeDir()
{
  const char* dir = getenv("XDG_RUNTIME_DIR");
  struct stat st;

  if (dir && *dir)
    return true;

  // Same fallback as several toolkits use; must be private to us
  char path[64];
  snprintf(path, sizeof(path), "/tmp/runtime-%u", (unsigned)getuid());
  if (mkdir(path, 0700) != 0 && errno != EEXIST) {
    vlog.error(_("XDG_RUNTIME_DIR is not set and %s cannot be created: %s"),
               path, strerror(errno));
    return false;
  }
  if (lstat(path, &st) != 0 || !S_ISDIR(st.st_mode) ||
      st.st_uid != getuid() || (st.st_mode & 077) != 0) {
    vlog.error(_("XDG_RUNTIME_DIR is not set and %s is not a private "
                 "directory owned by us"), path);
    return false;
  }
  vlog.info(_("XDG_RUNTIME_DIR not set, using %s"), path);
  setenv("XDG_RUNTIME_DIR", path, 1);
  return true;
}

static pid_t startSession(const std::vector<char*>& cmd,
                          const char* waylandSocket,
                          const char* xDisplay)
{
  pid_t pid = fork();
  if (pid < 0) {
    vlog.error(_("Failed to start session: %s"), strerror(errno));
    return -1;
  }
  if (pid > 0)
    return pid;

  // Child
  sigset_t set;
  sigemptyset(&set);
  sigprocmask(SIG_SETMASK, &set, nullptr);
  signal(SIGTERM, SIG_DFL);
  signal(SIGINT, SIG_DFL);
  signal(SIGHUP, SIG_DFL);
  signal(SIGCHLD, SIG_DFL);
  signal(SIGPIPE, SIG_DFL);

  setenv("WAYLAND_DISPLAY", waylandSocket, 1);
  unsetenv("WAYLAND_SOCKET");
  if (xDisplay && *xDisplay)
    setenv("DISPLAY", xDisplay, 1);
  else
    unsetenv("DISPLAY");
  setenv("XDG_SESSION_TYPE", "wayland", 1);

  std::vector<char*> argv(cmd);
  argv.push_back(nullptr);
  execvp(argv[0], argv.data());
  fprintf(stderr, "%s: failed to execute %s: %s\n", programName,
          argv[0], strerror(errno));
  _exit(127);
}

int main(int argc, char** argv)
{
  int displayNum = 0;
  bool explicitDisplay = false;
  int width = 1024, height = 768;
  int inetdSock = -1;
  std::vector<char*> sessionCmd;

  setlocale(LC_ALL, "");

  core::initStdIOLoggers();
  core::initSyslogLogger();
  core::LogWriter::setLogParams("*:stderr:30");

  programName = argv[0];

  for (int i = 1; i < argc;) {
    int ret;

    if (strcmp(argv[i], "--") == 0) {
      for (i++; i < argc; i++)
        sessionCmd.push_back(argv[i]);
      if (sessionCmd.empty()) {
        fprintf(stderr, _("%s: No session command after '--'\n"),
                programName);
        exit(1);
      }
      break;
    }

    if (argv[i][0] == ':') {
      char* end;
      long n = strtol(argv[i] + 1, &end, 10);
      if (argv[i][1] == '\0' || *end != '\0' || n < 0 || n > 65535 - 5900) {
        fprintf(stderr, _("%s: Invalid display '%s'\n"), programName,
                argv[i]);
        exit(1);
      }
      displayNum = n;
      explicitDisplay = true;
      i++;
      continue;
    }

    if (strcmp(argv[i], "-geometry") == 0) {
      if (i + 1 >= argc ||
          sscanf(argv[i + 1], "%dx%d", &width, &height) != 2 ||
          width < 1 || height < 1 || width > 16384 || height > 16384) {
        fprintf(stderr, _("%s: Invalid geometry '%s'\n"), programName,
                i + 1 < argc ? argv[i + 1] : "");
        exit(1);
      }
      i += 2;
      continue;
    }

    if (strcmp(argv[i], "-depth") == 0) {
      if (i + 1 >= argc) {
        fprintf(stderr, _("Required argument to %s not specified\n"),
                argv[i]);
        exit(1);
      }
      if (atoi(argv[i + 1]) != 24) {
        fprintf(stderr, _("%s: Depth %s is not supported in Wayland "
                          "mode, only 24\n"), programName, argv[i + 1]);
        exit(1);
      }
      i += 2;
      continue;
    }

    if (strcmp(argv[i], "-pixelformat") == 0) {
      if (i + 1 >= argc) {
        fprintf(stderr, _("Required argument to %s not specified\n"),
                argv[i]);
        exit(1);
      }
      if (strcasecmp(argv[i + 1], "rgb888") != 0) {
        fprintf(stderr, _("%s: Pixel format %s is not supported in "
                          "Wayland mode, only rgb888\n"),
                programName, argv[i + 1]);
        exit(1);
      }
      i += 2;
      continue;
    }

    if (strcmp(argv[i], "-inetd") == 0) {
      if ((inetdSock = dup(0)) == -1) {
        fprintf(stderr, _("Failed to allocate a new file descriptor for "
                          "-inetd: %s\n"), strerror(errno));
        exit(1);
      }
      int nullfd = open("/dev/null", O_RDWR);
      dup2(nullfd, 0);
      dup2(nullfd, 1);
      close(nullfd);
      i++;
      continue;
    }

    if (strcmp(argv[i], "-noclipboard") == 0) {
      // Only affects vncconfig, which is X11 only
      i++;
      continue;
    }

    if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "-help") == 0 ||
        strcmp(argv[i], "--help") == 0)
      usage();

    if (strcmp(argv[i], "-version") == 0 ||
        strcmp(argv[i], "--version") == 0 ||
        strcmp(argv[i], "-showconfig") == 0) {
      printVersion(stdout);
      return 0;
    }

    ret = core::Configuration::handleParamArg(argc, argv, i);
    if (ret > 0) {
      i += ret;
      continue;
    }

    if (argv[i][0] == '-') {
      fprintf(stderr, _("%s: Unrecognized option '%s'\n"), programName,
              argv[i]);
      fprintf(stderr, _("See '%s --wayland --help' for more "
                        "information.\n"), programName);
      exit(1);
    }

    fprintf(stderr, _("%s: Extra argument '%s'\n"), programName, argv[i]);
    fprintf(stderr, _("See '%s --wayland --help' for more information.\n"),
            programName);
    exit(1);
  }

  allowOverride.setImmutable();

  if (!ensureRuntimeDir())
    return 1;

  std::string socketName = (const char*)waylandDisplay;
  if (socketName.empty())
    socketName = "wayland-" + std::to_string(displayNum);
  if (socketName.find('/') != std::string::npos) {
    vlog.error(_("WaylandDisplay must be a plain socket name"));
    return 1;
  }

  if (pipe2(signalPipe, O_CLOEXEC | O_NONBLOCK) != 0) {
    vlog.error("pipe: %s", strerror(errno));
    return 1;
  }

  struct sigaction sa;
  memset(&sa, 0, sizeof(sa));
  sa.sa_handler = termHandler;
  sigaction(SIGHUP, &sa, nullptr);
  sigaction(SIGINT, &sa, nullptr);
  sigaction(SIGTERM, &sa, nullptr);
  sa.sa_handler = childHandler;
  sa.sa_flags = SA_NOCLDSTOP;
  sigaction(SIGCHLD, &sa, nullptr);
  signal(SIGPIPE, SIG_IGN);

  std::list<network::SocketListener*> listeners;
  int ret = 0;

  try {
    WaylandServerDesktop desktop(width, height, socketName.c_str(),
                                 xwaylandEnabled);
    if (!desktop.valid()) {
      vlog.error(_("Unable to create the Wayland display"));
      return 1;
    }

    vlog.info(_("Wayland display %s created in %s (%dx%d)"),
              desktop.socketName(), getenv("XDG_RUNTIME_DIR"),
              width, height);

    rfb::VNCServerST server(desktopName, &desktop);

    bool inetd = false;
    if (inetdSock != -1) {
      inetd = true;
      if (network::isSocketListening(inetdSock)) {
        listeners.push_back(new network::TcpListener(inetdSock));
        vlog.info(_("Listening for VNC connections on inetd socket"));
      } else {
        network::Socket* sock = new network::TcpSocket(inetdSock);
        if (!server.addSocket(sock)) {
          delete sock;
          throw std::runtime_error(_("Failed to listen for VNC "
                                     "connections on inetd socket"));
        }
      }
    }

    if (!inetd && ((const char*)rfbunixpath)[0] != '\0') {
      listeners.push_back(new network::UnixListener(rfbunixpath,
                                                    rfbunixmode));
      vlog.info(_("Listening for VNC connections on %s (mode %04o)"),
                (const char*)rfbunixpath, (int)rfbunixmode);
    }

    if (!inetd && rfbport != -1) {
      std::list<network::SocketListener*> tcp_listeners;
      const char* addr = interface;
      int port = rfbport;
      if (port == 0)
        port = 5900 + displayNum;
      if (strcasecmp(addr, "all") == 0)
        addr = nullptr;
      if (localhostOnly)
        network::createLocalTcpListeners(&tcp_listeners, port);
      else
        network::createTcpListeners(&tcp_listeners, addr, port);

      if (!tcp_listeners.empty()) {
        listeners.splice(listeners.end(), tcp_listeners);
        if (localhostOnly)
          vlog.info(_("Listening for VNC connections on local "
                      "interfaces, port %d"), port);
        else
          vlog.info(_("Listening for VNC connections on interface %s, "
                      "port %d"), (const char*)interface, port);
      }
    }

    if (!inetd && listeners.empty())
      throw std::runtime_error(_("No path or port configured for "
                                 "incoming connections"));

    (void)explicitDisplay;

    // Children (session, and anything started from it) talk to us
    setenv("WAYLAND_DISPLAY", desktop.socketName(), 1);

    bool sessionPending = !sessionCmd.empty();
    pid_t sessionPid = -1;
    struct timespec startTime;
    clock_gettime(CLOCK_MONOTONIC, &startTime);

    while (!caughtTerm) {
      std::vector<struct pollfd> fds;
      std::list<network::Socket*> sockets;
      int timeout;

      // Start the session once Xwayland is up (or has had its chance)
      if (sessionPending) {
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        bool waited = now.tv_sec - startTime.tv_sec >= 15;
        if (!xwaylandEnabled || desktop.xwaylandReady() || waited) {
          if (xwaylandEnabled && !desktop.xwaylandReady())
            vlog.error(_("Xwayland did not start in time, starting the "
                         "session without DISPLAY"));
          sessionPid = startSession(sessionCmd, desktop.socketName(),
                                    desktop.xwaylandReady() ?
                                    desktop.xwaylandDisplay() : nullptr);
          if (sessionPid < 0) {
            ret = 1;
            break;
          }
          vlog.info(_("Started session '%s' (pid %d)"), sessionCmd[0],
                    (int)sessionPid);
          sessionPending = false;
        }
      }

      if (caughtChild) {
        int status;
        pid_t pid;
        caughtChild = 0;
        while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
          if (pid == sessionPid) {
            if (WIFEXITED(status))
              vlog.info(_("Session exited with status %d"),
                        WEXITSTATUS(status));
            else
              vlog.info(_("Session terminated by signal %d"),
                        WTERMSIG(status));
            sessionPid = -1;
            caughtTerm = 1;
          }
        }
        if (caughtTerm)
          break;
      }

      fds.push_back({ desktop.eventFd(), POLLIN, 0 });
      fds.push_back({ signalPipe[0], POLLIN, 0 });
      for (network::SocketListener* listener : listeners)
        fds.push_back({ listener->getFd(), POLLIN, 0 });

      server.getSockets(&sockets);
      for (auto i = sockets.begin(); i != sockets.end(); ) {
        network::Socket* sock = *i;
        if (sock->isShutdownRead()) {
          int fd = sock->getFd();
          server.removeSocket(sock);
          delete sock;
          i = sockets.erase(i);
          if (fd == inetdSock) {
            vlog.info(_("inetd client gone. Terminating..."));
            caughtTerm = 1;
          }
          continue;
        }
        short events = POLLIN;
        if (sock->outStream().hasBufferedData())
          events |= POLLOUT;
        fds.push_back({ sock->getFd(), events, 0 });
        ++i;
      }
      if (caughtTerm)
        break;

      timeout = core::Timer::checkTimeouts();
      if (sessionPending && (timeout < 0 || timeout > 100))
        timeout = 100;

      desktop.flush();

      int n = poll(fds.data(), fds.size(), timeout);
      if (n < 0) {
        if (errno == EINTR)
          continue;
        throw core::socket_error("poll", errno);
      }

      if (fds[1].revents & POLLIN) {
        char buf[64];
        while (read(signalPipe[0], buf, sizeof(buf)) > 0)
          ;
      }

      // Wayland clients, Xwayland, frame timers, clipboard pipes
      desktop.dispatch();

      size_t idx = 2;
      for (network::SocketListener* listener : listeners) {
        if (fds[idx++].revents & POLLIN) {
          network::Socket* sock = listener->accept();
          if (sock) {
            if (!server.addSocket(sock))
              delete sock;
          } else {
            vlog.info(_("Client connection rejected"));
          }
        }
      }

      core::Timer::checkTimeouts();

      for (network::Socket* sock : sockets) {
        short revents = 0;
        for (size_t j = idx; j < fds.size(); j++) {
          if (fds[j].fd == sock->getFd()) {
            revents = fds[j].revents;
            break;
          }
        }
        if (revents & (POLLIN | POLLHUP | POLLERR))
          server.processSocketReadEvent(sock);
        if (revents & POLLOUT)
          server.processSocketWriteEvent(sock);
      }

      desktop.flush();
    }

    if (caughtTerm)
      vlog.info(_("Terminating"));

    server.closeClients(_("The server is shutting down"));

    if (sessionPid > 0)
      kill(sessionPid, SIGHUP);
  } catch (std::exception& e) {
    vlog.error("%s", e.what());
    ret = 1;
  }

  // Run listener destructors; remove UNIX sockets etc
  for (network::SocketListener* listener : listeners)
    delete listener;

  vlog.info(_("Terminated"));
  return ret;
}
