// SPDX-License-Identifier: GPL-2.0
/*
 * Framebuffer back end for the kernel off-mode charger UI.
 *
 * Only depends on struct fb_info and on the framebuffer memory itself, so it
 * works with any fbdev provider (drm_fb_helper based msm fbdev, simplefb,
 * efifb, the legacy FB_MSM driver, ...) and does not need fbcon, a colour
 * map or the console font code.
 */

#define pr_fmt(fmt)	"offmode_charger: " fmt

#include <linux/delay.h>
#include <linux/errno.h>
#include <linux/fb.h>
#include <linux/jiffies.h>
#include <linux/kernel.h>
#include <linux/limits.h>
#include <linux/math64.h>
#include <linux/module.h>
#include <linux/power_supply.h>
#include <linux/string.h>

#include "offmode_charger_internal.h"

#define LPM_FB_WAIT_MS	15000

extern struct fb_info *registered_fb[];
extern int num_registered_fb;

static struct fb_info *lpm_fb;
static u32 lpm_w, lpm_h, lpm_stride;
static u32 lpm_bpp;
static u32 lpm_black, lpm_white, lpm_green, lpm_yellow, lpm_red, lpm_gray;

/* last painted state, used to avoid repainting the whole panel every frame */
static int lpm_last_cap = -2;
static int lpm_last_status = -2;
static int lpm_last_online = -2;
static int lpm_last_present = -2;

static u32 lpm_rgb(u8 r, u8 g, u8 b)
{
	switch (lpm_bpp) {
	case 16:
		return ((u32)(r >> 3) << 11) | ((u32)(g >> 2) << 5) |
		       (b >> 3);
	default:
		return ((u32)r << 16) | ((u32)g << 8) | b;
	}
}

/* ------------------------------------------------------------------ */
/* primitive drawing							*/
/* ------------------------------------------------------------------ */

static void lpm_fill(int x, int y, int w, int h, u32 color)
{
	u8 *dst;
	int bytes, row, col;

	if (!lpm_fb || w <= 0 || h <= 0)
		return;

	if (x < 0) {
		w += x;
		x = 0;
	}
	if (y < 0) {
		h += y;
		y = 0;
	}
	if (x + w > (int)lpm_w)
		w = (int)lpm_w - x;
	if (y + h > (int)lpm_h)
		h = (int)lpm_h - y;
	if (w <= 0 || h <= 0)
		return;

	bytes = lpm_bpp >> 3;
	dst = (u8 *)lpm_fb->screen_buffer + (u32)y * lpm_stride +
	      (u32)x * bytes;

	for (row = 0; row < h; row++) {
		if (bytes == 4) {
			u32 *p = (u32 *)dst;

			for (col = 0; col < w; col++)
				p[col] = color;
		} else if (bytes == 2) {
			u16 *p = (u16 *)dst;

			for (col = 0; col < w; col++)
				p[col] = (u16)color;
		} else {
			u8 *p = dst;

			for (col = 0; col < w; col++) {
				*p++ = color & 0xff;
				*p++ = (color >> 8) & 0xff;
				*p++ = (color >> 16) & 0xff;
			}
		}
		dst += lpm_stride;
	}
}

static void lpm_glyph(int x, int y, int scale, unsigned char c, u32 fg)
{
	const u8 *g = offmode_charger_font_glyph(c);
	int row, bit;

	if (!g)
		return;

	for (row = 0; row < 8; row++) {
		for (bit = 0; bit < 8; bit++) {
			bool on = g[row] & (0x80 >> bit);

			lpm_fill(x + bit * scale, y + row * scale,
				 scale, scale, on ? fg : lpm_black);
		}
	}
}

static void lpm_text(const char *s, int scale, int y, u32 fg)
{
	int len = strlen(s);
	int x = ((int)lpm_w - len * 8 * scale) / 2;

	if (x < 0)
		x = 0;

	for (; *s; s++, x += 8 * scale)
		lpm_glyph(x, y, scale, *s, fg);
}

/* ------------------------------------------------------------------ */
/* layout								*/
/* ------------------------------------------------------------------ */

struct lpm_layout {
	int bw;
	int bh;
	int brd;
	int bx;
	int by;
	int bottom;
};

static struct lpm_layout lpm_compute_layout(void)
{
	struct lpm_layout l;

	l.bw = (int)(lpm_w * 45 / 100);
	if (l.bw > (int)(lpm_h * 32 / 100))
		l.bw = (int)(lpm_h * 32 / 100);
	l.bw = max(l.bw, 24);

	l.bh = l.bw * 17 / 10;
	if (l.bh > (int)(lpm_h * 55 / 100)) {
		l.bh = (int)(lpm_h * 55 / 100);
		l.bw = l.bh * 10 / 17;
	}

	l.brd = max(3, l.bw / 45);
	l.bx = ((int)lpm_w - l.bw) / 2;
	l.by = (int)(lpm_h * 38 / 100) - l.bh / 2;
	l.bottom = l.by + l.bh;

	return l;
}

static void lpm_paint_battery(const struct lpm_layout *l, int cap)
{
	int capw, caph, iw, ih, fill;
	u32 color;

	/* battery terminal */
	capw = max(8, l->bw / 4);
	caph = max(4, l->bw / 22);
	lpm_fill(l->bx + (l->bw - capw) / 2, l->by - caph, capw, caph,
		 lpm_gray);

	/* battery body */
	lpm_fill(l->bx, l->by, l->bw, l->brd, lpm_gray);
	lpm_fill(l->bx, l->by + l->bh - l->brd, l->bw, l->brd, lpm_gray);
	lpm_fill(l->bx, l->by, l->brd, l->bh, lpm_gray);
	lpm_fill(l->bx + l->bw - l->brd, l->by, l->brd, l->bh, lpm_gray);

	/* charge level */
	iw = l->bw - 2 * l->brd;
	ih = l->bh - 2 * l->brd;
	if (cap < 0 || iw <= 0 || ih <= 0)
		return;

	if (cap >= 50)
		color = lpm_green;
	else if (cap >= 20)
		color = lpm_yellow;
	else
		color = lpm_red;

	fill = ih * clamp(cap, 0, 100) / 100;
	lpm_fill(l->bx + l->brd, l->by + l->bh - l->brd - fill, iw, fill,
		 color);
}

static int lpm_percent_scale(int bw)
{
	int scale = max(1, bw / 32);

	return min(scale, max(1, (int)lpm_w / (4 * 8)));
}

static void lpm_draw_percent(const struct offmode_charger_state *st, int bw, int y)
{
	char buf[8];

	if (st->capacity < 0)
		return;

	snprintf(buf, sizeof(buf), "%d%%", clamp(st->capacity, 0, 100));
	lpm_text(buf, lpm_percent_scale(bw), y, lpm_white);
}

static const char *lpm_status_text(const struct offmode_charger_state *st,
				   bool anim, u32 *color)
{
	*color = lpm_white;

	if (st->temp != S32_MIN && st->temp >= 450) {
		*color = lpm_red;
		return "TEMP HIGH";
	}

	if (st->present == 0) {
		*color = lpm_red;
		return "NO BATTERY";
	}

	if (!st->online) {
		*color = lpm_yellow;
		return "NOT CHARGING";
	}

	if (st->status == POWER_SUPPLY_STATUS_FULL || st->capacity >= 100) {
		*color = lpm_green;
		return "FULL";
	}

	if (st->status == POWER_SUPPLY_STATUS_CHARGING)
		return anim ? "CHARGING..." : "CHARGING.";

	return "CONNECTED";
}

static int lpm_draw_status(const struct offmode_charger_state *st, int y, bool anim)
{
	const char *s;
	u32 color;
	int len, scale;

	s = lpm_status_text(st, anim, &color);
	len = strlen(s);

	scale = max(1, (int)lpm_w / (len * 8 * 2));
	scale = min(scale, 10);
	if (len * 8 * scale > (int)lpm_w)
		scale = 1;

	lpm_text(s, scale, y, color);

	return 8 * scale + 10;
}

/* "12m34s 18.4W" under the status line, once a charge session is running */
static void lpm_draw_stats(const struct offmode_charger_state *st, int y)
{
	char buf[40];
	u32 p = st->avg_power_uw;
	u64 ms = st->charge_ms;
	unsigned int min, sec;
	int len, scale;

	if (!ms)
		return;

	min = div_u64(ms, 60000);
	sec = div_u64(ms % 60000, 1000);

	if (min >= 60)
		snprintf(buf, sizeof(buf), "%uh%02um", min / 60, min % 60);
	else
		snprintf(buf, sizeof(buf), "%um%02us", min, sec);

	if (p) {
		len = strlen(buf);
		snprintf(buf + len, sizeof(buf) - len, "  %u.%uW",
			 p / 1000000, (p / 100000) % 10);
	}

	len = strlen(buf);
	scale = max(1, (int)lpm_w / (len * 8 * 3));
	scale = min(scale, 8);
	if (len * 8 * scale > (int)lpm_w)
		scale = 1;

	lpm_text(buf, scale, y, lpm_gray);
}

/* ------------------------------------------------------------------ */
/* public interface							*/
/* ------------------------------------------------------------------ */

int offmode_charger_display_init(void)
{
	unsigned long timeout = jiffies + msecs_to_jiffies(LPM_FB_WAIT_MS);

	for (;;) {
		struct fb_info *best = NULL;
		u32 best_area = 0;
		int i;

		for (i = 0; i < FB_MAX; i++) {
			struct fb_info *info = registered_fb[i];
			u32 area;

			if (!info || !info->screen_buffer)
				continue;
			if (info->state != FBINFO_STATE_RUNNING)
				continue;
			if (!info->var.xres || !info->var.yres)
				continue;
			if (info->var.bits_per_pixel != 16 &&
			    info->var.bits_per_pixel != 24 &&
			    info->var.bits_per_pixel != 32)
				continue;

			area = info->var.xres * info->var.yres;
			if (area > best_area) {
				best = info;
				best_area = area;
			}
		}

		if (best) {
			lpm_fb = best;
			break;
		}

		if (time_after(jiffies, timeout)) {
			pcdrv("no framebuffer registered (num_registered_fb=%d)",
			      num_registered_fb);
			return -ENODEV;
		}

		msleep(100);
	}

	lpm_w = lpm_fb->var.xres;
	lpm_h = lpm_fb->var.yres;
	lpm_bpp = lpm_fb->var.bits_per_pixel;
	lpm_stride = lpm_fb->fix.line_length;
	if (!lpm_stride)
		lpm_stride = lpm_w * lpm_bpp / 8;

	lpm_black  = lpm_rgb(0x00, 0x00, 0x00);
	lpm_white  = lpm_rgb(0xff, 0xff, 0xff);
	lpm_green  = lpm_rgb(0x00, 0xd4, 0x6a);
	lpm_yellow = lpm_rgb(0xff, 0xc1, 0x07);
	lpm_red    = lpm_rgb(0xff, 0x3b, 0x30);
	lpm_gray   = lpm_rgb(0x6e, 0x6e, 0x6e);

	pcdrv("framebuffer %ux%u %ubpp, stride %u",
	      lpm_w, lpm_h, lpm_bpp, lpm_stride);

	return 0;
}
/*
 * Blank or unblank the panel.  Unblanking forces a full repaint, because
 * the state may have changed while the screen was off.
 */
int offmode_charger_display_blank(bool blank)
{
	int ret;

	if (!lpm_fb)
		return -ENODEV;

	ret = fb_blank(lpm_fb,
		       blank ? FB_BLANK_POWERDOWN : FB_BLANK_UNBLANK);
	if (!blank) {
		lpm_last_cap = -2;
		lpm_last_status = -2;
		lpm_last_online = -2;
		lpm_last_present = -2;
	}

	return ret;
}

void offmode_charger_display_update(const struct offmode_charger_state *st, bool anim)
{
	struct lpm_layout lay;
	bool full;
	int pscale, y, band;

	if (!lpm_fb)
		return;

	full = st->capacity != lpm_last_cap ||
	       st->status != lpm_last_status ||
	       st->online != lpm_last_online ||
	       st->present != lpm_last_present;

	lay = lpm_compute_layout();
	pscale = lpm_percent_scale(lay.bw);
	y = lay.bottom + lay.bw / 10;

	if (full) {
		pcdbg("redraw: cap=%d status=%d online=%d present=%d",
		      st->capacity, st->status, st->online, st->present);
		lpm_fill(0, 0, (int)lpm_w, (int)lpm_h, lpm_black);
		lpm_paint_battery(&lay, st->capacity);

		lpm_last_cap = st->capacity;
		lpm_last_status = st->status;
		lpm_last_online = st->online;
		lpm_last_present = st->present;
	} else {
		/* only the text block changes between animation frames */
		band = 8 * pscale + lay.bw / 20 + 8 * 10 + 8 * 8 + 60;
		lpm_fill(0, y - 8, (int)lpm_w, band, lpm_black);
	}

	lpm_draw_percent(st, lay.bw, y);
	y += 8 * pscale + lay.bw / 20;
	y += lpm_draw_status(st, y, anim);
	lpm_draw_stats(st, y);
}

void offmode_charger_display_shutdown(void)
{
	if (!lpm_fb)
		return;

	lpm_fill(0, 0, (int)lpm_w, (int)lpm_h, lpm_black);
}
