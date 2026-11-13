/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Kernel level off-mode (charger) UI.
 *
 * When the bootloader boots the kernel with "androidboot.mode=charger" the
 * kernel normally starts /init, and Android's charger service (the "charged"
 * binary) draws the charging screen from user space.
 *
 * With CONFIG_OFFMODE_CHARGER=y the kernel runs the charging UI itself, before
 * (and instead of) exec'ing /init.  init/main.c only has to do:
 *
 *	if (offmode_charger_init())
 *		offmode_charger_start();
 *
 * offmode_charger_init() runs on every boot: it brings up the logging and
 * decides whether this is a charger boot.  offmode_charger_start() then takes
 * over the init thread and only returns when the user asked for a normal boot;
 * it must always be preceded by offmode_charger_init().
 */
#ifndef _LINUX_OFFMODE_CHARGER_H
#define _LINUX_OFFMODE_CHARGER_H

#include <linux/types.h>

#ifdef CONFIG_OFFMODE_CHARGER

bool offmode_charger_requested(void);
bool offmode_charger_init(void);
void offmode_charger_start(void);

#else /* !CONFIG_OFFMODE_CHARGER */

static inline bool offmode_charger_requested(void)
{
	return false;
}

static inline bool offmode_charger_init(void)
{
	return false;
}

static inline void offmode_charger_start(void)
{
}

#endif /* CONFIG_OFFMODE_CHARGER */

#endif /* _LINUX_OFFMODE_CHARGER_H */
