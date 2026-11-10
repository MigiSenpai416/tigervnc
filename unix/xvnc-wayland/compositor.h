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

/*
 * C interface between the wlroots based headless compositor (C) and
 * the TigerVNC server core (C++). wlroots headers are C only, so all
 * wlroots code lives behind this interface.
 */

#ifndef __XVNC_WAYLAND_COMPOSITOR_H__
#define __XVNC_WAYLAND_COMPOSITOR_H__

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct wvnc_compositor;

enum wvnc_selection {
  WVNC_SELECTION_CLIPBOARD = 0,
  WVNC_SELECTION_PRIMARY = 1,
};

struct wvnc_callbacks {
  void* data;
  /* Part of the framebuffer has changed */
  void (*damage)(void* data, int x, int y, int w, int h);
  /* New cursor image, w*h RGBA quadruplets, non-premultiplied alpha */
  void (*cursor)(void* data, int w, int h, int hotx, int hoty,
                 const uint8_t* rgba);
  /* A Wayland client took (or dropped) a selection */
  void (*selection_announce)(void* data, enum wvnc_selection which,
                             bool available);
  /* A Wayland client wants the data the VNC client announced */
  void (*selection_wanted)(void* data);
  /* Reply to wvnc_request_selection(), UTF-8, may contain CRLF */
  void (*selection_data)(void* data, const char* text, size_t len);
  /* Lock key LED state changed (rfb ledStates bits) */
  void (*leds)(void* data, unsigned int leds);
  /* Xwayland is up and accepting X11 clients on the given DISPLAY */
  void (*xwayland_ready)(void* data, const char* display);
};

struct wvnc_options {
  int width;
  int height;
  /* Wayland socket name, e.g. "wayland-1"; NULL picks a free one */
  const char* socket_name;
  bool xwayland;
};

struct wvnc_compositor* wvnc_compositor_create(const struct wvnc_options* opts,
                                               const struct wvnc_callbacks* cb);
void wvnc_compositor_destroy(struct wvnc_compositor* c);

const char* wvnc_socket_name(struct wvnc_compositor* c);

/* Event loop integration */
int wvnc_event_fd(struct wvnc_compositor* c);
void wvnc_dispatch(struct wvnc_compositor* c);
void wvnc_flush(struct wvnc_compositor* c);

/* XRGB8888 framebuffer (little endian 0x00RRGGBB words) */
uint8_t* wvnc_framebuffer(struct wvnc_compositor* c, int* width,
                          int* height, int* stride_pixels);
/* Resize the virtual output. On success the framebuffer pointer
 * changes; the previous one stays valid until the next resize. */
bool wvnc_resize(struct wvnc_compositor* c, int width, int height);

/* Input */
void wvnc_pointer_event(struct wvnc_compositor* c, int x, int y,
                        uint16_t button_mask);
void wvnc_key_event(struct wvnc_compositor* c, uint32_t keysym,
                    uint32_t qnum, bool down, bool raw_keyboard);

/* Clipboard */
void wvnc_set_vnc_selection(struct wvnc_compositor* c, bool available,
                            bool also_primary);
void wvnc_vnc_selection_data(struct wvnc_compositor* c, const char* text,
                             size_t len);
void wvnc_request_selection(struct wvnc_compositor* c);
/* The PRIMARY selection was forwarded to VNC clients, so the next
 * wvnc_request_selection() should read PRIMARY */
void wvnc_note_primary_announced(struct wvnc_compositor* c);

#ifdef __cplusplus
}
#endif

#endif
