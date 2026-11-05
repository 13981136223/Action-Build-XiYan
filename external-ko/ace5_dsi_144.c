// SPDX-License-Identifier: GPL-2.0
/*
 * ace5_dsi_144.c — 迭代 3：运行时注入 144Hz
 *
 * 配方来自修改版 dtbo 与原厂 dtbo 的逐属性对比：144 相对 120 只改
 * framerate / panel-clockrate / mdp-transfer-time-us / 两张 LTPO adfr 映射表首元素，
 * DSC、porch 总和、DDIC 命令表一字节不动。
 *
 * 不刷 dtbo：抓活的 struct dsi_display，把原生 120 档那条 mode 的时钟与帧率字段
 * 改写成 144 的值。厂商结构体没开源，偏移一律由 insmod 参数传入，写之前每个偏移
 * 都要读到"应有值"，对不上就拒绝写。
 *
 * 过程与状态走 /sys/module/ace5_dsi_144/parameters/{report,state,install}
 * —— 这台机 dmesg 刷得极快，不能只靠日志。
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

#define TAG "ace5_144"
#define WIN 2048
#define SUBWIN 256
#define REPORT_CAP 2600
#define PTRSZ 8			/* modes 是指针数组，步长恒 8 */

static const char panel_prefix[] = "qcom,mdss_dsi_panel";

/* 原生 120 档已知真值（迭代 1 实测），用于验偏移 */
static u32 expect_pclk = 1012000000;
static u32 expect_transfer = 6290;
static u32 expect_fps = 120;
static u32 expect_modes = 4;

/* 144 档目标值（来自修改版 dtbo） */
static u32 want_pclk = 1239700000;	/* 0x49e45220 */
static u32 want_transfer = 5205;	/* 0x1455 */
static u32 want_fps = 144;

static u64 off_panel;		/* dsi_display -> struct dsi_panel * */
static u64 off_num_modes;	/* dsi_display -> u32 modes_count */
static u64 off_modes;		/* dsi_display -> struct dsi_display_mode **modes */
static u64 off_mode_pclk;	/* mode -> u32 pclk */
static u64 off_mode_transfer;	/* mode -> u32 transfer_time_us */
static u64 off_mode_vrefresh;	/* mode -> drm_display_mode.vrefresh（u16） */
static u64 off_mode_clock;	/* mode -> drm_display_mode.clock（u32, kHz） */
static u64 off_adfr_hi;		/* panel -> adfr high-precision 表首元素 */
static u64 off_adfr_min;	/* panel -> adfr min 表首元素 */

static char report[REPORT_CAP];
static size_t report_len;
static DEFINE_SPINLOCK(report_lock);

static void *display;
static bool validated;
static bool installed;
static u64 saved32[24];
static u64 saved16[8];
static int n32, n16;

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

/* 只写此前用 nofault 读成功过的同一地址；copy_to_kernel_nofault 在 6.1 未导出 */
static int wr(unsigned long addr, const void *buf, size_t len)
{
	if (!kern_addr(addr))
		return -EINVAL;
	memcpy((void *)addr, buf, len);
	return 0;
}

static u32 rd_u32(unsigned long addr, bool *ok)
{
	u32 v = 0;
	int ret = rd(addr, &v, sizeof(v));

	if (ok)
		*ok = !ret;
	return ret ? 0 : v;
}

static bool name_inside(unsigned long a, int win)
{
	int off;

	for (off = 0; off + 8 <= win; off += 8) {
		char probe[40];

		if (rd(a + off, probe, sizeof(probe)))
			continue;
		probe[sizeof(probe) - 1] = '\0';
		if (!strncmp(probe, panel_prefix, strlen(panel_prefix)))
			return true;
	}
	return false;
}

static int try_capture(struct kprobe *p, struct pt_regs *regs)
{
	unsigned long cand = regs->regs[0];
	unsigned long flags;
	bool fresh = false;

	if (READ_ONCE(display))
		return 0;
	if (!kern_addr(cand))
		return 0;
	{
		int off;
		bool hit = false;

		for (off = 0; off + 8 <= WIN && !hit; off += 8) {
			unsigned long val;

			if (rd(cand + off, &val, sizeof(val)))
				continue;
			if (kern_addr(val) && name_inside(val, SUBWIN))
				hit = true;
		}
		if (!hit)
			return 0;
	}
	spin_lock_irqsave(&report_lock, flags);
	if (!display) {
		display = (void *)cand;
		fresh = true;
	}
	spin_unlock_irqrestore(&report_lock, flags);
	if (fresh)
		say("抓到 dsi_display 0x%lx", cand);
	return 0;
}

static struct kprobe kp = {
	.symbol_name = "dsi_display_prepare",
	.pre_handler = try_capture,
};

static void save32(unsigned long addr, u32 old)
{
	if (n32 + 2 > (int)ARRAY_SIZE(saved32)) {
		say("32 位保存表已满，后续改动无法回滚");
		return;
	}
	saved32[n32++] = addr;
	saved32[n32++] = old;
}

static void save16(unsigned long addr, u16 old)
{
	if (n16 + 2 > (int)ARRAY_SIZE(saved16)) {
		say("16 位保存表已满");
		return;
	}
	saved16[n16++] = addr;
	saved16[n16++] = old;
}

static int resolve(unsigned long *panel_out, unsigned long *modes_out)
{
	unsigned long d = (unsigned long)display, panel, modes;

	if (!d)
		return -ENOENT;
	if (rd(d + off_panel, &panel, sizeof(panel)) || !kern_addr(panel))
		return -EINVAL;
	if (rd(d + off_modes, &modes, sizeof(modes)) || !kern_addr(modes))
		return -EINVAL;
	*panel_out = panel;
	*modes_out = modes;
	return 0;
}

static bool validate_offsets(void)
{
	unsigned long panel, modes, mode0, d = (unsigned long)display;
	bool ok;
	u32 v;
	int i;

	if (!display) {
		say("还没抓到 display；先息屏亮屏一次再读 parameters/state");
		return false;
	}
	if (!off_panel || !off_modes || !off_num_modes || !off_mode_pclk) {
		say("必填偏移缺失：off_panel/off_modes/off_num_modes/off_mode_pclk");
		return false;
	}
	if (resolve(&panel, &modes)) {
		say("off_panel/off_modes 取不到 panel/modes");
		return false;
	}
	if (!name_inside(panel, SUBWIN)) {
		say("off_panel 指过去认不出面板名，偏移不对");
		return false;
	}

	v = rd_u32(d + off_num_modes, &ok);
	if (!ok || v != expect_modes) {
		say("off_num_modes=0x%llx 读到 %u，期望 4 → 拒绝", off_num_modes, v);
		return false;
	}
	if (rd(modes, &mode0, sizeof(mode0)) || !kern_addr(mode0)) {
		say("modes[0] 不是有效指针");
		return false;
	}
	v = rd_u32(mode0 + off_mode_pclk, &ok);
	if (!ok || v != expect_pclk) {
		say("off_mode_pclk=0x%llx 读到 %u，期望 %u → 拒绝",
		    off_mode_pclk, v, expect_pclk);
		return false;
	}
	if (off_mode_transfer) {
		v = rd_u32(mode0 + off_mode_transfer, &ok);
		if (!ok || v != expect_transfer) {
			say("off_mode_transfer=0x%llx 读到 %u，期望 %u → 拒绝",
			    off_mode_transfer, v, expect_transfer);
			return false;
		}
	}
	if (off_mode_vrefresh) {
		u16 vr = 0;

		if (rd(mode0 + off_mode_vrefresh, &vr, sizeof(vr)) || vr != expect_fps) {
			say("off_mode_vrefresh=0x%llx 读到 %u，期望 120 → 拒绝",
			    off_mode_vrefresh, vr);
			return false;
		}
	}
	if (off_adfr_hi) {
		v = rd_u32(panel + off_adfr_hi, &ok);
		if (!ok || v != expect_fps) {
			say("off_adfr_hi=0x%llx 读到 %u，期望 120 → 拒绝", off_adfr_hi, v);
			return false;
		}
	}
	if (off_adfr_min) {
		v = rd_u32(panel + off_adfr_min, &ok);
		if (!ok || v != expect_fps) {
			say("off_adfr_min=0x%llx 读到 %u，期望 120 → 拒绝", off_adfr_min, v);
			return false;
		}
	}
	for (i = 0; i < (int)expect_modes; i++) {
		unsigned long m;
		bool m_ok;

		if (rd(modes + i * PTRSZ, &m, sizeof(m)) || !kern_addr(m))
			continue;
		v = rd_u32(m + off_mode_pclk, &m_ok);
		if (m_ok && v == expect_pclk)
			say("modes[%d]=0x%lx 是原生 120 档（pclk 对得上）", i, m);
	}

	say("偏移全部验过：display=%px panel=0x%lx modes=0x%lx", display, panel, modes);
	validated = true;
	return true;
}

static int do_install(void)
{
	unsigned long panel, modes, mode;
	int i, changed = 0;

	if (!validated && !validate_offsets())
		return -EINVAL;
	if (resolve(&panel, &modes))
		return -EFAULT;

	for (i = 0; i < (int)expect_modes; i++) {
		bool ok;
		u32 pclk;

		if (rd(modes + i * PTRSZ, &mode, sizeof(mode)) || !kern_addr(mode))
			continue;
		pclk = rd_u32(mode + off_mode_pclk, &ok);
		if (!ok || pclk != expect_pclk)
			continue;

		say("改写 modes[%d]=0x%lx pclk %u -> %u", i, mode, pclk, want_pclk);
		save32(mode + off_mode_pclk, pclk);
		if (wr(mode + off_mode_pclk, &want_pclk, sizeof(want_pclk))) {
			say("写 pclk 失败");
			return -EIO;
		}
		changed++;

		if (off_mode_transfer) {
			u32 t = rd_u32(mode + off_mode_transfer, &ok);

			if (ok && t == expect_transfer) {
				save32(mode + off_mode_transfer, t);
				wr(mode + off_mode_transfer, &want_transfer,
				   sizeof(want_transfer));
			}
		}
		if (off_mode_clock) {
			u32 c = rd_u32(mode + off_mode_clock, &ok);

			if (ok && c) {
				u32 wc = (u32)((u64)c * want_fps / expect_fps);

				save32(mode + off_mode_clock, c);
				wr(mode + off_mode_clock, &wc, sizeof(wc));
				say("  clock %u -> %u kHz", c, wc);
			}
		}
		if (off_mode_vrefresh) {
			u16 vr = 0, wv = (u16)want_fps;

			if (!rd(mode + off_mode_vrefresh, &vr, sizeof(vr)) &&
			    vr == expect_fps) {
				save16(mode + off_mode_vrefresh, vr);
				wr(mode + off_mode_vrefresh, &wv, sizeof(wv));
				say("  vrefresh %u -> %u", vr, wv);
			}
		}
	}

	if (off_adfr_hi) {
		u32 v = rd_u32(panel + off_adfr_hi, NULL);

		if (v == expect_fps) {
			save32(panel + off_adfr_hi, v);
			wr(panel + off_adfr_hi, &want_fps, sizeof(want_fps));
			say("adfr 高刷表首元素 120 -> %u", want_fps);
		}
	}
	if (off_adfr_min) {
		u32 v = rd_u32(panel + off_adfr_min, NULL);

		if (v == expect_fps) {
			save32(panel + off_adfr_min, v);
			wr(panel + off_adfr_min, &want_fps, sizeof(want_fps));
			say("adfr 下探表首元素 120 -> %u", want_fps);
		}
	}

	installed = changed > 0;
	if (installed)
		say("已改 %d 条 mode；息屏再亮屏让它重新生效", changed);
	else
		say("一条都没改到，检查偏移对不对");
	return installed ? 0 : -ENOENT;
}

static int do_rollback(void)
{
	int i;

	for (i = n16 - 2; i >= 0; i -= 2) {
		u16 v = (u16)saved16[i + 1];

		if (wr((unsigned long)saved16[i], &v, sizeof(v)))
			say("回滚 16 位 0x%llx 失败", saved16[i]);
	}
	for (i = n32 - 2; i >= 0; i -= 2) {
		u32 v = (u32)saved32[i + 1];

		if (wr((unsigned long)saved32[i], &v, sizeof(v)))
			say("回滚 0x%llx 失败", saved32[i]);
	}
	say("已回滚 32 位 %d 处 + 16 位 %d 处", n32 / 2, n16 / 2);
	n32 = n16 = 0;
	installed = false;
	return 0;
}

static ssize_t report_get(char *buf, const struct kernel_param *kp)
{
	size_t n = READ_ONCE(report_len);

	if (n > PAGE_SIZE - 1)
		n = PAGE_SIZE - 1;
	memcpy(buf, report, n);
	return n;
}

static ssize_t state_get(char *buf, const struct kernel_param *kp)
{
	return scnprintf(buf, PAGE_SIZE,
			 "display=%px validated=%d installed=%d saved32=%d saved16=%d want_pclk=%u want_fps=%u\n",
			 display, validated, installed, n32 / 2, n16 / 2, want_pclk, want_fps);
}

static int state_set(const char *val, const struct kernel_param *kp)
{
	unsigned long flags;

	spin_lock_irqsave(&report_lock, flags);
	report_len = 0;
	report[0] = '\0';
	spin_unlock_irqrestore(&report_lock, flags);
	return 0;
}

static int install_set(const char *val, const struct kernel_param *kp)
{
	bool on = (*val == '1' || *val == 'y' || *val == 'Y');

	return on ? do_install() : do_rollback();
}

static ssize_t install_get(char *buf, const struct kernel_param *kp)
{
	return scnprintf(buf, PAGE_SIZE, "%d\n", installed ? 1 : 0);
}

static const struct kernel_param_ops report_ops = { .get = report_get };
static const struct kernel_param_ops state_ops = { .get = state_get, .set = state_set };
static const struct kernel_param_ops install_ops = { .get = install_get, .set = install_set };

module_param_cb(report, &report_ops, NULL, 0444);
module_param_cb(state, &state_ops, NULL, 0644);
module_param_cb(install, &install_ops, NULL, 0644);
MODULE_PARM_DESC(report, "验证与改写过程（读这里，不要只靠 dmesg）");
MODULE_PARM_DESC(state, "读状态；写任意值清空 report");
MODULE_PARM_DESC(install, "写 1 注入 144，写 0 回滚原厂值");

module_param(off_panel, ullong, 0444);
module_param(off_num_modes, ullong, 0444);
module_param(off_modes, ullong, 0444);
module_param(off_mode_pclk, ullong, 0444);
module_param(off_mode_transfer, ullong, 0444);
module_param(off_mode_vrefresh, ullong, 0444);
module_param(off_mode_clock, ullong, 0444);
module_param(off_adfr_hi, ullong, 0444);
module_param(off_adfr_min, ullong, 0444);
module_param(want_pclk, uint, 0644);
module_param(want_transfer, uint, 0644);
module_param(want_fps, uint, 0644);
MODULE_PARM_DESC(off_panel, "dsi_display -> dsi_panel * 偏移");
MODULE_PARM_DESC(off_num_modes, "dsi_display -> modes_count 偏移");
MODULE_PARM_DESC(off_modes, "dsi_display -> dsi_display_mode **modes 偏移");
MODULE_PARM_DESC(off_mode_pclk, "dsi_display_mode -> pclk 偏移");

static int __init ace5_144_init(void)
{
	int ret = register_kprobe(&kp);

	if (ret) {
		say("注册 dsi_display_prepare 失败 ret=%d", ret);
		return ret;
	}
	say("loaded；抓到 display 后 echo 1 > parameters/install，撤销写 0");
	return 0;
}

static void __exit ace5_144_exit(void)
{
	if (installed)
		do_rollback();
	unregister_kprobe(&kp);
	pr_info(TAG ": unloaded\n");
}

module_init(ace5_144_init);
module_exit(ace5_144_exit);
MODULE_DESCRIPTION("OnePlus Ace5 runtime 144Hz injector, offsets via module params");
MODULE_AUTHOR("XiYan");
MODULE_LICENSE("GPL");
