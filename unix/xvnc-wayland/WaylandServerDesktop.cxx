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

#include <signal.h>
#include <string.h>
#include <unistd.h>

#include <vector>

#include <core/LogWriter.h>
#include <core/Region.h>
#include <core/string.h>
#include <core/i18n.h>

#include <rfb/PixelFormat.h>
#include <rfb/ScreenSet.h>
#include <rfb/VNCServer.h>
#include <rfb/SConnection.h>

#include "WaylandServerDesktop.h"
#include "parameters.h"

static core::LogWriter vlog("WaylandDesktop");

// XRGB8888, little endian words: 0x00RRGGBB
static const rfb::PixelFormat fbFormat(32, 24, false, true,
                                       255, 255, 255, 16, 8, 0);

WaylandServerDesktop::WaylandServerDesktop(int width, int height,
                                           const char* socketName,
                                           bool xwayland,
                                           int nestedMode,
                                           const rfb::PixelFormat& pf)
  : comp(nullptr), server(nullptr), pb(nullptr), nativePF(pf),
    convert(!(pf == fbFormat)), xReady(false)
{
  struct wvnc_options opts;
  struct wvnc_callbacks cb;

  memset(&opts, 0, sizeof(opts));
  opts.width = width;
  opts.height = height;
  opts.socket_name = socketName;
  opts.xwayland = xwayland;
  opts.nested_mode = (enum wvnc_nested_mode)nestedMode;

  memset(&cb, 0, sizeof(cb));
  cb.data = this;
  cb.damage = cbDamage;
  cb.cursor = cbCursor;
  cb.selection_announce = cbSelectionAnnounce;
  cb.selection_wanted = cbSelectionWanted;
  cb.selection_data = cbSelectionData;
  cb.leds = cbLeds;
  cb.xwayland_ready = cbXwaylandReady;
  cb.info = cbInfo;

  comp = wvnc_compositor_create(&opts, &cb);
  if (comp)
    pb = createPixelBuffer();
}

WaylandServerDesktop::~WaylandServerDesktop()
{
  // The server must have dropped the pixel buffer already
  delete pb;
  wvnc_compositor_destroy(comp);
}

const char* WaylandServerDesktop::socketName() const
{
  return wvnc_socket_name(comp);
}

int WaylandServerDesktop::eventFd() const
{
  return wvnc_event_fd(comp);
}

void WaylandServerDesktop::dispatch()
{
  wvnc_dispatch(comp);
}

void WaylandServerDesktop::flush()
{
  wvnc_flush(comp);
}

rfb::PixelBuffer* WaylandServerDesktop::createPixelBuffer()
{
  int w, h, stride;
  uint8_t* data = wvnc_framebuffer(comp, &w, &h, &stride);
  if (!convert)
    return new rfb::FullFramePixelBuffer(fbFormat, w, h, data, stride);
  rfb::ManagedPixelBuffer* mpb = new rfb::ManagedPixelBuffer(nativePF, w, h);
  mpb->imageRect(fbFormat, core::Rect(0, 0, w, h), data, stride);
  return mpb;
}

void WaylandServerDesktop::init(rfb::VNCServer* vs)
{
  server = vs;
  // The display is always there, so the server can always be ready
  server->setPixelBuffer(pb);
  // The default layout is a single screen covering the framebuffer
  wvnc_set_screens(comp, nullptr, 0);
}

void WaylandServerDesktop::setScreens(const rfb::ScreenSet& layout)
{
  std::vector<struct wvnc_box> boxes;
  for (const rfb::Screen& s : layout)
    boxes.push_back({s.dimensions.tl.x, s.dimensions.tl.y,
                     s.dimensions.width(), s.dimensions.height()});
  wvnc_set_screens(comp, boxes.data(), boxes.size());
}

void WaylandServerDesktop::queryConnection(rfb::SConnection* conn)
{
  // FIXME: Xvnc asks vncconfig; there is no equivalent helper yet
  server->approveConnection(conn, false,
                            _("Unable to query the local user to "
                              "accept the connection."));
}

void WaylandServerDesktop::terminate()
{
  kill(getpid(), SIGTERM);
}

unsigned int WaylandServerDesktop::setScreenLayout(int fb_width,
                                                   int fb_height,
                                                   const rfb::ScreenSet& layout)
{
  rfb::PixelBuffer* oldpb;

  vlog.debug("Client requested %dx%d with %d screen(s)", fb_width,
             fb_height, layout.num_screens());

  if (!wvnc_resize(comp, fb_width, fb_height))
    return rfb::resultInvalid;

  // The virtual output covers the entire framebuffer. The screen
  // rectangles are used to give a nested compositor one output per
  // screen; ordinary windows only see the whole framebuffer.
  oldpb = pb;
  pb = createPixelBuffer();
  server->setPixelBuffer(pb, layout);
  delete oldpb;
  setScreens(layout);

  vlog.info(_("Framebuffer resized to %dx%d"), fb_width, fb_height);

  return rfb::resultSuccess;
}

void WaylandServerDesktop::keyEvent(uint32_t keysym, uint32_t keycode,
                                    bool down)
{
  wvnc_key_event(comp, keysym, keycode, down, rawKeyboard);
}

void WaylandServerDesktop::pointerEvent(const core::Point& pos,
                                        uint16_t buttonMask)
{
  wvnc_pointer_event(comp, pos.x, pos.y, buttonMask);
}

void WaylandServerDesktop::handleClipboardRequest()
{
  vlog.debug("Got request for local clipboard");
  wvnc_request_selection(comp);
}

void WaylandServerDesktop::handleClipboardAnnounce(bool available)
{
  vlog.debug("Remote clipboard %s", available ? "announced" : "lost");
  wvnc_set_vnc_selection(comp, available, setPrimary);
}

void WaylandServerDesktop::handleClipboardData(const char* data)
{
  vlog.debug("Got remote clipboard data, sending to Wayland clients");
  wvnc_vnc_selection_data(comp, data, strlen(data));
}

void WaylandServerDesktop::cbDamage(void* data, int x, int y, int w, int h)
{
  WaylandServerDesktop* self = (WaylandServerDesktop*)data;
  if (!self->server)
    return;
  core::Rect r(x, y, x + w, y + h);
  if (self->convert) {
    int fw, fh, stride;
    uint8_t* fb = wvnc_framebuffer(self->comp, &fw, &fh, &stride);
    r = r.intersect(core::Rect(0, 0, fw, fh));
    if (r.is_empty() || fw != self->pb->width() || fh != self->pb->height())
      return;
    ((rfb::ModifiablePixelBuffer*)self->pb)->imageRect(
      fbFormat, r, fb + ((size_t)r.tl.y * stride + r.tl.x) * 4, stride);
  }
  self->server->add_changed(core::Region(r));
}

void WaylandServerDesktop::cbCursor(void* data, int w, int h, int hotx,
                                    int hoty, const uint8_t* rgba)
{
  WaylandServerDesktop* self = (WaylandServerDesktop*)data;
  if (!self->server)
    return;
  try {
    self->server->setCursor(w, h, core::Point(hotx, hoty), rgba);
  } catch (std::exception& e) {
    vlog.error("setCursor: %s", e.what());
  }
}

void WaylandServerDesktop::cbSelectionAnnounce(void* data,
                                               enum wvnc_selection which,
                                               bool available)
{
  WaylandServerDesktop* self = (WaylandServerDesktop*)data;
  if (!self->server)
    return;
  if (which == WVNC_SELECTION_PRIMARY) {
    if (!sendPrimary)
      return;
    wvnc_note_primary_announced(self->comp);
  }
  vlog.debug("Local %s %s", which == WVNC_SELECTION_PRIMARY ?
             "PRIMARY" : "CLIPBOARD",
             available ? "changed" : "cleared");
  try {
    self->server->announceClipboard(available);
  } catch (std::exception& e) {
    vlog.error("announceClipboard: %s", e.what());
  }
}

void WaylandServerDesktop::cbSelectionWanted(void* data)
{
  WaylandServerDesktop* self = (WaylandServerDesktop*)data;
  if (!self->server)
    return;
  try {
    self->server->requestClipboard();
  } catch (std::exception& e) {
    vlog.error("requestClipboard: %s", e.what());
  }
}

void WaylandServerDesktop::cbSelectionData(void* data, const char* text,
                                           size_t len)
{
  WaylandServerDesktop* self = (WaylandServerDesktop*)data;
  std::string filtered;

  if (!self->server)
    return;

  if (!core::isValidUTF8(text, len)) {
    // Probably Latin-1 from an old X11 client
    filtered = core::convertLF(core::latin1ToUTF8(text, len).c_str());
  } else {
    filtered = core::convertLF(text, len);
  }

  try {
    self->server->sendClipboardData(filtered.c_str());
  } catch (std::exception& e) {
    vlog.error("sendClipboardData: %s", e.what());
  }
}

void WaylandServerDesktop::cbLeds(void* data, unsigned int leds)
{
  WaylandServerDesktop* self = (WaylandServerDesktop*)data;
  if (!self->server)
    return;
  self->server->setLEDState(leds);
}

void WaylandServerDesktop::cbXwaylandReady(void* data, const char* display)
{
  WaylandServerDesktop* self = (WaylandServerDesktop*)data;
  self->xDisplay = display ? display : "";
  self->xReady = true;
  vlog.info(_("Xwayland ready on DISPLAY=%s"), self->xDisplay.c_str());
}

void WaylandServerDesktop::cbInfo(void* /*data*/, const char* msg)
{
  vlog.info("%s", msg);
}
