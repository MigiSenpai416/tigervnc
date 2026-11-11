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

#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifdef HAVE_FREETYPE
#include <ft2build.h>
#include FT_FREETYPE_H
#endif

#include "deco_font.h"
#include "wm_text.h"

#define MAX_CPS 512

/* Font files tried in turn: the bundled one, then where distros keep
 * DejaVu Sans (Debian/Ubuntu, Fedora, Arch, openSUSE) */
static const char* const font_paths[] = {
#ifdef WVNC_FONT_FILE
  WVNC_FONT_FILE,
#endif
  "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
  "/usr/share/fonts/dejavu-sans-fonts/DejaVuSans.ttf",
  "/usr/share/fonts/dejavu/DejaVuSans.ttf",
  "/usr/share/fonts/TTF/DejaVuSans.ttf",
  "/usr/share/fonts/truetype/DejaVuSans.ttf",
};

static bool initialised;
static const char* font_path;
static int line_h = DECO_FONT_H, ascent = DECO_FONT_H - 2;
#ifdef HAVE_FREETYPE
static FT_Library ft;
static FT_Face face;
#endif

/* Next code point of UTF-8 text; invalid bytes come out as U+FFFD */
static uint32_t utf8_next(const unsigned char** p)
{
  const unsigned char* s = *p;
  uint32_t cp;
  int n;
  if (s[0] < 0x80) { *p = s + 1; return s[0]; }
  if ((s[0] & 0xe0) == 0xc0) { cp = s[0] & 0x1f; n = 1; }
  else if ((s[0] & 0xf0) == 0xe0) { cp = s[0] & 0x0f; n = 2; }
  else if ((s[0] & 0xf8) == 0xf0) { cp = s[0] & 0x07; n = 3; }
  else { *p = s + 1; return 0xfffd; }
  for (int i = 1; i <= n; i++) {
    if ((s[i] & 0xc0) != 0x80) { *p = s + i; return 0xfffd; }
    cp = (cp << 6) | (s[i] & 0x3f);
  }
  *p = s + n + 1;
  return cp;
}

static size_t decode(const char* utf8, uint32_t* cps)
{
  const unsigned char* p = (const unsigned char*)utf8;
  size_t n = 0;
  while (p && *p && n < MAX_CPS) {
    uint32_t cp = utf8_next(&p);
    if (cp < 0x20 || (cp >= 0x7f && cp < 0xa0))
      cp = ' ';
    cps[n++] = cp;
  }
  return n;
}

bool wvnc_text_init(int px)
{
  if (initialised)
    return font_path != NULL;
  initialised = true;
#ifdef HAVE_FREETYPE
  if (FT_Init_FreeType(&ft) != 0)
    return false;
  for (size_t i = 0; i < sizeof(font_paths) / sizeof(*font_paths); i++) {
    if (access(font_paths[i], R_OK) != 0)
      continue;
    if (FT_New_Face(ft, font_paths[i], 0, &face) != 0) {
      face = NULL;
      continue;
    }
    if (FT_Set_Pixel_Sizes(face, 0, px) != 0) {
      FT_Done_Face(face);
      face = NULL;
      continue;
    }
    font_path = font_paths[i];
    ascent = (int)(face->size->metrics.ascender >> 6);
    line_h = (int)((face->size->metrics.ascender -
                    face->size->metrics.descender) >> 6);
    return true;
  }
  FT_Done_FreeType(ft);
  ft = NULL;
#else
  (void)px;
#endif
  return false;
}

const char* wvnc_text_font_path(void)
{
  return font_path;
}

int wvnc_text_line_height(void)
{
  return line_h;
}

int wvnc_text_ascent(void)
{
  return ascent;
}

/* Glyph advances in pixels (26.6 rounded) */
static int advance(uint32_t cp)
{
#ifdef HAVE_FREETYPE
  if (face) {
    if (FT_Load_Char(face, cp, FT_LOAD_DEFAULT) != 0)
      return 0;
    return (int)((face->glyph->advance.x + 32) >> 6);
  }
#endif
  (void)cp;
  return DECO_FONT_W;
}

/* How many code points fit into max_w, and whether an ellipsis is
 * needed (then the count leaves room for it) */
static size_t layout(const uint32_t* cps, size_t n, int max_w,
                     bool* ellipsis, uint32_t* ell_cp, int* width)
{
  int w = 0, ell_w;
  size_t fit = 0;

#ifdef HAVE_FREETYPE
  *ell_cp = face && FT_Get_Char_Index(face, 0x2026) ? 0x2026 : '.';
#else
  *ell_cp = '.';
#endif
  ell_w = advance(*ell_cp) * (*ell_cp == '.' ? 3 : 1);

  for (size_t i = 0; i < n; i++)
    w += advance(cps[i]);
  *ellipsis = false;
  if (w <= max_w) {
    *width = w;
    return n;
  }
  *ellipsis = true;
  w = 0;
  while (fit < n) {
    int a = advance(cps[fit]);
    if (w + a + ell_w > max_w)
      break;
    w += a;
    fit++;
  }
  /* no trailing space before the ellipsis */
  while (fit > 0 && cps[fit - 1] == ' ') {
    fit--;
    w -= advance(' ');
  }
  *width = w + (ell_w <= max_w ? ell_w : 0);
  if (ell_w > max_w)
    *ellipsis = false;
  return fit;
}

int wvnc_text_width(const char* utf8, int max_w)
{
  uint32_t cps[MAX_CPS];
  size_t n = decode(utf8, cps);
  bool ell;
  uint32_t ell_cp;
  int w;
  layout(cps, n, max_w, &ell, &ell_cp, &w);
  return w;
}

static void blend(uint32_t* d, uint32_t rgb, unsigned cov)
{
  uint32_t r = (rgb >> 16) & 0xff, g = (rgb >> 8) & 0xff, b = rgb & 0xff;
  uint32_t o = *d, inv = 255 - cov;
  uint32_t oa = o >> 24, or_ = (o >> 16) & 0xff, og = (o >> 8) & 0xff,
           ob = o & 0xff;
  uint32_t na = cov + oa * inv / 255;
  uint32_t nr = r * cov / 255 + or_ * inv / 255;
  uint32_t ng = g * cov / 255 + og * inv / 255;
  uint32_t nb = b * cov / 255 + ob * inv / 255;
  *d = (na << 24) | (nr << 16) | (ng << 8) | nb;
}

static int draw_cp(uint32_t* dst, int dw, int dh, size_t stride, int x,
                   int y, uint32_t cp, uint32_t rgb)
{
#ifdef HAVE_FREETYPE
  if (face) {
    FT_GlyphSlot g;
    if (FT_Load_Char(face, cp, FT_LOAD_RENDER | FT_LOAD_TARGET_LIGHT) != 0)
      return 0;
    g = face->glyph;
    for (unsigned row = 0; row < g->bitmap.rows; row++) {
      int py = y + ascent - g->bitmap_top + (int)row;
      if (py < 0 || py >= dh)
        continue;
      for (unsigned col = 0; col < g->bitmap.width; col++) {
        int px = x + g->bitmap_left + (int)col;
        unsigned cov;
        if (px < 0 || px >= dw)
          continue;
        if (g->bitmap.pixel_mode == FT_PIXEL_MODE_MONO)
          cov = (g->bitmap.buffer[row * g->bitmap.pitch + col / 8] &
                 (0x80 >> (col % 8))) ? 255 : 0;
        else
          cov = g->bitmap.buffer[row * g->bitmap.pitch + col];
        if (cov)
          blend(&dst[(size_t)py * stride + px], rgb, cov);
      }
    }
    return (int)((g->advance.x + 32) >> 6);
  }
#endif
  if (cp > 0xff)
    cp = '?';   /* the bitmap font covers Latin-1 */
  for (int row = 0; row < DECO_FONT_H; row++) {
    unsigned char bits = deco_font[cp][row];
    int py = y + row;
    if (py < 0 || py >= dh)
      continue;
    for (int col = 0; col < DECO_FONT_W; col++)
      if ((bits & (0x80 >> col)) && x + col >= 0 && x + col < dw)
        blend(&dst[(size_t)py * stride + x + col], rgb, 255);
  }
  return DECO_FONT_W;
}

int wvnc_text_draw(uint32_t* dst, int dw, int dh, size_t stride_px,
                   int x, int y, const char* utf8, int max_w, uint32_t rgb)
{
  uint32_t cps[MAX_CPS];
  size_t n = decode(utf8, cps), fit;
  bool ell;
  uint32_t ell_cp;
  int w, pen = x;

  fit = layout(cps, n, max_w, &ell, &ell_cp, &w);
  for (size_t i = 0; i < fit; i++)
    pen += draw_cp(dst, dw, dh, stride_px, pen, y, cps[i], rgb);
  if (ell)
    for (int i = 0; i < (ell_cp == '.' ? 3 : 1); i++)
      pen += draw_cp(dst, dw, dh, stride_px, pen, y, ell_cp, rgb);
  return pen - x;
}
