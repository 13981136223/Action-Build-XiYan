// SPDX-License-Identifier: GPL-2.0
/*
 * ace5_dsi_offsets.c — 迭代 2b：偏移扫描器（结果走 sysfs，不靠 dmesg）
 *
 * 上一版两个问题：
 *   1) 只挂 dsi_display_set_mode，息屏/亮屏/改帧率都没让它命中；
 *   2) 输出全靠 pr_info，这台机器 KernelSU 日志刷得飞快，两分钟就冲干净。
 * 现在：多挂几个必然被调的入口，结果同时写进
 *   /sys/module/ace5_dsi_offsets/parameters/report 与 dmesg。
 * 仍然全程只读（copy_from_kernel_nofault），不写任何内存。
 */
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/stdarg.h>
#include <linux/kprobes.h>
#include <linux/uaccess.h>
#include <linux/string.h>
#include <linux/err.h>
#include <linux/spinlock.h>

#define TAG "ace5off"
#define WIN 2048
#define SUBWIN 256
#define REPORT_CAP 3000		/* module_param get 只能给一页，留足余量 */

static const char panel_prefix[] = "qcom,mdss_dsi_panel";
static const u32 expect_pclk = 1012000000;	/* 0x3c51e500 */
static const u32 expect_mode_cnt = 4;
static const u32 expect_fps = 120;

static char report[REPORT_CAP];
static size_t report_len;
static DEFINE_SPINLOCK(report_lock);

static void *captured;
static const char *captured_from;
static int identified;
static int fired;			/* 探针被调用过几次（不管有没有认出来） */
static const char *seen_from[8];
static int nseen;

/* 结果同时进 dmesg 和 report，避免日志被冲掉后什么都拿不到 */
__printf(1, 2)
static void say(const char *fmt, ...)
{
	char line[160];
	va_list ap;
	unsigned long flags;
	int n;

	va_start(ap, fmt);
	n = vsnprintf(line, sizeof(line), fmt, ap);
	va_end(ap);
	if (n < 0)
		return;
	pr_info(TAG ": %s\n", line);

	spin_lock_irqsave(&report_lock, flags);
	if (report_len + n + 1 < REPORT_CAP) {
		memcpy(report + report_len, line, n);
		report_len += n;
		report[report_len++] = '\n';
	}
	spin_unlock_irqrestore(&report_lock, flags);
}

static bool kern_addr(unsigned long a)
{
	return (long)a < 0 && !IS_ERR((void *)a);
}

static int rd(unsigned long addr, void *buf, size_t len)
{
	if (!kern_addr(addr))
		return -EINVAL;
	return copy_from_kernel_nofault(buf, (void *)addr, len);
}

static bool str_at(unsigned long a, const char *key)
{
	char probe[80];
	size_t klen = strlen(key);

	if (klen >= sizeof(probe))
		return false;
	if (rd(a, probe, sizeof(probe)))
		return false;
	probe[sizeof(probe) - 1] = '\0';
	return !strncmp(probe, key, klen);
}

static bool ptr_is_string(unsigned long a, char *out, size_t outlen)
{
	char probe[48];
	int i;

	if (rd(a, probe, sizeof(probe)))
		return false;
	for (i = 0; i < (int)sizeof(probe) - 1; i++) {
		if (!probe[i])
			break;
		if (probe[i] < 0x20 || probe[i] > 0x7e)
			return false;
	}
	if (i < 5)
		return false;
	probe[i] = '\0';
	strscpy(out, probe, outlen);
	return true;
}

/* 对象窗口里能不能找到面板名。三种存法都要覆盖：
 *   a) 窗口里内联着名字；
 *   b) 窗口里的指针直接指向名字（dsi_panel->name 若在第 0 偏移）；
 *   c) 窗口里的指针指向一个对象，名字在该对象前 512 字节内（name 不在第 0 偏移）。
 * 上一版只做了 a/b，所以息屏亮屏跑了也没认出来。 */
static bool name_inside(unsigned long a, int win)
{
	int off;

	for (off = 0; off + 8 <= win; off += 8)
		if (str_at(a + off, panel_prefix))
			return true;
	return false;
}

static bool identify(unsigned long base)
{
	int off, ptrs = 0;

	for (off = 0; off + 8 <= WIN; off += 8) {
		unsigned long val;

		if (str_at(base + off, panel_prefix))
			return true;
		if (rd(base + off, &val, sizeof(val)))
			continue;
		if (!kern_addr(val))
			continue;
		if (str_at(val, panel_prefix))
			return true;
		if (++ptrs <= 96 && name_inside(val, SUBWIN))
			return true;
	}
	return false;
}

static void scan_object(unsigned long base, const char *from)
{
	int off;
	char txt[96];

	say("对象 0x%lx 来自 %s，扫描 %d 字节", base, from, WIN);

	for (off = 0; off + 8 <= WIN; off += 8) {
		unsigned long val;
		u32 lo, hi;

		if (str_at(base + off, panel_prefix))
			say("+0x%03x 内联面板名", off);

		if (rd(base + off, &val, sizeof(val)))
			continue;

		if (kern_addr(val) && str_at(val, panel_prefix))
			say("+0x%03x ptr->面板名", off);

		lo = (u32)val;
		hi = (u32)(val >> 32);
		if (lo == expect_mode_cnt || hi == expect_mode_cnt)
			say("+0x%03x u32=4（%s）计数候选", off, lo == expect_mode_cnt ? "低半" : "高半");
		if (lo == expect_pclk || hi == expect_pclk)
			say("+0x%03x u32=1012000000（%s）时钟候选", off,
			    lo == expect_pclk ? "低半" : "高半");
		if (lo == expect_fps || hi == expect_fps)
			say("+0x%03x u32=120（%s）帧率候选", off, lo == expect_fps ? "低半" : "高半");

		if (!kern_addr(val))
			continue;

		if (!str_at(val, panel_prefix) && ptr_is_string(val, txt, sizeof(txt)))
			say("+0x%03x ptr->\"%s\"", off, txt);

		{
			int so;
			u32 sub;

			for (so = 0; so + 4 <= SUBWIN; so += 4) {
				if (rd(val + so, &sub, sizeof(sub)))
					break;
				if (sub == expect_pclk)
					say("+0x%03x ptr->子+0x%03x=1012000000", off, so);
				if (sub == expect_mode_cnt && so < 64)
					say("+0x%03x ptr->子+0x%03x=4", off, so);
			}
		}
	}
	say("扫描结束");
}

static int try_capture(struct pt_regs *regs, const char *from)
{
	unsigned long cand = regs->regs[0];
	unsigned long flags;
	bool fresh = false;

	if (READ_ONCE(identified))
		return 0;
	WRITE_ONCE(fired, READ_ONCE(fired) + 1);
	/* 记录到底是哪个函数被调到：这决定注入器该挂哪里 */
	{
		int i;
		bool known = false;

		for (i = 0; i < nseen; i++)
			if (seen_from[i] == from)
				known = true;
		if (!known && nseen < (int)ARRAY_SIZE(seen_from)) {
			seen_from[nseen++] = from;
			say("首次命中探针 %s（arg0=0x%lx）", from, regs->regs[0]);
		}
	}

	if (!kern_addr(cand) || !identify(cand))
		return 0;

	spin_lock_irqsave(&report_lock, flags);
	if (!identified) {
		identified = 1;
		captured = (void *)cand;
		captured_from = from;
		fresh = true;
	}
	spin_unlock_irqrestore(&report_lock, flags);

	if (fresh)
		scan_object(cand, from);
	return 0;				/* 返回 0：不跳过被探指令 */
}

static int hp_set_mode(struct kprobe *p, struct pt_regs *r) { return try_capture(r, "dsi_display_set_mode"); }
static int hp_prepare(struct kprobe *p, struct pt_regs *r) { return try_capture(r, "dsi_display_prepare"); }
static int hp_validate(struct kprobe *p, struct pt_regs *r) { return try_capture(r, "dsi_display_validate_mode"); }
static int hp_commit(struct kprobe *p, struct pt_regs *r) { return try_capture(r, "dsi_display_commit"); }

static struct kprobe probes[] = {
	{ .symbol_name = "dsi_display_set_mode",     .pre_handler = hp_set_mode },
	{ .symbol_name = "dsi_display_prepare",      .pre_handler = hp_prepare },
	{ .symbol_name = "dsi_display_validate_mode", .pre_handler = hp_validate },
	{ .symbol_name = "dsi_display_commit",       .pre_handler = hp_commit },
};

static ssize_t report_show(char *buf, const struct kernel_param *kp)
{
	size_t n = READ_ONCE(report_len);

	if (n > 4000)
		n = 4000;	/* param->get 只有一页，4000 留余量 */
	memcpy(buf, report, n);
	return n;
}

static ssize_t state_show(char *buf, const struct kernel_param *kp)
{
	return scnprintf(buf, 4000, "identified=%d fired=%d captured=%px from=%s report_len=%zu\n",
			 identified, fired, captured, captured_from ?: "-", READ_ONCE(report_len));
}

static int state_set(const char *val, const struct kernel_param *kp)
{
	unsigned long flags;

	spin_lock_irqsave(&report_lock, flags);
	report_len = 0;
	report[0] = '\0';
	spin_unlock_irqrestore(&report_lock, flags);
	pr_info(TAG ": report 已清空\n");
	return 0;
}

static const struct kernel_param_ops report_ops = { .get = report_show };
static const struct kernel_param_ops state_ops = { .get = state_show, .set = state_set };

module_param_cb(report, &report_ops, NULL, 0444);
MODULE_PARM_DESC(report, "扫描结果（读这里，不要靠 dmesg）");
module_param_cb(state, &state_ops, NULL, 0644);
MODULE_PARM_DESC(state, "写任意值清空 report；读看命中状态");

static int __init ace5_off_init(void)
{
	int i, registered = 0, ret;

	say("loaded，探针注册情况如下");
	for (i = 0; i < ARRAY_SIZE(probes); i++) {
		ret = register_kprobe(&probes[i]);
		say("注册 %s -> ret=%d", probes[i].symbol_name, ret);
		if (!ret)
			registered++;
	}
	if (!registered) {
		say("一个探针都没挂上，卸载");
		return -ENOENT;
	}
	say("就绪，等一次显示操作（息屏亮屏或改帧率）");
	return 0;
}

static void __exit ace5_off_exit(void)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(probes); i++) {
		if (probes[i].addr)
			unregister_kprobe(&probes[i]);
	}
	pr_info(TAG ": unloaded identified=%d fired=%d\n", identified, fired);
}

module_init(ace5_off_init);
module_exit(ace5_off_exit);
MODULE_DESCRIPTION("OnePlus Ace5 dsi_display offset scanner (report via sysfs, iteration 2b)");
MODULE_AUTHOR("XiYan");
MODULE_LICENSE("GPL");
