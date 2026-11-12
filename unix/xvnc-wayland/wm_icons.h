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
 * Application icons for the window switcher of the built-in window
 * manager. Icons are square, premultiplied ARGB8888 (native endian
 * 0xAARRGGBB words), reference counted.
 *
 * Sources, in the order the compositor tries them: pixels the client
 * hands over (xdg-toplevel-icon-v1 buffers, X11 _NET_WM_ICON), then
 * the freedesktop icon theme found through the application's .desktop
 * file. Image decoders are pluggable (see wm_icons.c): PNG through
 * libpng and SVG through lunasvg, each only if built in.
 */

#ifndef __XVNC_WAYLAND_WM_ICONS_H__
#define __XVNC_WAYLAND_WM_ICONS_H__

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct wvnc_icon {
  int refs;
  int size;          /* width = height */
  uint32_t* px;      /* size * size premultiplied ARGB */
};

struct wvnc_icon* wvnc_icon_ref(struct wvnc_icon* icon);
void wvnc_icon_unref(struct wvnc_icon* icon);

/* Scales any ARGB image (premultiplied or straight alpha) into a new
 * size x size icon, keeping the aspect ratio (centred) */
struct wvnc_icon* wvnc_icon_from_argb(const uint32_t* argb, int w, int h,
                                      size_t stride_px, bool premultiplied,
                                      int size);

/* The X11 _NET_WM_ICON property (CARDINAL[]: width, height, pixels,
 * repeated per image; straight alpha). Picks the best image for size. */
struct wvnc_icon* wvnc_icon_from_net_wm_icon(const uint32_t* data,
                                             size_t n, int size);

/* Icon theme lookup for an application. ids: app_id, or WM_CLASS
 * class and instance; tried in turn through their .desktop files
 * (by file name, then StartupWMClass) and then as icon names.
 * Results, including misses, are cached. NULL if nothing was found. */
struct wvnc_icon* wvnc_icon_for_app(const char* const* ids, int n,
                                    int size);

/* Icon theme lookup of one icon name (or an absolute file path) */
struct wvnc_icon* wvnc_icon_for_name(const char* name, int size);

/* Generic window icon drawn in code, for windows without one */
struct wvnc_icon* wvnc_icon_fallback(int size);

/* What the decoders can read, for the log */
const char* wvnc_icon_formats(void);

/* Decoders. Each fills a malloc'ed size*size premultiplied ARGB
 * buffer (SVG renders straight to size; PNG returns its own size and
 * the caller scales). */
#ifdef HAVE_LUNASVG
uint32_t* wvnc_svg_render(const char* path, int size);
#endif

#ifdef __cplusplus
}
#endif

#endif
