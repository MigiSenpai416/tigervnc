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
 * Text for the built-in window manager's UI (window switcher).
 * Anti-aliased with FreeType and a TrueType font file found without
 * fontconfig (the bundled one first); without FreeType or a font, the
 * built-in 6x13 bitmap font (deco_font.h) is used instead.
 */

#ifndef __XVNC_WAYLAND_WM_TEXT_H__
#define __XVNC_WAYLAND_WM_TEXT_H__

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Loads the font (once; later calls return the first result). px is
 * the pixel size of the anti-aliased font. Returns true if FreeType
 * text is available. */
bool wvnc_text_init(int px);

/* The font file in use, or NULL (bitmap font) */
const char* wvnc_text_font_path(void);

/* Line metrics in pixels: total line height and the baseline offset
 * from the top of the line */
int wvnc_text_line_height(void);
int wvnc_text_ascent(void);

/* Width of the text as drawn, cut to max_w with an ellipsis */
int wvnc_text_width(const char* utf8, int max_w);

/* Draws UTF-8 text with its top-left at (x, y) onto a premultiplied
 * ARGB8888 buffer, blending source-over; cut with an ellipsis ("...")
 * at max_w pixels. rgb is 0xRRGGBB. Returns the drawn width. */
int wvnc_text_draw(uint32_t* dst, int dw, int dh, size_t stride_px,
                   int x, int y, const char* utf8, int max_w,
                   uint32_t rgb);

#endif
