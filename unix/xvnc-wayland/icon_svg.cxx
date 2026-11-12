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

// SVG icon decoder for the window switcher (lunasvg). Only built when
// lunasvg is available (HAVE_LUNASVG); wm_icons.c calls it for .svg
// files.

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include <lunasvg.h>

#include "wm_icons.h"

uint32_t* wvnc_svg_render(const char* path, int size)
{
  struct stat st;

  // Icons are small; refuse anything that is clearly not one
  if (size <= 0 || stat(path, &st) != 0 || st.st_size > 4 * 1024 * 1024)
    return nullptr;

  try {
    std::unique_ptr<lunasvg::Document> doc =
      lunasvg::Document::loadFromFile(path);
    if (!doc)
      return nullptr;
    // Fit the document into size x size, keeping its aspect ratio
    float w = doc->width(), h = doc->height();
    int bw = size, bh = size;
    if (w > 0 && h > 0) {
      if (w >= h)
        bh = (int)(size * h / w + 0.5f);
      else
        bw = (int)(size * w / h + 0.5f);
      if (bw < 1) bw = 1;
      if (bh < 1) bh = 1;
    }
    lunasvg::Bitmap bitmap = doc->renderToBitmap(bw, bh);
    if (bitmap.isNull())
      return nullptr;
    uint32_t* px = (uint32_t*)calloc((size_t)size * size, 4);
    if (!px)
      return nullptr;
    // lunasvg bitmaps are premultiplied ARGB32, native endian
    int ox = (size - bitmap.width()) / 2, oy = (size - bitmap.height()) / 2;
    for (int y = 0; y < bitmap.height() && y + oy < size; y++)
      memcpy(px + (size_t)(y + oy) * size + ox,
             bitmap.data() + (size_t)y * bitmap.stride(),
             (size_t)bitmap.width() * 4);
    return px;
  } catch (...) {
    return nullptr;
  }
}
