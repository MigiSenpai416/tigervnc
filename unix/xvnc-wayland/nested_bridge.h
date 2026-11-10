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

/*
 * Clipboard bridge to a nested compositor.
 *
 * When the session is itself a Wayland compositor (sway, labwc, KDE,
 * ...) running as a client of ours, its applications use *its* seat, so
 * their selections never reach our seat. The bridge connects to the
 * nested compositor's own Wayland socket as a wlr-data-control client
 * and mirrors CLIPBOARD and PRIMARY in both directions between its seat
 * and ours. Data is never buffered: requests are forwarded as file
 * descriptors to whichever side owns the selection.
 */

#ifndef WVNC_NESTED_BRIDGE_H
#define WVNC_NESTED_BRIDGE_H

#include <stdbool.h>
#include <sys/types.h>

struct wl_event_loop;
struct wl_display;
struct wlr_seat;
struct wlr_data_source;
struct wlr_primary_selection_source;
struct nested_bridge;

/* Returns the path of the Wayland socket served by process `pid` in
 * `runtime_dir` (owned by our uid), or NULL. Caller frees. */
char* nested_find_socket(const char* runtime_dir, const char* own_socket,
                         pid_t pid);

struct nested_bridge* nested_bridge_create(struct wl_display* host_display,
                                           struct wlr_seat* host_seat,
                                           const char* socket_path,
                                           pid_t pid);
void nested_bridge_destroy(struct nested_bridge* b);
pid_t nested_bridge_pid(struct nested_bridge* b);
bool nested_bridge_active(struct nested_bridge* b);

/* Call whenever our seat's selection / primary selection changed */
void nested_bridge_host_selection(struct nested_bridge* b);
void nested_bridge_host_primary(struct nested_bridge* b);

/* True for sources the bridge put on our seat */
bool nested_bridge_is_source(struct wlr_data_source* source);
bool nested_bridge_is_primary_source(struct wlr_primary_selection_source* s);

#endif
