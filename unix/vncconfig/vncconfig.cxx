/* Copyright (C) 2002-2005 RealVNC Ltd.  All Rights Reserved.
 * Copyright 2012-2016 Pierre Ossman for Cendio AB
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
//
// VNC server configuration utility
//

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>
#include <errno.h>

#include <signal.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <string>
#include <vector>
#include <X11/X.h>
#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include "vncExt.h"

#include <core/Configuration.h>
#include <core/Exception.h>
#include <core/Logger_stdio.h>
#include <core/LogWriter.h>
#include <core/string.h>

/* vncconfig in 1.16 is not translated */
#ifndef _
#define _(String) (String)
#endif

#include "TXWindow.h"
#include "TXCheckbox.h"
#include "TXLabel.h"
#include "QueryConnectDialog.h"

static core::LogWriter vlog("vncconfig");

core::StringParameter displayname("display", "The X display", "");
core::BoolParameter noWindow("nowin", "Don't display a window", 0);
core::BoolParameter iconic("iconic", "Start with window iconified", 0);

#define ACCEPT_CUT_TEXT "AcceptCutText"
#define SEND_CUT_TEXT "SendCutText"

#define SET_PRIMARY "SetPrimary"
#define SEND_PRIMARY "SendPrimary"

char* programName = nullptr;
Display* dpy;
int vncExtEventBase, vncExtErrorBase;

// Wayland mode: the window talks to the server over the control socket
// (see waylandControlPath()) instead of the VNC X extension
static std::string waylandCtl;
static int waylandWatchFd = -1;
static bool waylandRequest(const std::string& path, const std::string& req,
                           std::string* result);

static bool isTrue(const char* data) {
  // Bool parameters read back as "on"/"off" (BoolParameter::getValueStr)
  return strcmp(data, "1") == 0 || strcasecmp(data, "on") == 0 ||
         strcasecmp(data, "true") == 0 || strcasecmp(data, "yes") == 0;
}

static bool getBoolParam(Display* dpy_, const char* param) {
  if (!waylandCtl.empty()) {
    std::string v;
    if (!waylandRequest(waylandCtl, std::string("get ") + param, &v))
      return false;
    while (!v.empty() && (v.back() == '\n' || v.back() == ' '))
      v.pop_back();
    return isTrue(v.c_str());
  }
  char* data;
  int len;
  bool ret = false;
  if (XVncExtGetParam(dpy_, param, &data, &len)) {
    ret = isTrue(data);
    XFree(data);
  }
  return ret;
}

static void setParam(Display* dpy_, const char* param, const char* value) {
  if (!waylandCtl.empty()) {
    if (!waylandRequest(waylandCtl, std::string("set ") + param + "=" + value,
                        nullptr))
      vlog.error(_("Setting the parameter %s failed"), param);
    return;
  }
  XVncExtSetParam(dpy_, param, value);
}

class VncConfigWindow : public TXWindow, public TXEventHandler,
                        public TXDeleteWindowCallback,
                        public TXCheckboxCallback,
                        public QueryResultCallback {
public:
  VncConfigWindow(Display* dpy_)
    : TXWindow(dpy_, 300, 100),
      acceptClipboard(dpy_, "Accept clipboard from viewers", this, false, this),
      setPrimaryCB(dpy_, "Also set primary selection", this, false, this),
      sendClipboard(dpy_, "Send clipboard to viewers", this, false, this),
      sendPrimaryCB(dpy_, "Send primary selection to viewers", this,false,this),
      queryConnectDialog(nullptr)
  {
    int y = yPad;
    acceptClipboard.move(xPad, y);
    acceptClipboard.checked(getBoolParam(dpy, ACCEPT_CUT_TEXT));
    y += acceptClipboard.height();
    setPrimaryCB.move(xPad + 10, y);
    setPrimaryCB.checked(getBoolParam(dpy, SET_PRIMARY));
    setPrimaryCB.disabled(!acceptClipboard.checked());
    y += setPrimaryCB.height();
    sendClipboard.move(xPad, y);
    sendClipboard.checked(getBoolParam(dpy, SEND_CUT_TEXT));
    y += sendClipboard.height();
    sendPrimaryCB.move(xPad + 10, y);
    sendPrimaryCB.checked(getBoolParam(dpy, SEND_PRIMARY));
    sendPrimaryCB.disabled(!sendClipboard.checked());
    y += sendPrimaryCB.height();
    setEventHandler(this);
    toplevel("VNC config", this, 0, nullptr, nullptr, iconic);
    if (waylandCtl.empty())
      XVncExtSelectInput(dpy, win(), VncExtQueryConnectMask);
  }

  // Wayland mode: a query from the control socket's "watch" connection
  void waylandQuery(uint32_t id, int timeout, const char* address,
                    const char* user) {
    vlog.debug("Query connection event");
    if (queryConnectDialog)
      delete queryConnectDialog;
    queryConnectDialog = nullptr;
    wlQueryId = id;
    if (timeout)
      queryConnectDialog = new QueryConnectDialog(dpy, address, user,
                                                  timeout, this);
    if (queryConnectDialog)
      queryConnectDialog->map();
  }

  // handleEvent()

  void handleEvent(TXWindow* /*w*/, XEvent* ev) override {
    if (ev->type == vncExtEventBase + VncExtQueryConnectNotify) {
       vlog.debug("Query connection event");
       if (queryConnectDialog)
         delete queryConnectDialog;
       queryConnectDialog = nullptr;
       char* qcAddress;
       char* qcUser;
       int qcTimeout;
       if (XVncExtGetQueryConnect(dpy, &qcAddress, &qcUser,
                                  &qcTimeout, &queryConnectId)) {
         if (qcTimeout)
           queryConnectDialog = new QueryConnectDialog(dpy, qcAddress,
                                                       qcUser, qcTimeout,
                                                       this);
         if (queryConnectDialog)
           queryConnectDialog->map();
         XFree(qcAddress);
         XFree(qcUser);
       }
    }
  }

  // TXDeleteWindowCallback method
  void deleteWindow(TXWindow* /*w*/) override {
    exit(1);
  }

  // TXCheckboxCallback method
  void checkboxSelect(TXCheckbox* checkbox) override {
    if (checkbox == &acceptClipboard) {
      setParam(dpy, ACCEPT_CUT_TEXT,
                      acceptClipboard.checked() ? "1" : "0");
      setPrimaryCB.disabled(!acceptClipboard.checked());
    } else if (checkbox == &sendClipboard) {
      setParam(dpy, SEND_CUT_TEXT,
                      sendClipboard.checked() ? "1" : "0");
      sendPrimaryCB.disabled(!sendClipboard.checked());
    } else if (checkbox == &setPrimaryCB) {
      setParam(dpy, SET_PRIMARY,
                      setPrimaryCB.checked() ? "1" : "0");
    } else if (checkbox == &sendPrimaryCB) {
      setParam(dpy, SEND_PRIMARY,
                      sendPrimaryCB.checked() ? "1" : "0");
    }
  }

  // QueryResultCallback interface
  void queryApproved() override {
    approve(true);
  }
  void queryRejected() override {
    approve(false);
  }

private:
  TXCheckbox acceptClipboard, setPrimaryCB;
  TXCheckbox sendClipboard, sendPrimaryCB;

  void approve(bool accept) {
    if (waylandCtl.empty()) {
      XVncExtApproveConnect(dpy, queryConnectId, accept ? 1 : 0);
      return;
    }
    std::string line = "approve " + std::to_string(wlQueryId) +
                       (accept ? " 1\n" : " 0\n");
    if (write(waylandWatchFd, line.data(), line.size()) !=
        (ssize_t)line.size())
      vlog.error(_("Answering the connection query failed"));
  }

  QueryConnectDialog* queryConnectDialog;
  void* queryConnectId;
  uint32_t wlQueryId = 0;
};

static void usage()
{
  fprintf(stderr,"Usage: %s [parameters]\n",
          programName);
  fprintf(stderr,"       %s [parameters] -connect "
          "[-view-only] <host>[:<port>]\n", programName);
  fprintf(stderr,"       %s [parameters] -disconnect\n", programName);
  fprintf(stderr,"       %s [parameters] [-set] <Xvnc-param>=<value> ...\n",
          programName);
  fprintf(stderr,"       %s [parameters] -list\n", programName);
  fprintf(stderr,"       %s [parameters] -get <param>\n", programName);
  fprintf(stderr,"       %s [parameters] -desc <param>\n",programName);
  fprintf(stderr,"\n"
          "Parameters can be turned on with -<param> or off with -<param>=0\n"
          "Parameters which take a value can be specified as "
          "-<param> <value>\n"
          "Other valid forms are <param>=<value> -<param>=<value> "
          "--<param>=<value>\n"
          "Parameter names are case-insensitive.  The parameters are:\n\n");
  core::Configuration::listParams(79, 14);
  exit(1);
}

// Wayland mode ("Xvnc --wayland") has no VNC X extension; the server
// listens on a control socket instead: $TIGERVNC_VNCCONFIG (set for the
// session) or $XDG_RUNTIME_DIR/tigervnc-<WAYLAND_DISPLAY>.vncconfig.

static std::string waylandControlPath()
{
  const char* env = getenv("TIGERVNC_VNCCONFIG");
  const char* wd = getenv("WAYLAND_DISPLAY");
  const char* rt = getenv("XDG_RUNTIME_DIR");
  std::string path;
  struct stat st;

  if (env && *env) {
    path = env;
  } else {
    if (!wd || !*wd || !rt || !*rt || strchr(wd, '/'))
      return "";
    path = std::string(rt) + "/tigervnc-" + wd + ".vncconfig";
  }
  if (stat(path.c_str(), &st) != 0 || !S_ISSOCK(st.st_mode))
    return "";
  return path;
}

// Sends one request; returns false on failure (message printed)
static bool waylandRequest(const std::string& path, const std::string& req,
                           std::string* result)
{
  struct sockaddr_un addr;
  int s = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (s < 0 || path.size() >= sizeof(addr.sun_path)) {
    if (s >= 0)
      close(s);
    return false;
  }
  memset(&addr, 0, sizeof(addr));
  addr.sun_family = AF_UNIX;
  strcpy(addr.sun_path, path.c_str());
  if (connect(s, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
    fprintf(stderr, _("%s: Cannot connect to %s: %s\n"), programName,
            path.c_str(), strerror(errno));
    close(s);
    return false;
  }
  std::string line = req + "\n";
  if (write(s, line.data(), line.size()) != (ssize_t)line.size()) {
    close(s);
    return false;
  }
  std::string reply;
  char buf[4096];
  ssize_t n;
  while ((n = read(s, buf, sizeof(buf))) > 0)
    reply.append(buf, n);
  close(s);

  size_t nl = reply.find('\n');
  std::string status = reply.substr(0, nl);
  std::string rest = nl == std::string::npos ? "" : reply.substr(nl + 1);
  if (status != "OK") {
    if (status.compare(0, 4, "ERR ") == 0)
      fprintf(stderr, "%s: %s\n", programName, status.c_str() + 4);
    return false;
  }
  if (result)
    *result = rest;
  return true;
}

// Opens the QueryConnect "watch" connection; -1 on failure
static int waylandWatch(const std::string& path)
{
  struct sockaddr_un addr;
  int s = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (s < 0 || path.size() >= sizeof(addr.sun_path)) {
    if (s >= 0)
      close(s);
    return -1;
  }
  memset(&addr, 0, sizeof(addr));
  addr.sun_family = AF_UNIX;
  strcpy(addr.sun_path, path.c_str());
  char ok[3];
  if (connect(s, (struct sockaddr*)&addr, sizeof(addr)) < 0 ||
      write(s, "watch\n", 6) != 6 || read(s, ok, 3) != 3 ||
      memcmp(ok, "OK\n", 3) != 0) {
    close(s);
    return -1;
  }
  return s;
}

static void runWindow(bool wayland);

static int waylandMain(const std::string& path, int argc, char** argv,
                       int i)
{
  std::string out;
  int ret = 0;

  if (i >= argc) {
    // Like on Xvnc: the window (clipboard settings) and the QueryConnect
    // prompt, as an X client of Xwayland, with the control socket in
    // place of the VNC extension
    if (!dpy) {
      fprintf(stderr, _("%s: Unable to open display \"%s\"\n"),
              programName, XDisplayName(displayname));
      return 1;
    }
    waylandCtl = path;
    waylandWatchFd = waylandWatch(path);
    if (waylandWatchFd < 0) {
      fprintf(stderr, _("%s: Cannot connect to %s: %s\n"), programName,
              path.c_str(), strerror(errno));
      return 1;
    }
    runWindow(true);
    return 0;
  }

  for (; i < argc; i++) {
    std::string req;
    std::string failMsg;
    bool print = false;

    if (strcmp(argv[i], "-connect") == 0) {
      i++;
      req = "connect ";
      if (i < argc && strcmp(argv[i], "-view-only") == 0) {
        req += "-view-only ";
        i++;
      }
      if (i >= argc) usage();
      req += argv[i];
      failMsg = core::format(_("%s: Connecting to %s failed"),
                             programName, argv[i]);
    } else if (strcmp(argv[i], "-disconnect") == 0) {
      req = "disconnect";
      failMsg = core::format(_("%s: Disconnecting all clients failed"),
                             programName);
    } else if (strcmp(argv[i], "-get") == 0 ||
               strcmp(argv[i], "-desc") == 0) {
      bool get = strcmp(argv[i], "-get") == 0;
      i++;
      if (i >= argc) usage();
      req = std::string(get ? "get " : "desc ") + argv[i];
      if (get)
        failMsg = core::format(
          _("%s: Getting the value of parameter %s failed"),
          programName, argv[i]);
      else
        failMsg = core::format(
          _("%s: Getting the description for parameter %s failed"),
          programName, argv[i]);
      print = true;
    } else if (strcmp(argv[i], "-list") == 0) {
      req = "list";
      failMsg = core::format(_("%s: Listing parameters failed"),
                             programName);
      print = true;
    } else {
      if (strcmp(argv[i], "-set") == 0) {
        i++;
        if (i >= argc) usage();
      } else if (argv[i][0] == '-') {
        fprintf(stderr, _("%s: Unrecognized option '%s'\n"),
                programName, argv[i]);
        exit(1);
      }
      if (!strchr(argv[i], '=')) {
        fprintf(stderr, _("%s: Invalid parameter syntax '%s'\n"),
                programName, argv[i]);
        exit(1);
      }
      req = std::string("set ") + argv[i];
      failMsg = core::format(_("%s: Setting the parameter %s failed"),
                             programName, argv[i]);
    }

    if (waylandRequest(path, req, &out)) {
      if (print)
        fputs(out.c_str(), stdout);
    } else {
      fprintf(stderr, "%s\n", failMsg.c_str());
      ret = 1;
    }
  }
  return ret;
}

void removeArgs(int* argc, char** argv, int first, int n)
{
  if (first + n > *argc) return;
  for (int i = first + n; i < *argc; i++)
    argv[i-n] = argv[i];
  *argc -= n;
}

int main(int argc, char** argv)
{
  programName = argv[0];
  core::initStdIOLoggers();
  core::LogWriter::setLogParams("*:stderr:30");

  // Process vncconfig's own parameters first, then we process the
  // other arguments when we have the X display.
  int i;
  for (i = 1; i < argc;) {
    int ret;

    ret = core::Configuration::handleParamArg(argc, argv, i);
    if (ret > 0) {
      i += ret;
      continue;
    }

    if (strcmp(argv[i], "-help") == 0) {
      usage();
    }

    if (strcmp(argv[i], "-version") == 0) {
      fprintf(stderr, "vncconfig (TigerVNC) %s\n", PACKAGE_VERSION);
      exit(0);
    }

    break;
  }

  dpy = XOpenDisplay(displayname);
  if (!dpy || !XVncExtQueryExtension(dpy, &vncExtEventBase,
                                     &vncExtErrorBase)) {
    // Wayland mode: Xwayland has no VNC extension
    std::string ctl = waylandControlPath();
    if (!ctl.empty()) {
      if (dpy && i < argc) {
        XCloseDisplay(dpy);
        dpy = nullptr;
      }
      return waylandMain(ctl, argc, argv, i);
    }
  }

  if (!dpy) {
    fprintf(stderr,"%s: unable to open display \"%s\"\n",
            programName, XDisplayName(displayname));
    exit(1);
  }

  if (!XVncExtQueryExtension(dpy, &vncExtEventBase, &vncExtErrorBase)) {
    fprintf(stderr,"No VNC extension on display %s\n",
            XDisplayName(displayname));
    exit(1);
  }

  if (i < argc) {
    for (; i < argc; i++) {
      if (strcmp(argv[i], "-connect") == 0) {
        Bool viewOnly = False;
        i++;
        if (strcmp(argv[i], "-view-only") == 0) {
          viewOnly = True;
          i++;
        }
        if (i >= argc) usage();
        if (!XVncExtConnect(dpy, argv[i], viewOnly)) {
          fprintf(stderr, "Connecting to %s failed\n",argv[i]);
        }
      } else if (strcmp(argv[i], "-disconnect") == 0) {
        if (!XVncExtConnect(dpy, "", False)) {
          fprintf(stderr, "Disconnecting all clients failed\n");
        }
      } else if (strcmp(argv[i], "-get") == 0) {
        i++;
        if (i >= argc) usage();
        char* data;
        int len;
        if (XVncExtGetParam(dpy, argv[i], &data, &len)) {
          printf("%.*s\n",len,data);
        } else {
          fprintf(stderr, "Getting param %s failed\n",argv[i]);
        }
        XFree(data);
      } else if (strcmp(argv[i], "-desc") == 0) {
        i++;
        if (i >= argc) usage();
        char* desc = XVncExtGetParamDesc(dpy, argv[i]);
        if (desc) {
          printf("%s\n",desc);
        } else {
          fprintf(stderr, "Getting description for param %s failed\n",argv[i]);
        }
        XFree(desc);
      } else if (strcmp(argv[i], "-list") == 0) {
        int nParams;
        char** list = XVncExtListParams(dpy, &nParams);
        for (int n = 0; n < nParams; n++) {
          printf("%s\n",list[n]);
        }
        XVncExtFreeParamList(list);
      } else if (strcmp(argv[i], "-set") == 0) {
        i++;
        if (i >= argc) usage();

        char* equal = strchr(argv[i], '=');
        if (!equal) {
          fprintf(stderr, "%s: Invalid parameter syntax '%s'\n",
                  programName, argv[i]);
          fprintf(stderr, "See '%s -help' for more information.\n",
                  programName);
          exit(1);
        }

        std::string name(argv[i], equal-argv[i]);
        std::string value(equal+1);

        if (!XVncExtSetParam(dpy, name.c_str(), value.c_str()))
          fprintf(stderr, "Setting param %s failed\n",argv[i]);
      } else if (argv[i][0] == '-') {
        fprintf(stderr, "%s: Unrecognized option '%s'\n",
                programName, argv[i]);
        fprintf(stderr, "See '%s -help' for more information.\n",
                programName);
        exit(1);
      } else {
        char* equal = strchr(argv[i], '=');
        if (!equal) {
          fprintf(stderr, "%s: Invalid parameter syntax '%s'\n",
                  programName, argv[i]);
          fprintf(stderr, "See '%s -help' for more information.\n",
                  programName);
          exit(1);
        }

        std::string name(argv[i], equal-argv[i]);
        std::string value(equal+1);

        if (!XVncExtSetParam(dpy, name.c_str(), value.c_str()))
          fprintf(stderr, "Setting param %s failed\n",argv[i]);
      }
    }

    return 0;
  }

  runWindow(false);
  return 0;
}

static void runWindow(bool wayland)
{
  try {
    TXWindow::init(dpy,"Vncconfig");

    VncConfigWindow w(dpy);
    if (!noWindow) w.map();

    std::string wlInput;

    while (true) {
      struct timeval tv;
      struct timeval* tvp = nullptr;

      // Process any incoming X events
      TXWindow::handleXEvents(dpy);
      
      // Process expired timers and get the time until the next one
      int timeoutMs = core::Timer::checkTimeouts();
      if (timeoutMs >= 0) {
        tv.tv_sec = timeoutMs / 1000;
        tv.tv_usec = (timeoutMs % 1000) * 1000;
        tvp = &tv;
      }
      
      // If there are X requests pending then poll, don't wait!
      if (XPending(dpy)) {
        tv.tv_usec = tv.tv_sec = 0;
        tvp = &tv;
      }
      
      // Wait for X events, VNC traffic, or the next timer expiry
      fd_set rfds;
      FD_ZERO(&rfds);
      FD_SET(ConnectionNumber(dpy), &rfds);
      if (wayland)
        FD_SET(waylandWatchFd, &rfds);
      int n = select(FD_SETSIZE, &rfds, nullptr, nullptr, tvp);
      if (n < 0) throw core::socket_error("select", errno);

      // Wayland mode: queries from the server
      if (wayland && FD_ISSET(waylandWatchFd, &rfds)) {
        char buf[1024];
        ssize_t len = read(waylandWatchFd, buf, sizeof(buf));
        if (len <= 0) {
          vlog.error(_("Lost the connection to the server"));
          exit(1);
        }
        wlInput.append(buf, len);
        size_t nl;
        while ((nl = wlInput.find('\n')) != std::string::npos) {
          std::string line = wlInput.substr(0, nl);
          wlInput.erase(0, nl + 1);
          // query\t<id>\t<timeout>\t<address>\t<user>
          std::vector<std::string> f;
          size_t pos = 0, tab;
          while ((tab = line.find('\t', pos)) != std::string::npos) {
            f.push_back(line.substr(pos, tab - pos));
            pos = tab + 1;
          }
          f.push_back(line.substr(pos));
          if (f.size() == 5 && f[0] == "query")
            w.waylandQuery(strtoul(f[1].c_str(), nullptr, 10),
                           atoi(f[2].c_str()), f[3].c_str(), f[4].c_str());
        }
      }
    }

    XCloseDisplay(dpy);

  } catch (std::exception& e) {
    vlog.error("%s", e.what());
  }
}
