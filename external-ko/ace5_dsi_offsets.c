// SPDX-License-Identifier: GPL-2.0
/*
 * ace5_dsi_offsets.c — 迭代 2：只读偏移扫描器
 *
 * 厂商显示驱动没开源，struct dsi_display / dsi_display_mode 的字段偏移不能猜。
 * 这里挂一个 kprobe 抓真实对象，再用"已知真值"当指纹反推偏移：
 *   - 面板名串 qcom,mdss_dsi_panel_AA577_P_3_A0020_dsc_cmd（指针指向 或 内联在对象里）
 *   - 官方 timing 档数 4（迭代 1 在运行时设备树里数出来）
 *   - DSI 时钟 1,012,000,000（迭代 1 读到的 panel-clockrate）
 * 命中才打印，不命中一条不报；全程只读，不写任何内存。
 */
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/kprobes.h>
#include <linux/uaccess.h>
#include <linux/string.h>
#include <linux/err.h>
#include <linux/spinlock.h>

#define TAG "ace5off"
#define WIN 2048			/* 一次扫描的窗口；结构体不会比这大 */
#define SUBWIN 256			/* 指针指向的子对象再挖一层的窗口 */

static const char panel_key[] = "qcom,mdss_dsi_panel_AA577_P_3_A0020_dsc_cmd";
static const u32 expect_pclk = 1012000000;	/* 0x3c51e500，运行时设备树实测 */
static const u32 expect_mode_cnt = 4;

static void *captured;			/* 已抓到并确认身份的 display 对象 */
static DEFINE_SPINLOCK(capture_lock);

static bool kern_addr(unsigned long a)
{
	/* arm64 内核 VA 高地址；同时排除 ERR_PTR 区间 */
	return (long)a < 0 && !IS_ERR((void *)a);
}

static int rd(unsigned long addr, void *buf, size_t len)
{
	if (!kern_addr(addr))
		return -EINVAL;
	return copy_from_kernel_nofault(buf, (void *)addr, len);
}

/* 该地址是否指向面板名串 */
static bool ptr_is_panel_name(unsigned long a)
{
	char probe[80];

	if (rd(a, probe, sizeof(probe)))
		return false;
	probe[sizeof(probe) - 1] = '\0';
	return !strncmp(probe, panel_key, sizeof(panel_key) - 1);
}

/* 该地址是否指向一段可打印串（找 name/label 之类字段） */
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

/* 对象里内联存放的面板名（dsi_panel->name 常是 char[] 而非指针） */
static bool inline_is_panel_name(unsigned long base, int off)
{
	char probe[80];

	if (rd(base + off, probe, sizeof(probe)))
		return false;
	probe[sizeof(probe) - 1] = '\0';
	return !strncmp(probe, panel_key, sizeof(panel_key) - 1);
}

static void scan_object(unsigned long base)
{
	int off;
	char txt[96];

	pr_info(TAG ": 扫描对象 0x%lx，窗口 %d\n", base, WIN);

	for (off = 0; off + 8 <= WIN; off += 8) {
		unsigned long val;
		u32 lo, hi;

		/* 内联串 */
		if (inline_is_panel_name(base, off))
			pr_info(TAG ": +0x%03x 内联面板名（dsi_panel->name 候选，注意 8 对齐）\n", off);

		if (rd(base + off, &val, sizeof(val)))
			continue;

		if (kern_addr(val) && ptr_is_panel_name(val))
			pr_info(TAG ": +0x%03x ptr -> 面板名（panel/dsi 指针候选）\n", off);

		lo = (u32)val;
		hi = (u32)(val >> 32);
		if (lo == expect_mode_cnt || hi == expect_mode_cnt)
			pr_info(TAG ": +0x%03x 含 u32=4（%s，modes/timings 计数候选）\n",
				off, lo == expect_mode_cnt ? "低半" : "高半");
		if (lo == expect_pclk || hi == expect_pclk)
			pr_info(TAG ": +0x%03x 含 u32=1012000000（%s，DSI 时钟候选）\n",
				off, lo == expect_pclk ? "低半" : "高半");

		if (!kern_addr(val))
			continue;

		/* 指不到面板名时，退一步看看是不是别的可读串 */
		if (!ptr_is_panel_name(val)) {
			if (ptr_is_string(val, txt, sizeof(txt)))
				pr_info(TAG ": +0x%03x ptr -> 串 \"%s\"\n", off, txt);
		}

		/* 子对象里再找 pclk：mode->timing_info.pclk 这类 */
		{
			int so;

			for (so = 0; so + 4 <= SUBWIN; so += 4) {
				u32 sub;

				if (rd(val + so, &sub, sizeof(sub)))
					break;
				if (sub == expect_pclk)
					pr_info(TAG ": +0x%03x ptr -> 子对象+0x%03x = 1012000000（pclk）\n",
						off, so);
			}
		}
	}
	pr_info(TAG ": 扫描结束\n");
}

/* arg0 是不是我们要的那个对象：窗口内必须能找到面板名 */
static bool identify(unsigned long cand)
{
	int off;
	char probe[80];

	for (off = 0; off + 8 <= WIN; off += 8) {
		unsigned long val;

		if (!rd(cand + off, probe, sizeof(probe)) &&
		    !strncmp(probe, panel_key, sizeof(panel_key) - 1))
			return true;
		if (!rd(cand + off, &val, sizeof(val)) &&
		    kern_addr(val) && !rd(val, probe, 24) &&
		    !strncmp(probe, panel_key, sizeof(panel_key) - 1))
			return true;
	}
	return false;
}

static int set_mode_entry(struct kprobe *p, struct pt_regs *regs)
{
	unsigned long cand = regs->regs[0];
	unsigned long flags;
	bool have;

	spin_lock_irqsave(&capture_lock, flags);
	have = captured != NULL;
	spin_unlock_irqrestore(&capture_lock, flags);
	if (have)
		return 0;			/* 已抓够，正常放行被探指令 */

	if (!kern_addr(cand) || !identify(cand))
		return 0;

	spin_lock_irqsave(&capture_lock, flags);
	if (!captured)
		captured = (void *)cand;
	have = true;
	spin_unlock_irqrestore(&capture_lock, flags);
	if (!have)
		return 0;

	pr_info(TAG ": 确认 display 对象 0x%lx（dsi_display_set_mode arg0）\n", cand);
	/*
	 * 这里在被探函数上下文里做一次扫描，只跑一次；窗口已压到 2048，
	 * 且 identify() 已确认可读，避免对野指针做无谓解引用。
	 */
	scan_object(cand);
	return 0;				/* 返回 0：不跳过被探指令，行为不变 */
}

static struct kprobe kp_setmode = {
	.symbol_name = "dsi_display_set_mode",
	.pre_handler = set_mode_entry,
};

static int __init ace5_off_init(void)
{
	int ret = register_kprobe(&kp_setmode);

	if (ret) {
		pr_warn(TAG ": 注册 dsi_display_set_mode kprobe 失败 ret=%d\n", ret);
		return ret;
	}
	pr_info(TAG ": 已挂探针，等一次模式切换（息屏再亮屏即可）\n");
	return 0;
}

static void __exit ace5_off_exit(void)
{
	unregister_kprobe(&kp_setmode);
	pr_info(TAG ": unloaded，captured=%px\n", captured);
}

module_init(ace5_off_init);
module_exit(ace5_off_exit);
MODULE_DESCRIPTION("OnePlus Ace5 dsi_display offset scanner (read-only, iteration 2)");
MODULE_AUTHOR("XiYan");
MODULE_LICENSE("GPL");
