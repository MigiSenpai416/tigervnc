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

#include <ctype.h>
#include <dirent.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#ifdef HAVE_LIBPNG
#include <png.h>
#endif

#include "wm_icons.h"

#define MAX_IMAGE_SIDE 1024   /* larger icon files are ignored */

struct wvnc_icon* wvnc_icon_ref(struct wvnc_icon* icon)
{
  if (icon)
    icon->refs++;
  return icon;
}

void wvnc_icon_unref(struct wvnc_icon* icon)
{
  if (!icon || --icon->refs > 0)
    return;
  free(icon->px);
  free(icon);
}

static struct wvnc_icon* icon_new(int size)
{
  struct wvnc_icon* icon = calloc(1, sizeof(*icon));
  if (!icon)
    return NULL;
  icon->px = calloc((size_t)size * size, 4);
  if (!icon->px) {
    free(icon);
    return NULL;
  }
  icon->refs = 1;
  icon->size = size;
  return icon;
}

static uint32_t premultiply(uint32_t p)
{
  uint32_t a = p >> 24;
  if (a == 255)
    return p;
  if (a == 0)
    return 0;
  uint32_t r = ((p >> 16) & 0xff) * a / 255;
  uint32_t g = ((p >> 8) & 0xff) * a / 255;
  uint32_t b = (p & 0xff) * a / 255;
  return (a << 24) | (r << 16) | (g << 8) | b;
}

/* Box filter resampling (area average for downscaling, smooth for
 * upscaling), in premultiplied space */
struct wvnc_icon* wvnc_icon_from_argb(const uint32_t* argb, int w, int h,
                                      size_t stride_px, bool premul,
                                      int size)
{
  struct wvnc_icon* icon;
  int dw, dh, ox, oy;

  if (!argb || w <= 0 || h <= 0 || size <= 0)
    return NULL;
  icon = icon_new(size);
  if (!icon)
    return NULL;
  if (w >= h) {
    dw = size;
    dh = (int)((long)h * size / w);
  } else {
    dh = size;
    dw = (int)((long)w * size / h);
  }
  if (dw < 1) dw = 1;
  if (dh < 1) dh = 1;
  ox = (size - dw) / 2;
  oy = (size - dh) / 2;

  double sx = (double)w / dw, sy = (double)h / dh;
  for (int y = 0; y < dh; y++) {
    double y0 = y * sy, y1 = y0 + sy;
    for (int x = 0; x < dw; x++) {
      double x0 = x * sx, x1 = x0 + sx;
      double acc[4] = { 0, 0, 0, 0 }, wsum = 0;
      for (int yy = (int)y0; yy < h && yy < y1; yy++) {
        double wy = (yy + 1 < y1 ? yy + 1 : y1) - (yy > y0 ? yy : y0);
        if (wy <= 0)
          continue;
        for (int xx = (int)x0; xx < w && xx < x1; xx++) {
          double wx = (xx + 1 < x1 ? xx + 1 : x1) - (xx > x0 ? xx : x0);
          uint32_t p;
          if (wx <= 0)
            continue;
          p = argb[(size_t)yy * stride_px + xx];
          if (!premul)
            p = premultiply(p);
          acc[0] += wx * wy * (p >> 24);
          acc[1] += wx * wy * ((p >> 16) & 0xff);
          acc[2] += wx * wy * ((p >> 8) & 0xff);
          acc[3] += wx * wy * (p & 0xff);
          wsum += wx * wy;
        }
      }
      if (wsum <= 0)
        continue;
      uint32_t c[4];
      for (int i = 0; i < 4; i++) {
        double v = acc[i] / wsum + 0.5;
        c[i] = v > 255 ? 255 : (uint32_t)v;
      }
      for (int i = 1; i < 4; i++)
        if (c[i] > c[0])
          c[i] = c[0];   /* stay valid premultiplied */
      icon->px[(size_t)(y + oy) * size + x + ox] =
        (c[0] << 24) | (c[1] << 16) | (c[2] << 8) | c[3];
    }
  }
  return icon;
}

struct wvnc_icon* wvnc_icon_from_net_wm_icon(const uint32_t* data, size_t n,
                                             int size)
{
  const uint32_t* best = NULL;
  uint32_t bw = 0, bh = 0;
  size_t i = 0;

  /* The smallest image at least as big as wanted, else the biggest */
  while (i + 2 <= n) {
    uint32_t w = data[i], h = data[i + 1];
    if (w == 0 || h == 0 || w > MAX_IMAGE_SIDE || h > MAX_IMAGE_SIDE ||
        (size_t)w * h > n - i - 2)
      break;
    bool big = w >= (uint32_t)size && h >= (uint32_t)size;
    bool best_big = bw >= (uint32_t)size && bh >= (uint32_t)size;
    if (!best || (big && (!best_big || w * h < bw * bh)) ||
        (!big && !best_big && w * h > bw * bh)) {
      best = data + i + 2;
      bw = w;
      bh = h;
    }
    i += 2 + (size_t)w * h;
  }
  if (!best)
    return NULL;
  return wvnc_icon_from_argb(best, (int)bw, (int)bh, bw, false, size);
}

/* ------------------------------------------------------------------ */
/* Image files                                                         */
/* ------------------------------------------------------------------ */

static bool has_suffix(const char* s, const char* suffix)
{
  size_t a = strlen(s), b = strlen(suffix);
  return a >= b && strcasecmp(s + a - b, suffix) == 0;
}

#ifdef HAVE_LIBPNG
static struct wvnc_icon* load_png(const char* path, int size)
{
  png_image img;
  uint32_t* buf;
  struct wvnc_icon* icon = NULL;

  memset(&img, 0, sizeof(img));
  img.version = PNG_IMAGE_VERSION;
  if (!png_image_begin_read_from_file(&img, path))
    return NULL;
  if (img.width == 0 || img.height == 0 || img.width > MAX_IMAGE_SIDE ||
      img.height > MAX_IMAGE_SIDE) {
    png_image_free(&img);
    return NULL;
  }
  /* BGRA bytes = native little endian 0xAARRGGBB words, straight
   * alpha for 8-bit output */
  img.format = PNG_FORMAT_BGRA;
  buf = malloc(PNG_IMAGE_SIZE(img));
  if (!buf) {
    png_image_free(&img);
    return NULL;
  }
  if (png_image_finish_read(&img, NULL, buf, 0, NULL))
    icon = wvnc_icon_from_argb(buf, (int)img.width, (int)img.height,
                               img.width, false, size);
  png_image_free(&img);
  free(buf);
  return icon;
}
#endif

static struct wvnc_icon* load_file_any(const char* path, int size);

/* An icon file; one that renders to nothing visible (e.g. an SVG made of
 * text only, as fonts are not loaded) counts as not found */
static struct wvnc_icon* load_file(const char* path, int size)
{
  struct wvnc_icon* icon = load_file_any(path, size);
  if (icon) {
    int n = icon->size * icon->size;
    for (int i = 0; i < n; i++)
      if (icon->px[i] >> 24)
        return icon;
    wvnc_icon_unref(icon);
  }
  return NULL;
}

static struct wvnc_icon* load_file_any(const char* path, int size)
{
  struct stat st;
  if (stat(path, &st) != 0 || !S_ISREG(st.st_mode))
    return NULL;
#ifdef HAVE_LIBPNG
  if (has_suffix(path, ".png"))
    return load_png(path, size);
#endif
#ifdef HAVE_LUNASVG
  if (has_suffix(path, ".svg")) {
    uint32_t* px = wvnc_svg_render(path, size);
    struct wvnc_icon* icon;
    if (!px)
      return NULL;
    icon = calloc(1, sizeof(*icon));
    if (!icon) {
      free(px);
      return NULL;
    }
    icon->refs = 1;
    icon->size = size;
    icon->px = px;
    return icon;
  }
#endif
  (void)size;
  return NULL;
}

const char* wvnc_icon_formats(void)
{
#if defined(HAVE_LIBPNG) && defined(HAVE_LUNASVG)
  return "PNG, SVG";
#elif defined(HAVE_LIBPNG)
  return "PNG";
#elif defined(HAVE_LUNASVG)
  return "SVG";
#else
  return "none";
#endif
}

/* ------------------------------------------------------------------ */
/* XDG base directories, .desktop files and the icon theme             */
/* ------------------------------------------------------------------ */

#define MAX_DATA_DIRS 16

static int data_dirs(char dirs[][1024])
{
  int n = 0;
  const char* home = getenv("HOME");
  const char* dh = getenv("XDG_DATA_HOME");
  const char* dd = getenv("XDG_DATA_DIRS");

  if (dh && *dh)
    snprintf(dirs[n++], 1024, "%s", dh);
  else if (home && *home)
    snprintf(dirs[n++], 1024, "%s/.local/share", home);
  if (!dd || !*dd)
    dd = "/usr/local/share:/usr/share";
  while (*dd && n < MAX_DATA_DIRS) {
    const char* end = strchr(dd, ':');
    size_t len = end ? (size_t)(end - dd) : strlen(dd);
    if (len > 0 && len < 1024 && dd[0] == '/') {
      memcpy(dirs[n], dd, len);
      dirs[n][len] = '\0';
      while (len > 1 && dirs[n][len - 1] == '/')
        dirs[n][--len] = '\0';
      n++;
    }
    dd += len;
    if (*dd == ':')
      dd++;
  }
  return n;
}

/* snprintf() into a path buffer; false if it did not fit */
static bool __attribute__((format(printf, 3, 4)))
path_fmt(char* buf, size_t len, const char* fmt, ...)
{
  va_list ap;
  int r;
  va_start(ap, fmt);
  r = vsnprintf(buf, len, fmt, ap);
  va_end(ap);
  return r >= 0 && (size_t)r < len;
}

/* Icon= (and StartupWMClass=) of a .desktop file's [Desktop Entry] */
static bool read_desktop(const char* path, char* icon, size_t icon_len,
                         char* wmclass, size_t wmclass_len)
{
  FILE* f = fopen(path, "r");
  char line[1024];
  bool in_entry = false, hidden = false;

  if (!f)
    return false;
  icon[0] = '\0';
  if (wmclass)
    wmclass[0] = '\0';
  while (fgets(line, sizeof(line), f)) {
    size_t len = strcspn(line, "\r\n");
    line[len] = '\0';
    if (line[0] == '[') {
      in_entry = strcmp(line, "[Desktop Entry]") == 0;
      continue;
    }
    if (!in_entry)
      continue;
    if (strncmp(line, "Icon=", 5) == 0)
      path_fmt(icon, icon_len, "%s", line + 5);
    else if (wmclass && strncmp(line, "StartupWMClass=", 15) == 0)
      path_fmt(wmclass, wmclass_len, "%s", line + 15);
    else if (strcmp(line, "Hidden=true") == 0)
      hidden = true;
  }
  fclose(f);
  return !hidden && icon[0] != '\0';
}

/* A desktop file named after the id, or one whose StartupWMClass is
 * the id; gives its Icon= value */
static bool desktop_icon_for(const char* id, char* icon, size_t len)
{
  char dirs[MAX_DATA_DIRS][1024];
  int n = data_dirs(dirs);
  char path[2048];

  if (!id || !*id || strchr(id, '/'))
    return false;
  for (int pass = 0; pass < 2; pass++) {
    for (int i = 0; i < n; i++) {
      char lower[256];
      snprintf(lower, sizeof(lower), "%s", id);
      for (char* p = lower; *p; p++)
        *p = (char)tolower((unsigned char)*p);
      if (path_fmt(path, sizeof(path), "%s/applications/%s.desktop",
                   dirs[i], pass == 0 ? id : lower) &&
          read_desktop(path, icon, len, NULL, 0))
        return true;
    }
  }
  for (int i = 0; i < n; i++) {
    char adir[1100];
    DIR* d;
    struct dirent* e;
    if (!path_fmt(adir, sizeof(adir), "%s/applications", dirs[i]))
      continue;
    d = opendir(adir);
    if (!d)
      continue;
    while ((e = readdir(d))) {
      char wmclass[256];
      if (!has_suffix(e->d_name, ".desktop"))
        continue;
      if (path_fmt(path, sizeof(path), "%s/%s", adir, e->d_name) &&
          read_desktop(path, icon, len, wmclass, sizeof(wmclass)) &&
          strcasecmp(wmclass, id) == 0) {
        closedir(d);
        return true;
      }
    }
    closedir(d);
  }
  return false;
}

static struct wvnc_icon* theme_lookup(const char* name, int size)
{
  /* Sizes from the wanted one up, then smaller ones */
  static const int sizes[] = { 32, 48, 64, 96, 128, 256, 512, 24, 22, 16 };
  static const char* const themes[] = { "hicolor", "Adwaita" };
  static const char* const subdirs[] = { "apps", "legacy" };
  char dirs[MAX_DATA_DIRS + 1][1024];
  int n = data_dirs(dirs);
  char path[2048];
  struct wvnc_icon* icon;
  const char* home = getenv("HOME");

  if (!name || !*name || strchr(name, '/'))
    return NULL;
  (void)size;

  /* PNG first, then SVG; per format the size order above */
  for (int fmt = 0; fmt < 2; fmt++) {
    const char* ext = fmt == 0 ? "png" : "svg";
    for (size_t t = 0; t < sizeof(themes) / sizeof(*themes); t++) {
      for (int b = -1; b < n; b++) {
        char base[1100];
        if (b < 0) {
          if (!home || !*home)
            continue;
          if (!path_fmt(base, sizeof(base), "%s/.icons/%s", home, themes[t]))
            continue;
        } else {
          if (!path_fmt(base, sizeof(base), "%s/icons/%s", dirs[b], themes[t]))
            continue;
        }
        for (size_t s = 0; s < sizeof(subdirs) / sizeof(*subdirs); s++) {
          if (fmt == 0) {
            for (size_t z = 0; z < sizeof(sizes) / sizeof(*sizes); z++) {
              if (path_fmt(path, sizeof(path), "%s/%dx%d/%s/%s.png", base,
                           sizes[z], sizes[z], subdirs[s], name) &&
                  (icon = load_file(path, size)))
                return icon;
            }
          } else {
            if (path_fmt(path, sizeof(path), "%s/scalable/%s/%s.svg", base,
                         subdirs[s], name) &&
                (icon = load_file(path, size)))
              return icon;
          }
        }
      }
    }
    for (int b = 0; b < n; b++) {
      if (path_fmt(path, sizeof(path), "%s/pixmaps/%s.%s", dirs[b], name,
                   ext) &&
          (icon = load_file(path, size)))
        return icon;
    }
  }
  return NULL;
}

/* Cache of lookups (hits and misses), keyed by request */
struct cache_entry {
  struct cache_entry* next;
  char* key;
  struct wvnc_icon* icon;   /* NULL: nothing found */
};
static struct cache_entry* cache;
static int cache_len;

static struct cache_entry* cache_find(const char* key)
{
  for (struct cache_entry* e = cache; e; e = e->next)
    if (strcmp(e->key, key) == 0)
      return e;
  return NULL;
}

static void cache_put(const char* key, struct wvnc_icon* icon)
{
  struct cache_entry* e;
  if (cache_len >= 256) {   /* crude bound: start over */
    while (cache) {
      e = cache;
      cache = e->next;
      wvnc_icon_unref(e->icon);
      free(e->key);
      free(e);
    }
    cache_len = 0;
  }
  e = calloc(1, sizeof(*e));
  if (!e)
    return;
  e->key = strdup(key);
  if (!e->key) {
    free(e);
    return;
  }
  e->icon = wvnc_icon_ref(icon);
  e->next = cache;
  cache = e;
  cache_len++;
}

static struct wvnc_icon* name_lookup(const char* name, int size)
{
  if (!name || !*name)
    return NULL;
  if (name[0] == '/')
    return load_file(name, size);
  return theme_lookup(name, size);
}

struct wvnc_icon* wvnc_icon_for_name(const char* name, int size)
{
  char key[600];
  struct cache_entry* e;
  struct wvnc_icon* icon;

  if (!name || !*name)
    return NULL;
  snprintf(key, sizeof(key), "name:%d:%s", size, name);
  if ((e = cache_find(key)))
    return wvnc_icon_ref(e->icon);
  icon = name_lookup(name, size);
  cache_put(key, icon);
  return icon;
}

struct wvnc_icon* wvnc_icon_for_app(const char* const* ids, int n, int size)
{
  char key[1024];
  size_t off;
  struct cache_entry* e;
  struct wvnc_icon* icon = NULL;
  char name[512];

  off = (size_t)snprintf(key, sizeof(key), "app:%d", size);
  for (int i = 0; i < n && off < sizeof(key); i++)
    off += (size_t)snprintf(key + off, sizeof(key) - off, ":%s",
                            ids[i] ? ids[i] : "");
  if ((e = cache_find(key)))
    return wvnc_icon_ref(e->icon);

  /* The .desktop file's Icon= first, then the id as icon name */
  for (int i = 0; i < n && !icon; i++)
    if (desktop_icon_for(ids[i], name, sizeof(name)))
      icon = name_lookup(name, size);
  for (int i = 0; i < n && !icon; i++) {
    char lower[256];
    if (!ids[i] || !*ids[i] || strchr(ids[i], '/'))
      continue;
    icon = theme_lookup(ids[i], size);
    snprintf(lower, sizeof(lower), "%s", ids[i]);
    for (char* p = lower; *p; p++)
      *p = (char)tolower((unsigned char)*p);
    if (!icon && strcmp(lower, ids[i]) != 0)
      icon = theme_lookup(lower, size);
  }
  cache_put(key, icon);
  return icon;
}

/* ------------------------------------------------------------------ */
/* Fallback icon                                                       */
/* ------------------------------------------------------------------ */

/* A small window: light frame, blue title bar, white content */
struct wvnc_icon* wvnc_icon_fallback(int size)
{
  static struct wvnc_icon* cached;
  struct wvnc_icon* icon;
  int m, bar;

  if (cached && cached->size == size)
    return wvnc_icon_ref(cached);
  icon = icon_new(size);
  if (!icon)
    return NULL;
  m = size / 8;
  bar = size / 4;
  for (int y = m; y < size - m; y++)
    for (int x = m / 2; x < size - m / 2; x++) {
      bool edge = y == m || y == size - m - 1 || x == m / 2 ||
                  x == size - m / 2 - 1;
      uint32_t p;
      if (edge)
        p = 0xffc8ccd4;
      else if (y < m + bar)
        p = 0xff3a6ea5;
      else
        p = 0xfff4f5f7;
      icon->px[(size_t)y * size + x] = p;
    }
  /* three little "buttons" on the bar */
  for (int i = 0; i < 3; i++) {
    int bx = size - m / 2 - 3 - (i + 1) * (size / 8 + 1);
    for (int y = m + bar / 3; y < m + bar - bar / 3; y++)
      for (int x = bx; x < bx + size / 8 - 1; x++)
        if (x > m / 2 && x < size - m / 2 - 1)
          icon->px[(size_t)y * size + x] = 0xffdde6f0;
  }
  wvnc_icon_unref(cached);
  cached = wvnc_icon_ref(icon);
  return icon;
}
