// SPDX-License-Identifier: GPL-2.0
/*
 * Kernel level off-mode (charger) UI.
 *
 * Replaces the user space "charged"/charger service for the off-mode charging
 * screen: when the bootloader boots with androidboot.mode=charger the kernel
 * keeps the display alive, watches the charger + fuel gauge, powers off when
 * the cable is pulled and continues to boot when the power key is held.
 *
 * Copyright (C) 2026
 */

#define pr_fmt(fmt)	"offmode_charger: " fmt

#include <linux/atomic.h>
#include <linux/console.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/init.h>
#include <linux/input.h>
#include <linux/jiffies.h>
#include <linux/kernel.h>
#include <linux/ktime.h>
#include <linux/limits.h>
#include <linux/math64.h>
#include <linux/module.h>
#include <linux/power_supply.h>
#include <linux/reboot.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/wait.h>
#include <linux/workqueue.h>

#include <linux/offmode_charger.h>

#include "offmode_charger_internal.h"

/* Wait this long for the charger/fuel gauge drivers to show up. */
#define LPM_PSY_WAIT_MS		10000
/* Give up and boot normally if the cable never shows up as online. */
#define LPM_NO_CHARGER_BOOT_MS	8000
/* Consecutive "not online" polls after which we power off. */
#define LPM_OFF_TICKS		3
/* Poll period, also the animation period. */
#define LPM_ANIM_MS		600
/* How long the power key has to be held to boot normally. */
#define LPM_BOOT_HOLD_MS	1500
/* How long the screen stays on after entering the charger UI. */
#define LPM_SCREEN_TIMEOUT_MS	10000

extern char *saved_command_line;

/*
 * Battery supply name.  The lookup also tries bms/batt and finally scans
 * the power_supply class for POWER_SUPPLY_TYPE_BATTERY, so this does not
 * need a module parameter.
 */
#define LPM_BATTERY_PSY_NAME	"battery"

/*
 * Xiaomi-style charge curve: keep the full current up to 80%, taper it
 * linearly down to 35% at 95% and start derating 3 C below LPM_TEMP_LIMIT.
 * At charge_limit the current drops to LPM_MIN_CURRENT_UA, i.e. charging stops.
 */
#define LPM_FAST_UNTIL	80
#define LPM_TAPER_END	95
#define LPM_TAPER_PCT	35
#define LPM_WARM_DELTA	3

static int charge_limit = 100;
module_param(charge_limit, int, 0644);
MODULE_PARM_DESC(charge_limit,
		 "Stop charging at this capacity in percent (100 disables)");

/* fixed policy knobs of the Xiaomi-style curve */
#define LPM_TEMP_LIMIT		45	/* pause charging above this (C) */
#define LPM_MIN_CURRENT_UA	100000	/* "stopped" charge current (uA) */

struct lpm_ctx {
	struct power_supply *batt;
	struct power_supply *usb;
};

static DECLARE_WAIT_QUEUE_HEAD(lpm_wait);
static struct delayed_work lpm_boot_work;
static void lpm_boot_work_fn(struct work_struct *work);
static bool lpm_boot_requested;
static bool lpm_wake_requested;
static bool lpm_running;

/* ------------------------------------------------------------------ */
/* boot mode detection							*/
/* ------------------------------------------------------------------ */

static bool lpm_cmdline_is_charger(void)
{
	if (!saved_command_line)
		return false;

	if (strstr(saved_command_line, "androidboot.mode=charger"))
		return true;

	/*
	 * A few bootloaders only pass the boolean form; harmless to accept,
	 * it is never rewritten by lpm_rewrite_cmdline().
	 */
	return strstr(saved_command_line, "androidboot.charger=1") != NULL;
}

static void lpm_dump_cmdline(void)
{
	const char *p = saved_command_line;
	char buf[161];
	int i;

	if (!p)
		return;

	for (i = 0; *p; i++, p += 160) {
		size_t n = strnlen(p, 160);

		memcpy(buf, p, n);
		buf[n] = '\0';
		pcdrv("cmdline[%d]: %s", i, buf);
	}
}

bool offmode_charger_init(void)
{
	bool charger;

	/*
	 * Start the log writer and dump the command line on every boot, not
	 * only in charger mode, so the decision is always traceable in
	 * /data/adb/offmode_charger/driver.log.
	 */
	offmode_charger_log_start();
	lpm_dump_cmdline();
	INIT_DELAYED_WORK(&lpm_boot_work, lpm_boot_work_fn);

	charger = lpm_cmdline_is_charger();
	pcdrv("boot mode: %s", charger ? "charger" : "normal");

	return charger;
}

/*
 * Android's init decides between "charger" and a normal boot by looking at
 * /proc/cmdline.  When we hand over to init after the user held the power key
 * we must make sure init does not start the user space charger service again,
 * but the command line is one big string and inserting a '\0' would cut off
 * everything behind it.  "normal " is exactly as long as "charger", so just
 * overwrite the value and keep the rest of the command line intact.
 */
static void lpm_rewrite_cmdline(void)
{
	static const char from[] = "androidboot.mode=charger";
	static const char to[] = "androidboot.mode=normal ";
	char *p;

	if (!saved_command_line)
		return;

	p = strstr(saved_command_line, from);
	if (!p)
		return;

	if (strlen(from) != strlen(to))
		return;

	memcpy(p, to, strlen(to));
	pcdrv("cmdline: charger mode cleared, continuing with a normal boot");
}

/* ------------------------------------------------------------------ */
/* power supply helpers							*/
/* ------------------------------------------------------------------ */

static bool lpm_psy_is_charger(int type)
{
	switch (type) {
	case POWER_SUPPLY_TYPE_USB:
	case POWER_SUPPLY_TYPE_USB_DCP:
	case POWER_SUPPLY_TYPE_USB_CDP:
	case POWER_SUPPLY_TYPE_USB_ACA:
	case POWER_SUPPLY_TYPE_USB_TYPE_C:
	case POWER_SUPPLY_TYPE_USB_PD:
	case POWER_SUPPLY_TYPE_USB_PD_DRP:
	case POWER_SUPPLY_TYPE_MAINS:
		return true;
	default:
		return false;
	}
}

struct lpm_psy_lookup {
	const char *name;
	bool charger;
	char found[32];
};

static int lpm_psy_lookup_cb(struct device *dev, void *data)
{
	struct lpm_psy_lookup *lk = data;
	struct power_supply *psy = dev_get_drvdata(dev);

	if (!psy || !psy->desc || !psy->desc->name)
		return 0;

	if (lk->name && !strcmp(psy->desc->name, lk->name)) {
		strlcpy(lk->found, psy->desc->name, sizeof(lk->found));
		return 1;
	}

	if (!lk->name) {
		if (lk->charger ? lpm_psy_is_charger(psy->desc->type) :
				  psy->desc->type == POWER_SUPPLY_TYPE_BATTERY) {
			strlcpy(lk->found, psy->desc->name, sizeof(lk->found));
			return 1;
		}
	}

	return 0;
}

static struct power_supply *lpm_get_psy(const char *name, bool charger)
{
	struct lpm_psy_lookup lk;
	struct power_supply *psy;

	if (name) {
		psy = power_supply_get_by_name(name);
		if (psy) {
			pcdbg("power supply \"%s\" found by name", name);
			return psy;
		}
	}

	memset(&lk, 0, sizeof(lk));
	lk.charger = charger;

	if (power_supply_class &&
	    class_for_each_device(power_supply_class, NULL, &lk,
				  lpm_psy_lookup_cb) == 1 && lk.found[0]) {
		pcdbg("power supply \"%s\" found by %s", lk.found,
		      charger ? "charger type" : "battery type");
		return power_supply_get_by_name(lk.found);
	}

	return NULL;
}

static struct power_supply *lpm_get_charger_psy(void)
{
	static const char * const names[] = {
		"usb", "main", "charger", "ac", "pc_port", "wireless",
	};
	struct power_supply *psy;
	int i;

	for (i = 0; i < ARRAY_SIZE(names); i++) {
		psy = power_supply_get_by_name(names[i]);
		if (psy) {
			pcdbg("charger supply \"%s\" found", names[i]);
			return psy;
		}
	}

	return lpm_get_psy(NULL, true);
}

static struct power_supply *lpm_get_battery_psy(void)
{
	static const char * const names[] = { "battery", "bms", "batt" };
	struct power_supply *psy;
	int i;

	psy = lpm_get_psy(LPM_BATTERY_PSY_NAME, false);
	if (psy)
		return psy;

	for (i = 0; i < ARRAY_SIZE(names); i++) {
		if (!strcmp(names[i], LPM_BATTERY_PSY_NAME))
			continue;

		psy = power_supply_get_by_name(names[i]);
		if (psy) {
			pcdbg("battery supply \"%s\" found", names[i]);
			return psy;
		}
	}

	return lpm_get_psy(NULL, false);
}

static int lpm_wait_supplies(struct lpm_ctx *ctx)
{
	unsigned long timeout = jiffies + msecs_to_jiffies(LPM_PSY_WAIT_MS);

	for (;;) {
		if (!ctx->batt)
			ctx->batt = lpm_get_battery_psy();
		if (!ctx->usb)
			ctx->usb = lpm_get_charger_psy();

		if (ctx->batt) {
			pcdrv("supplies ready: battery found");
			return 0;
		}

		if (time_after(jiffies, timeout)) {
			pcwarn("timed out waiting for the battery supply");
			return -ENODEV;
		}

		msleep(100);
	}
}

static void lpm_read_state(struct lpm_ctx *ctx, struct offmode_charger_state *st)
{
	union power_supply_propval v;

	st->capacity = -1;
	st->status = POWER_SUPPLY_STATUS_UNKNOWN;
	st->temp = S32_MIN;
	st->present = -1;
	st->online = 0;
	st->power_uw = 0;
	st->charge_ms = 0;
	st->avg_power_uw = 0;

	if (ctx->batt) {
		if (!power_supply_get_property(ctx->batt,
					       POWER_SUPPLY_PROP_CAPACITY, &v))
			st->capacity = clamp(v.intval, 0, 100);

		if (!power_supply_get_property(ctx->batt,
					       POWER_SUPPLY_PROP_STATUS, &v))
			st->status = v.intval;

		if (!power_supply_get_property(ctx->batt,
					       POWER_SUPPLY_PROP_TEMP, &v))
			st->temp = v.intval;

		if (!power_supply_get_property(ctx->batt,
					       POWER_SUPPLY_PROP_PRESENT, &v))
			st->present = !!v.intval;

		{
			int v_uv = 0, i_ua = 0;

			if (!power_supply_get_property(ctx->batt,
					POWER_SUPPLY_PROP_VOLTAGE_NOW, &v))
				v_uv = v.intval;
			if (!power_supply_get_property(ctx->batt,
					POWER_SUPPLY_PROP_CURRENT_NOW, &v))
				i_ua = v.intval;

			if (v_uv > 0 && i_ua > 0)
				st->power_uw = div_u64((u64)v_uv * i_ua,
						       1000000);
		}
	}

	if (ctx->usb) {
		if (!power_supply_get_property(ctx->usb,
					       POWER_SUPPLY_PROP_ONLINE, &v))
			st->online = !!v.intval;
		else if (!power_supply_get_property(ctx->usb,
						    POWER_SUPPLY_PROP_PRESENT, &v))
			st->online = !!v.intval;
		else
			st->online = 0;
	} else {
		/*
		 * No charger supply at all: trust the battery status so that
		 * boards with only a fuel gauge still work.
		 */
		st->online = st->status == POWER_SUPPLY_STATUS_CHARGING ||
			     st->status == POWER_SUPPLY_STATUS_FULL;
	}

	pcdbg("state: cap=%d status=%d temp=%d present=%d online=%d",
	      st->capacity, st->status, st->temp, st->present, st->online);
}

/* ------------------------------------------------------------------ */
/* power key								*/
/* ------------------------------------------------------------------ */

static void lpm_boot_work_fn(struct work_struct *work)
{
	pcdrv("power key held for %d ms, booting normally", LPM_BOOT_HOLD_MS);
	WRITE_ONCE(lpm_boot_requested, true);
	wake_up(&lpm_wait);
}

#if IS_ENABLED(CONFIG_INPUT)
static struct input_handle *lpm_input_handle;
static bool lpm_input_registered;

static void lpm_input_event(struct input_handle *handle, unsigned int type,
			    unsigned int code, int value)
{
	if (type != EV_KEY || code != KEY_POWER)
		return;

	pcdbg("power key event: value=%d", value);

	if (value == 1) {
		schedule_delayed_work(&lpm_boot_work,
				      msecs_to_jiffies(LPM_BOOT_HOLD_MS));
	} else if (value == 0) {
		/* released before the hold timeout: a short press */
		if (cancel_delayed_work(&lpm_boot_work) &&
		    !READ_ONCE(lpm_boot_requested)) {
			WRITE_ONCE(lpm_wake_requested, true);
			wake_up(&lpm_wait);
		}
	}
}

static int lpm_input_connect(struct input_handler *handler,
			     struct input_dev *dev,
			     const struct input_device_id *id)
{
	struct input_handle *handle;
	int error;

	if (!test_bit(KEY_POWER, dev->keybit))
		return -ENODEV;

	/* one power key is enough */
	if (lpm_input_handle)
		return -ENODEV;

	handle = kzalloc(sizeof(*handle), GFP_KERNEL);
	if (!handle)
		return -ENOMEM;

	handle->dev = dev;
	handle->handler = handler;
	handle->name = "offmode_charger";

	error = input_register_handle(handle);
	if (error) {
		kfree(handle);
		return error;
	}

	error = input_open_device(handle);
	if (error) {
		input_unregister_handle(handle);
		kfree(handle);
		return error;
	}

	lpm_input_handle = handle;
	pcdbg("power key handler attached to \"%s\"", dev->name);
	return 0;
}

static void lpm_input_disconnect(struct input_handle *handle)
{
	if (handle != lpm_input_handle)
		return;

	input_close_device(handle);
	input_unregister_handle(handle);
	lpm_input_handle = NULL;
	kfree(handle);
}

static const struct input_device_id lpm_input_ids[] = {
	{
		.flags = INPUT_DEVICE_ID_MATCH_EVBIT,
		.evbit = { BIT_MASK(EV_KEY) },
	},
	{ },
};

static struct input_handler lpm_input_handler = {
	.event		= lpm_input_event,
	.connect	= lpm_input_connect,
	.disconnect	= lpm_input_disconnect,
	.name		= "offmode_charger",
	.id_table	= lpm_input_ids,
};

static void lpm_input_register(void)
{
	lpm_input_registered = !input_register_handler(&lpm_input_handler);
	if (!lpm_input_registered)
		pcwarn("cannot register power key handler, hold-to-boot disabled");
}

static void lpm_input_unregister(void)
{
	if (!lpm_input_registered)
		return;

	if (lpm_input_handle)
		lpm_input_disconnect(lpm_input_handle);

	input_unregister_handler(&lpm_input_handler);
	lpm_input_registered = false;
}
#else /* !CONFIG_INPUT */
static void lpm_input_register(void) { }
static void lpm_input_unregister(void) { }
#endif /* CONFIG_INPUT */

/* ------------------------------------------------------------------ */
/* main loop								*/
/* ------------------------------------------------------------------ */

/* ------------------------------------------------------------------ */
/* charge session statistics						*/
/* ------------------------------------------------------------------ */

struct lpm_stats {
	ktime_t last;
	u64 ms;
	u64 energy_nj;
	u64 last_report_ms;
	int start_cap;
	bool active;
	bool started;
};

static struct lpm_stats lpm_stat;

static void lpm_stats_sample(struct offmode_charger_state *st)
{
	s64 dt;

	if (!lpm_stat.started) {
		lpm_stat.started = true;
		lpm_stat.last = ktime_get();
		lpm_stat.start_cap = st->capacity;
		lpm_stat.active = st->status == POWER_SUPPLY_STATUS_CHARGING;
	}

	dt = ktime_ms_delta(ktime_get(), lpm_stat.last);
	lpm_stat.last = ktime_get();
	if (dt <= 0 || dt > 5000)
		dt = 0;

	if (st->status == POWER_SUPPLY_STATUS_CHARGING)
		lpm_stat.active = true;

	if (lpm_stat.active && dt > 0) {
		lpm_stat.ms += dt;
		lpm_stat.energy_nj += (u64)st->power_uw * dt;
	}

	st->charge_ms = lpm_stat.ms;
	st->avg_power_uw = lpm_stat.ms ?
		(u32)div64_u64(lpm_stat.energy_nj, lpm_stat.ms) : 0;

	if (lpm_stat.active && lpm_stat.ms - lpm_stat.last_report_ms >= 600000) {
		lpm_stat.last_report_ms = lpm_stat.ms;
		pclog("charging: %llu min, %u.%u W avg, %llu.%llu Wh",
		      div_u64(lpm_stat.ms, 60000),
		      st->avg_power_uw / 1000000,
		      (st->avg_power_uw / 100000) % 10,
		      div64_u64(lpm_stat.energy_nj, 3600000000000ULL),
		      div64_u64(lpm_stat.energy_nj, 360000000000ULL) % 10);
	}
}

static void lpm_stats_report(const char *why,
			     const struct offmode_charger_state *st)
{
	u32 avg = st->avg_power_uw;

	if (!lpm_stat.ms)
		return;

	pclog("charge session %s: %llu min, avg %u.%u W, %llu.%llu Wh, %d%% -> %d%%",
	      why,
	      div_u64(lpm_stat.ms, 60000),
	      avg / 1000000, (avg / 100000) % 10,
	      div64_u64(lpm_stat.energy_nj, 3600000000000ULL),
	      div64_u64(lpm_stat.energy_nj, 360000000000ULL) % 10,
	      lpm_stat.start_cap, st->capacity);
}

/* ------------------------------------------------------------------ */
/* charging curve							*/
/* ------------------------------------------------------------------ */

struct lpm_ctl {
	struct power_supply *psy;
	const char *name;
	int max_ua;
	int last_ua;
	bool ready;
};

static struct lpm_ctl lpm_ctl;

static int lpm_ctl_setup(void)
{
	static const char * const names[] = { "main", "battery", "usb" };
	union power_supply_propval v;
	struct power_supply *psy;
	int i;

	for (i = 0; i < ARRAY_SIZE(names); i++) {
		psy = power_supply_get_by_name(names[i]);
		if (!psy)
			continue;

		if (power_supply_get_property(psy,
				POWER_SUPPLY_PROP_CONSTANT_CHARGE_CURRENT_MAX,
				&v) || v.intval <= 0) {
			power_supply_put(psy);
			continue;
		}

		/*
		 * qcom's charger returns 0 from property_is_writeable() for
		 * FCC even though set_property() handles it, so probe by
		 * writing the current value back instead.
		 */
		if (power_supply_set_property(psy,
				POWER_SUPPLY_PROP_CONSTANT_CHARGE_CURRENT_MAX,
				&v)) {
			power_supply_put(psy);
			continue;
		}

		lpm_ctl.psy = psy;
		lpm_ctl.name = names[i];
		lpm_ctl.max_ua = v.intval;
		lpm_ctl.last_ua = v.intval;
		lpm_ctl.ready = true;

		pclog("charge curve on \"%s\": max %d mA, stop at %d%%, pause above %d C",
		      names[i], v.intval / 1000, charge_limit, LPM_TEMP_LIMIT);
		return 0;
	}

	pcwarn("no writable CONSTANT_CHARGE_CURRENT_MAX, charge curve disabled");
	return -ENODEV;
}

static void lpm_ctl_apply(const struct offmode_charger_state *st)
{
	union power_supply_propval v;
	int limit = charge_limit;
	int pct = 100;
	int target;

	if (!lpm_ctl.ready)
		return;

	if (limit <= 0 || limit > 100)
		limit = 100;

	/* thermal derating first, it wins over everything else */
	if (st->temp != S32_MIN) {
		if (st->temp >= LPM_TEMP_LIMIT * 10)
			pct = 0;
		else if (st->temp >= (LPM_TEMP_LIMIT - LPM_WARM_DELTA) * 10)
			pct = 50;
	}

	/* Xiaomi-style curve: full current, then a linear taper */
	if (pct > 0 && st->capacity >= 0) {
		if (st->capacity >= LPM_TAPER_END) {
			pct = min(pct, LPM_TAPER_PCT);
		} else if (st->capacity > LPM_FAST_UNTIL) {
			int drop = 100 - LPM_TAPER_PCT;
			int span = LPM_TAPER_END - LPM_FAST_UNTIL;

			pct = min(pct, 100 - drop *
				  (st->capacity - LPM_FAST_UNTIL) / span);
		}
	}

	if (limit < 100 && st->capacity >= limit)
		pct = 0;

	target = pct ? lpm_ctl.max_ua * pct / 100 : LPM_MIN_CURRENT_UA;
	if (target < LPM_MIN_CURRENT_UA)
		target = LPM_MIN_CURRENT_UA;
	if (target == lpm_ctl.last_ua)
		return;

	v.intval = target;
	if (power_supply_set_property(lpm_ctl.psy,
			POWER_SUPPLY_PROP_CONSTANT_CHARGE_CURRENT_MAX, &v)) {
		pcdbg("cannot set charge current to %d mA", target / 1000);
		return;
	}

	pcdbg("curve: %d -> %d mA (cap %d%%, temp %d C, %d%%)",
	      lpm_ctl.last_ua / 1000, target / 1000, st->capacity,
	      st->temp == S32_MIN ? -1 : st->temp / 10, pct);

	lpm_ctl.last_ua = target;
}

static void lpm_ctl_restore(void)
{
	union power_supply_propval v;

	if (!lpm_ctl.ready)
		return;

	v.intval = lpm_ctl.max_ua;
	power_supply_set_property(lpm_ctl.psy,
				  POWER_SUPPLY_PROP_CONSTANT_CHARGE_CURRENT_MAX,
				  &v);
	power_supply_put(lpm_ctl.psy);
	lpm_ctl.ready = false;
}

static void __noreturn lpm_power_off(void)
{
	pcdrv("powering off");
	kernel_power_off();

	pcwarn("kernel_power_off() returned, no pm_power_off handler?");

	for (;;)
		msleep(1000);
}

void offmode_charger_start(void)
{
	struct lpm_ctx ctx = { NULL, NULL };
	struct offmode_charger_state st = { 0 };
	unsigned long deadline, screen_deadline;
	bool anim = false;
	bool screen_on = true;
	bool seen_online = false;
	bool console_off = false;
	int off_ticks = 0;

	if (lpm_running)
		return;

	lpm_running = true;
	pcdrv("entering kernel off-mode charger UI");

	if (lpm_wait_supplies(&ctx) < 0) {
		pcwarn("no battery power supply, booting normally");
		goto out;
	}

	suspend_console();
	console_off = true;

	if (offmode_charger_display_init() < 0)
		pcwarn("no usable framebuffer, charger UI runs headless");

	lpm_input_register();
	lpm_ctl_setup();

	deadline = jiffies + msecs_to_jiffies(LPM_NO_CHARGER_BOOT_MS);
	screen_deadline = jiffies + msecs_to_jiffies(LPM_SCREEN_TIMEOUT_MS);

	for (;;) {
		if (!ctx.usb)
			ctx.usb = lpm_get_charger_psy();

		if (READ_ONCE(lpm_wake_requested)) {
			WRITE_ONCE(lpm_wake_requested, false);
			if (!screen_on) {
				offmode_charger_display_blank(false);
				screen_on = true;
				pcdrv("screen on (power key)");
			}
			screen_deadline = jiffies +
				msecs_to_jiffies(LPM_SCREEN_TIMEOUT_MS);
		}

		lpm_read_state(&ctx, &st);
		lpm_stats_sample(&st);
		lpm_ctl_apply(&st);

		if (st.online) {
			if (!seen_online)
				pclog("charger online, start at %d%%", st.capacity);
			seen_online = true;
			off_ticks = 0;
		} else if (seen_online) {
			off_ticks++;
		}

		if (screen_on && time_after(jiffies, screen_deadline)) {
			screen_on = false;
			offmode_charger_display_blank(true);
			pcdrv("screen off");
		}

		if (screen_on)
			offmode_charger_display_update(&st, anim);

		if (READ_ONCE(lpm_boot_requested))
			break;

		if (seen_online && off_ticks >= LPM_OFF_TICKS) {
			pclog("charger offline for %d polls, powering off", off_ticks);
			lpm_stats_report("ended", &st);
			lpm_ctl_restore();
			offmode_charger_display_shutdown();
			resume_console();
			lpm_input_unregister();
			lpm_power_off();
		}

		if (!seen_online && time_after(jiffies, deadline)) {
			pcwarn("charger never came online, booting normally");
			break;
		}

		wait_event_timeout(lpm_wait,
				   READ_ONCE(lpm_boot_requested) ||
				   READ_ONCE(lpm_wake_requested),
				   msecs_to_jiffies(LPM_ANIM_MS));
		anim = !anim;
	}

out:
	cancel_delayed_work_sync(&lpm_boot_work);
	if (!screen_on)
		offmode_charger_display_blank(false);
	lpm_stats_report("ended", &st);
	lpm_ctl_restore();
	offmode_charger_display_shutdown();
	if (console_off)
		resume_console();
	lpm_input_unregister();
	lpm_rewrite_cmdline();

	if (ctx.batt)
		power_supply_put(ctx.batt);
	if (ctx.usb)
		power_supply_put(ctx.usb);

	lpm_running = false;
	pcdrv("leaving off-mode charger UI");
	offmode_charger_log_flush();
}
