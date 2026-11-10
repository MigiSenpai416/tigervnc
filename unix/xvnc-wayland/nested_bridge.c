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
 */

#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <wayland-client.h>
#include <wayland-server-core.h>
#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_primary_selection.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/util/log.h>

#include "wlr-data-control-unstable-v1-client-protocol.h"
#include "nested_bridge.h"

/* Offered on our own nested-side sources so we can recognise (and
 * ignore) the selection events they cause */
#define BRIDGE_MIME "application/x-tigervnc-nested-bridge"

enum { SEL_CLIPBOARD = 0, SEL_PRIMARY = 1 };

struct offer {
  struct zwlr_data_control_offer_v1* proxy;
  struct wl_array mime_types; /* char* */
  bool own;
  int refs; /* host sources still pointing at it + current selection */
};

struct host_source {
  union {
    struct wlr_data_source data;
    struct wlr_primary_selection_source primary;
  } u;
  struct nested_bridge* b;
  struct offer* offer;
};

struct nested_bridge {
  pid_t pid;
  struct wl_display* host_display;
  struct wlr_seat* host_seat;

  struct wl_display* dpy;
  struct wl_registry* registry;
  struct wl_seat* seat;
  struct zwlr_data_control_manager_v1* manager;
  uint32_t manager_version;
  struct zwlr_data_control_device_v1* device;
  struct wl_event_source* fd_source;
  bool dead;

  struct offer* pending_offer;      /* announced, not yet a selection */
  struct offer* current[2];         /* nested selection offers */
  struct zwlr_data_control_source_v1* own_source[2];
};

static const struct zwlr_data_control_source_v1_listener source_listener;

/* ---------------------------------------------------------------- utils */

char* nested_find_socket(const char* runtime_dir, const char* own_socket,
                         pid_t pid)
{
  DIR* dir;
  struct dirent* ent;
  char* found = NULL;

  if (!runtime_dir || pid <= 0)
    return NULL;
  dir = opendir(runtime_dir);
  if (!dir)
    return NULL;

  while (!found && (ent = readdir(dir)) != NULL) {
    struct sockaddr_un addr = { .sun_family = AF_UNIX };
    struct ucred cred;
    socklen_t len = sizeof(cred);
    struct stat st;
    char path[sizeof(addr.sun_path)];
    int fd;

    if (strncmp(ent->d_name, "wayland-", 8) != 0)
      continue;
    if (own_socket && strcmp(ent->d_name, own_socket) == 0)
      continue;
    if ((size_t)snprintf(path, sizeof(path), "%s/%s", runtime_dir,
                         ent->d_name) >= sizeof(path))
      continue;
    if (lstat(path, &st) != 0 || !S_ISSOCK(st.st_mode) ||
        st.st_uid != getuid())
      continue;

    fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0)
      continue;
    strcpy(addr.sun_path, path);
    if (connect(fd, (struct sockaddr*)&addr, sizeof(addr)) == 0 &&
        getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cred, &len) == 0 &&
        cred.pid == pid && cred.uid == getuid())
      found = strdup(path);
    close(fd);
  }
  closedir(dir);
  return found;
}

static void offer_unref(struct offer* o)
{
  char** m;
  if (!o || --o->refs > 0)
    return;
  if (o->proxy)
    zwlr_data_control_offer_v1_destroy(o->proxy);
  wl_array_for_each(m, &o->mime_types)
    free(*m);
  wl_array_release(&o->mime_types);
  free(o);
}

static void mark_dead(struct nested_bridge* b)
{
  b->dead = true;
  /* The fd is level triggered; stop watching it or we would spin */
  if (b->fd_source) {
    wl_event_source_remove(b->fd_source);
    b->fd_source = NULL;
  }
}

static void flush(struct nested_bridge* b)
{
  if (b->dpy && !b->dead)
    wl_display_flush(b->dpy);
}

/* ------------------------------------------- nested -> host (our seat) */

static void host_send(struct host_source* hs, const char* mime, int fd)
{
  struct nested_bridge* b = hs->b;
  if (b && !b->dead && hs->offer && hs->offer->proxy) {
    zwlr_data_control_offer_v1_receive(hs->offer->proxy, mime, fd);
    flush(b);
  }
  close(fd);
}

static void host_data_send(struct wlr_data_source* source, const char* mime,
                           int32_t fd)
{
  host_send(wl_container_of(source, (struct host_source*)NULL, u.data),
            mime, fd);
}

static void host_data_destroy(struct wlr_data_source* source)
{
  struct host_source* hs =
    wl_container_of(source, (struct host_source*)NULL, u.data);
  offer_unref(hs->offer);
  free(hs);
}

static const struct wlr_data_source_impl host_data_impl = {
  .send = host_data_send,
  .destroy = host_data_destroy,
};

static void host_primary_send(struct wlr_primary_selection_source* source,
                              const char* mime, int fd)
{
  host_send(wl_container_of(source, (struct host_source*)NULL, u.primary),
            mime, fd);
}

static void host_primary_destroy(struct wlr_primary_selection_source* source)
{
  struct host_source* hs =
    wl_container_of(source, (struct host_source*)NULL, u.primary);
  offer_unref(hs->offer);
  free(hs);
}

static const struct wlr_primary_selection_source_impl host_primary_impl = {
  .send = host_primary_send,
  .destroy = host_primary_destroy,
};

bool nested_bridge_is_source(struct wlr_data_source* source)
{
  return source && source->impl == &host_data_impl;
}

bool nested_bridge_is_primary_source(struct wlr_primary_selection_source* s)
{
  return s && s->impl == &host_primary_impl;
}

static void copy_mime_types(struct wl_array* dst, struct wl_array* src)
{
  char** m;
  wl_array_for_each(m, src) {
    char** p = wl_array_add(dst, sizeof(*p));
    if (p)
      *p = strdup(*m);
  }
}

static void nested_selection(struct nested_bridge* b, int which,
                             struct zwlr_data_control_offer_v1* proxy)
{
  struct offer* o = NULL;
  struct host_source* hs;

  if (b->pending_offer && b->pending_offer->proxy == proxy) {
    o = b->pending_offer;
    b->pending_offer = NULL;
  } else if (proxy) {
    /* Offer for the other selection type, re-announced */
    for (int i = 0; i < 2; i++)
      if (b->current[i] && b->current[i]->proxy == proxy)
        o = b->current[i];
    if (o)
      o->refs++;
  }

  offer_unref(b->current[which]);
  b->current[which] = o;

  /* Nested selection cleared, or it is our own copy of ours: nothing to
   * mirror (losing a selection doesn't clear the other side) */
  if (!o || o->own)
    return;

  hs = calloc(1, sizeof(*hs));
  if (!hs)
    return;
  hs->b = b;
  hs->offer = o;
  o->refs++;

  if (which == SEL_CLIPBOARD) {
    wlr_data_source_init(&hs->u.data, &host_data_impl);
    copy_mime_types(&hs->u.data.mime_types, &o->mime_types);
    wlr_seat_set_selection(b->host_seat, &hs->u.data,
                           wl_display_next_serial(b->host_display));
  } else {
    wlr_primary_selection_source_init(&hs->u.primary, &host_primary_impl);
    copy_mime_types(&hs->u.primary.mime_types, &o->mime_types);
    wlr_seat_set_primary_selection(b->host_seat, &hs->u.primary,
                                   wl_display_next_serial(b->host_display));
  }
}

static void offer_handle_offer(void* data,
                               struct zwlr_data_control_offer_v1* proxy,
                               const char* mime)
{
  struct offer* o = data;
  char** p;
  (void)proxy;
  if (strcmp(mime, BRIDGE_MIME) == 0) {
    o->own = true;
    return;
  }
  p = wl_array_add(&o->mime_types, sizeof(*p));
  if (p)
    *p = strdup(mime);
}

static const struct zwlr_data_control_offer_v1_listener offer_listener = {
  .offer = offer_handle_offer,
};

static void device_data_offer(void* data,
                              struct zwlr_data_control_device_v1* dev,
                              struct zwlr_data_control_offer_v1* proxy)
{
  struct nested_bridge* b = data;
  struct offer* o = calloc(1, sizeof(*o));
  (void)dev;
  if (!o) {
    zwlr_data_control_offer_v1_destroy(proxy);
    return;
  }
  o->proxy = proxy;
  o->refs = 1;
  wl_array_init(&o->mime_types);
  zwlr_data_control_offer_v1_add_listener(proxy, &offer_listener, o);
  offer_unref(b->pending_offer);
  b->pending_offer = o;
}

static void device_selection(void* data,
                             struct zwlr_data_control_device_v1* dev,
                             struct zwlr_data_control_offer_v1* proxy)
{
  (void)dev;
  nested_selection(data, SEL_CLIPBOARD, proxy);
}

static void device_primary_selection(void* data,
                                     struct zwlr_data_control_device_v1* dev,
                                     struct zwlr_data_control_offer_v1* proxy)
{
  (void)dev;
  nested_selection(data, SEL_PRIMARY, proxy);
}

static void device_finished(void* data,
                            struct zwlr_data_control_device_v1* dev)
{
  struct nested_bridge* b = data;
  (void)dev;
  wlr_log(WLR_INFO, "Nested compositor ended the clipboard bridge");
  mark_dead(b);
}

static const struct zwlr_data_control_device_v1_listener device_listener = {
  .data_offer = device_data_offer,
  .selection = device_selection,
  .finished = device_finished,
  .primary_selection = device_primary_selection,
};

/* ------------------------------------------- host (our seat) -> nested */

static void source_send(void* data, struct zwlr_data_control_source_v1* src,
                        const char* mime, int32_t fd)
{
  struct nested_bridge* b = data;
  bool primary = src == b->own_source[SEL_PRIMARY];

  if (primary && b->host_seat->primary_selection_source)
    wlr_primary_selection_source_send(b->host_seat->primary_selection_source,
                                      mime, fd);
  else if (!primary && b->host_seat->selection_source)
    wlr_data_source_send(b->host_seat->selection_source, mime, fd);
  else
    close(fd);
}

static void source_cancelled(void* data,
                             struct zwlr_data_control_source_v1* src)
{
  struct nested_bridge* b = data;
  for (int i = 0; i < 2; i++)
    if (b->own_source[i] == src)
      b->own_source[i] = NULL;
  zwlr_data_control_source_v1_destroy(src);
}

static const struct zwlr_data_control_source_v1_listener source_listener = {
  .send = source_send,
  .cancelled = source_cancelled,
};

static void push_to_nested(struct nested_bridge* b, int which,
                           struct wl_array* mime_types)
{
  struct zwlr_data_control_source_v1* src;
  char** m;

  if (!b->device || b->dead)
    return;
  if (which == SEL_PRIMARY && b->manager_version < 2)
    return;

  if (!mime_types) {
    /* Our seat lost its selection: leave the nested one alone, same
     * as losing PRIMARY doesn't clear the VNC clipboard */
    return;
  }

  src = zwlr_data_control_manager_v1_create_data_source(b->manager);
  zwlr_data_control_source_v1_add_listener(src, &source_listener, b);
  wl_array_for_each(m, mime_types)
    zwlr_data_control_source_v1_offer(src, *m);
  zwlr_data_control_source_v1_offer(src, BRIDGE_MIME);

  if (which == SEL_CLIPBOARD)
    zwlr_data_control_device_v1_set_selection(b->device, src);
  else
    zwlr_data_control_device_v1_set_primary_selection(b->device, src);
  /* The previous source gets "cancelled" and is destroyed there */
  b->own_source[which] = src;
  flush(b);
}

void nested_bridge_host_selection(struct nested_bridge* b)
{
  struct wlr_data_source* s;
  if (!b)
    return;
  s = b->host_seat->selection_source;
  if (nested_bridge_is_source(s))
    return; /* came from the nested side */
  push_to_nested(b, SEL_CLIPBOARD, s ? &s->mime_types : NULL);
}

void nested_bridge_host_primary(struct nested_bridge* b)
{
  struct wlr_primary_selection_source* s;
  if (!b)
    return;
  s = b->host_seat->primary_selection_source;
  if (nested_bridge_is_primary_source(s))
    return;
  push_to_nested(b, SEL_PRIMARY, s ? &s->mime_types : NULL);
}

/* ------------------------------------------------------- connection */

static void maybe_create_device(struct nested_bridge* b)
{
  if (b->device || !b->seat || !b->manager)
    return;
  b->device = zwlr_data_control_manager_v1_get_data_device(b->manager,
                                                           b->seat);
  zwlr_data_control_device_v1_add_listener(b->device, &device_listener, b);
  wlr_log(WLR_INFO, "Clipboard bridge to nested compositor (pid %d) "
          "ready, wlr-data-control v%u", (int)b->pid, b->manager_version);
  /* Mirror what we already have */
  nested_bridge_host_selection(b);
  nested_bridge_host_primary(b);
}

static void registry_global(void* data, struct wl_registry* reg,
                            uint32_t name, const char* iface,
                            uint32_t version)
{
  struct nested_bridge* b = data;
  if (!b->seat && strcmp(iface, wl_seat_interface.name) == 0) {
    b->seat = wl_registry_bind(reg, name, &wl_seat_interface, 1);
  } else if (!b->manager &&
             strcmp(iface, zwlr_data_control_manager_v1_interface.name) == 0) {
    b->manager_version = version < 2 ? version : 2;
    b->manager = wl_registry_bind(reg, name,
                                  &zwlr_data_control_manager_v1_interface,
                                  b->manager_version);
  }
  maybe_create_device(b);
}

static void registry_global_remove(void* data, struct wl_registry* reg,
                                   uint32_t name)
{
  (void)data; (void)reg; (void)name;
}

static const struct wl_registry_listener registry_listener = {
  .global = registry_global,
  .global_remove = registry_global_remove,
};

static int handle_fd(int fd, uint32_t mask, void* data)
{
  struct nested_bridge* b = data;
  (void)fd;

  if (b->dead)
    return 0;
  if (mask & WL_EVENT_READABLE) {
    if (wl_display_dispatch(b->dpy) < 0) {
      wlr_log(WLR_INFO, "Lost connection to nested compositor");
      mark_dead(b);
      return 0;
    }
  } else if (mask & (WL_EVENT_HANGUP | WL_EVENT_ERROR)) {
    mark_dead(b);
    return 0;
  }
  flush(b);
  return 0;
}

struct nested_bridge* nested_bridge_create(struct wl_display* host_display,
                                           struct wlr_seat* host_seat,
                                           const char* socket_path,
                                           pid_t pid)
{
  struct sockaddr_un addr = { .sun_family = AF_UNIX };
  struct nested_bridge* b;
  int fd;

  if (strlen(socket_path) >= sizeof(addr.sun_path))
    return NULL;
  fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (fd < 0)
    return NULL;
  strcpy(addr.sun_path, socket_path);
  if (connect(fd, (struct sockaddr*)&addr, sizeof(addr)) != 0) {
    close(fd);
    return NULL;
  }

  b = calloc(1, sizeof(*b));
  if (!b) {
    close(fd);
    return NULL;
  }
  b->pid = pid;
  b->host_display = host_display;
  b->host_seat = host_seat;
  b->dpy = wl_display_connect_to_fd(fd); /* takes the fd */
  if (!b->dpy) {
    free(b);
    return NULL;
  }

  /* Fully asynchronous: the nested compositor is our client, so a
   * blocking round trip here could deadlock with it */
  b->registry = wl_display_get_registry(b->dpy);
  wl_registry_add_listener(b->registry, &registry_listener, b);
  b->fd_source = wl_event_loop_add_fd(wl_display_get_event_loop(host_display),
                                      wl_display_get_fd(b->dpy),
                                      WL_EVENT_READABLE, handle_fd, b);
  flush(b);
  wlr_log(WLR_INFO, "Connecting clipboard bridge to %s", socket_path);
  return b;
}

pid_t nested_bridge_pid(struct nested_bridge* b)
{
  return b ? b->pid : 0;
}

bool nested_bridge_active(struct nested_bridge* b)
{
  return b && !b->dead;
}

/* A host source outlives the bridge if it is still our seat's
 * selection; it then offers nothing (requests get an empty pipe) */
static void detach_host_source(struct host_source* hs)
{
  hs->b = NULL;
  if (hs->offer && hs->offer->proxy) {
    zwlr_data_control_offer_v1_destroy(hs->offer->proxy);
    hs->offer->proxy = NULL;
  }
}

void nested_bridge_destroy(struct nested_bridge* b)
{
  if (!b)
    return;

  /* Host sources that still point at us must not touch us any more */
  if (nested_bridge_is_source(b->host_seat->selection_source)) {
    struct host_source* hs = wl_container_of(b->host_seat->selection_source,
                                             hs, u.data);
    detach_host_source(hs);
  }
  if (nested_bridge_is_primary_source(b->host_seat->primary_selection_source)) {
    struct host_source* hs =
      wl_container_of(b->host_seat->primary_selection_source, hs, u.primary);
    detach_host_source(hs);
  }

  if (b->fd_source)
    wl_event_source_remove(b->fd_source);
  offer_unref(b->pending_offer);
  for (int i = 0; i < 2; i++) {
    offer_unref(b->current[i]);
    if (b->own_source[i])
      zwlr_data_control_source_v1_destroy(b->own_source[i]);
  }
  if (b->device)
    zwlr_data_control_device_v1_destroy(b->device);
  if (b->manager)
    zwlr_data_control_manager_v1_destroy(b->manager);
  if (b->seat)
    wl_seat_destroy(b->seat);
  if (b->registry)
    wl_registry_destroy(b->registry);
  wl_display_disconnect(b->dpy);
  free(b);
}
