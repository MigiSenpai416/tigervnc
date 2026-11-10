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
#include <fcntl.h>
#include <stdlib.h>
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
#include <wlr/util/box.h>
#include <wlr/util/edges.h>
#include <wlr/util/log.h>
#include <wlr/xcursor.h>
#ifdef HAVE_XWAYLAND
#include <wlr/xwayland.h>
#include <xcb/xcb.h>
#endif

#include "compositor.h"

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
#define MAX_SELECTION_SIZE (16 * 1024 * 1024)

enum owner_type {
  OWNER_XDG,
  OWNER_XWAYLAND,
  OWNER_LAYER,
};

/* Common first member of anything we hang off scene node->data */
struct owner {
  enum owner_type type;
};

enum grab_mode {
  GRAB_NONE,
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
  bool cursor_is_default;

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
};

struct view {
  struct owner owner;
  struct wl_list link;
  struct wvnc_compositor* c;
  struct wlr_scene_tree* scene_tree;
  bool mapped;
  bool maximized, fullscreen;
  struct wlr_box saved; /* geometry before maximize/fullscreen */

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

#ifdef HAVE_XWAYLAND
  struct wlr_xwayland_surface* xsurface;
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
  }
#endif
}

static void view_set_activated(struct view* v, bool activated)
{
  if (v->owner.type == OWNER_XDG)
    wlr_xdg_toplevel_set_activated(v->xdg, activated);
#ifdef HAVE_XWAYLAND
  else
    wlr_xwayland_surface_activate(v->xsurface, activated);
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

  if (v->scene_tree)
    wlr_scene_node_raise_to_top(&v->scene_tree->node);
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
    if (v->mapped) {
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

static void view_place(struct view* v)
{
  struct wvnc_compositor* c = v->c;
  struct wlr_box box;
  struct wlr_box* area = &c->usable_area;

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

static void view_maximize(struct view* v, bool maximize, bool fullscreen)
{
  struct wvnc_compositor* c = v->c;
  bool was = v->maximized || v->fullscreen;
  struct wlr_box full = { 0, 0, c->width, c->height };

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

  if (v->fullscreen)
    view_set_box(v, &full);
  else if (v->maximized)
    view_set_box(v, &c->usable_area);
  else if (was && v->saved.width > 0 && v->saved.height > 0)
    view_set_box(v, &v->saved);
  else if (was && v->owner.type == OWNER_XDG)
    wlr_xdg_toplevel_set_size(v->xdg, 0, 0);
}

static void rearrange_views(struct wvnc_compositor* c)
{
  struct view* v;
  wl_list_for_each(v, &c->views, link) {
    if (!v->mapped)
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
}

/* xdg-shell ---------------------------------------------------------- */

static void xdg_map(struct wl_listener* listener, void* data)
{
  struct view* v = wl_container_of(listener, v, map);
  (void)data;
  v->mapped = true;
  if (!v->maximized && !v->fullscreen)
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
  if (c->grab_view == v) {
    c->grab_mode = GRAB_NONE;
    c->grab_view = NULL;
  }
  if (had_focus)
    focus_next_view(c);
}

static void xdg_commit(struct wl_listener* listener, void* data)
{
  struct view* v = wl_container_of(listener, v, commit);
  struct wvnc_compositor* c = v->c;
  (void)data;
  if (!v->xdg->base->initial_commit)
    return;

  wlr_xdg_toplevel_set_wm_capabilities(v->xdg,
    WLR_XDG_TOPLEVEL_WM_CAPABILITIES_MAXIMIZE |
    WLR_XDG_TOPLEVEL_WM_CAPABILITIES_FULLSCREEN);
  wlr_xdg_toplevel_set_bounds(v->xdg, c->usable_area.width,
                              c->usable_area.height);
  if (v->xdg->requested.fullscreen || v->xdg->requested.maximized) {
    struct wlr_box full = { 0, 0, c->width, c->height };
    struct wlr_box* b;
    v->saved = (struct wlr_box){ 0, 0, 0, 0 };
    v->fullscreen = v->xdg->requested.fullscreen;
    v->maximized = !v->fullscreen && v->xdg->requested.maximized;
    b = v->fullscreen ? &full : &c->usable_area;
    wlr_xdg_toplevel_set_maximized(v->xdg, v->maximized);
    wlr_xdg_toplevel_set_fullscreen(v->xdg, v->fullscreen);
    wlr_xdg_toplevel_set_size(v->xdg, b->width, b->height);
    wlr_scene_node_set_position(&v->scene_tree->node, b->x, b->y);
  } else {
    wlr_xdg_toplevel_set_size(v->xdg, 0, 0);
  }
}

static void xdg_destroy(struct wl_listener* listener, void* data)
{
  struct view* v = wl_container_of(listener, v, destroy);
  (void)data;
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
}

static void begin_grab(struct view* v, enum grab_mode mode, uint32_t edges)
{
  struct wvnc_compositor* c = v->c;
  struct wlr_box box;

  if (v->maximized || v->fullscreen)
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
  if (v->mapped)
    view_maximize(v, v->xdg->requested.maximized, v->fullscreen);
  wlr_xdg_surface_schedule_configure(v->xdg->base);
}

static void xdg_request_fullscreen(struct wl_listener* listener, void* data)
{
  struct view* v = wl_container_of(listener, v, request_fullscreen);
  (void)data;
  if (!v->xdg->base->initialized)
    return;
  if (v->mapped)
    view_maximize(v, v->maximized, v->xdg->requested.fullscreen);
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

  if (xs->override_redirect) {
    if (wlr_xwayland_surface_override_redirect_wants_focus(xs))
      focus_surface(c, xs->surface);
    return;
  }

  if (xs->fullscreen)
    view_maximize(v, false, true);
  else if (xs->maximized_horz && xs->maximized_vert)
    view_maximize(v, true, false);

  if (wlr_xwayland_surface_icccm_input_model(xs) !=
      WLR_ICCCM_INPUT_MODEL_NONE)
    focus_view(v);
}

static void xw_unmap(struct wl_listener* listener, void* data)
{
  struct view* v = wl_container_of(listener, v, unmap);
  struct wvnc_compositor* c = v->c;
  bool had_focus = v->xsurface->surface &&
    c->seat->keyboard_state.focused_surface == v->xsurface->surface;
  (void)data;

  v->mapped = false;
  if (c->grab_view == v) {
    c->grab_mode = GRAB_NONE;
    c->grab_view = NULL;
  }
  if (v->scene_tree) {
    wlr_scene_node_destroy(&v->scene_tree->node);
    v->scene_tree = NULL;
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
}

static void xw_request_activate(struct wl_listener* listener, void* data)
{
  struct view* v = wl_container_of(listener, v, request_activate);
  (void)data;
  if (v->mapped && !v->xsurface->override_redirect)
    focus_view(v);
}

static void xw_set_geometry(struct wl_listener* listener, void* data)
{
  struct view* v = wl_container_of(listener, v, set_geometry);
  (void)data;
  if (v->scene_tree)
    wlr_scene_node_set_position(&v->scene_tree->node,
                                v->xsurface->x, v->xsurface->y);
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
  if (v->scene_tree)
    wlr_scene_node_destroy(&v->scene_tree->node);
  wl_list_remove(&v->associate.link);
  wl_list_remove(&v->dissociate.link);
  wl_list_remove(&v->request_configure.link);
  wl_list_remove(&v->request_activate.link);
  wl_list_remove(&v->set_geometry.link);
  wl_list_remove(&v->request_move.link);
  wl_list_remove(&v->request_resize.link);
  wl_list_remove(&v->request_maximize.link);
  wl_list_remove(&v->request_fullscreen.link);
  wl_list_remove(&v->destroy.link);
  wl_list_remove(&v->map.link);
  wl_list_remove(&v->unmap.link);
  wl_list_remove(&v->link);
  free(v);
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

static void set_default_cursor(struct wvnc_compositor* c)
{
  struct wlr_xcursor* xc;

  if (c->cursor_is_default)
    return;

  xc = wlr_xcursor_manager_get_xcursor(c->xcursor_mgr, "default", 1);
  if (!xc)
    xc = wlr_xcursor_manager_get_xcursor(c->xcursor_mgr, "left_ptr", 1);
  if (!xc)
    return;

  struct wlr_xcursor_image* img = xc->images[0];
  send_cursor_argb(c, img->width, img->height, img->hotspot_x,
                   img->hotspot_y, (const uint32_t*)img->buffer,
                   img->width);
  c->cursor_is_default = true;
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
  if (c->grab_edges & WLR_EDGE_TOP) {
    top += dy;
    if (top >= bottom - 1) top = bottom - 1;
  } else if (c->grab_edges & WLR_EDGE_BOTTOM) {
    bottom += dy;
    if (bottom <= top + 1) bottom = top + 1;
  }
  if (c->grab_edges & WLR_EDGE_LEFT) {
    left += dx;
    if (left >= right - 1) left = right - 1;
  } else if (c->grab_edges & WLR_EDGE_RIGHT) {
    right += dx;
    if (right <= left + 1) right = left + 1;
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
    if (c->seat->pointer_state.focused_surface != surface)
      c->cursor_is_default = false;
    wlr_seat_pointer_notify_enter(c->seat, surface, sx, sy);
    wlr_seat_pointer_notify_motion(c->seat, time, sx, sy);
  } else {
    clear_cursor_surface(c);
    wlr_seat_pointer_clear_focus(c->seat);
    set_default_cursor(c);
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

  /* First button press: focus/raise and start an implicit grab */
  if ((changed & mask & 0x7) && c->grab_mode == GRAB_NONE &&
      (c->button_mask & 0x7) == 0) {
    double sx, sy;
    struct owner* owner;
    struct wlr_surface* s = surface_at(c, c->px, c->py, &sx, &sy, &owner);
    focus_owner(c, owner, s);
    clear_implicit_grab(c);
    if (s) {
      c->implicit_grab = s;
      c->implicit_grab_destroy.notify = implicit_grab_destroy;
      wl_signal_add(&s->events.destroy, &c->implicit_grab_destroy);
    }
  }

  c->button_mask = mask;

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

/* Find a keycode and the modifier mask producing sym at any level */
static xkb_keycode_t find_keysym_any(struct wvnc_compositor* c,
                                     xkb_keysym_t sym, xkb_mod_mask_t* mods)
{
  struct xkb_keymap* map = c->keyboard.keymap;
  struct xkb_state* state = c->keyboard.xkb_state;
  if (!map)
    return XKB_KEYCODE_INVALID;
  for (xkb_keycode_t kc = xkb_keymap_min_keycode(map);
       kc <= xkb_keymap_max_keycode(map); kc++) {
    xkb_layout_index_t layout = xkb_state_key_get_layout(state, kc);
    if (layout == XKB_LAYOUT_INVALID)
      continue;
    xkb_level_index_t levels = xkb_keymap_num_levels_for_key(map, kc, layout);
    for (xkb_level_index_t lvl = 0; lvl < levels; lvl++) {
      const xkb_keysym_t* syms;
      int n = xkb_keymap_key_get_syms_by_level(map, kc, layout, lvl, &syms);
      if (n != 1 || syms[0] != sym)
        continue;
      xkb_mod_mask_t masks[8];
      size_t nm = xkb_keymap_key_get_mods_for_level(map, kc, layout, lvl,
                                                    masks, 8);
      if (nm == 0)
        continue;
      /* Prefer the simplest mask */
      xkb_mod_mask_t best = masks[0];
      for (size_t i = 1; i < nm; i++)
        if (__builtin_popcount(masks[i]) < __builtin_popcount(best))
          best = masks[i];
      *mods = best;
      return kc;
    }
  }
  return XKB_KEYCODE_INVALID;
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

static xkb_keycode_t qnum_to_keycode(uint32_t qnum)
{
  if (qnum == 0 || qnum >= code_map_qnum_to_xorgevdev_len)
    return XKB_KEYCODE_INVALID;
  if (code_map_qnum_to_xorgevdev[qnum] == 0)
    return XKB_KEYCODE_INVALID;
  return code_map_qnum_to_xorgevdev[qnum];
}

void wvnc_key_event(struct wvnc_compositor* c, uint32_t keysym,
                    uint32_t qnum, bool down, bool raw)
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

struct wvnc_compositor* wvnc_compositor_create(const struct wvnc_options* opts,
                                               const struct wvnc_callbacks* cb)
{
  struct wvnc_compositor* c = calloc(1, sizeof(*c));
  const char* dbg = getenv("TIGERVNC_WLR_DEBUG");

  wlr_log_init(dbg && *dbg ? WLR_DEBUG : WLR_ERROR, NULL);

  /* We read rendered frames back from the output buffer, so the
   * scene must always composite into it */
  setenv("WLR_SCENE_DISABLE_DIRECT_SCANOUT", "1", 1);

  c->cb = *cb;
  c->width = opts->width;
  c->height = opts->height;
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
  wlr_xdg_output_manager_v1_create(c->display, c->output_layout);

  c->scene = wlr_scene_create();
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

  c->new_output.notify = handle_new_output;
  wl_signal_add(&c->backend->events.new_output, &c->new_output);

  c->xdg_shell = wlr_xdg_shell_create(c->display, 6);
  c->new_xdg_toplevel.notify = handle_new_xdg_toplevel;
  wl_signal_add(&c->xdg_shell->events.new_toplevel, &c->new_xdg_toplevel);
  c->new_xdg_popup.notify = handle_new_xdg_popup;
  wl_signal_add(&c->xdg_shell->events.new_popup, &c->new_xdg_popup);

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
    wl_display_destroy_clients(c->display);

    /* wlroots asserts that nobody listens any more when its objects
     * go away */
    remove_listener(&c->new_xdg_toplevel);
    remove_listener(&c->new_xdg_popup);
    remove_listener(&c->new_layer_surface);
    remove_listener(&c->request_set_cursor);
    remove_listener(&c->request_set_selection);
    remove_listener(&c->request_set_primary_selection);
    remove_listener(&c->set_selection);
    remove_listener(&c->set_primary_selection);
    remove_listener(&c->implicit_grab_destroy);
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
  arrange_layers(c);
  rearrange_views(c);
  wlr_output_schedule_frame(c->output);

  return true;
}
