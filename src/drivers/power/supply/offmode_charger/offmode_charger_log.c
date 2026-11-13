// SPDX-License-Identifier: GPL-2.0
/*
 * Kernel log + persistent logs for the off-mode charger UI.
 *
 * Two files are kept:
 *
 *   driver.log  - driver plumbing: boot mode detection, init, framebuffer,
 *                 power key, errors and the verbose debug lines
 *   charger.log - charging: session progress, curve changes, summary
 *
 * Every line also goes to the kernel log.  The UI runs before init, so
 * /data is normally not mounted while the screen is up: lines are buffered
 * and a small thread writes them as soon as the partition appears, even
 * after init has been started.
 */

#define pr_fmt(fmt)	"offmode_charger: " fmt

#include <linux/delay.h>
#include <linux/err.h>
#include <linux/fs.h>
#include <linux/jiffies.h>
#include <linux/kernel.h>
#include <linux/kthread.h>
#include <linux/module.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/string.h>
#include <linux/syscalls.h>
#include <linux/uaccess.h>
#include <linux/version.h>

#include "offmode_charger_internal.h"

#define PCLOG_BUF_SIZE		(32 * 1024)
#define PCLOG_LINE_MAX		192
#define PCLOG_IDLE_EXIT		30	/* seconds after the last write */
#define PCLOG_IDLE_GIVEUP	300	/* seconds without ever writing */
#define PCLOG_MKDIR_RETRY	(2 * HZ)

enum pclog_chan {
	PCLOG_DRIVER = 0,
	PCLOG_CHARGER,
	PCLOG_CHANS,
};

static const char * const pclog_path[PCLOG_CHANS] = {
	[PCLOG_DRIVER]  = "/data/offmode_charger/driver.log",
	[PCLOG_CHARGER] = "/data/offmode_charger/charger.log",
};

static const char * const pclog_dirs[] = {
	"/data/offmode_charger",
};

static DEFINE_SPINLOCK(pclog_lock);
static char pclog_buf[PCLOG_CHANS][PCLOG_BUF_SIZE];
static size_t pclog_len[PCLOG_CHANS];
static unsigned int pclog_dropped[PCLOG_CHANS];
static bool pclog_wrote;
static int pclog_open_err[PCLOG_CHANS];
static int pclog_mkdir_err;
static struct task_struct *pclog_task;
static unsigned long pclog_next_mkdir;

/* ------------------------------------------------------------------ */
/* directory handling							*/
/* ------------------------------------------------------------------ */

static int pclog_mkdir(const char *path)
{
#if LINUX_VERSION_CODE < KERNEL_VERSION(5, 10, 0)
	mm_segment_t old_fs;
	int ret;

	old_fs = get_fs();
	set_fs(KERNEL_DS);
	ret = ksys_mkdir((const char __user *)path, 0755);
	set_fs(old_fs);

	return ret;
#else
	/*
	 * set_fs() is gone since 5.10 and ksys_mkdir() cannot be handed a
	 * kernel pointer safely anymore.  Rely on the directories being
	 * present (create it from an init script, or with a Magisk
	 * module).
	 */
	return -EOPNOTSUPP;
#endif
}

/*
 * /data is not mounted while the UI is up, in that case the lookups simply
 * fail with -ENOENT and we retry later.  Rate limited so the writer thread
 * does not hammer the VFS every second.
 */
static void pclog_prepare_dir(void)
{
	int i;

	if (time_before(jiffies, pclog_next_mkdir))
		return;

	pclog_next_mkdir = jiffies + PCLOG_MKDIR_RETRY;

	for (i = 0; i < ARRAY_SIZE(pclog_dirs); i++) {
		int ret = pclog_mkdir(pclog_dirs[i]);

		if (ret && ret != -EEXIST && ret != -ENOENT) {
			if (ret != pclog_mkdir_err) {
				pclog_mkdir_err = ret;
				offmode_charger_drv("mkdir %s failed: %d",
						    pclog_dirs[i], ret);
			}
			return;
		}
	}
}

/* ------------------------------------------------------------------ */
/* buffers								*/
/* ------------------------------------------------------------------ */

static void pclog_emit(int warn, enum pclog_chan chan, const char *line, int len)
{
	unsigned long flags;

	if (warn)
		pr_warn("%s\n", line);
	else
		pr_info("%s\n", line);

	spin_lock_irqsave(&pclog_lock, flags);
	if (pclog_len[chan] + len + 1 > sizeof(pclog_buf[chan])) {
		if (!pclog_dropped[chan])
			pr_warn("%s log buffer full, dropping entries\n",
				chan == PCLOG_CHARGER ? "charger" : "driver");
		pclog_dropped[chan]++;
	} else {
		memcpy(pclog_buf[chan] + pclog_len[chan], line, len);
		pclog_len[chan] += len;
		pclog_buf[chan][pclog_len[chan]++] = '\n';
	}
	spin_unlock_irqrestore(&pclog_lock, flags);
}

static void pclog_va(int warn, enum pclog_chan chan,
		     const char *fmt, va_list args)
{
	char line[PCLOG_LINE_MAX];
	int n;

	n = vscnprintf(line, sizeof(line), fmt, args);
	pclog_emit(warn, chan, line, n);
}

/* charging events */
void offmode_charger_log(const char *fmt, ...)
{
	va_list args;

	va_start(args, fmt);
	pclog_va(0, PCLOG_CHARGER, fmt, args);
	va_end(args);
}

/* driver plumbing */
void offmode_charger_drv(const char *fmt, ...)
{
	va_list args;

	va_start(args, fmt);
	pclog_va(0, PCLOG_DRIVER, fmt, args);
	va_end(args);
}

void offmode_charger_warn(const char *fmt, ...)
{
	va_list args;

	va_start(args, fmt);
	pclog_va(1, PCLOG_DRIVER, fmt, args);
	va_end(args);
}

/* ------------------------------------------------------------------ */
/* writer								*/
/* ------------------------------------------------------------------ */

static void pclog_write_chan(enum pclog_chan chan)
{
	size_t len, left;
	unsigned long flags;
	struct file *f;
	ssize_t ret;
	loff_t pos = 0;
	char *tmp;

	spin_lock_irqsave(&pclog_lock, flags);
	len = pclog_len[chan];
	spin_unlock_irqrestore(&pclog_lock, flags);

	if (!len)
		return;

	tmp = kmalloc(len, GFP_KERNEL);
	if (!tmp)
		return;

	spin_lock_irqsave(&pclog_lock, flags);
	len = min(len, pclog_len[chan]);
	memcpy(tmp, pclog_buf[chan], len);
	spin_unlock_irqrestore(&pclog_lock, flags);

	f = filp_open(pclog_path[chan], O_WRONLY | O_CREAT | O_APPEND, 0644);
	if (IS_ERR(f)) {
		int err = PTR_ERR(f);

		if (err == -ENOENT)
			pclog_prepare_dir();

		if (err != pclog_open_err[chan]) {
			pclog_open_err[chan] = err;
			offmode_charger_drv("cannot open %s: %d",
					    pclog_path[chan], err);
		}
		kfree(tmp);
		return;
	}

	ret = kernel_write(f, tmp, len, &pos);
	filp_close(f, NULL);
	kfree(tmp);

	if (ret <= 0)
		return;

	spin_lock_irqsave(&pclog_lock, flags);
	if (pclog_len[chan] >= (size_t)ret) {
		left = pclog_len[chan] - ret;
		memmove(pclog_buf[chan], pclog_buf[chan] + ret, left);
		pclog_len[chan] = left;
	} else {
		pclog_len[chan] = 0;
	}
	spin_unlock_irqrestore(&pclog_lock, flags);

	pclog_wrote = true;
}

static int pclog_fn(void *unused)
{
	unsigned int idle = 0;

	while (!kthread_should_stop()) {
		int i;

		for (i = 0; i < PCLOG_CHANS; i++)
			pclog_write_chan(i);

		if (!READ_ONCE(pclog_len[PCLOG_DRIVER]) &&
		    !READ_ONCE(pclog_len[PCLOG_CHARGER])) {
			idle++;
			if (pclog_wrote && idle > PCLOG_IDLE_EXIT)
				break;
			if (!pclog_wrote && idle > PCLOG_IDLE_GIVEUP)
				break;
		} else {
			idle = 0;
		}

		msleep(1000);
	}

	pclog_task = NULL;
	return 0;
}

void offmode_charger_log_start(void)
{
	if (pclog_task)
		return;

	pclog_task = kthread_run(pclog_fn, NULL, "offmode_chg_log");
	if (IS_ERR(pclog_task)) {
		pclog_task = NULL;
		pr_warn("cannot start the log thread\n");
	}
}

void offmode_charger_log_flush(void)
{
	int i;

	for (i = 0; i < PCLOG_CHANS; i++)
		pclog_write_chan(i);
}
