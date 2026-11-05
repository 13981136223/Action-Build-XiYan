// SPDX-License-Identifier: GPL-2.0
/*
 * ace5_dsi_probe.c — 只读探针（迭代 1）
 *
 * 回答三个问题，全程不写任何显示结构：
 *   1) 这颗 ko 能否被目标内核接受（vermagic / __versions CRC / KCFI）
 *   2) 运行时设备树里 AA577 面板实际有几套 timing，144 档在不在
 *   3) 迭代 2 要挂的函数在 kallsyms 里叫什么
 *
 * 读设备树走 of_* 公共 API，因此不需要任何厂商结构体偏移。
 */
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/of.h>
#include <linux/kprobes.h>
#include <linux/string.h>
#include <linux/utsname.h>

#define MOD_TAG "ace5dsi"

/* 与本机 dtbo 静态解析结果对齐；不符只报警，不加载后续逻辑 */
static const char *expect_release = "6.1.118-android14-11-o-g778e4490e346";
static const char *expect_panel = "qcom,mdss_dsi_panel_AA577_P_3_A0020_dsc_cmd";

static const char *const timing_props[] = {
	"qcom,mdss-dsi-panel-width",
	"qcom,mdss-dsi-panel-height",
	"qcom,mdss-dsi-panel-framerate",
	"qcom,mdss-dsi-panel-clockrate",
	"qcom,mdss-dsi-h-front-porch",
	"qcom,mdss-dsi-h-back-porch",
	"qcom,mdss-dsi-h-pulse-width",
	"qcom,mdss-dsi-v-front-porch",
	"qcom,mdss-dsi-v-back-porch",
	"qcom,mdss-dsi-v-pulse-width",
	"qcom,mdss-mdp-transfer-time-us",
};

static const char *const want_symbols[] = {
	"dsi_panel_get_modes",
	"dsi_panel_read_display_timings",
	"dsi_panel_dts_parse",
	"dsi_panel_init",
	"dsi_display_init",
	"dsi_display_set_mode",
	"dsi_display_prepare",
	"dsi_ctrl_enable",
	"dsi_phy_hw_init",
	"oplus_display0_params",
};

static unsigned long (*sym_kallsyms_lookup_name)(const char *name);

/* kallsyms_lookup_name 自 5.7 起不导出，用 kprobe 取回它自己的地址 */
static int __init resolve_kallsyms(void)
{
	struct kprobe kp = { .symbol_name = "kallsyms_lookup_name" };
	int ret = register_kprobe(&kp);

	if (ret < 0) {
		pr_warn(MOD_TAG ": register_kprobe(kallsyms_lookup_name) ret=%d\n", ret);
		return ret;
	}
	sym_kallsyms_lookup_name = (unsigned long (*)(const char *))kp.addr;
	unregister_kprobe(&kp);
	pr_info(MOD_TAG ": kallsyms_lookup_name @ 0x%lx\n",
		(unsigned long)sym_kallsyms_lookup_name);
	return 0;
}

static void dump_timing(struct device_node *tn)
{
	int i;

	pr_info(MOD_TAG ": -- timing %s\n", tn->full_name);
	for (i = 0; i < ARRAY_SIZE(timing_props); i++) {
		u32 v;

		if (!of_property_read_u32(tn, timing_props[i], &v))
			pr_info(MOD_TAG ":    %-36s = %u (0x%x)\n",
				timing_props[i], v, v);
	}
}

static void walk_timings(struct device_node *panel)
{
	struct device_node *timings, *tn;
	int n = 0;

	timings = of_get_child_by_name(panel, "qcom,mdss-dsi-display-timings");
	if (!timings) {
		pr_warn(MOD_TAG ": %s 没有 display-timings 子节点\n", panel->full_name);
		return;
	}
	for_each_child_of_node(timings, tn) {
		n++;
		dump_timing(tn);
	}
	pr_info(MOD_TAG ": 面板 %s 共 %d 套 timing\n", panel->full_name, n);
	of_node_put(timings);
}

static void scan_devicetree(void)
{
	struct device_node *p = of_find_node_by_name(NULL, expect_panel);

	if (!p) {
		struct device_node *np = NULL;

		pr_warn(MOD_TAG ": 未直接命中 %s，改按属性扫描\n", expect_panel);
		while ((np = of_find_node_with_property(np, "qcom,mdss-dsi-panel-name")) != NULL) {
			const char *slash = strrchr(np->full_name, '/');

			if (slash && strstr(slash + 1, "mdss_dsi_panel")) {
				walk_timings(np);
				continue;
			}
		}
		return;
	}
	pr_info(MOD_TAG ": 命中面板节点 %s\n", p->full_name);
	walk_timings(p);
	of_node_put(p);
}

static void check_symbols(void)
{
	int i, hits = 0;

	if (!sym_kallsyms_lookup_name) {
		pr_warn(MOD_TAG ": kallsyms 不可用，跳过符号检查\n");
		return;
	}
	for (i = 0; i < ARRAY_SIZE(want_symbols); i++) {
		unsigned long a = sym_kallsyms_lookup_name(want_symbols[i]);

		if (a) {
			hits++;
			pr_info(MOD_TAG ": 符号 %s @ 0x%lx\n", want_symbols[i], a);
		} else {
			pr_info(MOD_TAG ": 符号缺失 %s\n", want_symbols[i]);
		}
	}
	pr_info(MOD_TAG ": 候选 hook 命中 %d/%zu\n", hits, ARRAY_SIZE(want_symbols));
}

static int __init ace5_dsi_init(void)
{
	pr_info(MOD_TAG ": loaded, release=%s\n", init_utsname()->release);
	if (strncmp(init_utsname()->release, expect_release, strlen(expect_release)))
		pr_warn(MOD_TAG ": 内核版本与基线 %s 不一致，只读结果仅供参考\n", expect_release);
	if (resolve_kallsyms())
		return -ENOENT;
	scan_devicetree();
	check_symbols();
	pr_info(MOD_TAG ": read-only probe done, nothing hooked\n");
	return 0;
}

static void __exit ace5_dsi_exit(void)
{
	pr_info(MOD_TAG ": unloaded\n");
}

module_init(ace5_dsi_init);
module_exit(ace5_dsi_exit);
MODULE_DESCRIPTION("OnePlus Ace5 DSI read-only ABI probe (iteration 1)");
MODULE_AUTHOR("XiYan");
MODULE_LICENSE("GPL");
