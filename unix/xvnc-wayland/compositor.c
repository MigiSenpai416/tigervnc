/* Copyright 2026 The TigerVNC Wayland-mode fork contributors
 *
 * Parts derived from wlroots' tinywl (CC0 / public domain).
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
 * Headless wlroots compositor used by "Xvnc --wayland".
 *
 * The compositor owns a single virtual output (wlroots headless
 * backend + pixman renderer). Every rendered frame is copied into a
 * persistent XRGB8888 framebuffer that the TigerVNC server core
 * serves to VNC clients. Input from VNC clients is injected into a
 * virtual wlr_keyboard and directly into the seat for the pointer.
 *
 * Window management policy (deliberately minimal):
 *   - floating, stacking windows; new windows are centred (cascaded)
 *   - click to focus and raise
 *   - client side decorations; interactive move/resize, maximize and
 *     fullscreen requests are honoured
 *   - layer-shell (panels, backgrounds, launchers) is supported
 *   - X11 clients run through Xwayland with the built-in wlroots XWM
 */

#define _GNU_SOURCE

#include <assert.h>
#include <errno.h>
#include <math.h>
#include <fcntl.h>
#include <poll.h>
#include <stdlib.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <drm_fourcc.h>
#include <pixman.h>
#include <wayland-server-core.h>
#include <xkbcommon/xkbcommon.h>

#include <wlr/backend.h>
#include <wlr/backend/headless.h>
#include <wlr/interfaces/wlr_buffer.h>
#include <wlr/interfaces/wlr_keyboard.h>
#include <wlr/render/allocator.h>
#include <wlr/render/pixman.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/render/wlr_texture.h>
#include <wlr/types/wlr_buffer.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_data_control_v1.h>
#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_ext_data_control_v1.h>
#include <wlr/types/wlr_fractional_scale_v1.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_layer_shell_v1.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_pointer.h>
#include <wlr/types/wlr_presentation_time.h>
#include <wlr/types/wlr_primary_selection.h>
#include <wlr/types/wlr_primary_selection_v1.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_screencopy_v1.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_single_pixel_buffer_v1.h>
#include <wlr/types/wlr_subcompositor.h>
#include <wlr/types/wlr_viewporter.h>
#include <wlr/types/wlr_xcursor_manager.h>
#include <wlr/types/wlr_xdg_output_v1.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/types/wlr_xdg_toplevel_icon_v1.h>
#include <wlr/util/box.h>
#include <wlr/util/edges.h>
#include <wlr/util/log.h>
#include <wlr/xcursor.h>
#ifdef HAVE_XWAYLAND
#include <wlr/xwayland.h>
#include <xcb/xcb.h>
#include <xcb/xcbext.h>
#endif

#include <wlr/types/wlr_output_management_v1.h>
#include <wlr/util/box.h>

#include "compositor.h"
#include "nested_bridge.h"
#include "wm_icons.h"
#include "wm_text.h"

extern const unsigned short code_map_qnum_to_xorgevdev[];
extern const unsigned int code_map_qnum_to_xorgevdev_len;

/* rfb/ledStates.h */
#define WVNC_LED_SCROLL (1 << 0)
#define WVNC_LED_NUM    (1 << 1)
#define WVNC_LED_CAPS   (1 << 2)

/* linux/input-event-codes.h */
#define WVNC_BTN_LEFT   0x110
#define WVNC_BTN_RIGHT  0x111
#define WVNC_BTN_MIDDLE 0x112
#define WVNC_BTN_SIDE   0x113
#define WVNC_BTN_EXTRA  0x114

#define MAX_PRESSED_KEYS 64
#define MAX_SCREENS 16
#define MAX_SELECTION_SIZE (16 * 1024 * 1024)
#define SWITCHER_MAX 64

enum owner_type {
  OWNER_XDG,
  OWNER_XWAYLAND,
  OWNER_LAYER,
  OWNER_DECO,
};

/* Common first member of anything we hang off scene node->data */
struct owner {
  enum owner_type type;
};

enum grab_mode {
  GRAB_NONE,
  GRAB_DECO,  /* button pressed on a title bar button */
  GRAB_MOVE,
  GRAB_RESIZE,
};

struct view;

struct pressed_key {
  uint32_t id;
  uint32_t keycode; /* xkb keycode */
};

struct pending_write {
  struct wl_list link;
  struct wvnc_compositor* c;
  int fd;
  char* data;
  size_t len, off;
  struct wl_event_source* source;
};

struct pending_read {
  struct wvnc_compositor* c;
  int fd;
  char* data;
  size_t len, cap;
  struct wl_event_source* source;
};

struct vnc_data_source {
  struct wlr_data_source base;
  struct wvnc_compositor* c;
};

struct vnc_primary_source {
  struct wlr_primary_selection_source base;
  struct wvnc_compositor* c;
};

struct wvnc_compositor {
  struct wvnc_callbacks cb;

  struct wl_display* display;
  struct wl_event_loop* loop;
  struct wlr_backend* backend;
  struct wlr_renderer* renderer;
  struct wlr_allocator* allocator;
  struct wlr_compositor* compositor;
  struct wlr_scene* scene;
  struct wlr_output_layout* output_layout;
  struct wlr_scene_output_layout* scene_layout;
  const char* socket;

  /* The one and only output */
  struct wlr_output* output;
  struct wlr_scene_output* scene_output;
  struct wl_listener output_frame;
  struct wl_listener output_destroy;
  struct wl_listener new_output;
  int width, height;
  bool full_damage;

  /* Framebuffer served over VNC */
  uint32_t* fb;
  uint32_t* old_fb;

  /* Scene layers, bottom to top */
  struct wlr_scene_rect* background;
  struct wlr_scene_tree* layer_trees[4];
  struct wlr_scene_tree* views_tree;
  struct wlr_scene_tree* unmanaged_tree;
  struct wlr_scene_tree* overlay_tree;
  struct wlr_scene_tree* switcher_tree;   /* above everything */
  struct wlr_box usable_area;

  struct wlr_xdg_shell* xdg_shell;
  struct wl_listener new_xdg_toplevel;
  struct wl_listener new_xdg_popup;
  struct wl_list views; /* struct view.link, front to back */
  int cascade;

  struct wlr_layer_shell_v1* layer_shell;
  struct wl_listener new_layer_surface;
  struct wl_list layers; /* struct layer.link */

  /* Seat */
  struct wlr_seat* seat;
  struct wlr_keyboard keyboard;
  struct wl_listener keyboard_modifiers;
  struct wl_listener request_set_cursor;
  struct wl_listener request_set_selection;
  struct wl_listener request_set_primary_selection;
  struct wl_listener set_selection;
  struct wl_listener set_primary_selection;
  struct pressed_key pressed[MAX_PRESSED_KEYS];
  int num_pressed;
  unsigned int leds;

  /* Pointer state */
  double px, py;
  uint16_t button_mask;
  struct wlr_surface* implicit_grab;
  struct wl_listener implicit_grab_destroy;
  enum grab_mode grab_mode;
  struct view* grab_view;
  double grab_x, grab_y;
  struct wlr_box grab_box;
  uint32_t grab_edges;

  /* Cursor image */
  struct wlr_xcursor_manager* xcursor_mgr;
  struct wlr_surface* cursor_surface;
  struct wl_listener cursor_surface_commit;
  struct wl_listener cursor_surface_destroy;
  int cursor_hotx, cursor_hoty;
  bool cursor_is_default;     /* a cursor of ours (cursor_named) is shown */
  const char* cursor_named;

  /* Clipboard */
  struct wl_list pending_writes;
  struct wl_list pending_fds; /* fds waiting for VNC data */
  bool vnc_data_valid;
  char* vnc_data;
  size_t vnc_data_len;
  bool vnc_data_requested;
  struct pending_read* reader;
  enum wvnc_selection last_announced;

#ifdef HAVE_XWAYLAND
  struct wlr_xwayland* xwayland;
  struct wl_listener xwayland_ready;
  struct wl_listener xwayland_new_surface;
#endif

  /* wlr-output-management */
  struct wlr_output_manager_v1* output_manager;
  struct wl_listener output_manager_apply;
  struct wl_listener output_manager_test;

  /* Nested compositor support */
  enum wvnc_nested_mode nested_mode;
  bool x11_title_bars;
  struct wlr_box screens[MAX_SCREENS];
  int num_screens; /* 0: one screen covering the framebuffer */
  unsigned int view_seq;
  struct nested_bridge* bridge;

  /* X11 title bar button under the pointer (hover highlight) */
  struct view* hover_view;
  int hover_kind;

  /* Application icons: xdg-toplevel-icon-v1, and our own X connection
   * to Xwayland for _NET_WM_ICON */
  struct wlr_xdg_toplevel_icon_manager_v1* icon_manager;
  struct wl_listener set_icon;
#ifdef HAVE_XWAYLAND
  xcb_connection_t* icon_xcb;
  bool icon_xcb_failed;
  xcb_atom_t net_wm_icon;
#endif

  /* Alt+Tab window switcher */
  struct {
    bool active;
    struct view* views[SWITCHER_MAX];   /* MRU order */
    int n, sel;
    struct view* prev;                  /* focused when it opened */
    struct wlr_scene_buffer* buffer;
  } sw;
};

struct view {
  struct owner owner;
  struct wl_list link;
  struct wvnc_compositor* c;
  struct wlr_scene_tree* scene_tree;
  bool mapped;
  bool maximized, fullscreen;
  bool minimized;       /* hidden, restorable through Alt+Tab */
  bool activated;
  struct wlr_box saved; /* geometry before maximize/fullscreen */
  /* Switcher icon, resolved on demand; xdg_icon: what the client set
   * through xdg-toplevel-icon-v1 */
  struct wvnc_icon* icon;
  bool icon_valid;
  struct wlr_xdg_toplevel_icon_v1* xdg_icon;
  /* An output window of a nested compositor: it gets a whole screen,
   * stays at the bottom and can't be moved or resized */
  bool nested;
  bool nested_checked;
  unsigned int seq;
  struct wlr_box nested_box;

  /* xdg */
  struct wlr_xdg_toplevel* xdg;
  struct wl_listener map;
  struct wl_listener unmap;
  struct wl_listener commit;
  struct wl_listener destroy;
  struct wl_listener request_move;
  struct wl_listener request_resize;
  struct wl_listener request_maximize;
  struct wl_listener request_fullscreen;
  struct wl_listener request_minimize;
  struct wl_listener set_app_id;   /* xdg app_id / X11 WM_CLASS */

#ifdef HAVE_XWAYLAND
  struct wlr_xwayland_surface* xsurface;
  /* Title bar for managed X11 windows */
  struct wlr_scene_tree* deco;
  struct wlr_scene_rect* deco_bar;
  struct wlr_scene_rect* deco_buttons[3];   /* minimize, maximize, close cells */
  struct wlr_scene_rect* deco_edges[4];     /* resize: top, bottom, l, r */
  struct wlr_scene_buffer* deco_title;
  char* deco_title_text;                    /* what deco_title shows */
  int deco_title_width;
  struct wlr_scene_buffer* deco_icons[3];   /* glyphs on the cells */
  uint64_t deco_icon_key[3];                /* glyph + colour drawn, or 0 */
  struct deco_part {
    struct owner owner;
    struct view* view;
    int kind;
  } deco_parts[5];
  struct wl_listener set_title;
  struct wl_listener map_request;
  struct wl_listener associate;
  struct wl_listener dissociate;
  struct wl_listener request_configure;
  struct wl_listener request_activate;
  struct wl_listener set_geometry;
#endif
};

struct popup {
  struct wlr_xdg_popup* xdg_popup;
  struct wvnc_compositor* c;
  struct wl_listener commit;
  struct wl_listener destroy;
};

struct layer {
  struct owner owner;
  struct wl_list link;
  struct wvnc_compositor* c;
  struct wlr_layer_surface_v1* layer_surface;
  struct wlr_scene_layer_surface_v1* scene;
  struct wl_listener map;
  struct wl_listener unmap;
  struct wl_listener commit;
  struct wl_listener destroy;
  struct wl_listener new_popup;
};

static uint32_t now_msec(void)
{
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

static void set_nonblock(int fd)
{
  int flags = fcntl(fd, F_GETFL);
  if (flags >= 0)
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

/* ------------------------------------------------------------------ */
/* Framebuffer                                                         */
/* ------------------------------------------------------------------ */

static void copy_rect(struct wvnc_compositor* c, const uint8_t* src,
                      size_t src_stride, int x1, int y1, int x2, int y2)
{
  if (x1 < 0) x1 = 0;
  if (y1 < 0) y1 = 0;
  if (x2 > c->width) x2 = c->width;
  if (y2 > c->height) y2 = c->height;
  if (x2 <= x1 || y2 <= y1)
    return;

  for (int y = y1; y < y2; y++) {
    memcpy(c->fb + (size_t)y * c->width + x1,
           src + (size_t)y * src_stride + (size_t)x1 * 4,
           (size_t)(x2 - x1) * 4);
  }

  if (c->cb.damage)
    c->cb.damage(c->cb.data, x1, y1, x2 - x1, y2 - y1);
}

static void capture_frame(struct wvnc_compositor* c,
                          struct wlr_output_state* state)
{
  void* data;
  uint32_t format;
  size_t stride;

  if (!(state->committed & WLR_OUTPUT_STATE_BUFFER) || !state->buffer)
    return;

  if (!wlr_buffer_begin_data_ptr_access(state->buffer,
                                        WLR_BUFFER_DATA_PTR_ACCESS_READ,
                                        &data, &format, &stride)) {
    wlr_log(WLR_ERROR, "Unable to access output buffer");
    return;
  }

  if (format != DRM_FORMAT_XRGB8888 && format != DRM_FORMAT_ARGB8888) {
    wlr_log(WLR_ERROR, "Unexpected output buffer format 0x%08x", format);
    wlr_buffer_end_data_ptr_access(state->buffer);
    return;
  }

  if (state->buffer->width != c->width ||
      state->buffer->height != c->height) {
    /* Stale buffer from before a resize */
    wlr_buffer_end_data_ptr_access(state->buffer);
    return;
  }

  if (c->full_damage || !(state->committed & WLR_OUTPUT_STATE_DAMAGE)) {
    copy_rect(c, data, stride, 0, 0, c->width, c->height);
    c->full_damage = false;
  } else {
    int n;
    const pixman_box32_t* rects =
      pixman_region32_rectangles((pixman_region32_t*)&state->damage, &n);
    for (int i = 0; i < n; i++)
      copy_rect(c, data, stride, rects[i].x1, rects[i].y1,
                rects[i].x2, rects[i].y2);
  }

  wlr_buffer_end_data_ptr_access(state->buffer);
}

static void handle_output_frame(struct wl_listener* listener, void* data)
{
  struct wvnc_compositor* c = wl_container_of(listener, c, output_frame);
  struct timespec now;

  (void)data;

  if (wlr_scene_output_needs_frame(c->scene_output) || c->full_damage) {
    struct wlr_output_state state;
    wlr_output_state_init(&state);
    if (wlr_scene_output_build_state(c->scene_output, &state, NULL)) {
      capture_frame(c, &state);
      if (!wlr_output_commit_state(c->output, &state))
        wlr_log(WLR_ERROR, "Output commit failed");
    }
    wlr_output_state_finish(&state);
  }

  clock_gettime(CLOCK_MONOTONIC, &now);
  wlr_scene_output_send_frame_done(c->scene_output, &now);

  /* The scene only sends frame events to visible surfaces. A nested
   * compositor output that is covered (several outputs on one screen)
   * would then stop rendering, and not even draw itself at a new size
   * once it is uncovered. Its outputs are always "on", so keep them
   * running. */
  {
    struct view* v;
    wl_list_for_each(v, &c->views, link) {
      if (v->nested && v->mapped)
        wlr_surface_send_frame_done(v->xdg->base->surface, &now);
    }
  }
}

static void handle_output_destroy(struct wl_listener* listener, void* data)
{
  struct wvnc_compositor* c = wl_container_of(listener, c, output_destroy);
  (void)data;
  wl_list_remove(&c->output_frame.link);
  wl_list_remove(&c->output_destroy.link);
  c->output = NULL;
  c->scene_output = NULL;
}

static void update_output_manager(struct wvnc_compositor* c);

static void handle_new_output(struct wl_listener* listener, void* data)
{
  struct wvnc_compositor* c = wl_container_of(listener, c, new_output);
  struct wlr_output* output = data;
  struct wlr_output_state state;

  if (c->output) {
    wlr_log(WLR_ERROR, "Ignoring unexpected extra output");
    return;
  }

  wlr_output_init_render(output, c->allocator, c->renderer);

  wlr_output_state_init(&state);
  wlr_output_state_set_enabled(&state, true);
  wlr_output_state_set_render_format(&state, DRM_FORMAT_XRGB8888);
  wlr_output_state_set_custom_mode(&state, c->width, c->height, 60000);
  if (!wlr_output_commit_state(output, &state))
    wlr_log(WLR_ERROR, "Unable to enable output");
  wlr_output_state_finish(&state);

  c->output = output;
  c->output_frame.notify = handle_output_frame;
  wl_signal_add(&output->events.frame, &c->output_frame);
  c->output_destroy.notify = handle_output_destroy;
  wl_signal_add(&output->events.destroy, &c->output_destroy);

  struct wlr_output_layout_output* lo =
    wlr_output_layout_add(c->output_layout, output, 0, 0);
  c->scene_output = wlr_scene_output_create(c->scene, output);
  wlr_scene_output_layout_add_output(c->scene_layout, lo, c->scene_output);
  c->full_damage = true;
  update_output_manager(c);
}

uint8_t* wvnc_framebuffer(struct wvnc_compositor* c, int* width,
                          int* height, int* stride_pixels)
{
  *width = c->width;
  *height = c->height;
  *stride_pixels = c->width;
  return (uint8_t*)c->fb;
}

/* ------------------------------------------------------------------ */
/* Window management                                                   */
/* ------------------------------------------------------------------ */

static void arrange_layers(struct wvnc_compositor* c);

static struct wlr_surface* view_surface(struct view* v)
{
  if (v->owner.type == OWNER_XDG)
    return v->xdg->base->surface;
#ifdef HAVE_XWAYLAND
  return v->xsurface->surface;
#else
  return NULL;
#endif
}

static void view_get_box(struct view* v, struct wlr_box* box)
{
  if (v->owner.type == OWNER_XDG) {
    struct wlr_box* geo = &v->xdg->base->geometry;
    box->x = v->scene_tree->node.x + geo->x;
    box->y = v->scene_tree->node.y + geo->y;
    box->width = geo->width;
    box->height = geo->height;
  }
#ifdef HAVE_XWAYLAND
  else {
    box->x = v->xsurface->x;
    box->y = v->xsurface->y;
    box->width = v->xsurface->width;
    box->height = v->xsurface->height;
  }
#endif
}

/* Moves and resizes a view so that its window geometry is "box" */
static void deco_update(struct view* v);

static void view_set_box(struct view* v, const struct wlr_box* box)
{
  if (v->owner.type == OWNER_XDG) {
    struct wlr_box* geo = &v->xdg->base->geometry;
    wlr_scene_node_set_position(&v->scene_tree->node,
                                box->x - geo->x, box->y - geo->y);
    wlr_xdg_toplevel_set_size(v->xdg, box->width, box->height);
  }
#ifdef HAVE_XWAYLAND
  else {
    if (v->scene_tree)
      wlr_scene_node_set_position(&v->scene_tree->node, box->x, box->y);
    wlr_xwayland_surface_configure(v->xsurface, box->x, box->y,
                                   box->width, box->height);
    /* The title bar and the resize borders follow the new size */
    deco_update(v);
  }
#endif
}

static void view_set_activated(struct view* v, bool activated)
{
  v->activated = activated;
  if (v->owner.type == OWNER_XDG)
    wlr_xdg_toplevel_set_activated(v->xdg, activated);
#ifdef HAVE_XWAYLAND
  else {
    wlr_xwayland_surface_activate(v->xsurface, activated);
    deco_update(v);
  }
#endif
}

static struct view* focused_view(struct wvnc_compositor* c)
{
  struct wlr_surface* focused = c->seat->keyboard_state.focused_surface;
  struct view* v;
  if (!focused)
    return NULL;
  wl_list_for_each(v, &c->views, link) {
    if (v->mapped && view_surface(v) == focused)
      return v;
  }
  return NULL;
}

static void focus_surface(struct wvnc_compositor* c,
                          struct wlr_surface* surface)
{
  struct wlr_keyboard* kb = &c->keyboard;
  if (c->seat->keyboard_state.focused_surface == surface)
    return;
  if (surface)
    wlr_seat_keyboard_notify_enter(c->seat, surface, kb->keycodes,
                                   kb->num_keycodes, &kb->modifiers);
  else
    wlr_seat_keyboard_notify_clear_focus(c->seat);
}

static void focus_view(struct view* v)
{
  struct wvnc_compositor* c = v->c;
  struct view* prev = focused_view(c);

  if (v->scene_tree) {
    if (v->nested)
      wlr_scene_node_lower_to_bottom(&v->scene_tree->node);
    else
      wlr_scene_node_raise_to_top(&v->scene_tree->node);
  }
  wl_list_remove(&v->link);
  wl_list_insert(&c->views, &v->link);
#ifdef HAVE_XWAYLAND
  if (v->owner.type == OWNER_XWAYLAND)
    wlr_xwayland_surface_restack(v->xsurface, NULL, XCB_STACK_MODE_ABOVE);
#endif

  if (prev == v)
    return;
  if (prev)
    view_set_activated(prev, false);
  view_set_activated(v, true);
  focus_surface(c, view_surface(v));
}

static void focus_next_view(struct wvnc_compositor* c)
{
  struct view* v;
  wl_list_for_each(v, &c->views, link) {
    if (v->mapped && !v->minimized) {
#ifdef HAVE_XWAYLAND
      if (v->owner.type == OWNER_XWAYLAND && v->xsurface->override_redirect)
        continue;
#endif
      focus_view(v);
      return;
    }
  }
  focus_surface(c, NULL);
}

/* Screens (from the viewer's layout) ------------------------------- */

/* The screen containing a point, or the nearest one */
static struct wlr_box screen_at(struct wvnc_compositor* c, int x, int y)
{
  struct wlr_box full = { 0, 0, c->width, c->height };
  int best = -1;
  double best_d = 0;

  for (int i = 0; i < c->num_screens; i++) {
    struct wlr_box* b = &c->screens[i];
    double cx, cy, d;
    if (wlr_box_contains_point(b, x, y))
      return *b;
    wlr_box_closest_point(b, x, y, &cx, &cy);
    d = (cx - x) * (cx - x) + (cy - y) * (cy - y);
    if (best < 0 || d < best_d) {
      best = i;
      best_d = d;
    }
  }
  return best < 0 ? full : c->screens[best];
}

/* The part of a screen not taken by panels (layer-shell exclusive
 * zones) */
static struct wlr_box usable_in(struct wvnc_compositor* c,
                                struct wlr_box screen)
{
  struct wlr_box r;
  if (!wlr_box_intersection(&r, &screen, &c->usable_area))
    return screen;
  return r;
}

static struct wlr_box view_screen(struct view* v)
{
  struct wlr_box box;
  view_get_box(v, &box);
  return screen_at(v->c, box.x + box.width / 2, box.y + box.height / 2);
}

/* wlr-output-management ---------------------------------------------- */

static void update_output_manager(struct wvnc_compositor* c)
{
  struct wlr_output_configuration_v1* config;
  if (!c->output_manager || !c->output)
    return;
  config = wlr_output_configuration_v1_create();
  if (!config)
    return;
  wlr_output_configuration_head_v1_create(config, c->output);
  wlr_output_manager_v1_set_configuration(c->output_manager, config);
}

/* Only the size of our one output can be changed */
static bool check_output_config(struct wvnc_compositor* c,
                                struct wlr_output_configuration_v1* config,
                                int* width, int* height)
{
  struct wlr_output_configuration_head_v1* head;
  bool found = false;

  wl_list_for_each(head, &config->heads, link) {
    struct wlr_output_head_v1_state* st = &head->state;
    if (st->output != c->output || !st->enabled)
      return false;
    if (st->x != 0 || st->y != 0 || st->scale != 1.0f ||
        st->transform != WL_OUTPUT_TRANSFORM_NORMAL)
      return false;
    if (st->mode) {
      *width = st->mode->width;
      *height = st->mode->height;
    } else {
      *width = st->custom_mode.width;
      *height = st->custom_mode.height;
    }
    if (*width < 1 || *height < 1 || *width > 16384 || *height > 16384)
      return false;
    found = true;
  }
  return found;
}

static void handle_output_manager_test(struct wl_listener* listener,
                                       void* data)
{
  struct wvnc_compositor* c =
    wl_container_of(listener, c, output_manager_test);
  struct wlr_output_configuration_v1* config = data;
  int w, h;
  if (check_output_config(c, config, &w, &h))
    wlr_output_configuration_v1_send_succeeded(config);
  else
    wlr_output_configuration_v1_send_failed(config);
  wlr_output_configuration_v1_destroy(config);
}

static void handle_output_manager_apply(struct wl_listener* listener,
                                        void* data)
{
  struct wvnc_compositor* c =
    wl_container_of(listener, c, output_manager_apply);
  struct wlr_output_configuration_v1* config = data;
  int w, h;
  bool ok = check_output_config(c, config, &w, &h);
  if (ok && (w != c->width || h != c->height))
    ok = c->cb.output_resize && c->cb.output_resize(c->cb.data, w, h);
  if (ok)
    wlr_output_configuration_v1_send_succeeded(config);
  else
    wlr_output_configuration_v1_send_failed(config);
  wlr_output_configuration_v1_destroy(config);
  update_output_manager(c);
}

static void view_place(struct view* v)
{
  struct wvnc_compositor* c = v->c;
  struct wlr_box box;
  /* New windows open on the screen with the pointer */
  struct wlr_box screen_area = usable_in(c, screen_at(c, c->px, c->py));
  struct wlr_box* area = &screen_area;

  view_get_box(v, &box);
  box.x = area->x + (area->width - box.width) / 2 + c->cascade;
  box.y = area->y + (area->height - box.height) / 2 + c->cascade;
  c->cascade = (c->cascade + 32) % 160;
  if (box.x < area->x) box.x = area->x;
  if (box.y < area->y) box.y = area->y;

  if (v->owner.type == OWNER_XDG) {
    struct wlr_box* geo = &v->xdg->base->geometry;
    wlr_scene_node_set_position(&v->scene_tree->node,
                                box.x - geo->x, box.y - geo->y);
  }
}

/* X11 title bars ----------------------------------------------------- */

#define DECO_HEIGHT 22
#define DECO_BORDER 5     /* invisible resize border around the frame */
#define DECO_CORNER 16    /* corner zone along each edge */
#define DECO_BTN_W 32     /* button cell width (full bar height) */
#define DECO_GLYPH 10     /* glyph box, 1px strokes */
#define DECO_MIN_W (3 * DECO_BTN_W + 24)
enum { DECO_BAR, DECO_MINIMIZE, DECO_MAXIMIZE, DECO_CLOSE, DECO_EDGE };

#include "deco_font.h"

static bool view_has_deco(struct view* v)
{
#ifdef HAVE_XWAYLAND
  struct wlr_xwayland_surface* xs = v->xsurface;
  if (v->owner.type != OWNER_XWAYLAND || !v->c->x11_title_bars)
    return false;
  if (!xs || xs->override_redirect || v->fullscreen || v->nested)
    return false;
  /* _MOTIF_WM_HINTS asking for no title (e.g. own decorations) */
  if (xs->decorations & WLR_XWAYLAND_SURFACE_DECORATIONS_NO_TITLE)
    return false;
  /* Windows with a parent (dialogs etc.) get one too, like most WMs */
  return true;
#else
  (void)v;
  return false;
#endif
}

/* A wlr_buffer over malloc'ed premultiplied ARGB8888 pixels (title
 * text, button glyphs, the window switcher) */
struct text_buffer {
  struct wlr_buffer base;
  uint32_t* data;
  size_t stride;
};

static void text_buffer_destroy(struct wlr_buffer* wb)
{
  struct text_buffer* tb = wl_container_of(wb, tb, base);
  wlr_buffer_finish(wb);
  free(tb->data);
  free(tb);
}

static bool text_buffer_begin(struct wlr_buffer* wb, uint32_t flags,
                              void** data, uint32_t* format, size_t* stride)
{
  struct text_buffer* tb = wl_container_of(wb, tb, base);
  if (flags & WLR_BUFFER_DATA_PTR_ACCESS_WRITE)
    return false;
  *data = tb->data;
  *format = DRM_FORMAT_ARGB8888;
  *stride = tb->stride;
  return true;
}

static void text_buffer_end(struct wlr_buffer* wb)
{
  (void)wb;
}

static const struct wlr_buffer_impl text_buffer_impl = {
  .destroy = text_buffer_destroy,
  .begin_data_ptr_access = text_buffer_begin,
  .end_data_ptr_access = text_buffer_end,
};

/* A transparent w x h pixel buffer to draw into; NULL on failure */
static struct text_buffer* pixel_buffer_create(int w, int h)
{
  struct text_buffer* tb = calloc(1, sizeof(*tb));
  if (!tb)
    return NULL;
  tb->stride = (size_t)w * 4;
  tb->data = calloc((size_t)w * h, 4);
  if (!tb->data) {
    free(tb);
    return NULL;
  }
  wlr_buffer_init(&tb->base, &text_buffer_impl, w, h);
  return tb;
}

#ifdef HAVE_XWAYLAND
/* Next code point of UTF-8 text; invalid bytes come out as '?' */
static uint32_t utf8_next(const unsigned char** p)
{
  const unsigned char* s = *p;
  uint32_t cp;
  int n;
  if (s[0] < 0x80) { *p = s + 1; return s[0]; }
  if ((s[0] & 0xe0) == 0xc0) { cp = s[0] & 0x1f; n = 1; }
  else if ((s[0] & 0xf0) == 0xe0) { cp = s[0] & 0x0f; n = 2; }
  else if ((s[0] & 0xf8) == 0xf0) { cp = s[0] & 0x07; n = 3; }
  else { *p = s + 1; return '?'; }
  for (int i = 1; i <= n; i++) {
    if ((s[i] & 0xc0) != 0x80) { *p = s + i; return '?'; }
    cp = (cp << 6) | (s[i] & 0x3f);
  }
  *p = s + n + 1;
  return cp;
}

/* Title text in white, cut with "..." to max_w pixels; NULL if empty */
static struct wlr_buffer* render_title(const char* title, int max_w)
{
  uint32_t cps[512];
  size_t n = 0, fit;
  const unsigned char* p = (const unsigned char*)title;

  while (p && *p && n < sizeof(cps) / sizeof(*cps)) {
    uint32_t cp = utf8_next(&p);
    if (cp < 0x20 || (cp >= 0x7f && cp < 0xa0))
      cp = ' ';
    cps[n++] = cp > 0xff ? '?' : cp;   /* the font covers Latin-1 */
  }
  fit = max_w / DECO_FONT_W;
  if (n == 0 || fit == 0)
    return NULL;
  if (n > fit) {
    n = fit;
    for (size_t i = n >= 3 ? n - 3 : 0; i < n; i++)
      cps[i] = '.';
  }

  int w = (int)n * DECO_FONT_W, h = DECO_FONT_H;
  struct text_buffer* tb = pixel_buffer_create(w, h);
  if (!tb)
    return NULL;
  for (size_t i = 0; i < n; i++)
    for (int y = 0; y < h; y++) {
      unsigned char bits = deco_font[cps[i]][y];
      for (int x = 0; x < DECO_FONT_W; x++)
        if (bits & (0x80 >> x))
          tb->data[(size_t)y * w + i * DECO_FONT_W + x] = 0xffffffff;
    }
  return &tb->base;
}

static void deco_set_title(struct view* v, int w)
{
  const char* title = v->xsurface->title ? v->xsurface->title : "";
  /* left of the buttons, with some padding */
  int max_w = w - 3 * DECO_BTN_W - 12;

  if (v->deco_title_text && strcmp(v->deco_title_text, title) == 0 &&
      v->deco_title_width == max_w)
    return;
  free(v->deco_title_text);
  v->deco_title_text = strdup(title);
  v->deco_title_width = max_w;

  struct wlr_buffer* buf = render_title(title, max_w);
  wlr_scene_buffer_set_buffer(v->deco_title, buf);
  wlr_scene_node_set_enabled(&v->deco_title->node, buf != NULL);
  if (buf)
    wlr_buffer_drop(buf);   /* the scene buffer keeps a lock */
}

/* Windows 11 style caption glyphs: 1px strokes in a 10x10 box */
enum { GLYPH_MINIMIZE = 1, GLYPH_MAXIMIZE, GLYPH_RESTORE, GLYPH_CLOSE };

static void glyph_plot(uint32_t* px, int x, int y, uint32_t colour)
{
  if (x >= 0 && x < DECO_GLYPH && y >= 0 && y < DECO_GLYPH)
    px[y * DECO_GLYPH + x] = colour;
}

static void glyph_box(uint32_t* px, int x0, int y0, int x1, int y1,
                      uint32_t colour)
{
  for (int x = x0; x <= x1; x++) {
    glyph_plot(px, x, y0, colour);
    glyph_plot(px, x, y1, colour);
  }
  for (int y = y0; y <= y1; y++) {
    glyph_plot(px, x0, y, colour);
    glyph_plot(px, x1, y, colour);
  }
}

static struct wlr_buffer* render_glyph(int glyph, uint32_t colour)
{
  struct text_buffer* tb = pixel_buffer_create(DECO_GLYPH, DECO_GLYPH);
  uint32_t* px;
  const int L = DECO_GLYPH - 1;
  if (!tb)
    return NULL;
  px = tb->data;
  switch (glyph) {
  case GLYPH_MINIMIZE:     /* a dash across the middle */
    for (int x = 0; x <= L; x++)
      glyph_plot(px, x, DECO_GLYPH / 2, colour);
    break;
  case GLYPH_MAXIMIZE:     /* a square */
    glyph_box(px, 0, 0, L, L, colour);
    break;
  case GLYPH_RESTORE:      /* a square with another one behind it */
    glyph_box(px, 0, 2, L - 2, L, colour);
    for (int x = 2; x <= L; x++)
      glyph_plot(px, x, 0, colour);
    for (int y = 0; y <= L - 2; y++)
      glyph_plot(px, L, y, colour);
    glyph_plot(px, 2, 1, colour);
    glyph_plot(px, L - 1, L - 2, colour);
    break;
  case GLYPH_CLOSE:        /* both diagonals */
    for (int i = 0; i <= L; i++) {
      glyph_plot(px, i, i, colour);
      glyph_plot(px, L - i, i, colour);
    }
    break;
  }
  return &tb->base;
}

static void deco_set_icon(struct view* v, int i, int glyph, uint32_t colour)
{
  uint64_t key = ((uint64_t)glyph << 32) | colour;
  struct wlr_scene_buffer* node = v->deco_icons[i];
  if (v->deco_icon_key[i] == key)
    return;
  v->deco_icon_key[i] = key;
  struct wlr_buffer* buf = render_glyph(glyph, colour);
  wlr_scene_buffer_set_buffer(node, buf);
  wlr_scene_node_set_enabled(&node->node, buf != NULL);
  if (buf)
    wlr_buffer_drop(buf);   /* the scene buffer keeps a lock */
}

#endif

static void deco_update(struct view* v)
{
#ifdef HAVE_XWAYLAND
  /* Bar colours (focused, unfocused) and the lighter hover cells */
  static const float bar_active[4] = { 0.20f, 0.33f, 0.53f, 1.0f };
  static const float bar_inactive[4] = { 0.45f, 0.47f, 0.50f, 1.0f };
  static const float hover_active[4] = { 0.345f, 0.451f, 0.616f, 1.0f };
  static const float hover_inactive[4] = { 0.549f, 0.565f, 0.592f, 1.0f };
  static const float close_hover[4] = { 0xc4 / 255.0f, 0x2b / 255.0f,
                                        0x1c / 255.0f, 1.0f };
  static const float invisible[4] = { 0, 0, 0, 0 };
  const uint32_t glyph_active = 0xfff2f2f2, glyph_inactive = 0xffd6d8dc;
  bool show = v->scene_tree && view_has_deco(v);
  int w, h;

  if (!show) {
    if (v->deco)
      wlr_scene_node_set_enabled(&v->deco->node, false);
    return;
  }

  if (!v->deco) {
    v->deco = wlr_scene_tree_create(v->scene_tree);
    v->deco_bar = wlr_scene_rect_create(v->deco, 1, DECO_HEIGHT,
                                        bar_active);
    for (int i = 0; i < 3; i++)
      v->deco_buttons[i] = wlr_scene_rect_create(v->deco, DECO_BTN_W,
                                                 DECO_HEIGHT, invisible);
    v->deco_title = wlr_scene_buffer_create(v->deco, NULL);
    for (int i = 0; i < 4; i++)
      v->deco_edges[i] = wlr_scene_rect_create(v->deco, 1, 1, invisible);
    /* Glyphs sit above the cells; node.data maps clicks to the button */
    for (int i = 0; i < 3; i++) {
      v->deco_icons[i] = wlr_scene_buffer_create(v->deco, NULL);
      v->deco_icon_key[i] = 0;
    }
    for (int i = 0; i < 5; i++) {
      v->deco_parts[i].owner.type = OWNER_DECO;
      v->deco_parts[i].view = v;
      v->deco_parts[i].kind = i;
    }
    v->deco_bar->node.data = &v->deco_parts[DECO_BAR];
    for (int i = 0; i < 3; i++) {
      v->deco_buttons[i]->node.data = &v->deco_parts[DECO_MINIMIZE + i];
      v->deco_icons[i]->node.data = &v->deco_parts[DECO_MINIMIZE + i];
    }
    /* the title text belongs to the bar (drag, double click) */
    v->deco_title->node.data = &v->deco_parts[DECO_BAR];
    for (int i = 0; i < 4; i++)
      v->deco_edges[i]->node.data = &v->deco_parts[DECO_EDGE];
  }

  w = v->xsurface->width > DECO_MIN_W ? v->xsurface->width : DECO_MIN_W;
  h = DECO_HEIGHT + v->xsurface->height;   /* frame height */
  wlr_scene_node_set_enabled(&v->deco->node, true);
  wlr_scene_node_set_position(&v->deco->node, 0, -DECO_HEIGHT);
  wlr_scene_rect_set_size(v->deco_bar, w, DECO_HEIGHT);
  wlr_scene_rect_set_color(v->deco_bar,
                           v->activated ? bar_active : bar_inactive);

  /* minimize, maximize/restore, close from left to right */
  int hover = v->c->hover_view == v ? v->c->hover_kind : -1;
  for (int i = 0; i < 3; i++) {
    int kind = DECO_MINIMIZE + i;
    int cx = w - (3 - i) * DECO_BTN_W;
    int glyph = kind == DECO_MINIMIZE ? GLYPH_MINIMIZE :
                kind == DECO_CLOSE ? GLYPH_CLOSE :
                v->maximized ? GLYPH_RESTORE : GLYPH_MAXIMIZE;
    uint32_t colour = v->activated ? glyph_active : glyph_inactive;
    const float* cell = invisible;
    if (hover == kind) {
      if (kind == DECO_CLOSE) {
        cell = close_hover;
        colour = 0xffffffff;
      } else {
        cell = v->activated ? hover_active : hover_inactive;
      }
    }
    wlr_scene_rect_set_color(v->deco_buttons[i], cell);
    wlr_scene_node_set_position(&v->deco_buttons[i]->node, cx, 0);
    wlr_scene_node_set_position(&v->deco_icons[i]->node,
                                cx + (DECO_BTN_W - DECO_GLYPH) / 2,
                                (DECO_HEIGHT - DECO_GLYPH) / 2);
    deco_set_icon(v, i, glyph, colour);
  }
  wlr_scene_node_set_position(&v->deco_title->node, 6,
                              (DECO_HEIGHT - DECO_FONT_H) / 2);
  deco_set_title(v, w);

  /* Resize borders, no resizing when maximized (like moving) */
  bool resizable = !v->maximized;
  for (int i = 0; i < 4; i++)
    wlr_scene_node_set_enabled(&v->deco_edges[i]->node, resizable);
  wlr_scene_rect_set_size(v->deco_edges[0], w + 2 * DECO_BORDER, DECO_BORDER);
  wlr_scene_node_set_position(&v->deco_edges[0]->node, -DECO_BORDER,
                              -DECO_BORDER);
  wlr_scene_rect_set_size(v->deco_edges[1], w + 2 * DECO_BORDER, DECO_BORDER);
  wlr_scene_node_set_position(&v->deco_edges[1]->node, -DECO_BORDER, h);
  wlr_scene_rect_set_size(v->deco_edges[2], DECO_BORDER, h);
  wlr_scene_node_set_position(&v->deco_edges[2]->node, -DECO_BORDER, 0);
  wlr_scene_rect_set_size(v->deco_edges[3], DECO_BORDER, h);
  wlr_scene_node_set_position(&v->deco_edges[3]->node, w, 0);
#else
  (void)v;
#endif
}

/* The title bar button under the pointer changed */
static void set_deco_hover(struct wvnc_compositor* c, struct view* v,
                           int kind)
{
  struct view* old = c->hover_view;
  if (old == v && c->hover_kind == kind)
    return;
  c->hover_view = v;
  c->hover_kind = kind;
  if (old && old != v)
    deco_update(old);
  if (v)
    deco_update(v);
}

#ifdef HAVE_XWAYLAND
/* Which edges a press at (px, py) on a resize border grabs */
static uint32_t deco_edges_at(struct view* v, int px, int py)
{
  struct wlr_xwayland_surface* xs = v->xsurface;
  int left = xs->x, right = xs->x + xs->width;
  int top = xs->y - DECO_HEIGHT, bottom = xs->y + xs->height;
  uint32_t edges = 0;

  /* The borders lie outside the frame */
  if (py < top || py >= bottom) {
    edges = py < top ? WLR_EDGE_TOP : WLR_EDGE_BOTTOM;
    if (px < left + DECO_CORNER)
      edges |= WLR_EDGE_LEFT;
    else if (px >= right - DECO_CORNER)
      edges |= WLR_EDGE_RIGHT;
  } else {
    edges = px < left ? WLR_EDGE_LEFT : WLR_EDGE_RIGHT;
    if (py < top + DECO_CORNER)
      edges |= WLR_EDGE_TOP;
    else if (py >= bottom - DECO_CORNER)
      edges |= WLR_EDGE_BOTTOM;
  }
  return edges;
}
#endif

static const char* edge_cursor(uint32_t edges)
{
  switch (edges) {
  case WLR_EDGE_TOP: return "top_side";
  case WLR_EDGE_BOTTOM: return "bottom_side";
  case WLR_EDGE_LEFT: return "left_side";
  case WLR_EDGE_RIGHT: return "right_side";
  case WLR_EDGE_TOP | WLR_EDGE_LEFT: return "top_left_corner";
  case WLR_EDGE_TOP | WLR_EDGE_RIGHT: return "top_right_corner";
  case WLR_EDGE_BOTTOM | WLR_EDGE_LEFT: return "bottom_left_corner";
  case WLR_EDGE_BOTTOM | WLR_EDGE_RIGHT: return "bottom_right_corner";
  }
  return "default";
}

static void view_maximize(struct view* v, bool maximize, bool fullscreen)
{
  struct wvnc_compositor* c = v->c;
  bool was = v->maximized || v->fullscreen;
  /* Maximize/fullscreen on the screen the window is on */
  struct wlr_box full = was && v->saved.width > 0 ?
    screen_at(c, v->saved.x + v->saved.width / 2,
              v->saved.y + v->saved.height / 2) : view_screen(v);
  struct wlr_box area = usable_in(c, full);

  if (maximize || fullscreen) {
    if (!was)
      view_get_box(v, &v->saved);
    v->maximized = maximize && !fullscreen;
    v->fullscreen = fullscreen;
  } else {
    v->maximized = false;
    v->fullscreen = false;
  }

  if (v->owner.type == OWNER_XDG) {
    wlr_xdg_toplevel_set_maximized(v->xdg, v->maximized);
    wlr_xdg_toplevel_set_fullscreen(v->xdg, v->fullscreen);
  }
#ifdef HAVE_XWAYLAND
  else {
    wlr_xwayland_surface_set_maximized(v->xsurface, v->maximized,
                                       v->maximized);
    wlr_xwayland_surface_set_fullscreen(v->xsurface, v->fullscreen);
  }
#endif

  if (v->fullscreen) {
    view_set_box(v, &full);
  } else if (v->maximized) {
    /* Leave room for the title bar */
    if (view_has_deco(v) && area.height > 2 * DECO_HEIGHT) {
      area.y += DECO_HEIGHT;
      area.height -= DECO_HEIGHT;
    }
    view_set_box(v, &area);
  }
  else if (was && v->saved.width > 0 && v->saved.height > 0)
    view_set_box(v, &v->saved);
  else if (was && v->owner.type == OWNER_XDG)
    wlr_xdg_toplevel_set_size(v->xdg, 0, 0);
  deco_update(v);
}

/* Minimize ----------------------------------------------------------- */

static void clear_implicit_grab(struct wvnc_compositor* c);

static void switcher_view_gone(struct view* v);

static bool view_is_child(struct view* v)
{
  if (v->owner.type == OWNER_XDG)
    return v->xdg->parent != NULL;
#ifdef HAVE_XWAYLAND
  return v->xsurface->parent != NULL;
#else
  return false;
#endif
}

/* Hides a window (or shows it again). The window keeps its place in
 * the focus (MRU) list, at the back while minimized, and comes back
 * through the window switcher, a click on nothing else being
 * possible: there is no task bar. */
static void view_set_minimized(struct view* v, bool minimized)
{
  struct wvnc_compositor* c = v->c;

  if (v->nested || !v->mapped || v->minimized == minimized) {
#ifdef HAVE_XWAYLAND
    /* keep the X11 iconic state truthful even when nothing changes */
    if (v->owner.type == OWNER_XWAYLAND && v->mapped &&
        !v->xsurface->override_redirect)
      wlr_xwayland_surface_set_minimized(v->xsurface, v->minimized);
#endif
    return;
  }
#ifdef HAVE_XWAYLAND
  if (v->owner.type == OWNER_XWAYLAND && v->xsurface->override_redirect)
    return;
#endif

  v->minimized = minimized;
  if (v->scene_tree)
    wlr_scene_node_set_enabled(&v->scene_tree->node, !minimized);
#ifdef HAVE_XWAYLAND
  if (v->owner.type == OWNER_XWAYLAND)
    wlr_xwayland_surface_set_minimized(v->xsurface, minimized);
#endif

  if (minimized) {
    bool had_focus = focused_view(c) == v;
    if (c->grab_view == v) {
      c->grab_mode = GRAB_NONE;
      c->grab_view = NULL;
    }
    if (c->hover_view == v)
      c->hover_view = NULL;
    if (c->implicit_grab == view_surface(v))
      clear_implicit_grab(c);
    /* to the back of the focus order */
    wl_list_remove(&v->link);
    wl_list_insert(c->views.prev, &v->link);
    if (had_focus) {
      view_set_activated(v, false);
      focus_next_view(c);
    }
  } else {
    focus_view(v);
  }
}

/* Window switcher (Alt+Tab) ------------------------------------------ */

/*
 * Hold Alt and press Tab: a popup in the middle of the screen lists the
 * windows (most recently used first, minimized ones too), Tab and
 * Shift+Tab move the selection, releasing Alt raises, focuses and
 * un-minimizes the selected window, Esc closes the popup without
 * changing anything. While it is open the keyboard belongs to the
 * compositor (no client has keyboard focus). If a nested compositor
 * has the keyboard, Alt+Tab is its business and is passed on.
 */

#define SW_ICON 32
#define SW_PAD 10
#define SW_ROW_H 44
#define SW_RADIUS 10
#define SW_ROW_RADIUS 6
#define SW_TEXT_PX 15
#define SW_MIN_W 360
#define SW_MAX_W 720
static const uint32_t sw_bg = 0xff2b2b2b;           /* popup */
static const uint32_t sw_sel = 0xff4a4d52;          /* selected row */
static const uint32_t sw_border = 0xff5a5d63;       /* popup outline */
static const uint32_t sw_text = 0xffffff;
static const uint32_t sw_text_min = 0xa8a8a8;       /* minimized */

static const char* view_title(struct view* v)
{
  const char* t = NULL;
  if (v->owner.type == OWNER_XDG) {
    t = v->xdg->title;
    if (!t || !*t)
      t = v->xdg->app_id;
  }
#ifdef HAVE_XWAYLAND
  else {
    t = v->xsurface->title;
    if (!t || !*t)
      t = v->xsurface->class;
  }
#endif
  return t && *t ? t : "(untitled)";
}

#ifdef HAVE_XWAYLAND
/* Our own X connection to Xwayland, for reading window properties
 * without disturbing wlroots' window manager connection. Uses the
 * same MIT-MAGIC-COOKIE-1 as the session (TIGERVNC_XWAYLAND_AUTH). */
static bool read_cookie(const char* path, const char* display,
                        char* cookie, int* cookie_len)
{
  unsigned char buf[4096];
  const char* num = display[0] == ':' ? display + 1 : display;
  size_t n, off = 0;
  FILE* f = fopen(path, "rb");
  if (!f)
    return false;
  n = fread(buf, 1, sizeof(buf), f);
  fclose(f);
  while (off + 2 <= n) {
    const unsigned char* field[4];
    size_t len[4];
    off += 2;   /* family */
    for (int i = 0; i < 4; i++) {
      if (off + 2 > n)
        return false;
      len[i] = ((size_t)buf[off] << 8) | buf[off + 1];
      off += 2;
      if (off + len[i] > n)
        return false;
      field[i] = buf + off;
      off += len[i];
    }
    if (len[1] == strlen(num) && memcmp(field[1], num, len[1]) == 0 &&
        len[2] == 18 && memcmp(field[2], "MIT-MAGIC-COOKIE-1", 18) == 0 &&
        len[3] <= 64) {
      memcpy(cookie, field[3], len[3]);
      *cookie_len = (int)len[3];
      return true;
    }
  }
  return false;
}

static xcb_connection_t* icon_connection(struct wvnc_compositor* c)
{
  const char* display;
  const char* auth = getenv("TIGERVNC_XWAYLAND_AUTH");
  xcb_connection_t* conn;

  if (c->icon_xcb || c->icon_xcb_failed || !c->xwayland)
    return c->icon_xcb;
  display = c->xwayland->display_name;
  if (!display || !*display)
    return NULL;
  if (auth && *auth) {
    char cookie[64];
    char name[] = "MIT-MAGIC-COOKIE-1";
    xcb_auth_info_t info = { 18, name, 0, cookie };
    if (!read_cookie(auth, display, cookie, &info.datalen)) {
      c->icon_xcb_failed = true;
      return NULL;
    }
    conn = xcb_connect_to_display_with_auth_info(display, &info, NULL);
  } else {
    conn = xcb_connect(display, NULL);
  }
  if (!conn || xcb_connection_has_error(conn)) {
    if (conn)
      xcb_disconnect(conn);
    c->icon_xcb_failed = true;
    wvnc_log_info("Window switcher: cannot connect to Xwayland for X11 "
                  "window icons");
    return NULL;
  }
  c->icon_xcb = conn;
  return conn;
}

/* Waits (at most until deadline_ms) for the reply to a request on our
 * icon connection. Never blocks for good: Xwayland is our client, so
 * it may itself be waiting for us. */
static void* icon_wait_reply(xcb_connection_t* conn, unsigned int seq,
                             uint32_t deadline_ms)
{
  for (;;) {
    void* reply = NULL;
    xcb_generic_error_t* err = NULL;
    xcb_generic_event_t* ev;
    struct pollfd pfd = { xcb_get_file_descriptor(conn), POLLIN, 0 };
    if (xcb_poll_for_reply(conn, seq, &reply, &err)) {
      free(err);
      return reply;
    }
    while ((ev = xcb_poll_for_event(conn)))   /* also reads the socket */
      free(ev);
    if (xcb_poll_for_reply(conn, seq, &reply, &err)) {
      free(err);
      return reply;
    }
    if (xcb_connection_has_error(conn))
      return NULL;
    int left = (int)(deadline_ms - now_msec());
    if (left <= 0 || poll(&pfd, 1, left) <= 0)
      return NULL;
  }
}

static struct wvnc_icon* net_wm_icon(struct wvnc_compositor* c,
                                     struct view* v)
{
  xcb_connection_t* conn = icon_connection(c);
  uint32_t deadline = now_msec() + 250;
  struct wvnc_icon* icon = NULL;

  if (!conn)
    return NULL;
  if (c->net_wm_icon == XCB_ATOM_NONE) {
    xcb_intern_atom_cookie_t ck =
      xcb_intern_atom(conn, 0, strlen("_NET_WM_ICON"), "_NET_WM_ICON");
    xcb_flush(conn);
    xcb_intern_atom_reply_t* r = icon_wait_reply(conn, ck.sequence, deadline);
    if (!r)
      return NULL;
    c->net_wm_icon = r->atom;
    free(r);
  }
  /* up to 1 MiB of icon data */
  xcb_get_property_cookie_t ck =
    xcb_get_property(conn, 0, v->xsurface->window_id, c->net_wm_icon,
                     XCB_ATOM_CARDINAL, 0, 256 * 1024);
  xcb_flush(conn);
  xcb_get_property_reply_t* r = icon_wait_reply(conn, ck.sequence, deadline);
  if (!r)
    return NULL;
  if (r->format == 32 && r->type == XCB_ATOM_CARDINAL)
    icon = wvnc_icon_from_net_wm_icon(xcb_get_property_value(r),
                                      (size_t)xcb_get_property_value_length(r) / 4,
                                      SW_ICON);
  free(r);
  return icon;
}
#endif

/* An xdg-toplevel-icon-v1 icon: the buffer nearest our size, else its
 * theme icon name */
static struct wvnc_icon* xdg_toplevel_icon(struct wlr_xdg_toplevel_icon_v1* ti)
{
  struct wlr_xdg_toplevel_icon_v1_buffer* b;
  struct wlr_xdg_toplevel_icon_v1_buffer* best = NULL;
  struct wvnc_icon* icon = NULL;
  int best_d = 0;

  wl_list_for_each(b, &ti->buffers, link) {
    int d = b->buffer->width - SW_ICON * (b->scale > 0 ? b->scale : 1);
    if (d < 0)
      d = -2 * d;   /* prefer scaling down */
    if (!best || d < best_d) {
      best = b;
      best_d = d;
    }
  }
  if (best) {
    void* data;
    uint32_t format;
    size_t stride;
    if (wlr_buffer_begin_data_ptr_access(best->buffer,
                                         WLR_BUFFER_DATA_PTR_ACCESS_READ,
                                         &data, &format, &stride)) {
      if (format == DRM_FORMAT_ARGB8888 || format == DRM_FORMAT_XRGB8888) {
        int w = best->buffer->width, h = best->buffer->height;
        uint32_t* px = malloc((size_t)w * h * 4);
        if (px) {
          for (int y = 0; y < h; y++) {
            memcpy(px + (size_t)y * w, (uint8_t*)data + y * stride,
                   (size_t)w * 4);
            if (format == DRM_FORMAT_XRGB8888)
              for (int x = 0; x < w; x++)
                px[(size_t)y * w + x] |= 0xff000000;
          }
          icon = wvnc_icon_from_argb(px, w, h, w, true, SW_ICON);
          free(px);
        }
      }
      wlr_buffer_end_data_ptr_access(best->buffer);
    }
  }
  if (!icon && ti->name)
    icon = wvnc_icon_for_name(ti->name, SW_ICON);
  return icon;
}

/* The icon shown for a window: what the client provides, then the
 * icon theme entry of its application, then a generic one */
static struct wvnc_icon* view_icon(struct view* v)
{
  struct wvnc_icon* icon = NULL;
  if (v->icon_valid)
    return v->icon;

  if (v->owner.type == OWNER_XDG) {
    if (v->xdg_icon)
      icon = xdg_toplevel_icon(v->xdg_icon);
    if (!icon && v->xdg->app_id) {
      const char* ids[] = { v->xdg->app_id };
      icon = wvnc_icon_for_app(ids, 1, SW_ICON);
    }
  }
#ifdef HAVE_XWAYLAND
  else {
    icon = net_wm_icon(v->c, v);
    if (!icon) {
      const char* ids[] = { v->xsurface->class, v->xsurface->instance };
      icon = wvnc_icon_for_app(ids, 2, SW_ICON);
    }
  }
#endif
  if (!icon)
    icon = wvnc_icon_fallback(SW_ICON);
  wvnc_icon_unref(v->icon);
  v->icon = icon;
  v->icon_valid = true;
  return icon;
}

static void view_icon_changed(struct view* v)
{
  wvnc_icon_unref(v->icon);
  v->icon = NULL;
  v->icon_valid = false;
}

/* Coverage (0..255) of a rounded rectangle at pixel (x, y), for
 * anti-aliased corners */
static unsigned round_cov(int x, int y, int w, int h, int r)
{
  double cx, cy, d;
  if (x >= r && x < w - r)
    return 255;
  if (y >= r && y < h - r)
    return 255;
  cx = x < r ? r - 0.5 : w - r - 0.5;
  cy = y < r ? r - 0.5 : h - r - 0.5;
  d = sqrt((x - cx) * (x - cx) + (y - cy) * (y - cy));
  if (d <= r - 1)
    return 255;
  if (d >= r)
    return 0;
  return (unsigned)((r - d) * 255);
}

static uint32_t scale_px(uint32_t p, unsigned cov)
{
  if (cov >= 255)
    return p;
  uint32_t a = (p >> 24) * cov / 255, r = ((p >> 16) & 0xff) * cov / 255,
           g = ((p >> 8) & 0xff) * cov / 255, b = (p & 0xff) * cov / 255;
  return (a << 24) | (r << 16) | (g << 8) | b;
}

static void over(uint32_t* d, uint32_t s)
{
  uint32_t sa = s >> 24, inv = 255 - sa;
  if (sa == 255) { *d = s; return; }
  if (sa == 0 && s == 0) return;
  uint32_t o = *d;
  uint32_t a = sa + (o >> 24) * inv / 255;
  uint32_t r = ((s >> 16) & 0xff) + ((o >> 16) & 0xff) * inv / 255;
  uint32_t g = ((s >> 8) & 0xff) + ((o >> 8) & 0xff) * inv / 255;
  uint32_t b = (s & 0xff) + (o & 0xff) * inv / 255;
  if (r > 255) r = 255;
  if (g > 255) g = 255;
  if (b > 255) b = 255;
  *d = (a << 24) | (r << 16) | (g << 8) | b;
}

static void fill_round(uint32_t* px, int stride, int x0, int y0, int w,
                       int h, int r, uint32_t colour)
{
  for (int y = 0; y < h; y++)
    for (int x = 0; x < w; x++) {
      unsigned cov = round_cov(x, y, w, h, r);
      if (cov)
        over(&px[(size_t)(y0 + y) * stride + x0 + x], scale_px(colour, cov));
    }
}

static void switcher_render(struct wvnc_compositor* c)
{
  struct wlr_box screen = screen_at(c, c->px, c->py);
  int text_x = SW_PAD + 10 + SW_ICON + 12;
  int max_text = 0, w, h, rows, first;
  struct text_buffer* tb;

  wvnc_text_init(SW_TEXT_PX);

  /* Size: the widest title, within limits and the screen */
  int limit = SW_MAX_W < screen.width - 40 ? SW_MAX_W : screen.width - 40;
  for (int i = 0; i < c->sw.n; i++) {
    int tw = wvnc_text_width(view_title(c->sw.views[i]), 10000);
    if (tw > max_text)
      max_text = tw;
  }
  w = text_x + max_text + SW_PAD + 14;
  if (w < SW_MIN_W) w = SW_MIN_W;
  if (w > limit) w = limit;
  if (w < 120) w = 120;
  rows = c->sw.n;
  int max_rows = (screen.height - 40 - 2 * SW_PAD) / SW_ROW_H;
  if (max_rows < 1) max_rows = 1;
  if (rows > max_rows) rows = max_rows;
  /* scroll so that the selection is visible */
  first = 0;
  if (c->sw.sel >= rows)
    first = c->sw.sel - rows + 1;
  h = 2 * SW_PAD + rows * SW_ROW_H;

  tb = pixel_buffer_create(w, h);
  if (!tb)
    return;
  /* outline, then the body 1px inside it */
  fill_round(tb->data, w, 0, 0, w, h, SW_RADIUS, sw_border);
  for (int y = 1; y < h - 1; y++)
    for (int x = 1; x < w - 1; x++)
      if (round_cov(x - 1, y - 1, w - 2, h - 2, SW_RADIUS - 1) == 255)
        tb->data[(size_t)y * w + x] = sw_bg;
      else if (round_cov(x - 1, y - 1, w - 2, h - 2, SW_RADIUS - 1))
        over(&tb->data[(size_t)y * w + x],
             scale_px(sw_bg, round_cov(x - 1, y - 1, w - 2, h - 2,
                                       SW_RADIUS - 1)));

  for (int r = 0; r < rows; r++) {
    int i = first + r;
    struct view* v = c->sw.views[i];
    int ry = SW_PAD + r * SW_ROW_H;
    struct wvnc_icon* icon = view_icon(v);
    if (i == c->sw.sel)
      fill_round(tb->data, w, SW_PAD, ry, w - 2 * SW_PAD, SW_ROW_H,
                 SW_ROW_RADIUS, sw_sel);
    if (icon) {
      int ix = SW_PAD + 10, iy = ry + (SW_ROW_H - SW_ICON) / 2;
      for (int y = 0; y < SW_ICON && y < icon->size; y++)
        for (int x = 0; x < SW_ICON && x < icon->size; x++)
          over(&tb->data[(size_t)(iy + y) * w + ix + x],
               icon->px[(size_t)y * icon->size + x]);
    }
    int ty = ry + (SW_ROW_H - wvnc_text_line_height()) / 2;
    wvnc_text_draw(tb->data, w, h, w, text_x, ty, view_title(v),
                   w - text_x - SW_PAD - 12,
                   v->minimized ? sw_text_min : sw_text);
  }

  if (!c->sw.buffer)
    c->sw.buffer = wlr_scene_buffer_create(c->switcher_tree, NULL);
  wlr_scene_buffer_set_buffer(c->sw.buffer, &tb->base);
  wlr_buffer_drop(&tb->base);
  wlr_scene_node_set_position(&c->sw.buffer->node,
                              screen.x + (screen.width - w) / 2,
                              screen.y + (screen.height - h) / 2);
  wlr_scene_node_set_enabled(&c->sw.buffer->node, true);
}

static bool switcher_candidate(struct view* v)
{
  if (!v->mapped || v->nested || view_is_child(v))
    return false;
#ifdef HAVE_XWAYLAND
  if (v->owner.type == OWNER_XWAYLAND &&
      (v->xsurface->override_redirect || v->xsurface->skip_taskbar))
    return false;
#endif
  return true;
}

static bool switcher_begin(struct wvnc_compositor* c)
{
  struct view* v;
  struct view* f = focused_view(c);

  /* a nested compositor has its own window switching */
  if (f && f->nested)
    return false;
  c->sw.n = 0;
  wl_list_for_each(v, &c->views, link) {
    if (c->sw.n < SWITCHER_MAX && switcher_candidate(v))
      c->sw.views[c->sw.n++] = v;
  }
  if (c->sw.n == 0)
    return false;
  /* X11 icons can change at any time; read them afresh */
  for (int i = 0; i < c->sw.n; i++)
    if (c->sw.views[i]->owner.type == OWNER_XWAYLAND)
      view_icon_changed(c->sw.views[i]);
  c->sw.active = true;
  c->sw.sel = 0;
  c->sw.prev = f;
  /* The keyboard is ours while the popup is open; the focused window
   * stays drawn as active */
  focus_surface(c, NULL);
  return true;
}

static void switcher_close(struct wvnc_compositor* c)
{
  c->sw.active = false;
  c->sw.n = 0;
  if (c->sw.buffer) {
    wlr_scene_node_destroy(&c->sw.buffer->node);
    c->sw.buffer = NULL;
  }
}

static void switcher_step(struct wvnc_compositor* c, int dir)
{
  if (!c->sw.active || c->sw.n == 0)
    return;
  c->sw.sel = (c->sw.sel + dir + c->sw.n) % c->sw.n;
  switcher_render(c);
}

static void switcher_finish(struct wvnc_compositor* c, bool commit)
{
  struct view* sel = NULL;
  struct view* prev = c->sw.prev;
  if (!c->sw.active)
    return;
  if (commit && c->sw.sel < c->sw.n)
    sel = c->sw.views[c->sw.sel];
  switcher_close(c);

  if (!sel)
    sel = prev;
  if (prev && prev != sel)
    view_set_activated(prev, false);
  if (sel && sel->minimized)
    view_set_minimized(sel, false);   /* also focuses it */
  else if (sel)
    focus_view(sel);
  else
    focus_next_view(c);
}

/* A window went away or was unmapped while the popup is open */
static void switcher_view_gone(struct view* v)
{
  struct wvnc_compositor* c = v->c;
  int k = -1;
  if (c->sw.prev == v)
    c->sw.prev = NULL;
  if (!c->sw.active)
    return;
  for (int i = 0; i < c->sw.n; i++)
    if (c->sw.views[i] == v)
      k = i;
  if (k < 0)
    return;
  memmove(&c->sw.views[k], &c->sw.views[k + 1],
          (size_t)(c->sw.n - k - 1) * sizeof(c->sw.views[0]));
  c->sw.n--;
  if (c->sw.sel > k)
    c->sw.sel--;
  if (c->sw.n == 0) {
    switcher_close(c);
    focus_next_view(c);
    return;
  }
  if (c->sw.sel >= c->sw.n)
    c->sw.sel = c->sw.n - 1;
  switcher_render(c);
}

/* Bookkeeping when a window unmaps or goes away */
static void view_forget(struct view* v)
{
  struct wvnc_compositor* c = v->c;
  if (c->hover_view == v)
    c->hover_view = NULL;
  switcher_view_gone(v);
}

/* Nested compositors ------------------------------------------------ */

/*
 * A session can itself be a full Wayland compositor (sway, labwc, KDE,
 * ...) using its wayland backend, i.e. showing its outputs as top level
 * windows on our display. Such windows get a whole screen each, are
 * fullscreen (so undecorated), stay at the bottom so that ordinary
 * windows of ours remain reachable, and follow the VNC client's
 * framebuffer size and screen layout. A clipboard bridge connects the
 * nested compositor's seat with ours.
 */

static pid_t view_client_pid(struct view* v)
{
  pid_t pid = 0;
  if (v->owner.type != OWNER_XDG)
    return 0;
  wl_client_get_credentials(wl_resource_get_client(v->xdg->resource),
                            &pid, NULL, NULL);
  return pid;
}

static void nested_start_bridge(struct wvnc_compositor* c, pid_t pid,
                                const char* path)
{
  if (c->bridge && nested_bridge_pid(c->bridge) == pid &&
      nested_bridge_active(c->bridge))
    return;
  nested_bridge_destroy(c->bridge);
  c->bridge = nested_bridge_create(c->display, c->seat, path, pid);
  if (!c->bridge)
    wvnc_log_info("Unable to connect the clipboard bridge to %s", path);
}

/* Decides whether a new top level window is a nested compositor
 * output. Returns true if it is. */
static bool nested_detect(struct view* v)
{
  struct wvnc_compositor* c = v->c;
  pid_t pid;
  char* path;

  if (v->owner.type != OWNER_XDG || v->xdg->parent)
    return false;
  if (c->nested_mode == WVNC_NESTED_OFF)
    return false;

  pid = view_client_pid(v);

  if (c->bridge && pid > 0 && nested_bridge_pid(c->bridge) == pid &&
      nested_bridge_active(c->bridge))
    return true;

  /* A client that serves its own Wayland socket is a compositor */
  path = pid > 0 ? nested_find_socket(getenv("XDG_RUNTIME_DIR"),
                                      c->socket, pid) : NULL;
  if (path) {
    wvnc_log_info("Client pid %d (%s) is a nested compositor serving "
            "%s, giving it the whole screen", (int)pid, v->xdg->app_id ? v->xdg->app_id : "?", path);
    nested_start_bridge(c, pid, path);
    free(path);
    return true;
  }

  if (c->nested_mode == WVNC_NESTED_ON) {
    wvnc_log_info("NestedCompositor=On: treating window of pid %d as "
            "a nested compositor output (no clipboard bridge, its socket "
            "wasn't found)", (int)pid);
    return true;
  }
  return false;
}

static void nested_position(struct view* v)
{
  struct wlr_box* geo = &v->xdg->base->geometry;
  wlr_scene_node_set_position(&v->scene_tree->node,
                              v->nested_box.x - geo->x,
                              v->nested_box.y - geo->y);
}

static void arrange_nested(struct wvnc_compositor* c)
{
  struct view* list[64];
  struct wlr_box full = { 0, 0, c->width, c->height };
  struct view* v;
  int n = 0;

  wl_list_for_each(v, &c->views, link) {
    if (!v->nested || !v->xdg->base->initialized)
      continue;
    if (n < (int)(sizeof(list) / sizeof(list[0])))
      list[n++] = v;
  }

  /* Oldest first, so outputs keep their screen across focus changes */
  for (int i = 1; i < n; i++)
    for (int j = i; j > 0 && list[j - 1]->seq > list[j]->seq; j--) {
      struct view* t = list[j];
      list[j] = list[j - 1];
      list[j - 1] = t;
    }

  for (int i = 0; i < n; i++) {
    struct wlr_box box = full;
    v = list[i];
    /* One output per screen when there are several of both; a single
     * output always covers the whole framebuffer */
    if (c->num_screens >= 2 && n >= 2 && i < c->num_screens)
      box = c->screens[i];
    v->nested_box = box;
    v->maximized = false;
    v->fullscreen = true;
    wlr_xdg_toplevel_set_maximized(v->xdg, false);
    wlr_xdg_toplevel_set_fullscreen(v->xdg, true);
    wlr_xdg_toplevel_set_size(v->xdg, box.width, box.height);
    nested_position(v);
  }
}

static void nested_view_gone(struct view* v)
{
  struct wvnc_compositor* c = v->c;
  struct view* o;
  pid_t pid;

  if (!v->nested || !c->bridge)
    return;
  pid = nested_bridge_pid(c->bridge);
  wl_list_for_each(o, &c->views, link) {
    if (o != v && o->nested && view_client_pid(o) == pid)
      return;
  }
  nested_bridge_destroy(c->bridge);
  c->bridge = NULL;
}

static void rearrange_views(struct wvnc_compositor* c);

void wvnc_set_screens(struct wvnc_compositor* c, const struct wvnc_box* boxes,
                      int n)
{
  c->num_screens = 0;
  for (int i = 0; i < n && c->num_screens < MAX_SCREENS; i++) {
    struct wlr_box b = { boxes[i].x, boxes[i].y,
                         boxes[i].width, boxes[i].height };
    /* Clip to the framebuffer */
    if (b.x < 0) { b.width += b.x; b.x = 0; }
    if (b.y < 0) { b.height += b.y; b.y = 0; }
    if (b.x + b.width > c->width) b.width = c->width - b.x;
    if (b.y + b.height > c->height) b.height = c->height - b.y;
    if (b.width <= 0 || b.height <= 0)
      continue;
    c->screens[c->num_screens++] = b;
  }
  if (c->num_screens == 1 && c->screens[0].x == 0 && c->screens[0].y == 0 &&
      c->screens[0].width == c->width && c->screens[0].height == c->height)
    c->num_screens = 0;
  rearrange_views(c); /* includes arrange_nested() */
}

static void rearrange_views(struct wvnc_compositor* c)
{
  struct view* v;
  wl_list_for_each(v, &c->views, link) {
    if (!v->mapped || v->nested)
      continue;
    if (v->fullscreen || v->maximized) {
      view_maximize(v, v->maximized, v->fullscreen);
      continue;
    }
#ifdef HAVE_XWAYLAND
    if (v->owner.type == OWNER_XWAYLAND && v->xsurface->override_redirect)
      continue;
#endif
    /* Keep at least part of the window reachable */
    struct wlr_box box;
    view_get_box(v, &box);
    int nx = box.x, ny = box.y;
    if (nx > c->width - 32) nx = c->width - 32;
    if (ny > c->height - 32) ny = c->height - 32;
    if (nx + box.width < 32) nx = 0;
    if (ny < 0) ny = 0;
    if (nx != box.x || ny != box.y) {
      box.x = nx;
      box.y = ny;
      if (v->owner.type == OWNER_XDG) {
        struct wlr_box* geo = &v->xdg->base->geometry;
        wlr_scene_node_set_position(&v->scene_tree->node,
                                    box.x - geo->x, box.y - geo->y);
      }
#ifdef HAVE_XWAYLAND
      else
        view_set_box(v, &box);
#endif
    }
  }
  arrange_nested(c);
}

/* xdg-shell ---------------------------------------------------------- */

static void xdg_map(struct wl_listener* listener, void* data)
{
  struct view* v = wl_container_of(listener, v, map);
  (void)data;
  v->mapped = true;
  v->minimized = false;
  if (!v->nested && !v->nested_checked) {
    /* The socket may not have existed at the initial commit */
    v->nested_checked = true;
    if (nested_detect(v)) {
      v->nested = true;
      arrange_nested(v->c);
    }
  }
  if (v->nested)
    nested_position(v);
  else if (!v->maximized && !v->fullscreen)
    view_place(v);
  focus_view(v);
}

static void xdg_unmap(struct wl_listener* listener, void* data)
{
  struct view* v = wl_container_of(listener, v, unmap);
  struct wvnc_compositor* c = v->c;
  bool had_focus = focused_view(c) == v;
  (void)data;
  v->mapped = false;
  if (v->minimized) {
    v->minimized = false;
    wlr_scene_node_set_enabled(&v->scene_tree->node, true);
  }
  view_forget(v);
  if (c->grab_view == v) {
    c->grab_mode = GRAB_NONE;
    c->grab_view = NULL;
  }
  if (had_focus)
    focus_next_view(c);
  if (v->nested)
    arrange_nested(c);
}

/* Size and state of a toplevel before it maps: maximized/fullscreen
 * windows open on the screen with the pointer */
static void xdg_initial_state(struct view* v)
{
  struct wvnc_compositor* c = v->c;
  if (v->xdg->requested.fullscreen || v->xdg->requested.maximized) {
    struct wlr_box full = screen_at(c, c->px, c->py);
    struct wlr_box area = usable_in(c, full);
    struct wlr_box* b;
    v->saved = (struct wlr_box){ 0, 0, 0, 0 };
    v->fullscreen = v->xdg->requested.fullscreen;
    v->maximized = !v->fullscreen && v->xdg->requested.maximized;
    b = v->fullscreen ? &full : &area;
    wlr_xdg_toplevel_set_maximized(v->xdg, v->maximized);
    wlr_xdg_toplevel_set_fullscreen(v->xdg, v->fullscreen);
    wlr_xdg_toplevel_set_size(v->xdg, b->width, b->height);
    wlr_scene_node_set_position(&v->scene_tree->node, b->x, b->y);
  } else {
    wlr_xdg_toplevel_set_size(v->xdg, 0, 0);
  }
}

static void xdg_commit(struct wl_listener* listener, void* data)
{
  struct view* v = wl_container_of(listener, v, commit);
  struct wvnc_compositor* c = v->c;
  (void)data;
  if (!v->xdg->base->initial_commit) {
    /* The window geometry offset may change with any commit */
    if (v->nested) {
      nested_position(v);
      /* Make sure a frame event follows even if it is covered */
      if (c->output)
        wlr_output_schedule_frame(c->output);
    }
    return;
  }

  if (!v->nested && nested_detect(v))
    v->nested = true;
  if (v->nested) {
    wlr_xdg_toplevel_set_wm_capabilities(v->xdg,
      WLR_XDG_TOPLEVEL_WM_CAPABILITIES_FULLSCREEN);
    arrange_nested(c);
    return;
  }

  wlr_xdg_toplevel_set_wm_capabilities(v->xdg,
    WLR_XDG_TOPLEVEL_WM_CAPABILITIES_MAXIMIZE |
    WLR_XDG_TOPLEVEL_WM_CAPABILITIES_FULLSCREEN |
    WLR_XDG_TOPLEVEL_WM_CAPABILITIES_MINIMIZE);
  wlr_xdg_toplevel_set_bounds(v->xdg, c->usable_area.width,
                              c->usable_area.height);
  xdg_initial_state(v);
}

static void xdg_destroy(struct wl_listener* listener, void* data)
{
  struct view* v = wl_container_of(listener, v, destroy);
  struct wvnc_compositor* c = v->c;
  bool nested = v->nested;
  (void)data;
  nested_view_gone(v);
  view_forget(v);
  wvnc_icon_unref(v->icon);
  if (v->xdg_icon)
    wlr_xdg_toplevel_icon_v1_unref(v->xdg_icon);
  wl_list_remove(&v->request_minimize.link);
  wl_list_remove(&v->set_app_id.link);
  wl_list_remove(&v->map.link);
  wl_list_remove(&v->unmap.link);
  wl_list_remove(&v->commit.link);
  wl_list_remove(&v->destroy.link);
  wl_list_remove(&v->request_move.link);
  wl_list_remove(&v->request_resize.link);
  wl_list_remove(&v->request_maximize.link);
  wl_list_remove(&v->request_fullscreen.link);
  wl_list_remove(&v->link);
  free(v);
  if (nested)
    arrange_nested(c);
}

static void begin_grab(struct view* v, enum grab_mode mode, uint32_t edges)
{
  struct wvnc_compositor* c = v->c;
  struct wlr_box box;

  if (v->maximized || v->fullscreen || v->nested)
    return;
  if ((c->button_mask & 0x7) == 0)
    return; /* Only allowed while a button is held */

  view_get_box(v, &box);
  c->grab_view = v;
  c->grab_mode = mode;
  c->grab_box = box;
  c->grab_edges = edges;
  c->grab_x = c->px;
  c->grab_y = c->py;
  wlr_seat_pointer_clear_focus(c->seat);
}

static void xdg_request_move(struct wl_listener* listener, void* data)
{
  struct view* v = wl_container_of(listener, v, request_move);
  (void)data;
  begin_grab(v, GRAB_MOVE, 0);
}

static void xdg_request_resize(struct wl_listener* listener, void* data)
{
  struct view* v = wl_container_of(listener, v, request_resize);
  struct wlr_xdg_toplevel_resize_event* event = data;
  begin_grab(v, GRAB_RESIZE, event->edges);
}

static void xdg_request_maximize(struct wl_listener* listener, void* data)
{
  struct view* v = wl_container_of(listener, v, request_maximize);
  (void)data;
  if (!v->xdg->base->initialized)
    return;
  if (v->nested)
    arrange_nested(v->c); /* always a whole screen */
  else if (v->mapped)
    view_maximize(v, v->xdg->requested.maximized, v->fullscreen);
  else
    xdg_initial_state(v); /* asked for after the initial commit */
  wlr_xdg_surface_schedule_configure(v->xdg->base);
}

static void xdg_request_minimize(struct wl_listener* listener, void* data)
{
  struct view* v = wl_container_of(listener, v, request_minimize);
  (void)data;
  if (v->xdg->base->initialized && v->mapped && !v->nested)
    view_set_minimized(v, true);
}

static void xdg_set_app_id(struct wl_listener* listener, void* data)
{
  struct view* v = wl_container_of(listener, v, set_app_id);
  (void)data;
  view_icon_changed(v);
}

static void xdg_request_fullscreen(struct wl_listener* listener, void* data)
{
  struct view* v = wl_container_of(listener, v, request_fullscreen);
  (void)data;
  if (!v->xdg->base->initialized)
    return;
  if (v->nested)
    arrange_nested(v->c);
  else if (v->mapped)
    view_maximize(v, v->maximized, v->xdg->requested.fullscreen);
  else
    xdg_initial_state(v); /* asked for after the initial commit */
  wlr_xdg_surface_schedule_configure(v->xdg->base);
}

static void handle_new_xdg_toplevel(struct wl_listener* listener, void* data)
{
  struct wvnc_compositor* c = wl_container_of(listener, c, new_xdg_toplevel);
  struct wlr_xdg_toplevel* xdg = data;
  struct view* v = calloc(1, sizeof(*v));

  v->owner.type = OWNER_XDG;
  v->c = c;
  v->xdg = xdg;
  v->seq = ++c->view_seq;
  v->scene_tree = wlr_scene_xdg_surface_create(c->views_tree, xdg->base);
  v->scene_tree->node.data = &v->owner;
  xdg->base->data = v->scene_tree;
  wl_list_insert(&c->views, &v->link);

  v->map.notify = xdg_map;
  wl_signal_add(&xdg->base->surface->events.map, &v->map);
  v->unmap.notify = xdg_unmap;
  wl_signal_add(&xdg->base->surface->events.unmap, &v->unmap);
  v->commit.notify = xdg_commit;
  wl_signal_add(&xdg->base->surface->events.commit, &v->commit);
  v->destroy.notify = xdg_destroy;
  wl_signal_add(&xdg->events.destroy, &v->destroy);
  v->request_move.notify = xdg_request_move;
  wl_signal_add(&xdg->events.request_move, &v->request_move);
  v->request_resize.notify = xdg_request_resize;
  wl_signal_add(&xdg->events.request_resize, &v->request_resize);
  v->request_maximize.notify = xdg_request_maximize;
  wl_signal_add(&xdg->events.request_maximize, &v->request_maximize);
  v->request_fullscreen.notify = xdg_request_fullscreen;
  wl_signal_add(&xdg->events.request_fullscreen, &v->request_fullscreen);
  v->request_minimize.notify = xdg_request_minimize;
  wl_signal_add(&xdg->events.request_minimize, &v->request_minimize);
  v->set_app_id.notify = xdg_set_app_id;
  wl_signal_add(&xdg->events.set_app_id, &v->set_app_id);
}

/* xdg-toplevel-icon-v1: a client set (or unset) its window's icon */
static void handle_set_icon(struct wl_listener* listener, void* data)
{
  struct wvnc_compositor* c = wl_container_of(listener, c, set_icon);
  struct wlr_xdg_toplevel_icon_manager_v1_set_icon_event* ev = data;
  struct view* v;
  wl_list_for_each(v, &c->views, link) {
    if (v->owner.type != OWNER_XDG || v->xdg != ev->toplevel)
      continue;
    if (v->xdg_icon)
      wlr_xdg_toplevel_icon_v1_unref(v->xdg_icon);
    v->xdg_icon = ev->icon ? wlr_xdg_toplevel_icon_v1_ref(ev->icon) : NULL;
    view_icon_changed(v);
    if (c->sw.active)
      switcher_render(c);
    return;
  }
}

static void popup_commit(struct wl_listener* listener, void* data)
{
  struct popup* p = wl_container_of(listener, p, commit);
  struct wvnc_compositor* c = p->c;
  struct wlr_scene_tree* tree = p->xdg_popup->base->data;
  (void)data;

  if (!p->xdg_popup->base->initial_commit)
    return;

  /* The constraint box is in the coordinate space of the root
   * toplevel (or layer surface); find that node via its owner tag */
  struct wlr_scene_node* n = tree ? &tree->node : NULL;
  while (n && !n->data)
    n = n->parent ? &n->parent->node : NULL;
  int lx = 0, ly = 0;
  if (n)
    wlr_scene_node_coords(n, &lx, &ly);
  struct wlr_box box = { -lx, -ly, c->width, c->height };
  wlr_xdg_popup_unconstrain_from_box(p->xdg_popup, &box);
}

static void popup_destroy(struct wl_listener* listener, void* data)
{
  struct popup* p = wl_container_of(listener, p, destroy);
  (void)data;
  wl_list_remove(&p->commit.link);
  wl_list_remove(&p->destroy.link);
  free(p);
}

static void setup_popup(struct wvnc_compositor* c,
                        struct wlr_xdg_popup* xdg_popup,
                        struct wlr_scene_tree* parent_tree)
{
  struct popup* p = calloc(1, sizeof(*p));
  p->c = c;
  p->xdg_popup = xdg_popup;
  xdg_popup->base->data =
    wlr_scene_xdg_surface_create(parent_tree, xdg_popup->base);
  p->commit.notify = popup_commit;
  wl_signal_add(&xdg_popup->base->surface->events.commit, &p->commit);
  p->destroy.notify = popup_destroy;
  wl_signal_add(&xdg_popup->events.destroy, &p->destroy);
}

static void handle_new_xdg_popup(struct wl_listener* listener, void* data)
{
  struct wvnc_compositor* c = wl_container_of(listener, c, new_xdg_popup);
  struct wlr_xdg_popup* xdg_popup = data;
  struct wlr_xdg_surface* parent;

  /* Layer shell popups get their parent later, see layer_new_popup() */
  if (!xdg_popup->parent)
    return;
  parent = wlr_xdg_surface_try_from_wlr_surface(xdg_popup->parent);
  if (!parent || !parent->data)
    return;
  setup_popup(c, xdg_popup, parent->data);
}

/* layer-shell -------------------------------------------------------- */

static void arrange_layers(struct wvnc_compositor* c)
{
  struct wlr_box full = { 0, 0, c->width, c->height };
  struct wlr_box usable = full;
  struct layer* l;

  /* Exclusive surfaces first, as other compositors do */
  for (int pass = 0; pass < 2; pass++) {
    wl_list_for_each(l, &c->layers, link) {
      struct wlr_layer_surface_v1* ls = l->layer_surface;
      bool exclusive = ls->current.exclusive_zone > 0;
      if (!ls->initialized)
        continue;
      if ((pass == 0) != exclusive)
        continue;
      wlr_scene_layer_surface_v1_configure(l->scene, &full, &usable);
    }
  }

  if (memcmp(&usable, &c->usable_area, sizeof(usable)) != 0) {
    c->usable_area = usable;
    rearrange_views(c);
  }
}

static void layer_map(struct wl_listener* listener, void* data)
{
  struct layer* l = wl_container_of(listener, l, map);
  struct wvnc_compositor* c = l->c;
  (void)data;
  arrange_layers(c);
  if (l->layer_surface->current.keyboard_interactive ==
      ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_EXCLUSIVE)
    focus_surface(c, l->layer_surface->surface);
}

static void layer_unmap(struct wl_listener* listener, void* data)
{
  struct layer* l = wl_container_of(listener, l, unmap);
  struct wvnc_compositor* c = l->c;
  (void)data;
  arrange_layers(c);
  if (c->seat->keyboard_state.focused_surface == l->layer_surface->surface)
    focus_next_view(c);
}

static void layer_commit(struct wl_listener* listener, void* data)
{
  struct layer* l = wl_container_of(listener, l, commit);
  struct wvnc_compositor* c = l->c;
  struct wlr_layer_surface_v1* ls = l->layer_surface;
  (void)data;

  if (ls->current.committed & WLR_LAYER_SURFACE_V1_STATE_LAYER)
    wlr_scene_node_reparent(&l->scene->tree->node,
                            c->layer_trees[ls->current.layer]);

  if (ls->initial_commit || ls->current.committed)
    arrange_layers(c);
}

static void layer_new_popup(struct wl_listener* listener, void* data)
{
  struct layer* l = wl_container_of(listener, l, new_popup);
  setup_popup(l->c, data, l->scene->tree);
}

static void layer_destroy(struct wl_listener* listener, void* data)
{
  struct layer* l = wl_container_of(listener, l, destroy);
  struct wvnc_compositor* c = l->c;
  (void)data;
  wl_list_remove(&l->map.link);
  wl_list_remove(&l->unmap.link);
  wl_list_remove(&l->commit.link);
  wl_list_remove(&l->destroy.link);
  wl_list_remove(&l->new_popup.link);
  wl_list_remove(&l->link);
  free(l);
  arrange_layers(c);
}

static void handle_new_layer_surface(struct wl_listener* listener,
                                     void* data)
{
  struct wvnc_compositor* c = wl_container_of(listener, c, new_layer_surface);
  struct wlr_layer_surface_v1* ls = data;
  struct layer* l;

  if (!ls->output)
    ls->output = c->output;
  if (!ls->output) {
    wlr_layer_surface_v1_destroy(ls);
    return;
  }

  l = calloc(1, sizeof(*l));
  l->owner.type = OWNER_LAYER;
  l->c = c;
  l->layer_surface = ls;
  l->scene = wlr_scene_layer_surface_v1_create(
    c->layer_trees[ls->pending.layer], ls);
  l->scene->tree->node.data = &l->owner;
  ls->data = l;
  wl_list_insert(&c->layers, &l->link);

  l->map.notify = layer_map;
  wl_signal_add(&ls->surface->events.map, &l->map);
  l->unmap.notify = layer_unmap;
  wl_signal_add(&ls->surface->events.unmap, &l->unmap);
  l->commit.notify = layer_commit;
  wl_signal_add(&ls->surface->events.commit, &l->commit);
  l->destroy.notify = layer_destroy;
  wl_signal_add(&ls->events.destroy, &l->destroy);
  l->new_popup.notify = layer_new_popup;
  wl_signal_add(&ls->events.new_popup, &l->new_popup);
}

/* Xwayland ----------------------------------------------------------- */

#ifdef HAVE_XWAYLAND

static void xw_map(struct wl_listener* listener, void* data)
{
  struct view* v = wl_container_of(listener, v, map);
  struct wvnc_compositor* c = v->c;
  struct wlr_xwayland_surface* xs = v->xsurface;
  (void)data;

  v->scene_tree = wlr_scene_tree_create(xs->override_redirect ?
                                        c->unmanaged_tree : c->views_tree);
  v->scene_tree->node.data = &v->owner;
  wlr_scene_subsurface_tree_create(v->scene_tree, xs->surface);
  wlr_scene_node_set_position(&v->scene_tree->node, xs->x, xs->y);
  v->mapped = true;
  v->minimized = false;

  if (xs->override_redirect) {
    if (wlr_xwayland_surface_override_redirect_wants_focus(xs))
      focus_surface(c, xs->surface);
    return;
  }

  if (xs->fullscreen) {
    view_maximize(v, false, true);
  } else if (xs->maximized_horz && xs->maximized_vert) {
    view_maximize(v, true, false);
  } else if (view_has_deco(v) && xs->y < DECO_HEIGHT) {
    /* Keep the title bar on screen */
    struct wlr_box box = { xs->x, DECO_HEIGHT, xs->width, xs->height };
    view_set_box(v, &box);
  }

  if (wlr_xwayland_surface_icccm_input_model(xs) !=
      WLR_ICCCM_INPUT_MODEL_NONE)
    focus_view(v);
  deco_update(v);

  /* Mapped iconic (WM_HINTS initial_state) */
  if (xs->hints && (xs->hints->flags & XCB_ICCCM_WM_HINT_STATE) &&
      xs->hints->initial_state == XCB_ICCCM_WM_STATE_ICONIC)
    view_set_minimized(v, true);
  else
    wlr_xwayland_surface_set_minimized(xs, false);
}

/* Called on the client's MapRequest, before the X server maps the
 * window. Apply the final geometry (and the zero border width that
 * wlr_xwayland_surface_configure() always sets) now rather than after
 * mapping: changing the border width of a mapped window makes the X
 * server reallocate its composite pixmap, and contents drawn before
 * that (e.g. xeyes, which draws only on Expose) can be lost, leaving
 * a black window until the client happens to redraw. */
static void xw_map_request(struct wl_listener* listener, void* data)
{
  struct view* v = wl_container_of(listener, v, map_request);
  struct wlr_xwayland_surface* xs = v->xsurface;
  int16_t y = xs->y;
  (void)data;

  if (xs->override_redirect || xs->fullscreen ||
      (xs->maximized_horz && xs->maximized_vert))
    return;   /* placed by xw_map() */
  if (view_has_deco(v) && y < DECO_HEIGHT)
    y = DECO_HEIGHT;   /* keep the title bar on screen */
  wlr_xwayland_surface_configure(xs, xs->x, y, xs->width, xs->height);
}

static void xw_unmap(struct wl_listener* listener, void* data)
{
  struct view* v = wl_container_of(listener, v, unmap);
  struct wvnc_compositor* c = v->c;
  bool had_focus = v->xsurface->surface &&
    c->seat->keyboard_state.focused_surface == v->xsurface->surface;
  (void)data;

  v->mapped = false;
  v->minimized = false;
  view_forget(v);
  if (c->grab_view == v) {
    c->grab_mode = GRAB_NONE;
    c->grab_view = NULL;
  }
  if (v->scene_tree) {
    wlr_scene_node_destroy(&v->scene_tree->node);
    v->scene_tree = NULL;
    v->deco = NULL;   /* destroyed with its parent */
  }
  if (had_focus)
    focus_next_view(c);
}

static void xw_associate(struct wl_listener* listener, void* data)
{
  struct view* v = wl_container_of(listener, v, associate);
  (void)data;
  wl_list_remove(&v->map.link);
  wl_list_remove(&v->unmap.link);
  v->map.notify = xw_map;
  wl_signal_add(&v->xsurface->surface->events.map, &v->map);
  v->unmap.notify = xw_unmap;
  wl_signal_add(&v->xsurface->surface->events.unmap, &v->unmap);

  /* If Xwayland's WL_SURFACE_SERIAL message arrives after the surface
   * already committed its buffer, wlroots only maps the surface on the
   * next commit, which a static window (e.g. an idle xterm) may never
   * make. Map it now in that case. */
  if (!v->xsurface->surface->mapped &&
      wlr_surface_has_buffer(v->xsurface->surface)) {
    wlr_log(WLR_DEBUG, "Mapping already committed Xwayland surface");
    wlr_surface_map(v->xsurface->surface);
  }
}

static void xw_dissociate(struct wl_listener* listener, void* data)
{
  struct view* v = wl_container_of(listener, v, dissociate);
  (void)data;
  wl_list_remove(&v->map.link);
  wl_list_remove(&v->unmap.link);
  wl_list_init(&v->map.link);
  wl_list_init(&v->unmap.link);
}

static void xw_request_configure(struct wl_listener* listener, void* data)
{
  struct view* v = wl_container_of(listener, v, request_configure);
  struct wlr_xwayland_surface_configure_event* ev = data;
  if (v->maximized || v->fullscreen) {
    /* Re-assert our geometry */
    view_maximize(v, v->maximized, v->fullscreen);
    return;
  }
  wlr_xwayland_surface_configure(v->xsurface, ev->x, ev->y,
                                 ev->width, ev->height);
  if (v->scene_tree)
    wlr_scene_node_set_position(&v->scene_tree->node, ev->x, ev->y);
  deco_update(v);
}

static void xw_request_activate(struct wl_listener* listener, void* data)
{
  struct view* v = wl_container_of(listener, v, request_activate);
  (void)data;
  if (!v->mapped || v->xsurface->override_redirect)
    return;
  if (v->minimized)
    view_set_minimized(v, false);   /* also focuses it */
  else
    focus_view(v);
}

/* Iconify (WM_CHANGE_STATE, _NET_WM_STATE_HIDDEN) and back */
static void xw_request_minimize(struct wl_listener* listener, void* data)
{
  struct view* v = wl_container_of(listener, v, request_minimize);
  struct wlr_xwayland_minimize_event* ev = data;
  if (!v->mapped || v->xsurface->override_redirect) {
    wlr_xwayland_surface_set_minimized(v->xsurface, false);
    return;
  }
  view_set_minimized(v, ev->minimize);
}

static void xw_set_class(struct wl_listener* listener, void* data)
{
  struct view* v = wl_container_of(listener, v, set_app_id);
  (void)data;
  view_icon_changed(v);
}

static void xw_set_geometry(struct wl_listener* listener, void* data)
{
  struct view* v = wl_container_of(listener, v, set_geometry);
  (void)data;
  if (v->scene_tree)
    wlr_scene_node_set_position(&v->scene_tree->node,
                                v->xsurface->x, v->xsurface->y);
  deco_update(v);
}

static void xw_request_move(struct wl_listener* listener, void* data)
{
  struct view* v = wl_container_of(listener, v, request_move);
  (void)data;
  if (v->mapped)
    begin_grab(v, GRAB_MOVE, 0);
}

static void xw_request_resize(struct wl_listener* listener, void* data)
{
  struct view* v = wl_container_of(listener, v, request_resize);
  struct wlr_xwayland_resize_event* ev = data;
  if (v->mapped)
    begin_grab(v, GRAB_RESIZE, ev->edges);
}

static void xw_request_maximize(struct wl_listener* listener, void* data)
{
  struct view* v = wl_container_of(listener, v, request_maximize);
  struct wlr_xwayland_surface* xs = v->xsurface;
  (void)data;
  if (!v->mapped)
    return;
  view_maximize(v, xs->maximized_horz && xs->maximized_vert,
                v->fullscreen);
}

static void xw_request_fullscreen(struct wl_listener* listener, void* data)
{
  struct view* v = wl_container_of(listener, v, request_fullscreen);
  (void)data;
  if (!v->mapped)
    return;
  view_maximize(v, v->maximized, v->xsurface->fullscreen);
}

static void xw_destroy(struct wl_listener* listener, void* data)
{
  struct view* v = wl_container_of(listener, v, destroy);
  struct wvnc_compositor* c = v->c;
  (void)data;
  if (c->grab_view == v) {
    c->grab_mode = GRAB_NONE;
    c->grab_view = NULL;
  }
  view_forget(v);
  wvnc_icon_unref(v->icon);
  if (v->scene_tree)
    wlr_scene_node_destroy(&v->scene_tree->node);
  wl_list_remove(&v->request_minimize.link);
  wl_list_remove(&v->set_app_id.link);
  wl_list_remove(&v->associate.link);
  wl_list_remove(&v->dissociate.link);
  wl_list_remove(&v->request_configure.link);
  wl_list_remove(&v->request_activate.link);
  wl_list_remove(&v->set_geometry.link);
  wl_list_remove(&v->request_move.link);
  wl_list_remove(&v->request_resize.link);
  wl_list_remove(&v->request_maximize.link);
  wl_list_remove(&v->request_fullscreen.link);
  wl_list_remove(&v->set_title.link);
  wl_list_remove(&v->map_request.link);
  wl_list_remove(&v->destroy.link);
  wl_list_remove(&v->map.link);
  wl_list_remove(&v->unmap.link);
  wl_list_remove(&v->link);
  free(v->deco_title_text);
  free(v);
}

static void xw_set_title(struct wl_listener* listener, void* data)
{
  struct view* v = wl_container_of(listener, v, set_title);
  (void)data;
  deco_update(v);
}

static void handle_xwayland_new_surface(struct wl_listener* listener,
                                        void* data)
{
  struct wvnc_compositor* c =
    wl_container_of(listener, c, xwayland_new_surface);
  struct wlr_xwayland_surface* xs = data;
  struct view* v = calloc(1, sizeof(*v));

  v->owner.type = OWNER_XWAYLAND;
  v->c = c;
  v->xsurface = xs;
  xs->data = v;
  wl_list_init(&v->map.link);
  wl_list_init(&v->unmap.link);
  /* Unmapped X windows go to the back of the focus list */
  wl_list_insert(c->views.prev, &v->link);

  v->associate.notify = xw_associate;
  wl_signal_add(&xs->events.associate, &v->associate);
  v->dissociate.notify = xw_dissociate;
  wl_signal_add(&xs->events.dissociate, &v->dissociate);
  v->destroy.notify = xw_destroy;
  wl_signal_add(&xs->events.destroy, &v->destroy);
  v->request_configure.notify = xw_request_configure;
  wl_signal_add(&xs->events.request_configure, &v->request_configure);
  v->request_activate.notify = xw_request_activate;
  wl_signal_add(&xs->events.request_activate, &v->request_activate);
  v->set_geometry.notify = xw_set_geometry;
  wl_signal_add(&xs->events.set_geometry, &v->set_geometry);
  v->request_move.notify = xw_request_move;
  wl_signal_add(&xs->events.request_move, &v->request_move);
  v->request_resize.notify = xw_request_resize;
  wl_signal_add(&xs->events.request_resize, &v->request_resize);
  v->request_maximize.notify = xw_request_maximize;
  wl_signal_add(&xs->events.request_maximize, &v->request_maximize);
  v->request_fullscreen.notify = xw_request_fullscreen;
  wl_signal_add(&xs->events.request_fullscreen, &v->request_fullscreen);
  v->set_title.notify = xw_set_title;
  wl_signal_add(&xs->events.set_title, &v->set_title);
  v->map_request.notify = xw_map_request;
  wl_signal_add(&xs->events.map_request, &v->map_request);
  v->request_minimize.notify = xw_request_minimize;
  wl_signal_add(&xs->events.request_minimize, &v->request_minimize);
  v->set_app_id.notify = xw_set_class;
  wl_signal_add(&xs->events.set_class, &v->set_app_id);
}

static void handle_xwayland_ready(struct wl_listener* listener, void* data)
{
  struct wvnc_compositor* c = wl_container_of(listener, c, xwayland_ready);
  (void)data;

  wlr_xwayland_set_seat(c->xwayland, c->seat);

  struct wlr_xcursor* xc =
    wlr_xcursor_manager_get_xcursor(c->xcursor_mgr, "default", 1);
  if (!xc)
    xc = wlr_xcursor_manager_get_xcursor(c->xcursor_mgr, "left_ptr", 1);
  if (xc) {
    struct wlr_xcursor_image* img = xc->images[0];
    wlr_xwayland_set_cursor(c->xwayland, img->buffer, img->width * 4,
                            img->width, img->height,
                            img->hotspot_x, img->hotspot_y);
  }

  if (c->cb.xwayland_ready)
    c->cb.xwayland_ready(c->cb.data, c->xwayland->display_name);
}

#endif /* HAVE_XWAYLAND */

/* ------------------------------------------------------------------ */
/* Cursor                                                              */
/* ------------------------------------------------------------------ */

static void send_cursor_argb(struct wvnc_compositor* c, int w, int h,
                             int hx, int hy, const uint32_t* argb,
                             size_t stride_px)
{
  uint8_t* rgba;

  if (!c->cb.cursor)
    return;

  if (w <= 0 || h <= 0 || !argb) {
    static const uint8_t empty[4] = { 0, 0, 0, 0 };
    c->cb.cursor(c->cb.data, 1, 1, 0, 0, empty);
    return;
  }

  rgba = malloc((size_t)w * h * 4);
  if (!rgba)
    return;
  for (int y = 0; y < h; y++) {
    for (int x = 0; x < w; x++) {
      uint32_t p = argb[(size_t)y * stride_px + x];
      unsigned a = p >> 24;
      unsigned r = (p >> 16) & 0xff, g = (p >> 8) & 0xff, b = p & 0xff;
      uint8_t* o = rgba + ((size_t)y * w + x) * 4;
      if (a != 0 && a != 255) {
        /* Un-premultiply */
        r = (r * 255 + a / 2) / a;
        g = (g * 255 + a / 2) / a;
        b = (b * 255 + a / 2) / a;
        if (r > 255) r = 255;
        if (g > 255) g = 255;
        if (b > 255) b = 255;
      }
      o[0] = r; o[1] = g; o[2] = b; o[3] = a;
    }
  }

  if (hx < 0) hx = 0;
  if (hy < 0) hy = 0;
  if (hx >= w) hx = w - 1;
  if (hy >= h) hy = h - 1;
  c->cb.cursor(c->cb.data, w, h, hx, hy, rgba);
  free(rgba);
}

/* One of our own cursors (default arrow, resize arrows) */
static void set_named_cursor(struct wvnc_compositor* c, const char* name)
{
  struct wlr_xcursor* xc;

  if (c->cursor_is_default && c->cursor_named &&
      strcmp(c->cursor_named, name) == 0)
    return;

  xc = wlr_xcursor_manager_get_xcursor(c->xcursor_mgr, name, 1);
  if (!xc)
    xc = wlr_xcursor_manager_get_xcursor(c->xcursor_mgr, "default", 1);
  if (!xc)
    xc = wlr_xcursor_manager_get_xcursor(c->xcursor_mgr, "left_ptr", 1);
  if (!xc)
    return;
  c->cursor_named = name;

  struct wlr_xcursor_image* img = xc->images[0];
  send_cursor_argb(c, img->width, img->height, img->hotspot_x,
                   img->hotspot_y, (const uint32_t*)img->buffer,
                   img->width);
  c->cursor_is_default = true;
}

static void set_default_cursor(struct wvnc_compositor* c)
{
  set_named_cursor(c, "default");
}

static void clear_cursor_surface(struct wvnc_compositor* c)
{
  if (!c->cursor_surface)
    return;
  wl_list_remove(&c->cursor_surface_commit.link);
  wl_list_remove(&c->cursor_surface_destroy.link);
  c->cursor_surface = NULL;
}

static void update_cursor_from_surface(struct wvnc_compositor* c)
{
  struct wlr_surface* s = c->cursor_surface;
  struct wlr_texture* tex = wlr_surface_get_texture(s);
  uint32_t* pixels;
  int w, h;

  c->cursor_is_default = false;

  if (!tex) {
    send_cursor_argb(c, 0, 0, 0, 0, NULL, 0);
    return;
  }

  w = tex->width;
  h = tex->height;
  pixels = calloc((size_t)w * h, 4);
  if (!pixels)
    return;

  struct wlr_texture_read_pixels_options opts = {
    .data = pixels,
    .format = DRM_FORMAT_ARGB8888,
    .stride = w * 4,
  };
  if (wlr_texture_read_pixels(tex, &opts)) {
    int scale = s->current.scale > 0 ? s->current.scale : 1;
    send_cursor_argb(c, w, h, c->cursor_hotx * scale,
                     c->cursor_hoty * scale, pixels, w);
  } else {
    wlr_log(WLR_DEBUG, "Unable to read cursor pixels");
  }
  free(pixels);
}

static void cursor_surface_commit(struct wl_listener* listener, void* data)
{
  struct wvnc_compositor* c =
    wl_container_of(listener, c, cursor_surface_commit);
  (void)data;
  c->cursor_hotx -= c->cursor_surface->current.dx;
  c->cursor_hoty -= c->cursor_surface->current.dy;
  update_cursor_from_surface(c);
}

static void cursor_surface_destroy(struct wl_listener* listener, void* data)
{
  struct wvnc_compositor* c =
    wl_container_of(listener, c, cursor_surface_destroy);
  (void)data;
  clear_cursor_surface(c);
}

static void handle_request_set_cursor(struct wl_listener* listener,
                                      void* data)
{
  struct wvnc_compositor* c = wl_container_of(listener, c, request_set_cursor);
  struct wlr_seat_pointer_request_set_cursor_event* ev = data;

  if (ev->seat_client != c->seat->pointer_state.focused_client)
    return;
  if (c->grab_mode != GRAB_NONE)
    return;

  clear_cursor_surface(c);

  if (!ev->surface) {
    send_cursor_argb(c, 0, 0, 0, 0, NULL, 0);
    c->cursor_is_default = false;
    return;
  }

  c->cursor_surface = ev->surface;
  c->cursor_hotx = ev->hotspot_x;
  c->cursor_hoty = ev->hotspot_y;
  c->cursor_surface_commit.notify = cursor_surface_commit;
  wl_signal_add(&ev->surface->events.commit, &c->cursor_surface_commit);
  c->cursor_surface_destroy.notify = cursor_surface_destroy;
  wl_signal_add(&ev->surface->events.destroy, &c->cursor_surface_destroy);
  update_cursor_from_surface(c);
}

/* ------------------------------------------------------------------ */
/* Pointer input                                                       */
/* ------------------------------------------------------------------ */

static struct owner* owner_from_node(struct wlr_scene_node* node)
{
  struct wlr_scene_tree* tree = node->parent;
  while (tree && !tree->node.data)
    tree = tree->node.parent;
  return tree ? tree->node.data : NULL;
}

static struct wlr_surface* surface_at(struct wvnc_compositor* c,
                                      double lx, double ly,
                                      double* sx, double* sy,
                                      struct owner** owner)
{
  struct wlr_scene_node* node =
    wlr_scene_node_at(&c->scene->tree.node, lx, ly, sx, sy);
  *owner = NULL;
  if (!node || node->type != WLR_SCENE_NODE_BUFFER)
    return NULL;
  struct wlr_scene_buffer* buffer = wlr_scene_buffer_from_node(node);
  struct wlr_scene_surface* ss = wlr_scene_surface_try_from_buffer(buffer);
  if (!ss)
    return NULL;
  *owner = owner_from_node(node);
  return ss->surface;
}

/* Layout position of the scene buffer showing a surface */
static bool find_surface_node(struct wlr_scene_tree* tree,
                              struct wlr_surface* surface,
                              int* lx, int* ly)
{
  struct wlr_scene_node* child;
  wl_list_for_each(child, &tree->children, link) {
    if (!child->enabled)
      continue;
    if (child->type == WLR_SCENE_NODE_TREE) {
      if (find_surface_node(wlr_scene_tree_from_node(child), surface, lx, ly))
        return true;
    } else if (child->type == WLR_SCENE_NODE_BUFFER) {
      struct wlr_scene_surface* ss = wlr_scene_surface_try_from_buffer(
        wlr_scene_buffer_from_node(child));
      if (ss && ss->surface == surface)
        return wlr_scene_node_coords(child, lx, ly);
    }
  }
  return false;
}

static void clear_implicit_grab(struct wvnc_compositor* c)
{
  if (!c->implicit_grab)
    return;
  wl_list_remove(&c->implicit_grab_destroy.link);
  c->implicit_grab = NULL;
}

static void implicit_grab_destroy(struct wl_listener* listener, void* data)
{
  struct wvnc_compositor* c =
    wl_container_of(listener, c, implicit_grab_destroy);
  (void)data;
  clear_implicit_grab(c);
}

static void process_grab_motion(struct wvnc_compositor* c)
{
  struct view* v = c->grab_view;
  int dx = (int)(c->px - c->grab_x), dy = (int)(c->py - c->grab_y);
  struct wlr_box box = c->grab_box;

  if (c->grab_mode == GRAB_DECO || !v)
    return;

  if (c->grab_mode == GRAB_MOVE) {
    box.x += dx;
    box.y += dy;
    if (v->owner.type == OWNER_XDG) {
      struct wlr_box* geo = &v->xdg->base->geometry;
      wlr_scene_node_set_position(&v->scene_tree->node,
                                  box.x - geo->x, box.y - geo->y);
    }
#ifdef HAVE_XWAYLAND
    else
      view_set_box(v, &box);
#endif
    return;
  }

  int left = box.x, right = box.x + box.width;
  int top = box.y, bottom = box.y + box.height;
  int minw = 1, minh = 1;
#ifdef HAVE_XWAYLAND
  if (v->owner.type == OWNER_XWAYLAND) {
    /* Room for the title bar buttons, and the window's own minimum */
    xcb_size_hints_t* hints = v->xsurface->size_hints;
    if (view_has_deco(v))
      minw = DECO_MIN_W;
    if (hints && (hints->flags & XCB_ICCCM_SIZE_HINT_P_MIN_SIZE)) {
      if (hints->min_width > minw) minw = hints->min_width;
      if (hints->min_height > minh) minh = hints->min_height;
    }
  }
#endif
  if (c->grab_edges & WLR_EDGE_TOP) {
    top += dy;
    if (top > bottom - minh) top = bottom - minh;
  } else if (c->grab_edges & WLR_EDGE_BOTTOM) {
    bottom += dy;
    if (bottom < top + minh) bottom = top + minh;
  }
  if (c->grab_edges & WLR_EDGE_LEFT) {
    left += dx;
    if (left > right - minw) left = right - minw;
  } else if (c->grab_edges & WLR_EDGE_RIGHT) {
    right += dx;
    if (right < left + minw) right = left + minw;
  }
  box.x = left;
  box.y = top;
  box.width = right - left;
  box.height = bottom - top;
  view_set_box(v, &box);
}

static void pointer_motion(struct wvnc_compositor* c, uint32_t time)
{
  double sx, sy;
  struct owner* owner;
  struct wlr_surface* surface;

  if (c->grab_mode != GRAB_NONE) {
    process_grab_motion(c);
    return;
  }

  /* Implicit grab: keep delivering to the surface that got the press */
  if (c->implicit_grab && (c->button_mask & 0x7)) {
    int lx, ly;
    if (find_surface_node(&c->scene->tree, c->implicit_grab, &lx, &ly)) {
      wlr_seat_pointer_notify_motion(c->seat, time, c->px - lx, c->py - ly);
      return;
    }
  }

  surface = surface_at(c, c->px, c->py, &sx, &sy, &owner);
  if (surface) {
    set_deco_hover(c, NULL, -1);
    if (c->seat->pointer_state.focused_surface != surface)
      c->cursor_is_default = false;
    wlr_seat_pointer_notify_enter(c->seat, surface, sx, sy);
    wlr_seat_pointer_notify_motion(c->seat, time, sx, sy);
  } else {
    clear_cursor_surface(c);
    wlr_seat_pointer_clear_focus(c->seat);
    const char* name = "default";
    struct view* hover = NULL;
    int hover_kind = -1;
#ifdef HAVE_XWAYLAND
    struct wlr_scene_node* node =
      wlr_scene_node_at(&c->scene->tree.node, c->px, c->py, &sx, &sy);
    if (node && (node->type == WLR_SCENE_NODE_RECT ||
                 node->type == WLR_SCENE_NODE_BUFFER) && node->data &&
        ((struct owner*)node->data)->type == OWNER_DECO) {
      struct deco_part* part = node->data;
      if (part->kind == DECO_EDGE && node->type == WLR_SCENE_NODE_RECT)
        name = edge_cursor(deco_edges_at(part->view, c->px, c->py));
      else if (part->kind == DECO_MINIMIZE || part->kind == DECO_MAXIMIZE ||
               part->kind == DECO_CLOSE) {
        hover = part->view;
        hover_kind = part->kind;
      }
    }
#endif
    set_deco_hover(c, hover, hover_kind);
    set_named_cursor(c, name);
  }
}

static void focus_owner(struct wvnc_compositor* c, struct owner* owner,
                        struct wlr_surface* surface)
{
  if (!owner)
    return;
  if (owner->type == OWNER_LAYER) {
    struct layer* l = (struct layer*)owner;
    if (l->layer_surface->current.keyboard_interactive !=
        ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE)
      focus_surface(c, l->layer_surface->surface);
    return;
  }
  struct view* v = (struct view*)owner;
#ifdef HAVE_XWAYLAND
  if (v->owner.type == OWNER_XWAYLAND && v->xsurface->override_redirect) {
    if (wlr_xwayland_surface_override_redirect_wants_focus(v->xsurface))
      focus_surface(c, surface);
    return;
  }
#endif
  (void)surface;
  focus_view(v);
}

static void notify_button(struct wvnc_compositor* c, uint32_t time,
                          uint32_t button, bool pressed)
{
  wlr_seat_pointer_notify_button(c->seat, time, button,
    pressed ? WL_POINTER_BUTTON_STATE_PRESSED :
              WL_POINTER_BUTTON_STATE_RELEASED);
}

static void deco_press(struct wvnc_compositor* c, struct deco_part* part,
                       uint16_t mask)
{
#ifdef HAVE_XWAYLAND
  struct view* v = part->view;
  focus_view(v);
  if (!(mask & 1))
    return;   /* left button only */
  switch (part->kind) {
  case DECO_BAR:
    c->button_mask = mask;   /* begin_grab wants a held button */
    begin_grab(v, GRAB_MOVE, 0);
    break;
  case DECO_MINIMIZE:
    view_set_minimized(v, true);
    break;
  case DECO_MAXIMIZE:
    view_maximize(v, !v->maximized, false);
    break;
  case DECO_CLOSE:
    wlr_xwayland_surface_close(v->xsurface);
    break;
  case DECO_EDGE:
    c->button_mask = mask;
    begin_grab(v, GRAB_RESIZE, deco_edges_at(v, c->px, c->py));
    break;
  }
#else
  (void)c; (void)part; (void)mask;
#endif
}

void wvnc_pointer_event(struct wvnc_compositor* c, int x, int y,
                        uint16_t mask)
{
  static const uint32_t buttons[] = {
    WVNC_BTN_LEFT, WVNC_BTN_MIDDLE, WVNC_BTN_RIGHT,
  };
  uint32_t time = now_msec();
  uint16_t changed = mask ^ c->button_mask;
  bool frame = false;

  if (x != (int)c->px || y != (int)c->py) {
    c->px = x;
    c->py = y;
    pointer_motion(c, time);
    frame = true;
  }

  /* A click closes the window switcher, nothing selected */
  if ((changed & mask & 0x7) && c->sw.active)
    switcher_finish(c, false);

  /* First button press: focus/raise and start an implicit grab */
  if ((changed & mask & 0x7) && c->grab_mode == GRAB_NONE &&
      (c->button_mask & 0x7) == 0) {
    double sx, sy;
    struct owner* owner;
    struct wlr_surface* s;
    struct wlr_scene_node* node =
      wlr_scene_node_at(&c->scene->tree.node, c->px, c->py, &sx, &sy);
    if (node && (node->type == WLR_SCENE_NODE_RECT ||
                 (node->type == WLR_SCENE_NODE_BUFFER &&
                  !wlr_scene_surface_try_from_buffer(
                     wlr_scene_buffer_from_node(node)))) &&
        node->data && ((struct owner*)node->data)->type == OWNER_DECO) {
      deco_press(c, node->data, mask);
      c->button_mask = mask;
      if (c->grab_mode == GRAB_NONE)
        c->grab_mode = GRAB_DECO;   /* swallow until release */
      goto buttons_done;
    }
    s = surface_at(c, c->px, c->py, &sx, &sy, &owner);
    focus_owner(c, owner, s);
    clear_implicit_grab(c);
    if (s) {
      c->implicit_grab = s;
      c->implicit_grab_destroy.notify = implicit_grab_destroy;
      wl_signal_add(&s->events.destroy, &c->implicit_grab_destroy);
    }
  }

  c->button_mask = mask;
buttons_done:

  for (int i = 0; i < 3; i++) {
    if (!(changed & (1 << i)))
      continue;
    if (c->grab_mode == GRAB_NONE)
      notify_button(c, time, buttons[i], mask & (1 << i));
    frame = true;
  }

  /* Back / forward (rfb extended buttons 8 and 9) */
  if (changed & (1 << 7)) {
    notify_button(c, time, WVNC_BTN_SIDE, mask & (1 << 7));
    frame = true;
  }
  if (changed & (1 << 8)) {
    notify_button(c, time, WVNC_BTN_EXTRA, mask & (1 << 8));
    frame = true;
  }

  /* Scroll wheel: one discrete step per press */
  for (int i = 3; i < 7; i++) {
    if (!(changed & mask & (1 << i)))
      continue;
    enum wl_pointer_axis axis = (i < 5) ? WL_POINTER_AXIS_VERTICAL_SCROLL :
                                          WL_POINTER_AXIS_HORIZONTAL_SCROLL;
    double dir = (i == 3 || i == 5) ? -1.0 : 1.0;
    wlr_seat_pointer_notify_axis(c->seat, time, axis, 15.0 * dir,
                                 (int32_t)(120 * dir),
                                 WL_POINTER_AXIS_SOURCE_WHEEL,
                                 WL_POINTER_AXIS_RELATIVE_DIRECTION_IDENTICAL);
    frame = true;
  }

  if ((c->button_mask & 0x7) == 0) {
    if (c->grab_mode != GRAB_NONE) {
      c->grab_mode = GRAB_NONE;
      c->grab_view = NULL;
      pointer_motion(c, time);
    }
    if (c->implicit_grab) {
      clear_implicit_grab(c);
      pointer_motion(c, time);
    }
  }

  if (frame)
    wlr_seat_pointer_notify_frame(c->seat);
}

/* ------------------------------------------------------------------ */
/* Keyboard input                                                      */
/* ------------------------------------------------------------------ */

static void keyboard_modifiers(struct wl_listener* listener, void* data)
{
  struct wvnc_compositor* c = wl_container_of(listener, c, keyboard_modifiers);
  (void)data;
  wlr_seat_set_keyboard(c->seat, &c->keyboard);
  wlr_seat_keyboard_notify_modifiers(c->seat, &c->keyboard.modifiers);
}

static const struct wlr_keyboard_impl keyboard_impl = {
  .name = "vnc-keyboard",
};

static void send_key(struct wvnc_compositor* c, xkb_keycode_t kc, bool down)
{
  struct wlr_keyboard_key_event ev = {
    .time_msec = now_msec(),
    .keycode = kc - 8,
    .update_state = true,
    .state = down ? WL_KEYBOARD_KEY_STATE_PRESSED :
                    WL_KEYBOARD_KEY_STATE_RELEASED,
  };

  /* Updates xkb state and keycodes[], and emits modifiers */
  wlr_keyboard_notify_key(&c->keyboard, &ev);
  wlr_seat_set_keyboard(c->seat, &c->keyboard);
  wlr_seat_keyboard_notify_key(c->seat, ev.time_msec, ev.keycode, ev.state);
}

static bool key_is_down(struct wvnc_compositor* c, xkb_keycode_t kc)
{
  for (size_t i = 0; i < c->keyboard.num_keycodes; i++)
    if (c->keyboard.keycodes[i] == kc - 8)
      return true;
  return false;
}

static xkb_keycode_t find_keysym_current(struct wvnc_compositor* c,
                                         xkb_keysym_t sym)
{
  struct xkb_keymap* map = c->keyboard.keymap;
  struct xkb_state* state = c->keyboard.xkb_state;
  if (!map)
    return XKB_KEYCODE_INVALID;
  for (xkb_keycode_t kc = xkb_keymap_min_keycode(map);
       kc <= xkb_keymap_max_keycode(map); kc++) {
    if (xkb_state_key_get_one_sym(state, kc) == sym)
      return kc;
  }
  return XKB_KEYCODE_INVALID;
}

/*
 * Find a keycode and the Shift/AltGr state producing sym. Only Shift
 * and AltGr (Mod5) can be faked; the other modifiers (NumLock, Caps
 * Lock, held Ctrl, ...) stay as they are, so each candidate is tried
 * on a scratch copy of the real state. Candidates that change fewer
 * modifiers come first.
 */
static xkb_keycode_t find_keysym_any(struct wvnc_compositor* c,
                                     xkb_keysym_t sym, xkb_mod_mask_t* mods)
{
  struct xkb_keymap* map = c->keyboard.keymap;
  struct xkb_state* state = c->keyboard.xkb_state;
  struct xkb_state* scratch;
  xkb_mod_mask_t shift = 0, l3 = 0, fakeable, depressed, cur;
  xkb_mod_index_t idx;
  xkb_keycode_t found = XKB_KEYCODE_INVALID;

  if (!map)
    return XKB_KEYCODE_INVALID;
  idx = xkb_keymap_mod_get_index(map, XKB_MOD_NAME_SHIFT);
  if (idx != XKB_MOD_INVALID)
    shift = 1u << idx;
  idx = xkb_keymap_mod_get_index(map, "Mod5");
  if (idx != XKB_MOD_INVALID)
    l3 = 1u << idx;
  fakeable = shift | l3;
  depressed = xkb_state_serialize_mods(state, XKB_STATE_MODS_DEPRESSED);
  cur = depressed & fakeable;

  scratch = xkb_state_new(map);
  if (!scratch)
    return XKB_KEYCODE_INVALID;

  xkb_mod_mask_t combos[4] = { 0, shift, l3, shift | l3 };
  for (int changes = 0; changes <= 2 && found == XKB_KEYCODE_INVALID;
       changes++) {
    for (int i = 0; i < 4 && found == XKB_KEYCODE_INVALID; i++) {
      xkb_mod_mask_t combo = combos[i];
      if ((combo & ~fakeable) || __builtin_popcount(combo ^ cur) != changes)
        continue;
      if (i > 0 && combo == combos[0])
        continue;   /* modifier missing from the keymap */
      xkb_state_update_mask(scratch, (depressed & ~fakeable) | combo,
        xkb_state_serialize_mods(state, XKB_STATE_MODS_LATCHED),
        xkb_state_serialize_mods(state, XKB_STATE_MODS_LOCKED),
        xkb_state_serialize_layout(state, XKB_STATE_LAYOUT_DEPRESSED),
        xkb_state_serialize_layout(state, XKB_STATE_LAYOUT_LATCHED),
        xkb_state_serialize_layout(state, XKB_STATE_LAYOUT_LOCKED));
      for (xkb_keycode_t kc = xkb_keymap_min_keycode(map);
           kc <= xkb_keymap_max_keycode(map); kc++) {
        if (xkb_state_key_get_one_sym(scratch, kc) == sym) {
          found = kc;
          *mods = combo;
          break;
        }
      }
    }
  }
  xkb_state_unref(scratch);
  return found;
}

static void update_leds(struct wvnc_compositor* c)
{
  struct xkb_state* state = c->keyboard.xkb_state;
  unsigned int leds = 0;
  if (!state)
    return;
  if (xkb_state_led_name_is_active(state, XKB_LED_NAME_SCROLL) > 0)
    leds |= WVNC_LED_SCROLL;
  if (xkb_state_led_name_is_active(state, XKB_LED_NAME_NUM) > 0)
    leds |= WVNC_LED_NUM;
  if (xkb_state_led_name_is_active(state, XKB_LED_NAME_CAPS) > 0)
    leds |= WVNC_LED_CAPS;
  if (leds != c->leds) {
    c->leds = leds;
    if (c->cb.leds)
      c->cb.leds(c->cb.data, leds);
  }
}

void wvnc_sync_leds(struct wvnc_compositor* c)
{
  c->leds = ~0u;   /* report even if nothing is lit */
  update_leds(c);
}

static xkb_keycode_t qnum_to_keycode(uint32_t qnum)
{
  if (qnum == 0 || qnum >= code_map_qnum_to_xorgevdev_len)
    return XKB_KEYCODE_INVALID;
  if (code_map_qnum_to_xorgevdev[qnum] == 0)
    return XKB_KEYCODE_INVALID;
  return code_map_qnum_to_xorgevdev[qnum];
}

/* Keypad keys: the ones NumLock (or a fake Shift) switches */
static bool key_is_keypad(struct wvnc_compositor* c, xkb_keycode_t kc)
{
  struct xkb_keymap* map = c->keyboard.keymap;
  int levels = xkb_keymap_num_levels_for_key(map, kc, 0);
  for (int l = 0; l < levels; l++) {
    const xkb_keysym_t* syms;
    int n = xkb_keymap_key_get_syms_by_level(map, kc, 0, l, &syms);
    for (int i = 0; i < n; i++)
      if (syms[i] >= XKB_KEY_KP_Space && syms[i] <= XKB_KEY_KP_9)
        return true;
  }
  return false;
}

/* Same pairs as Xvnc's altKeysym table (keypad part) */
static const struct { xkb_keysym_t kp, plain; } keypad_alt[] = {
  { XKB_KEY_KP_Space, XKB_KEY_space }, { XKB_KEY_KP_Tab, XKB_KEY_Tab },
  { XKB_KEY_KP_Enter, XKB_KEY_Return }, { XKB_KEY_KP_F1, XKB_KEY_F1 },
  { XKB_KEY_KP_F2, XKB_KEY_F2 }, { XKB_KEY_KP_F3, XKB_KEY_F3 },
  { XKB_KEY_KP_F4, XKB_KEY_F4 }, { XKB_KEY_KP_Home, XKB_KEY_Home },
  { XKB_KEY_KP_Left, XKB_KEY_Left }, { XKB_KEY_KP_Up, XKB_KEY_Up },
  { XKB_KEY_KP_Right, XKB_KEY_Right }, { XKB_KEY_KP_Down, XKB_KEY_Down },
  { XKB_KEY_KP_Page_Up, XKB_KEY_Page_Up },
  { XKB_KEY_KP_Page_Down, XKB_KEY_Page_Down },
  { XKB_KEY_KP_End, XKB_KEY_End }, { XKB_KEY_KP_Begin, XKB_KEY_Begin },
  { XKB_KEY_KP_Insert, XKB_KEY_Insert },
  { XKB_KEY_KP_Delete, XKB_KEY_Delete },
  { XKB_KEY_KP_Equal, XKB_KEY_equal },
  { XKB_KEY_KP_Multiply, XKB_KEY_asterisk },
  { XKB_KEY_KP_Add, XKB_KEY_plus },
  { XKB_KEY_KP_Separator, XKB_KEY_comma },
  { XKB_KEY_KP_Subtract, XKB_KEY_minus },
  { XKB_KEY_KP_Decimal, XKB_KEY_period },
  { XKB_KEY_KP_Divide, XKB_KEY_slash },
  { XKB_KEY_KP_0, XKB_KEY_0 }, { XKB_KEY_KP_1, XKB_KEY_1 },
  { XKB_KEY_KP_2, XKB_KEY_2 }, { XKB_KEY_KP_3, XKB_KEY_3 },
  { XKB_KEY_KP_4, XKB_KEY_4 }, { XKB_KEY_KP_5, XKB_KEY_5 },
  { XKB_KEY_KP_6, XKB_KEY_6 }, { XKB_KEY_KP_7, XKB_KEY_7 },
  { XKB_KEY_KP_8, XKB_KEY_8 }, { XKB_KEY_KP_9, XKB_KEY_9 },
};

static bool shift_change_needed(struct wvnc_compositor* c,
                                xkb_mod_mask_t mods)
{
  xkb_mod_index_t shift = xkb_keymap_mod_get_index(c->keyboard.keymap,
                                                   XKB_MOD_NAME_SHIFT);
  xkb_mod_mask_t active = xkb_state_serialize_mods(c->keyboard.xkb_state,
                                                   XKB_STATE_MODS_DEPRESSED);
  if (shift == XKB_MOD_INVALID)
    return false;
  return ((mods ^ active) & (1u << shift)) != 0;
}

static bool mod_held(struct wvnc_compositor* c, const char* name)
{
  return c->keyboard.xkb_state &&
    xkb_state_mod_name_is_active(c->keyboard.xkb_state, name,
                                 XKB_STATE_MODS_DEPRESSED) > 0;
}

/* An exclusive keyboard layer surface (e.g. a lock screen) has focus */
static bool exclusive_layer_focused(struct wvnc_compositor* c)
{
  struct wlr_surface* f = c->seat->keyboard_state.focused_surface;
  struct layer* l;
  if (!f)
    return false;
  wl_list_for_each(l, &c->layers, link) {
    if (l->layer_surface->surface == f &&
        l->layer_surface->current.keyboard_interactive ==
          ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_EXCLUSIVE)
      return true;
  }
  return false;
}

static void process_key(struct wvnc_compositor* c, uint32_t keysym,
                        uint32_t qnum, bool down, bool raw,
                        bool avoid_numlock);

void wvnc_key_event(struct wvnc_compositor* c, uint32_t keysym,
                    uint32_t qnum, bool down, bool raw, bool avoid_numlock)
{
  uint32_t sym = keysym;

  if (sym == 0 && qnum) {
    xkb_keycode_t kc = qnum_to_keycode(qnum);
    if (kc != XKB_KEYCODE_INVALID && c->keyboard.xkb_state)
      sym = xkb_state_key_get_one_sym(c->keyboard.xkb_state, kc);
  }

  /*
   * Window switcher. Keys still go through process_key() so that the
   * keyboard state stays right, but while the switcher is open no
   * client has keyboard focus, so nobody receives them.
   */
  if (down && (sym == XKB_KEY_Tab || sym == XKB_KEY_ISO_Left_Tab) &&
      (c->sw.active || (mod_held(c, XKB_MOD_NAME_ALT) &&
                        !exclusive_layer_focused(c)))) {
    bool back = sym == XKB_KEY_ISO_Left_Tab ||
                mod_held(c, XKB_MOD_NAME_SHIFT);
    if (c->sw.active) {
      switcher_step(c, back ? -1 : 1);
    } else if (switcher_begin(c)) {
      c->sw.sel = back ? c->sw.n - 1 : (c->sw.n > 1 ? 1 : 0);
      switcher_render(c);
    }
  } else if (down && c->sw.active) {
    switch (sym) {
    case XKB_KEY_Up: case XKB_KEY_Left: case XKB_KEY_KP_Up:
      switcher_step(c, -1);
      break;
    case XKB_KEY_Down: case XKB_KEY_Right: case XKB_KEY_KP_Down:
      switcher_step(c, 1);
      break;
    }
  }

  process_key(c, keysym, qnum, down, raw, avoid_numlock);

  if (!c->sw.active)
    return;
  if (down && sym == XKB_KEY_Escape)
    switcher_finish(c, false);
  else if (down && (sym == XKB_KEY_Return || sym == XKB_KEY_KP_Enter))
    switcher_finish(c, true);
  else if (!mod_held(c, XKB_MOD_NAME_ALT))
    switcher_finish(c, true);   /* Alt released */
}

static void process_key(struct wvnc_compositor* c, uint32_t keysym,
                        uint32_t qnum, bool down, bool raw,
                        bool avoid_numlock)
{
  uint32_t id = qnum ? (0x80000000u | qnum) : keysym;
  xkb_keycode_t kc = XKB_KEYCODE_INVALID;

  if (!down) {
    for (int i = 0; i < c->num_pressed; i++) {
      if (c->pressed[i].id != id)
        continue;
      kc = c->pressed[i].keycode;
      c->pressed[i] = c->pressed[--c->num_pressed];
      break;
    }
    if (kc == XKB_KEYCODE_INVALID && raw)
      kc = qnum_to_keycode(qnum);
    if (kc == XKB_KEYCODE_INVALID)
      kc = find_keysym_current(c, keysym);
    if (kc != XKB_KEYCODE_INVALID && key_is_down(c, kc))
      send_key(c, kc, false);
    update_leds(c);
    return;
  }

  xkb_mod_mask_t mods = 0;
  bool fake_mods = false;

  if (raw)
    kc = qnum_to_keycode(qnum);

  if (kc == XKB_KEYCODE_INVALID)
    kc = find_keysym_current(c, keysym);

  if (kc == XKB_KEYCODE_INVALID) {
    kc = find_keysym_any(c, keysym, &mods);
    fake_mods = (kc != XKB_KEYCODE_INVALID);
  }

  /*
   * Like Xvnc: a fake Shift on a keypad key acts like NumLock in X11
   * but confuses applications elsewhere, so use the equivalent key
   * outside the keypad if there is one.
   */
  if (fake_mods && avoid_numlock && shift_change_needed(c, mods) &&
      key_is_keypad(c, kc)) {
    for (size_t i = 0; i < sizeof(keypad_alt) / sizeof(keypad_alt[0]); i++) {
      xkb_keysym_t alt;
      xkb_keycode_t kc2;
      xkb_mod_mask_t mods2 = 0;
      if (keypad_alt[i].kp == keysym)
        alt = keypad_alt[i].plain;
      else if (keypad_alt[i].plain == keysym)
        alt = keypad_alt[i].kp;
      else
        continue;
      kc2 = find_keysym_current(c, alt);
      if (kc2 != XKB_KEYCODE_INVALID) {
        kc = kc2;
        fake_mods = false;
        break;
      }
      kc2 = find_keysym_any(c, alt, &mods2);
      if (kc2 == XKB_KEYCODE_INVALID ||
          (shift_change_needed(c, mods2) && key_is_keypad(c, kc2)))
        continue;
      kc = kc2;
      mods = mods2;
      break;
    }
  }

  /* A keypad keysym that needs a NumLock state we cannot fake (e.g.
   * XK_KP_4 with NumLock off): the plain key is closer than nothing */
  if (kc == XKB_KEYCODE_INVALID) {
    for (size_t i = 0; i < sizeof(keypad_alt) / sizeof(keypad_alt[0]); i++) {
      if (keypad_alt[i].kp != keysym)
        continue;
      kc = find_keysym_current(c, keypad_alt[i].plain);
      if (kc == XKB_KEYCODE_INVALID) {
        kc = find_keysym_any(c, keypad_alt[i].plain, &mods);
        fake_mods = (kc != XKB_KEYCODE_INVALID);
      }
      break;
    }
  }

  if (kc == XKB_KEYCODE_INVALID) {
    wlr_log(WLR_DEBUG, "No key for keysym 0x%x, ignoring", keysym);
    return;
  }

  /* Fake the needed Shift / AltGr presses for this one key, and
   * temporarily release Shift if the level requires it to be up */
  xkb_keycode_t faked[4];
  int nfaked = 0;
  xkb_keycode_t released[4];
  int nreleased = 0;

  if (fake_mods) {
    struct xkb_keymap* map = c->keyboard.keymap;
    xkb_mod_index_t shift = xkb_keymap_mod_get_index(map, XKB_MOD_NAME_SHIFT);
    xkb_mod_index_t l3 = xkb_keymap_mod_get_index(map, "Mod5");
    xkb_mod_mask_t active = xkb_state_serialize_mods(c->keyboard.xkb_state,
                                                     XKB_STATE_MODS_DEPRESSED);
    xkb_mod_mask_t dummy;
    bool need_shift = shift != XKB_MOD_INVALID && (mods & (1u << shift));
    bool have_shift = shift != XKB_MOD_INVALID && (active & (1u << shift));
    bool need_l3 = l3 != XKB_MOD_INVALID && (mods & (1u << l3));
    bool have_l3 = l3 != XKB_MOD_INVALID && (active & (1u << l3));

    if (need_shift && !have_shift) {
      xkb_keycode_t s = find_keysym_any(c, XKB_KEY_Shift_L, &dummy);
      if (s != XKB_KEYCODE_INVALID) {
        send_key(c, s, true);
        faked[nfaked++] = s;
      }
    } else if (!need_shift && have_shift) {
      static const xkb_keysym_t shifts[] = { XKB_KEY_Shift_L, XKB_KEY_Shift_R };
      for (int i = 0; i < 2; i++) {
        xkb_keycode_t s = find_keysym_any(c, shifts[i], &dummy);
        if (s != XKB_KEYCODE_INVALID && key_is_down(c, s)) {
          send_key(c, s, false);
          released[nreleased++] = s;
        }
      }
    }
    if (need_l3 && !have_l3) {
      xkb_keycode_t s = find_keysym_any(c, XKB_KEY_ISO_Level3_Shift, &dummy);
      if (s != XKB_KEYCODE_INVALID) {
        send_key(c, s, true);
        faked[nfaked++] = s;
      }
    }
  }

  /* Wayland clients do their own key repeat, so VNC client side
   * repeats (press without release) are not forwarded again */
  if (!key_is_down(c, kc)) {
    send_key(c, kc, true);
    if (c->num_pressed < MAX_PRESSED_KEYS) {
      c->pressed[c->num_pressed].id = id;
      c->pressed[c->num_pressed].keycode = kc;
      c->num_pressed++;
    }
  }

  for (int i = nfaked - 1; i >= 0; i--)
    send_key(c, faked[i], false);
  for (int i = 0; i < nreleased; i++)
    send_key(c, released[i], true);

  update_leds(c);
}

/* ------------------------------------------------------------------ */
/* Clipboard                                                           */
/* ------------------------------------------------------------------ */

static const char* text_mime_types[] = {
  "text/plain;charset=utf-8",
  "UTF8_STRING",
  "text/plain",
  "TEXT",
  "STRING",
};
#define NUM_TEXT_MIME_TYPES (sizeof(text_mime_types) / sizeof(*text_mime_types))

struct pending_fd {
  struct wl_list link;
  int fd;
};

static void pending_write_free(struct pending_write* w)
{
  if (w->source)
    wl_event_source_remove(w->source);
  close(w->fd);
  free(w->data);
  wl_list_remove(&w->link);
  free(w);
}

static int pending_write_ready(int fd, uint32_t mask, void* data)
{
  struct pending_write* w = data;
  (void)fd;
  if (mask & (WL_EVENT_HANGUP | WL_EVENT_ERROR)) {
    pending_write_free(w);
    return 0;
  }
  while (w->off < w->len) {
    ssize_t n = write(w->fd, w->data + w->off, w->len - w->off);
    if (n < 0) {
      if (errno == EINTR)
        continue;
      if (errno == EAGAIN)
        return 0;
      break;
    }
    w->off += n;
  }
  pending_write_free(w);
  return 0;
}

static void start_write(struct wvnc_compositor* c, int fd)
{
  struct pending_write* w = calloc(1, sizeof(*w));
  w->c = c;
  w->fd = fd;
  w->len = c->vnc_data_len;
  w->data = malloc(w->len ? w->len : 1);
  memcpy(w->data, c->vnc_data, w->len);
  set_nonblock(fd);
  wl_list_insert(&c->pending_writes, &w->link);
  w->source = wl_event_loop_add_fd(c->loop, fd, WL_EVENT_WRITABLE,
                                   pending_write_ready, w);
}

static void drop_pending_fds(struct wvnc_compositor* c)
{
  struct pending_fd *p, *tmp;
  wl_list_for_each_safe(p, tmp, &c->pending_fds, link) {
    close(p->fd);
    wl_list_remove(&p->link);
    free(p);
  }
}

static void vnc_source_send_fd(struct wvnc_compositor* c, int fd)
{
  if (c->vnc_data_valid) {
    start_write(c, fd);
    return;
  }

  struct pending_fd* p = calloc(1, sizeof(*p));
  p->fd = fd;
  wl_list_insert(&c->pending_fds, &p->link);
  if (!c->vnc_data_requested) {
    c->vnc_data_requested = true;
    if (c->cb.selection_wanted)
      c->cb.selection_wanted(c->cb.data);
  }
}

static void vnc_data_source_send(struct wlr_data_source* source,
                                 const char* mime, int32_t fd)
{
  struct vnc_data_source* s = wl_container_of(source, s, base);
  (void)mime;
  vnc_source_send_fd(s->c, fd);
}

static void vnc_data_source_destroy(struct wlr_data_source* source)
{
  struct vnc_data_source* s = wl_container_of(source, s, base);
  free(s);
}

static const struct wlr_data_source_impl vnc_data_source_impl = {
  .send = vnc_data_source_send,
  .destroy = vnc_data_source_destroy,
};

static void vnc_primary_source_send(struct wlr_primary_selection_source* source,
                                    const char* mime, int fd)
{
  struct vnc_primary_source* s = wl_container_of(source, s, base);
  (void)mime;
  vnc_source_send_fd(s->c, fd);
}

static void vnc_primary_source_destroy(struct wlr_primary_selection_source* source)
{
  struct vnc_primary_source* s = wl_container_of(source, s, base);
  free(s);
}

static const struct wlr_primary_selection_source_impl vnc_primary_source_impl = {
  .send = vnc_primary_source_send,
  .destroy = vnc_primary_source_destroy,
};

static void add_text_mime_types(struct wl_array* types)
{
  for (size_t i = 0; i < NUM_TEXT_MIME_TYPES; i++) {
    char** p = wl_array_add(types, sizeof(char*));
    *p = strdup(text_mime_types[i]);
  }
}

static bool is_vnc_source(struct wlr_seat* seat, enum wvnc_selection which)
{
  if (which == WVNC_SELECTION_CLIPBOARD)
    return seat->selection_source &&
           seat->selection_source->impl == &vnc_data_source_impl;
  return seat->primary_selection_source &&
         seat->primary_selection_source->impl == &vnc_primary_source_impl;
}

void wvnc_set_vnc_selection(struct wvnc_compositor* c, bool available,
                            bool also_primary)
{
  uint32_t serial = wl_display_next_serial(c->display);

  /* A new announcement invalidates previous data */
  free(c->vnc_data);
  c->vnc_data = NULL;
  c->vnc_data_len = 0;
  c->vnc_data_valid = false;
  c->vnc_data_requested = false;
  drop_pending_fds(c);

  if (!available) {
    if (is_vnc_source(c->seat, WVNC_SELECTION_CLIPBOARD))
      wlr_seat_set_selection(c->seat, NULL, serial);
    if (is_vnc_source(c->seat, WVNC_SELECTION_PRIMARY))
      wlr_seat_set_primary_selection(c->seat, NULL, serial);
    return;
  }

  struct vnc_data_source* ds = calloc(1, sizeof(*ds));
  ds->c = c;
  wlr_data_source_init(&ds->base, &vnc_data_source_impl);
  add_text_mime_types(&ds->base.mime_types);
  wlr_seat_set_selection(c->seat, &ds->base, serial);

  if (also_primary) {
    struct vnc_primary_source* ps = calloc(1, sizeof(*ps));
    ps->c = c;
    wlr_primary_selection_source_init(&ps->base, &vnc_primary_source_impl);
    add_text_mime_types(&ps->base.mime_types);
    wlr_seat_set_primary_selection(c->seat, &ps->base, serial);
  }
}

void wvnc_vnc_selection_data(struct wvnc_compositor* c, const char* text,
                             size_t len)
{
  struct pending_fd *p, *tmp;

  free(c->vnc_data);
  c->vnc_data = malloc(len ? len : 1);
  memcpy(c->vnc_data, text, len);
  c->vnc_data_len = len;
  c->vnc_data_valid = true;
  c->vnc_data_requested = false;

  wl_list_for_each_safe(p, tmp, &c->pending_fds, link) {
    start_write(c, p->fd);
    wl_list_remove(&p->link);
    free(p);
  }
}

static void reader_free(struct pending_read* r)
{
  if (r->source)
    wl_event_source_remove(r->source);
  close(r->fd);
  free(r->data);
  if (r->c->reader == r)
    r->c->reader = NULL;
  free(r);
}

static int reader_ready(int fd, uint32_t mask, void* data)
{
  struct pending_read* r = data;
  struct wvnc_compositor* c = r->c;

  for (;;) {
    if (r->cap - r->len < 4096) {
      size_t ncap = r->cap ? r->cap * 2 : 8192;
      if (ncap > MAX_SELECTION_SIZE + 8192)
        break;
      char* n = realloc(r->data, ncap);
      if (!n)
        break;
      r->data = n;
      r->cap = ncap;
    }
    ssize_t n = read(fd, r->data + r->len, r->cap - r->len - 1);
    if (n < 0) {
      if (errno == EINTR)
        continue;
      if (errno == EAGAIN && !(mask & (WL_EVENT_HANGUP | WL_EVENT_ERROR)))
        return 0;
      if (errno == EAGAIN)
        return 0;
      break;
    }
    if (n == 0) {
      r->data[r->len] = '\0';
      if (c->cb.selection_data)
        c->cb.selection_data(c->cb.data, r->data, r->len);
      reader_free(r);
      return 0;
    }
    r->len += n;
    if (r->len > MAX_SELECTION_SIZE)
      break;
  }

  wlr_log(WLR_ERROR, "Failed to read selection data");
  reader_free(r);
  return 0;
}

static const char* pick_mime(struct wl_array* types)
{
  for (size_t i = 0; i < NUM_TEXT_MIME_TYPES; i++) {
    char** t;
    wl_array_for_each(t, types) {
      if (strcmp(*t, text_mime_types[i]) == 0)
        return *t;
    }
  }
  return NULL;
}

void wvnc_request_selection(struct wvnc_compositor* c)
{
  int fds[2];
  const char* mime;
  enum wvnc_selection which = c->last_announced;

  if (which == WVNC_SELECTION_CLIPBOARD) {
    if (!c->seat->selection_source ||
        is_vnc_source(c->seat, WVNC_SELECTION_CLIPBOARD))
      return;
    mime = pick_mime(&c->seat->selection_source->mime_types);
  } else {
    if (!c->seat->primary_selection_source ||
        is_vnc_source(c->seat, WVNC_SELECTION_PRIMARY))
      return;
    mime = pick_mime(&c->seat->primary_selection_source->mime_types);
  }
  if (!mime) {
    wlr_log(WLR_INFO, "Selection has no text representation");
    return;
  }

  if (c->reader)
    reader_free(c->reader);

  if (pipe2(fds, O_CLOEXEC) < 0)
    return;
  set_nonblock(fds[0]);

  struct pending_read* r = calloc(1, sizeof(*r));
  r->c = c;
  r->fd = fds[0];
  r->source = wl_event_loop_add_fd(c->loop, fds[0], WL_EVENT_READABLE,
                                   reader_ready, r);
  c->reader = r;

  /* The source takes ownership of the write end */
  if (which == WVNC_SELECTION_CLIPBOARD)
    wlr_data_source_send(c->seat->selection_source, mime, fds[1]);
  else
    wlr_primary_selection_source_send(c->seat->primary_selection_source,
                                      mime, fds[1]);
}

static void handle_request_set_selection(struct wl_listener* listener,
                                         void* data)
{
  struct wvnc_compositor* c =
    wl_container_of(listener, c, request_set_selection);
  struct wlr_seat_request_set_selection_event* ev = data;
  wlr_seat_set_selection(c->seat, ev->source, ev->serial);
}

static void handle_request_set_primary_selection(struct wl_listener* listener,
                                                 void* data)
{
  struct wvnc_compositor* c =
    wl_container_of(listener, c, request_set_primary_selection);
  struct wlr_seat_request_set_primary_selection_event* ev = data;
  wlr_seat_set_primary_selection(c->seat, ev->source, ev->serial);
}

static void handle_set_selection(struct wl_listener* listener, void* data)
{
  struct wvnc_compositor* c = wl_container_of(listener, c, set_selection);
  (void)data;
  nested_bridge_host_selection(c->bridge);
  if (is_vnc_source(c->seat, WVNC_SELECTION_CLIPBOARD))
    return;
  c->last_announced = WVNC_SELECTION_CLIPBOARD;
  if (c->cb.selection_announce)
    c->cb.selection_announce(c->cb.data, WVNC_SELECTION_CLIPBOARD,
                             c->seat->selection_source != NULL);
}

static void handle_set_primary_selection(struct wl_listener* listener,
                                         void* data)
{
  struct wvnc_compositor* c =
    wl_container_of(listener, c, set_primary_selection);
  (void)data;
  nested_bridge_host_primary(c->bridge);
  if (is_vnc_source(c->seat, WVNC_SELECTION_PRIMARY))
    return;
  /* Losing PRIMARY doesn't clear the VNC clipboard (same as Xvnc) */
  if (!c->seat->primary_selection_source)
    return;
  if (c->cb.selection_announce)
    c->cb.selection_announce(c->cb.data, WVNC_SELECTION_PRIMARY, true);
}

/* Called by the C++ side once it has decided to forward PRIMARY */
void wvnc_note_primary_announced(struct wvnc_compositor* c);
void wvnc_note_primary_announced(struct wvnc_compositor* c)
{
  c->last_announced = WVNC_SELECTION_PRIMARY;
}

/* ------------------------------------------------------------------ */
/* Setup                                                               */
/* ------------------------------------------------------------------ */

static void setup_keyboard(struct wvnc_compositor* c)
{
  struct xkb_context* ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
  /* NULL names: honours XKB_DEFAULT_{RULES,MODEL,LAYOUT,VARIANT,OPTIONS} */
  struct xkb_keymap* keymap =
    xkb_keymap_new_from_names(ctx, NULL, XKB_KEYMAP_COMPILE_NO_FLAGS);

  wlr_keyboard_init(&c->keyboard, &keyboard_impl, "vnc-keyboard");
  if (keymap)
    wlr_keyboard_set_keymap(&c->keyboard, keymap);
  else
    wlr_log(WLR_ERROR, "Failed to compile keymap");
  wlr_keyboard_set_repeat_info(&c->keyboard, 25, 600);
  xkb_keymap_unref(keymap);
  xkb_context_unref(ctx);

  c->keyboard_modifiers.notify = keyboard_modifiers;
  wl_signal_add(&c->keyboard.events.modifiers, &c->keyboard_modifiers);
  wlr_seat_set_keyboard(c->seat, &c->keyboard);
}

static struct wvnc_callbacks* log_cb;

void wvnc_log_info(const char* fmt, ...)
{
  char buf[512];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  if (log_cb && log_cb->info)
    log_cb->info(log_cb->data, buf);
  else
    wlr_log(WLR_INFO, "%s", buf);
}

struct wvnc_compositor* wvnc_compositor_create(const struct wvnc_options* opts,
                                               const struct wvnc_callbacks* cb)
{
  struct wvnc_compositor* c = calloc(1, sizeof(*c));
  const char* dbg = getenv("TIGERVNC_WLR_DEBUG");
  char* saved_scanout_env;

  wlr_log_init(dbg && *dbg ? WLR_DEBUG : WLR_ERROR, NULL);

  /* We read rendered frames back from the output buffer, so the
   * scene must always composite into it. wlroots reads this in
   * wlr_scene_create(); the session (e.g. a nested compositor) must
   * not inherit it, so the old value is put back afterwards. */
  {
    const char* v = getenv("WLR_SCENE_DISABLE_DIRECT_SCANOUT");
    saved_scanout_env = v ? strdup(v) : NULL;
  }
  setenv("WLR_SCENE_DISABLE_DIRECT_SCANOUT", "1", 1);

  c->cb = *cb;
  c->width = opts->width;
  c->height = opts->height;
  c->nested_mode = opts->nested_mode;
  c->x11_title_bars = opts->x11_title_bars;
  log_cb = &c->cb;
  c->usable_area = (struct wlr_box){ 0, 0, c->width, c->height };
  wl_list_init(&c->views);
  wl_list_init(&c->layers);
  wl_list_init(&c->pending_writes);
  wl_list_init(&c->pending_fds);

  c->fb = calloc((size_t)c->width * c->height, 4);
  if (!c->fb)
    goto fail;

  c->display = wl_display_create();
  if (!c->display)
    goto fail;
  c->loop = wl_display_get_event_loop(c->display);

  c->backend = wlr_headless_backend_create(c->loop);
  if (!c->backend) {
    wlr_log(WLR_ERROR, "Failed to create headless backend");
    goto fail;
  }

  c->renderer = wlr_pixman_renderer_create();
  if (!c->renderer) {
    wlr_log(WLR_ERROR, "Failed to create pixman renderer");
    goto fail;
  }
  wlr_renderer_init_wl_shm(c->renderer, c->display);

  c->allocator = wlr_allocator_autocreate(c->backend, c->renderer);
  if (!c->allocator) {
    wlr_log(WLR_ERROR, "Failed to create allocator");
    goto fail;
  }

  c->compositor = wlr_compositor_create(c->display, 6, c->renderer);
  wlr_subcompositor_create(c->display);
  wlr_data_device_manager_create(c->display);
  wlr_primary_selection_v1_device_manager_create(c->display);
  wlr_data_control_manager_v1_create(c->display);
  wlr_ext_data_control_manager_v1_create(c->display, 1);
  wlr_viewporter_create(c->display);
  wlr_single_pixel_buffer_manager_v1_create(c->display);
  wlr_presentation_create(c->display, c->backend, 2);
  wlr_fractional_scale_manager_v1_create(c->display, 1);
  wlr_screencopy_manager_v1_create(c->display);

  c->output_layout = wlr_output_layout_create(c->display);
  c->output_manager = wlr_output_manager_v1_create(c->display);
  c->output_manager_apply.notify = handle_output_manager_apply;
  wl_signal_add(&c->output_manager->events.apply, &c->output_manager_apply);
  c->output_manager_test.notify = handle_output_manager_test;
  wl_signal_add(&c->output_manager->events.test, &c->output_manager_test);
  wlr_xdg_output_manager_v1_create(c->display, c->output_layout);

  c->scene = wlr_scene_create();
  if (saved_scanout_env) {
    setenv("WLR_SCENE_DISABLE_DIRECT_SCANOUT", saved_scanout_env, 1);
    free(saved_scanout_env);
    saved_scanout_env = NULL;
  } else {
    unsetenv("WLR_SCENE_DISABLE_DIRECT_SCANOUT");
  }
  c->scene_layout = wlr_scene_attach_output_layout(c->scene, c->output_layout);

  {
    const float bg[4] = { 0.18f, 0.20f, 0.25f, 1.0f };
    c->background = wlr_scene_rect_create(&c->scene->tree, c->width,
                                          c->height, bg);
  }
  c->layer_trees[ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND] =
    wlr_scene_tree_create(&c->scene->tree);
  c->layer_trees[ZWLR_LAYER_SHELL_V1_LAYER_BOTTOM] =
    wlr_scene_tree_create(&c->scene->tree);
  c->views_tree = wlr_scene_tree_create(&c->scene->tree);
  c->layer_trees[ZWLR_LAYER_SHELL_V1_LAYER_TOP] =
    wlr_scene_tree_create(&c->scene->tree);
  c->unmanaged_tree = wlr_scene_tree_create(&c->scene->tree);
  c->overlay_tree = wlr_scene_tree_create(&c->scene->tree);
  c->layer_trees[ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY] = c->overlay_tree;
  c->switcher_tree = wlr_scene_tree_create(&c->scene->tree);

  c->new_output.notify = handle_new_output;
  wl_signal_add(&c->backend->events.new_output, &c->new_output);

  c->xdg_shell = wlr_xdg_shell_create(c->display, 6);
  c->new_xdg_toplevel.notify = handle_new_xdg_toplevel;
  wl_signal_add(&c->xdg_shell->events.new_toplevel, &c->new_xdg_toplevel);
  c->new_xdg_popup.notify = handle_new_xdg_popup;
  wl_signal_add(&c->xdg_shell->events.new_popup, &c->new_xdg_popup);

  c->icon_manager = wlr_xdg_toplevel_icon_manager_v1_create(c->display, 1);
  if (c->icon_manager) {
    int sizes[] = { 32, 48, 64 };
    wlr_xdg_toplevel_icon_manager_v1_set_sizes(c->icon_manager, sizes, 3);
    c->set_icon.notify = handle_set_icon;
    wl_signal_add(&c->icon_manager->events.set_icon, &c->set_icon);
  }

  c->layer_shell = wlr_layer_shell_v1_create(c->display, 4);
  c->new_layer_surface.notify = handle_new_layer_surface;
  wl_signal_add(&c->layer_shell->events.new_surface, &c->new_layer_surface);

  c->seat = wlr_seat_create(c->display, "seat0");
  wlr_seat_set_capabilities(c->seat, WL_SEAT_CAPABILITY_POINTER |
                                     WL_SEAT_CAPABILITY_KEYBOARD);
  setup_keyboard(c);
  c->request_set_cursor.notify = handle_request_set_cursor;
  wl_signal_add(&c->seat->events.request_set_cursor, &c->request_set_cursor);
  c->request_set_selection.notify = handle_request_set_selection;
  wl_signal_add(&c->seat->events.request_set_selection,
                &c->request_set_selection);
  c->request_set_primary_selection.notify =
    handle_request_set_primary_selection;
  wl_signal_add(&c->seat->events.request_set_primary_selection,
                &c->request_set_primary_selection);
  c->set_selection.notify = handle_set_selection;
  wl_signal_add(&c->seat->events.set_selection, &c->set_selection);
  c->set_primary_selection.notify = handle_set_primary_selection;
  wl_signal_add(&c->seat->events.set_primary_selection,
                &c->set_primary_selection);

  c->xcursor_mgr = wlr_xcursor_manager_create(getenv("XCURSOR_THEME"), 24);
  wlr_xcursor_manager_load(c->xcursor_mgr, 1);

  if (opts->socket_name && *opts->socket_name) {
    if (wl_display_add_socket(c->display, opts->socket_name) != 0) {
      wlr_log(WLR_ERROR, "Unable to create Wayland socket \"%s\" (is it "
              "in use, and is XDG_RUNTIME_DIR set?)", opts->socket_name);
      goto fail;
    }
    c->socket = opts->socket_name;
  } else {
    c->socket = wl_display_add_socket_auto(c->display);
    if (!c->socket) {
      wlr_log(WLR_ERROR, "Unable to create Wayland socket");
      goto fail;
    }
  }

  if (!wlr_backend_start(c->backend)) {
    wlr_log(WLR_ERROR, "Failed to start backend");
    goto fail;
  }

  if (!wlr_headless_add_output(c->backend, c->width, c->height) ||
      !c->output) {
    wlr_log(WLR_ERROR, "Failed to create virtual output");
    goto fail;
  }

#ifdef HAVE_XWAYLAND
  if (opts->xwayland) {
    c->xwayland = wlr_xwayland_create(c->display, c->compositor, false);
    if (!c->xwayland) {
      wlr_log(WLR_ERROR, "Failed to start Xwayland, X11 clients will "
              "not be available");
    } else {
      c->xwayland_ready.notify = handle_xwayland_ready;
      wl_signal_add(&c->xwayland->events.ready, &c->xwayland_ready);
      c->xwayland_new_surface.notify = handle_xwayland_new_surface;
      wl_signal_add(&c->xwayland->events.new_surface,
                    &c->xwayland_new_surface);
    }
  }
#endif

  set_default_cursor(c);

  wvnc_text_init(SW_TEXT_PX);
  wvnc_log_info("Window switcher (Alt+Tab): icon formats %s, %s",
                wvnc_icon_formats(),
                wvnc_text_font_path() ? wvnc_text_font_path() :
                "built-in bitmap font (no TrueType font found)");

  return c;

fail:
  wvnc_compositor_destroy(c);
  return NULL;
}

/* Remove a listener if it was ever added (the struct is calloc'ed) */
static void remove_listener(struct wl_listener* l)
{
  if (l->link.next != NULL) {
    wl_list_remove(&l->link);
    l->link.next = l->link.prev = NULL;
  }
}

void wvnc_compositor_destroy(struct wvnc_compositor* c)
{
  if (!c)
    return;
  if (c->display) {
#ifdef HAVE_XWAYLAND
    if (c->xwayland) {
      wl_list_remove(&c->xwayland_ready.link);
      wl_list_remove(&c->xwayland_new_surface.link);
      wlr_xwayland_destroy(c->xwayland);
    }
#endif
    if (c->reader)
      reader_free(c->reader);
    while (!wl_list_empty(&c->pending_writes)) {
      struct pending_write* w =
        wl_container_of(c->pending_writes.next, w, link);
      pending_write_free(w);
    }
    drop_pending_fds(c);
    nested_bridge_destroy(c->bridge);
    c->bridge = NULL;
    wl_display_destroy_clients(c->display);

    /* wlroots asserts that nobody listens any more when its objects
     * go away */
    remove_listener(&c->output_manager_apply);
    remove_listener(&c->output_manager_test);
    remove_listener(&c->new_xdg_toplevel);
    remove_listener(&c->new_xdg_popup);
    remove_listener(&c->new_layer_surface);
    remove_listener(&c->request_set_cursor);
    remove_listener(&c->request_set_selection);
    remove_listener(&c->request_set_primary_selection);
    remove_listener(&c->set_selection);
    remove_listener(&c->set_primary_selection);
    remove_listener(&c->implicit_grab_destroy);
    remove_listener(&c->set_icon);
#ifdef HAVE_XWAYLAND
    if (c->icon_xcb)
      xcb_disconnect(c->icon_xcb);
    c->icon_xcb = NULL;
#endif
    remove_listener(&c->cursor_surface_destroy);
    remove_listener(&c->cursor_surface_commit);
    if (c->keyboard_modifiers.link.next != NULL) {
      remove_listener(&c->keyboard_modifiers);
      wlr_keyboard_finish(&c->keyboard);
    }
    if (c->backend) {
      remove_listener(&c->new_output);
      wlr_backend_destroy(c->backend);   /* destroys the output too */
    }
    if (c->scene)
      wlr_scene_node_destroy(&c->scene->tree.node);
    if (c->xcursor_mgr)
      wlr_xcursor_manager_destroy(c->xcursor_mgr);
    if (c->allocator)
      wlr_allocator_destroy(c->allocator);
    if (c->renderer)
      wlr_renderer_destroy(c->renderer);
    wl_display_destroy(c->display);
  }
  free(c->vnc_data);
  free(c->fb);
  free(c->old_fb);
  free(c);
}

const char* wvnc_socket_name(struct wvnc_compositor* c)
{
  return c->socket;
}

int wvnc_event_fd(struct wvnc_compositor* c)
{
  return wl_event_loop_get_fd(c->loop);
}

void wvnc_dispatch(struct wvnc_compositor* c)
{
  wl_event_loop_dispatch(c->loop, 0);
}

void wvnc_flush(struct wvnc_compositor* c)
{
  wl_event_loop_dispatch_idle(c->loop);
  wl_display_flush_clients(c->display);
}

bool wvnc_resize(struct wvnc_compositor* c, int width, int height)
{
  struct wlr_output_state state;
  uint32_t* fb;

  if (!c->output || width <= 0 || height <= 0)
    return false;

  if (width == c->width && height == c->height)
    return true;

  fb = calloc((size_t)width * height, 4);
  if (!fb)
    return false;

  wlr_output_state_init(&state);
  wlr_output_state_set_custom_mode(&state, width, height, 60000);
  if (!wlr_output_commit_state(c->output, &state)) {
    wlr_output_state_finish(&state);
    free(fb);
    wlr_log(WLR_ERROR, "Failed to resize output to %dx%d", width, height);
    return false;
  }
  wlr_output_state_finish(&state);

  /* Keep the old picture until the next frame is rendered */
  for (int y = 0; y < height && y < c->height; y++)
    memcpy(fb + (size_t)y * width, c->fb + (size_t)y * c->width,
           (size_t)(width < c->width ? width : c->width) * 4);

  free(c->old_fb);
  c->old_fb = c->fb;
  c->fb = fb;
  c->width = width;
  c->height = height;
  c->full_damage = true;

  wlr_scene_rect_set_size(c->background, width, height);
  /* The old screen layout may not fit any more; the caller sets the
   * new one with wvnc_set_screens() */
  c->num_screens = 0;
  arrange_layers(c);
  rearrange_views(c);
  wlr_output_schedule_frame(c->output);
  update_output_manager(c);

  return true;
}
