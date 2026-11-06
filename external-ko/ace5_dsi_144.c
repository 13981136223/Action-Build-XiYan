// SPDX-License-Identifier: GPL-2.0
/*
 * ace5_dsi_144.c — 一加 Ace 5 运行时开启 144Hz（不刷 dtbo）
 *
 * 配方来自原厂 dtbo 与修改版 dtbo 的逐属性对比：144 相对 120 只改
 *   panel-framerate       120 -> 144
 *   panel-clockrate  1012000000 -> 1239700000   (x1.2250)
 *   mdp-transfer-time-us      6290 -> 5205
 *   两张 LTPO adfr 映射表首元素 120 -> 144
 * DSC 参数、porch、DDIC 命令表一字不动。
 *
 * 手法参照一加15 同类 ko（oneplus15_display_165_180_auto：只引用 26 个内核符号，
 * 不碰 DRM 接口，运行时定位 dsi_display 布局后改 mode，并把 ADFR 钳在
 * oplus_adfr_property_update 上）。字段偏移不写死，靠四条 timing 的天然指纹
 * 现场发现：DSI 时钟全等 1012000000、帧率序列含两条 120、drm clock 约
 * 455817kHz、panel 内 adfr 表 120/90/72/60。发现不了就拒改。
 *
 * 原厂 DT 本来就有两条 120（sdc_fhd_120 与 oplus_fhd_120，DRM 模式表也是
 * 120/60/90/120），所以把**最后一条 120** 改写成 144：不扩数组、不改计数，
 * 原生 120 仍可回退，风险面最小。
 *
 * 默认 probe_only=1：只发现不写内存。确认 state 后才 echo 0 > probe_only，
 * 再 echo 1 > apply。过程写进 parameters/report（这台机 dmesg 冲得极快）。
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
#include <linux/of.h>

#define TAG "ace5_144"
#define OBJ_WIN 2048			/* dsi_display 对象扫描窗口 */
#define MODE_WIN 768			/* modes 指针数组校验用窗口 */
#define FIELD_WIN 256			/* 单个 mode 内部字段扫描窗口：不得超过结构体，否则会扫进相邻对象 */
#define COPY_MAX_OFF 0x100		/* 时钟副本只认结构体前 256 字节内的 */
#define REPORT_CAP 3000

/* 原厂 120 档指纹（迭代 1 从运行时设备树实测） */
static u32 src_clk = 1012000000;
static u32 src_transfer = 6290;
static u32 src_pix_khz = 455817;	/* (1264+54)*(2780+102)*120/1000 */
static u32 src_adfr[4] = { 120, 90, 72, 60 };

/* 144 档目标 */
static u32 want_clk = 1239700000;
static u32 want_fps = 144;
static u32 want_transfer = 5205;

static bool probe_only = true;
static bool applied;
static bool layout_ok;
static u32 failure_code;
static int want_slot = -1;
static u32 apply_step;		/* 0=只校验 1=transfer 2=+fps 3=+DSI时钟 4=+kHz 5=+adfr/DT */		/* -1 = 自动选最后一条 120 */
static char panel_node[64] = "qcom,mdss_dsi_panel_AA577_P_3_A0020_dsc_cmd";

static struct {
	unsigned long display;
	unsigned long modes_arr;
	u32 modes_off;
	u32 clk_off;		/* mode 内首个等于 1012000000 的字段 */
	u32 clk_offs[4];	/* 该值的全部副本 */
	u32 clk_n;
	u32 priv_off;		/* mode 内指向 priv_info 的指针字段 */
	u32 priv_clk_off;	/* priv_info 里的时钟字段 */
	u32 fps_off;
	u32 fps_u16;
	u32 transfer_off;
	u32 khz_off;
	u32 panel_off;
	u32 adfr_off;
	u32 count;
	u32 stride;
	u32 contig;
	u32 derived[4];	/* 随帧率等比的派生时钟字段 */
	u32 derived_n;
	int slot;
} L;

#define MAX_PATCH 4
struct save_rec {
	unsigned long mode;
	u32 clk;
	u32 clk_vals[4];
	u32 clk_n;
	u32 derived_vals[4];
	u32 derived_n;
	u32 priv_clk;
	u32 fps32;
	u16 fps16;
	u32 transfer;
	u32 khz;
	u32 adfr;
	bool have_adfr;
};
static struct save_rec SAVED[MAX_PATCH];
static u32 saved_n;
static int adfr_calls;

static char report[REPORT_CAP];
static size_t report_len;
static DEFINE_SPINLOCK(report_lock);

__printf(1, 2)
static void say(const char *fmt, ...)
{
	char line[180];
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

static bool kern(unsigned long a)
{
	return (long)a < 0 && !IS_ERR((void *)a);
}

static int rd(unsigned long a, void *buf, size_t len)
{
	if (!kern(a))
		return -EINVAL;
	return copy_from_kernel_nofault(buf, (void *)a, len);
}

static u32 r32(unsigned long a, bool *ok)
{
	u32 v = 0;
	int ret = rd(a, &v, sizeof(v));

	if (ok)
		*ok = !ret;
	return ret ? 0 : v;
}

static unsigned long rptr(unsigned long a, bool *ok)
{
	u64 v = 0;
	int ret = rd(a, &v, sizeof(v));

	if (ok)
		*ok = !ret;
	return ret ? 0 : (unsigned long)v;
}

/* 只写发现阶段已经读成功过的地址；6.1 未导出 copy_to_kernel_nofault */
static void w32(unsigned long a, u32 v)
{
	if (kern(a))
		memcpy((void *)a, &v, sizeof(v));
}

static void w16(unsigned long a, u16 v)
{
	if (kern(a))
		memcpy((void *)a, &v, sizeof(v));
}

static bool is32(unsigned long a, u32 key)
{
	bool ok;

	return r32(a, &ok) == key && ok;
}

static u32 find_u32(unsigned long base, u32 key, u32 win)
{
	u32 off;

	for (off = 0; off + 4 <= win; off += 4)
		if (is32(base + off, key))
			return off;
	return U32_MAX;
}

static u32 find_u16(unsigned long base, u32 key, u32 win)
{
	u32 off;

	for (off = 0; off + 2 <= win; off += 2) {
		u16 v = 0;

		if (!rd(base + off, &v, sizeof(v)) && v == (u16)key)
			return off;
	}
	return U32_MAX;
}

static u32 find_khz(unsigned long base)
{
	u32 off;

	for (off = 0; off + 4 <= FIELD_WIN; off += 4) {
		bool ok;
		u32 v = r32(base + off, &ok);

		if (ok && v > src_pix_khz - 3000 && v < src_pix_khz + 3000)
			return off;
	}
	return U32_MAX;
}

/* 读某条 mode 的帧率字段 */
static u32 mode_fps(unsigned long m, u32 off, u32 is_u16, bool *ok)
{
	if (is_u16) {
		u16 v = 0;

		*ok = !rd(m + off, &v, sizeof(v));
		return v;
	}
	return r32(m + off, ok);
}

/* 设备树侧的二次确认：必须有两条 120，才谈得上把其中一条变成 144 */
static bool dt_confirm(u32 *count, u32 *n120)
{
	struct device_node *np, *tn, *sub;
	u32 c = 0, hit = 0;

	np = of_find_node_by_name(NULL, panel_node);
	if (!np) {
		say("设备树找不到 %s", panel_node);
		return false;
	}
	tn = of_get_child_by_name(np, "qcom,mdss-dsi-display-timings");
	of_node_put(np);
	if (!tn) {
		say("面板节点无 display-timings");
		return false;
	}
	for_each_child_of_node(tn, sub) {
		u32 v;

		c++;
		if (!of_property_read_u32(sub, "qcom,mdss-dsi-panel-framerate", &v) && v == 120)
			hit++;
	}
	of_node_put(tn);
	*count = c;
	*n120 = hit;
	say("设备树确认：timing=%u 其中 120 档=%u", c, hit);
	return c > 0 && hit >= 2;
}

/* 试把 dsi_display+slot 当成 modes 指针数组，逐项验证 */
static unsigned long mode_at(u32 i);
static void find_derived_fields(u32 count, u32 stride, u32 fps_off);

/* 某偏移是否在每条 mode 上都成立（用于剔除跨到相邻对象的偏移） */
static bool off_holds_all(unsigned long arr, u32 count, u32 off, u32 want, u32 tol)
{
	u32 i;

	for (i = 0; i < count; i++) {
		bool ok;
		unsigned long m = mode_at(i);
		u32 v;

		if (!kern(m))
			return false;
		v = r32(m + off, &ok);
		if (!ok)
			return false;
		if (tol) {
			if (v + tol < want || v > want + tol)
				return false;
		} else if (v != want) {
			return false;
		}
	}
	return true;
}

/* 连续结构体数组：dsi_display_mode *modes（本条驱动就是这种）。
 * 用"同一时钟在每个元素里重复出现"推 stride。 */
static bool try_contig_at(unsigned long d, u32 slot, u32 count)
{
	bool ok;
	unsigned long arr = rptr(d + slot, &ok);
	u32 clk_off, fps_off = U32_MAX, stride, i, var_u16;
	int last120 = -1, n120 = 0;

	if (!ok || !kern(arr))
		return false;
	clk_off = find_u32(arr, src_clk, 0x400);
	if (clk_off == U32_MAX)
		return false;

	for (stride = 0x40; stride <= 0x800; stride += 8) {
		if (!is32(arr + stride + clk_off, src_clk))
			continue;
		if (count > 2 && !is32(arr + 2 * stride + clk_off, src_clk))
			continue;
		if (count > 3 && !is32(arr + 3 * stride + clk_off, src_clk))
			continue;

		fps_off = find_u32(arr, 120, stride);
		var_u16 = 0;
		if (fps_off == U32_MAX || fps_off == clk_off) {
			fps_off = find_u16(arr, 120, stride);
			var_u16 = 1;
		}
		if (fps_off == U32_MAX || fps_off == clk_off)
			continue;
		n120 = 0; last120 = -1;
		for (i = 0; i < count; i++) {
			u32 f;

			if (var_u16) {
				u16 v = 0;

				if (rd(arr + i * stride + fps_off, &v, sizeof(v)))
					break;
				f = v;
			} else {
				f = r32(arr + i * stride + fps_off, NULL);
			}
			if (f == 120) { n120++; last120 = i; }
			else if (f != 60 && f != 90)
				break;
		}
		if (i < count || n120 < 2)
			continue;

		L.modes_off = slot;
		L.modes_arr = arr;
		L.stride = stride;
		L.contig = 1;
		L.clk_off = clk_off;
		L.fps_off = fps_off;
		L.fps_u16 = var_u16;
		L.count = count;
		L.slot = want_slot >= 0 && want_slot < (int)count ? want_slot : last120;
		L.transfer_off = find_u32(arr, src_transfer, stride);
		L.khz_off = find_khz(arr);
		find_derived_fields(count, stride, fps_off);
		say("连续数组命中 display+0x%x -> 0x%lx stride=0x%x clk_off=0x%x fps_off=0x%x(%s) 120数=%d 槽=%d",
		    slot, arr, stride, clk_off, fps_off, var_u16 ? "u16" : "u32", n120, L.slot);
		return true;
	}
	return false;
}

static unsigned long mode_at(u32 i)
{
	bool ok;

	if (L.contig)
		return L.modes_arr + (unsigned long)i * L.stride;
	return rptr(L.modes_arr + i * 8, &ok);
}

/* 识别"随帧率等比变化"的字段：对四条 mode 满足 v_i*fps_0 == v_0*fps_i。
 * 真机实测 +0x38 (852681600) 就是这种：60/90/120 严格按比例。
 * 上次崩机就是因为只改了帧率与 DSI 时钟，把这些派生时钟留在 120 档。 */
static void find_derived_fields(u32 count, u32 stride, u32 fps_off)
{
	u32 off, i, v0;
	u16 f0 = 0, fi;

	L.derived_n = 0;
	if (!count || rd(L.modes_arr + fps_off, &f0, sizeof(f0)) || !f0)
		return;
	for (off = 0; off + 4 <= stride && L.derived_n < 4; off += 4) {
		bool ok;
		bool good = true;

		if (off == fps_off || off == L.clk_off)
			continue;
		v0 = r32(L.modes_arr + off, &ok);
		if (!ok || !v0)
			continue;
		for (i = 1; i < count; i++) {
			u32 vi = r32(L.modes_arr + i * stride + off, NULL);

			fi = 0;
			if (rd(L.modes_arr + i * stride + fps_off, &fi, sizeof(fi)) || !fi) {
				good = false;
				break;
			}
			if (!vi || (u64)v0 * fi != (u64)vi * f0) {
				good = false;
				break;
			}
		}
		if (good) {
			L.derived[L.derived_n++] = off;
			say("派生时钟字段 +0x%x（值 %u，随帧率等比）", off, v0);
		}
	}
}

static bool try_modes_at(unsigned long d, u32 slot, u32 count)
{
	bool ok;
	unsigned long arr = rptr(d + slot, &ok);
	unsigned long m0;
	u32 i, clk_off, fps_off, fps16 = 0;
	int last120 = -1, n120 = 0;

	if (!ok || !kern(arr))
		return false;
	m0 = rptr(arr, &ok);
	if (!ok || !kern(m0))
		return false;

	clk_off = find_u32(m0, src_clk, FIELD_WIN);
	if (clk_off == U32_MAX)
		return false;
	/* 收集全部副本：作者把 mode_clock / mode_pixel / priv_clock 分成三个参数，
	 * 说明同一时钟在驱动里存了不止一份，只改一份可能不生效 */
	{
		u32 o;

		L.clk_n = 0;
		for (o = 0; o + 4 <= FIELD_WIN && o < COPY_MAX_OFF && L.clk_n < 4; o += 4)
			if (is32(m0 + o, src_clk))
				L.clk_offs[L.clk_n++] = o;
	}

	/* 帧率先在 0 号上定位（u32 优先，其次 u16） */
	fps_off = find_u32(m0, 120, FIELD_WIN);
	if (fps_off != U32_MAX) {
		fps16 = 0;
	} else {
		fps_off = find_u16(m0, 120, FIELD_WIN);
		if (fps_off == U32_MAX)
			return false;
		fps16 = 1;
	}

	for (i = 0; i < count; i++) {
		unsigned long m = rptr(arr + i * 8, &ok);
		bool fok;
		u32 f;

		if (!ok || !kern(m))
			return false;
		if (!is32(m + clk_off, src_clk))
			return false;
		f = mode_fps(m, fps_off, fps16, &fok);
		if (!fok)
			return false;
		if (f == 120) {
			n120++;
			last120 = i;
		}
	}

	L.modes_off = slot;
	L.modes_arr = arr;
	L.contig = 0;
	L.stride = 0;
	L.clk_off = clk_off;
	L.fps_off = fps_off;
	L.fps_u16 = fps16;
	L.count = count;
	L.slot = want_slot >= 0 && want_slot < (int)count ? want_slot : last120;

	m0 = rptr(arr + (L.slot >= 0 ? L.slot : 0) * 8, &ok);
	if (ok && kern(m0)) {
		u32 po;

		L.transfer_off = find_u32(m0, src_transfer, FIELD_WIN);
		L.khz_off = find_khz(m0);
		/* priv_info：mode 内某个指针，指向的对象里也有同一个时钟值 */
		L.priv_off = 0;
		L.priv_clk_off = 0;
		for (po = 0; po + 8 <= FIELD_WIN && !L.priv_off; po += 8) {
			unsigned long sub = rptr(m0 + po, &ok);
			u32 si;

			if (!ok || !kern(sub) || sub == m0)
				continue;
			for (si = 0; si + 4 <= 256; si += 4) {
				if (is32(sub + si, src_clk)) {
					L.priv_off = po;
					L.priv_clk_off = si;
					break;
				}
			}
		}
	}
	/* 复验与筛选：跨到相邻对象的偏移在这里被剔除 */
	{
		u32 keep = 0, k;

		for (k = 0; k < L.clk_n; k++)
			if (off_holds_all(arr, count, L.clk_offs[k], src_clk, 0))
				L.clk_offs[keep++] = L.clk_offs[k];
		if (keep != L.clk_n)
			say("  剔除 %u 个跨对象的时钟副本偏移", L.clk_n - keep);
		L.clk_n = keep;
	}
	if (L.transfer_off != U32_MAX && !off_holds_all(arr, count, L.transfer_off, src_transfer, 0)) {
		say("  transfer_off 未在全部 mode 上成立，丢弃");
		L.transfer_off = U32_MAX;
	}
	if (L.khz_off != U32_MAX) {
		u32 khz0 = r32(m0 + L.khz_off, NULL);

		if (!khz0 || !off_holds_all(arr, count, L.khz_off, khz0, 3000)) {
			say("  drm_khz_off 未在全部 mode 上成立，丢弃");
			L.khz_off = U32_MAX;
		}
	}
	say("指针数组命中 display+0x%x -> 0x%lx 条数=%u clk_off=0x%x 副本=%u priv_off=0x%x priv_clk=0x%x fps_off=0x%x(%s) 120数=%d 槽=%d",
	    L.modes_off, L.modes_arr, L.count, L.clk_off, L.clk_n, L.priv_off, L.priv_clk_off,
	    L.fps_off, L.fps_u16 ? "u16" : "u32", n120, L.slot);
	if (L.transfer_off != U32_MAX)
		say("  transfer_off=0x%x", L.transfer_off);
	if (L.khz_off != U32_MAX)
		say("  drm_khz_off=0x%x", L.khz_off);
	return L.slot >= 0;
}

static bool discover_layout(unsigned long d, u32 count)
{
	u32 slot;

	for (slot = 0; slot + 8 <= OBJ_WIN; slot += 8)
		if (try_modes_at(d, slot, count))
			return true;
	/* 本条驱动是连续结构体数组，回退再试一遍 */
	for (slot = 0; slot + 8 <= OBJ_WIN; slot += 8)
		if (try_contig_at(d, slot, count))
			return true;
	say("没能在 dsi_display 里定位 modes（count=%u，两种布局都试过）", count);
	failure_code = 1;
	return false;
}

static bool discover_panel(unsigned long d)
{
	u32 slot, in;

	for (slot = 0; slot + 8 <= OBJ_WIN; slot += 8) {
		bool ok;
		unsigned long p = rptr(d + slot, &ok);
		char probe[40];
		bool named = false;

		if (!ok || !kern(p))
			continue;
		for (in = 0; in + 8 <= MODE_WIN; in += 8) {
			if (rd(p + in, probe, sizeof(probe)))
				continue;
			probe[sizeof(probe) - 1] = '\0';
			if (!strncmp(probe, "qcom,mdss_dsi_panel", 19)) {
				named = true;
				break;
			}
		}
		if (!named)
			continue;

		L.panel_off = slot;
		for (in = 0; in + 16 <= MODE_WIN; in += 4) {
			if (is32(p + in, src_adfr[0]) && is32(p + in + 4, src_adfr[1]) &&
			    is32(p + in + 8, src_adfr[2]) && is32(p + in + 12, src_adfr[3])) {
				L.adfr_off = in;
				break;
			}
		}
		say("panel：display+0x%x -> 0x%lx，adfr 高刷表偏移=0x%x",
		    slot, p, L.adfr_off);
		return true;
	}
	say("没找到 panel 指针（可选功能，不影响改 mode）");
	return false;
}


/* 运行时设备树侧：把选中的那条 120 timing 的两张 adfr 表首元素改成 144。
 * 依据：一加15 样本里引用 of_get_property（只读不改结构体），并把 adfr 表
 * 扩到 60..199；Oplus 框架在 oplus_adfr_property_update 里回读这张表，
 * 所以只改驱动内存不够，得让 DT 侧也说 144。 */
static int dt_adfr_rewrite(u32 fps)
{
	struct device_node *np, *tn, *sub;
	int done = 0;

	np = of_find_node_by_name(NULL, panel_node);
	if (!np)
		return -ENOENT;
	tn = of_get_child_by_name(np, "qcom,mdss-dsi-display-timings");
	of_node_put(np);
	if (!tn)
		return -ENOENT;
	for_each_child_of_node(tn, sub) {
		const __be32 *v;
		int len;
		int i;
		static const char *const tabs[] = {
			"oplus,adfr-high-precision-fps-mapping-table",
			"oplus,adfr-min-fps-mapping-table",
		};

		v = of_get_property(sub, "qcom,mdss-dsi-panel-framerate", &len);
		if (!v || len < 4 || be32_to_cpup(v) != 120)
			continue;
		/* 每条 120 timing 都改；驱动侧只改选中槽，DT 侧保守全改，
		 * 免得框架按另一条 timing 的表把 144 又钳回 120 */
		for (i = 0; i < 2; i++) {
			__be32 *cell;

			cell = (__force __be32 *)of_get_property(sub, tabs[i], &len);
			if (!cell || len < 4)
				continue;
			if (be32_to_cpup(cell) != 120)
				continue;
			say("DT %s 首元素 120 -> %u", tabs[i], fps);
			w32((unsigned long)cell, cpu_to_be32(fps));
			done++;
		}
	}
	of_node_put(tn);
	if (!done)
		say("DT 侧没改到 adfr 表（可能属性名不同）");
	return done ? 0 : -ENOENT;
}

static bool capture_at(unsigned long cand, const char *from)
{
	u32 count = 4, n120 = 0;

	if (READ_ONCE(L.display))
		return true;
	if (!kern(cand))
		return false;
	if (!dt_confirm(&count, &n120))
		return false;
	if (!discover_layout(cand, count))
		return false;
	L.display = cand;
	layout_ok = true;
	discover_panel(cand);
	say("布局发现完成（来自 %s），probe_only=%d", from, probe_only);
	return true;
}

static int capture_display(struct kprobe *p, struct pt_regs *regs)
{
	capture_at(regs->regs[0], "dsi_display_prepare");
	return 0;
}

static int watch_adfr(struct kprobe *p, struct pt_regs *regs)
{
	adfr_calls++;		/* 一期只观测，不猜参数语义 */
	return 0;
}

static struct kprobe kp_disp = {
	.symbol_name = "dsi_display_prepare",
	.pre_handler = capture_display,
};
/* 真机实测 validate_mode 最先被调到，作为第二个抓取入口 */
static struct kprobe kp_val = {
	.symbol_name = "dsi_display_validate_mode",
	.pre_handler = capture_display,
};
static struct kprobe kp_adfr = {
	.symbol_name = "oplus_adfr_property_update",
	.pre_handler = watch_adfr,
};
static bool adfr_hooked;

/* 参照样本：ADFR 探针只在启用期间挂，撤销时解除，不常驻。 */
static void hook_adfr(void)
{
	int ret;

	if (adfr_hooked)
		return;
	ret = register_kprobe(&kp_adfr);
	if (ret) {
		say("ADFR 探针挂载失败 ret=%d（不影响改 mode）", ret);
		return;
	}
	adfr_hooked = true;
	say("ADFR 探针已挂（仅启用期间）");
}

static void unhook_adfr(void)
{
	if (!adfr_hooked)
		return;
	unregister_kprobe(&kp_adfr);
	adfr_hooked = false;
	say("ADFR 探针已撤，累计命中 %d 次", adfr_calls);
}



static int save_original(unsigned long m, struct save_rec *sv)
{
	bool ok;
	int i;

	sv->mode = m;
	sv->clk = r32(m + L.clk_off, &ok);
	if (!ok || sv->clk != src_clk) {
		say("槽 0x%lx 当前 clk=%u 不是 %u，跳过", m, sv->clk, src_clk);
		return -EIO;
	}
	sv->clk_n = L.clk_n;
	for (i = 0; i < (int)L.clk_n && i < 4; i++)
		sv->clk_vals[i] = r32(m + L.clk_offs[i], NULL);
	sv->derived_n = L.derived_n;
	for (i = 0; i < (int)L.derived_n && i < 4; i++)
		sv->derived_vals[i] = r32(m + L.derived[i], NULL);
	sv->priv_clk = 0;
	if (L.priv_off) {
		unsigned long sub = rptr(m + L.priv_off, &ok);

		if (ok && kern(sub))
			sv->priv_clk = r32(sub + L.priv_clk_off, NULL);
	}
	{
		u16 v = 0;

		if (rd(m + L.fps_off, &v, sizeof(v)))
			return -EIO;
		sv->fps16 = v;
	}
	sv->transfer = (L.transfer_off != U32_MAX) ? r32(m + L.transfer_off, NULL) : 0;
	sv->khz = (L.khz_off != U32_MAX) ? r32(m + L.khz_off, NULL) : 0;
	sv->have_adfr = false;
	if (L.panel_off && L.adfr_off != U32_MAX) {
		unsigned long p = rptr(L.display + L.panel_off, &ok);

		if (ok && kern(p)) {
			sv->adfr = r32(p + L.adfr_off, NULL);
			sv->have_adfr = (sv->adfr == src_adfr[0]);
		}
	}
	return 0;
}

static void patch_one(unsigned long m, struct save_rec *sv)
{
	int i;

	say("写入 mode=0x%lx（step=%u）：clk %u->%u fps->%u",
	    m, apply_step, sv->clk, want_clk, want_fps);
	if (apply_step >= 3) {
		w32(m + L.clk_off, want_clk);
		for (i = 0; i < (int)L.clk_n && i < 4; i++)
			w32(m + L.clk_offs[i], want_clk);
		if (L.priv_off && sv->priv_clk == src_clk) {
			bool pok;
			unsigned long sub = rptr(m + L.priv_off, &pok);

			if (pok && kern(sub)) {
				say("  priv_info 时钟 %u -> %u", sv->priv_clk, want_clk);
				w32(sub + L.priv_clk_off, want_clk);
			}
		}
		for (i = 0; i < (int)L.derived_n && i < 4; i++) {
			u32 ov = sv->derived_vals[i];
			u32 nv = (u32)((u64)ov * want_fps / 120);

			if (ov) {
				say("  派生 +0x%x: %u -> %u", L.derived[i], ov, nv);
				w32(m + L.derived[i], nv);
			}
		}
	}
	if (apply_step >= 2)
		w16(m + L.fps_off, (u16)want_fps);
	if (apply_step >= 1 && L.transfer_off != U32_MAX && sv->transfer == src_transfer)
		w32(m + L.transfer_off, want_transfer);
	if (apply_step >= 4 && L.khz_off != U32_MAX && sv->khz) {
		u32 nk = (u32)((u64)sv->khz * want_fps / 120);

		say("  drm clock %u -> %u kHz", sv->khz, nk);
		w32(m + L.khz_off, nk);
	}
	if (apply_step >= 5 && sv->have_adfr) {
		bool ok;
		unsigned long p = rptr(L.display + L.panel_off, &ok);

		if (ok && kern(p)) {
			say("  panel adfr 高刷表首元素 120 -> %u", want_fps);
			w32(p + L.adfr_off, want_fps);
		}
	}
}

static int do_apply(void)
{
	struct save_rec *sv;
	unsigned long m;
	u16 f;
	int i;

	if (probe_only) {
		say("probe_only=1，拒绝对内存做任何改动");
		failure_code = 2;
		return -EPERM;
	}
	if (!layout_ok || L.slot < 0) {
		say("还没有布局；息屏亮屏一次让 dsi_display_prepare 被调到，再读 state");
		failure_code = 3;
		return -ENOENT;
	}
	if (!fps_sequence_ok()) {
		failure_code = 7;
		return -EINVAL;
	}

	saved_n = 0;
	for (i = 0; i < (int)L.count && saved_n < MAX_PATCH; i++) {
		m = mode_at(i);
		if (!kern(m))
			continue;
		f = 0;
		if (rd(m + L.fps_off, &f, sizeof(f)) || f != 120)
			continue;
		sv = &SAVED[saved_n];
		if (save_original(m, sv))
			continue;
		patch_one(m, sv);
		saved_n++;
	}

	if (!saved_n) {
		say("没有可改写的 120 档");
		failure_code = 5;
		return -EFAULT;
	}
	applied = (apply_step > 0);
	if (apply_step >= 5) {
		dt_adfr_rewrite(want_fps);
		hook_adfr();
	}
	say("已按 step=%u 改写 %u 条 120 档。验收：切一次帧率，驱动日志应出现 fps=144 且 clk_rate=%u",
	    apply_step, saved_n, want_clk);
	return 0;
}

static void restore_one(struct save_rec *sv)
{
	unsigned long m = sv->mode;
	int i;

	if (!kern(m))
		return;
	w32(m + L.clk_off, sv->clk);
	for (i = 0; i < (int)sv->clk_n && i < 4; i++)
		if (sv->clk_vals[i])
			w32(m + L.clk_offs[i], sv->clk_vals[i]);
	if (L.priv_off && sv->priv_clk) {
		bool pok;
		unsigned long sub = rptr(m + L.priv_off, &pok);

		if (pok && kern(sub))
			w32(sub + L.priv_clk_off, sv->priv_clk);
	}
	w16(m + L.fps_off, sv->fps16);
	for (i = 0; i < (int)sv->derived_n && i < 4; i++)
		if (sv->derived_vals[i])
			w32(m + L.derived[i], sv->derived_vals[i]);
	if (L.transfer_off != U32_MAX && sv->transfer)
		w32(m + L.transfer_off, sv->transfer);
	if (L.khz_off != U32_MAX && sv->khz)
		w32(m + L.khz_off, sv->khz);
	if (sv->have_adfr) {
		bool ok;
		unsigned long p = rptr(L.display + L.panel_off, &ok);

		if (ok && kern(p))
			w32(p + L.adfr_off, sv->adfr);
	}
}

static int do_restore(void)
{
	u32 k;

	for (k = 0; k < saved_n; k++)
		restore_one(&SAVED[k]);
	unhook_adfr();
	saved_n = 0;
	saved = false;
	applied = false;
	say("已还原全部被改写的 120 档原值；重启一次最彻底");
	return 0;
}

static int report_get(char *buf, const struct kernel_param *kp)
{
	size_t n = READ_ONCE(report_len);

	if (n > 4000)
		n = 4000;
	memcpy(buf, report, n);
	return (int)n;
}

static int state_get(char *buf, const struct kernel_param *kp)
{
	return scnprintf(buf, 4000,
		"display=0x%lx layout=%d applied=%d probe_only=%d fail=%u slot=%d count=%u "
		"modes_off=0x%x arr=0x%lx clk_off=0x%x fps_off=0x%x(%s) transfer_off=0x%x "
		"khz_off=0x%x panel_off=0x%x adfr_off=0x%x clk_n=%u priv_off=0x%x priv_clk_off=0x%x contig=%u stride=0x%x adfr_calls=%d\n",
		L.display, layout_ok, applied, probe_only, failure_code, L.slot, L.count,
		L.modes_off, L.modes_arr, L.clk_off, L.fps_off, L.fps_u16 ? "u16" : "u32",
		L.transfer_off, L.khz_off, L.panel_off, L.adfr_off, L.clk_n, L.priv_off, L.priv_clk_off, L.contig, L.stride, adfr_calls);
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

static int apply_set(const char *val, const struct kernel_param *kp)
{
	bool on = (*val == '1' || *val == 'y' || *val == 'Y');

	return on ? do_apply() : do_restore();
}

static int apply_get(char *buf, const struct kernel_param *kp)
{
	return scnprintf(buf, 4000, "%d\n", applied ? 1 : 0);
}

static int rescan_set(const char *val, const struct kernel_param *kp)
{
	u32 count = L.count ? L.count : 4;

	if (!L.display)
		return -ENOENT;
	layout_ok = false;
	if (discover_layout(L.display, count)) {
		discover_panel(L.display);
		layout_ok = true;
		return 0;
	}
	return -EIO;
}

static const struct kernel_param_ops report_ops = { .get = report_get };
static const struct kernel_param_ops state_ops = { .get = state_get, .set = state_set };
static const struct kernel_param_ops apply_ops = { .get = apply_get, .set = apply_set };
static const struct kernel_param_ops rescan_ops = { .set = rescan_set };

module_param_cb(report, &report_ops, NULL, 0444);
module_param_cb(state, &state_ops, NULL, 0644);
module_param_cb(apply, &apply_ops, NULL, 0644);
module_param_cb(rescan, &rescan_ops, NULL, 0200);
MODULE_PARM_DESC(report, "发现与改写过程（读这个，别只靠 dmesg）");
MODULE_PARM_DESC(state, "当前布局与状态；写任意值清空 report");
MODULE_PARM_DESC(apply, "1=把选中的 120 档改写成 144；0=还原");
MODULE_PARM_DESC(rescan, "写任意值重新发现布局");

module_param(probe_only, bool, 0644);
module_param(want_clk, uint, 0644);
module_param(want_fps, uint, 0644);
module_param(want_transfer, uint, 0644);
module_param(apply_step, uint, 0644);
MODULE_PARM_DESC(apply_step, "分步写入 0=只校验 1=transfer 2=+fps 3=+DSI时钟 4=+drm kHz 5=+adfr/DT");
module_param(want_slot, int, 0644);
module_param(src_clk, uint, 0444);
module_param_string(panel_node, panel_node, sizeof(panel_node), 0444);
MODULE_PARM_DESC(probe_only, "1=只发现不改内存（默认）");
MODULE_PARM_DESC(want_clk, "144 档 DSI 时钟，默认 1239700000");
MODULE_PARM_DESC(want_slot, "改第几条 mode；-1 表示自动取最后一条 120");
MODULE_PARM_DESC(panel_node, "面板设备树节点名");

static int __init ace5_144_init(void)
{
	int ret;

	L.slot = -1;
	L.transfer_off = U32_MAX;
	L.khz_off = U32_MAX;
	L.adfr_off = U32_MAX;

	ret = register_kprobe(&kp_disp);
	if (ret) {
		say("注册 dsi_display_prepare 失败 ret=%d", ret);
		failure_code = 10;
		return ret;
	}
	ret = register_kprobe(&kp_val);
	if (ret)
		say("注册 dsi_display_validate_mode 失败 ret=%d（有 prepare 兜底）", ret);
	say("loaded：等一次显示操作（息屏亮屏）触发布局发现，然后读 parameters/state");
	return 0;
}

static void __exit ace5_144_exit(void)
{
	if (applied)
		do_restore();
	unregister_kprobe(&kp_disp);
	if (kp_val.addr)
		unregister_kprobe(&kp_val);
	unhook_adfr();
	pr_info(TAG ": unloaded\n");
}

module_init(ace5_144_init);
module_exit(ace5_144_exit);
MODULE_DESCRIPTION("OnePlus Ace5 runtime 144Hz; layout discovered, no dtbo change");
MODULE_AUTHOR("XiYan");
MODULE_LICENSE("GPL");
