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

#ifndef __WAYLAND_SERVER_DESKTOP_H__
#define __WAYLAND_SERVER_DESKTOP_H__

#include <string>

#include <rfb/SDesktop.h>
#include <rfb/PixelBuffer.h>

#include "compositor.h"

namespace rfb { class VNCServer; }

// rfb::SDesktop implementation backed by our own headless Wayland
// compositor. Unlike w0vncserver this desktop *is* the display: it
// exists for the life time of the process, like Xvnc's X server.
class WaylandServerDesktop : public rfb::SDesktop {
public:
  WaylandServerDesktop(int width, int height, const char* socketName,
                       bool xwayland, int nestedMode);
  virtual ~WaylandServerDesktop();

  bool valid() const { return comp != nullptr; }
  const char* socketName() const;
  const char* xwaylandDisplay() const { return xDisplay.c_str(); }
  bool xwaylandReady() const { return xReady; }

  int eventFd() const;
  void dispatch();
  void flush();

  // rfb::SDesktop
  void init(rfb::VNCServer* vs) override;
  void queryConnection(rfb::SConnection* conn) override;
  void terminate() override;
  unsigned int setScreenLayout(int fb_width, int fb_height,
                               const rfb::ScreenSet& layout) override;
  void keyEvent(uint32_t keysym, uint32_t keycode, bool down) override;
  void pointerEvent(const core::Point& pos, uint16_t buttonMask) override;
  void handleClipboardRequest() override;
  void handleClipboardAnnounce(bool available) override;
  void handleClipboardData(const char* data) override;

private:
  static void cbDamage(void* data, int x, int y, int w, int h);
  static void cbCursor(void* data, int w, int h, int hotx, int hoty,
                       const uint8_t* rgba);
  static void cbSelectionAnnounce(void* data, enum wvnc_selection which,
                                  bool available);
  static void cbSelectionWanted(void* data);
  static void cbSelectionData(void* data, const char* text, size_t len);
  static void cbLeds(void* data, unsigned int leds);
  static void cbXwaylandReady(void* data, const char* display);

  rfb::PixelBuffer* createPixelBuffer();
  void setScreens(const rfb::ScreenSet& layout);

  struct wvnc_compositor* comp;
  rfb::VNCServer* server;
  rfb::PixelBuffer* pb;
  std::string xDisplay;
  bool xReady;
};

#endif
