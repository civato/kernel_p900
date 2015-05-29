/*
 * Copyright (c) 2012 Samsung Electronics Co., Ltd.
 *		http://www.samsung.com
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
*/

#include <linux/io.h>
#include <linux/slab.h>
#include <linux/mutex.h>
#include <linux/suspend.h>
#include <linux/opp.h>
#include <linux/clk.h>
#include <linux/list.h>
#include <linux/rculist.h>
#include <linux/device.h>
#include <linux/sysfs_helpers.h>
#include <linux/devfreq.h>
#include <linux/platform_device.h>
#include <linux/regulator/consumer.h>
#include <linux/module.h>
#include <linux/pm_qos.h>
#include <linux/reboot.h>
#include <linux/kobject.h>
#include <linux/errno.h>

#include <mach/regs-clock.h>
#include <mach/devfreq.h>
#include <mach/asv-exynos.h>
#include <mach/tmu.h>

#include <plat/pll.h>
#include <plat/clock.h>

#include "exynos5420_ppmu.h"
#include <mach/sec_debug.h>

#define INT_VOLT_STEP		6250
#define COLD_VOLT_OFFSET	37500
#define LIMIT_COLD_VOLTAGE	1250000
#define MIN_COLD_VOLTAGE	950000

static struct pm_qos_request exynos5_int_qos;
static struct pm_qos_request boot_int_qos;

cputime64_t int_pre_time;


enum int_bus_idx {
	LV_0 = 0,
	LV_1,
	LV_1_1,
	LV_1_2,
	LV_1_3,
	LV_2,
	LV_3,
	LV_4,
	LV_5,
	LV_6,
	LV_END,
};

enum int_bus_pll {
	SW_MUX = 0,
	C_PLL,
	D_PLL,
	M_PLL,
	I_PLL,
};

struct int_bus_opp_table {
	unsigned int idx;
	unsigned long freq;
	unsigned long volt;
	cputime64_t time_in_state;
};

struct int_bus_opp_table int_bus_opp_list[] = {
	{LV_0,   600000, 1075000, 0},	/* ISP Special Level */
	{LV_1,   500000,  987500, 0},	/* ISP Special Level */
	{LV_1_1, 480000,  987500, 0},	/* ISP Special Level */
	{LV_1_2, 460000,  987500, 0},	/* ISP Special Level */
	{LV_1_3, 440000,  987500, 0},	/* ISP Special Level */
	{LV_2,   400000,  987500, 0},
	{LV_3,   333000,  950000, 0},
	{LV_4,   222000,  950000, 0},
	{LV_5,   133000,  950000, 0},
	{LV_6,    83000,  925000, 0},
};

struct int_clk_info {
	unsigned int idx;
	unsigned long target_freq;
	enum int_bus_pll src_pll;
};

struct int_pm_clks {
	struct list_head node;
	const char *clk_name;
	struct clk *clk;
	const char *parent_clk_name;
	struct clk *parent_clk;
	const char *p_parent_clk_name;
	struct clk *p_parent_clk;
	struct int_clk_info *clk_info;
};

struct busfreq_data_int {
	struct list_head list;
	struct device *dev;
	struct devfreq *devfreq;
	struct opp *curr_opp;
	struct mutex lock;

	struct clk *mout_mpll;
	struct clk *mout_dpll;
	struct clk *mout_cpll;
	struct clk *mout_spll;
	struct clk *fout_spll;
	struct clk *fout_ipll;
	struct clk *fin_ipll;
	struct clk *ipll;

	bool spll_enabled;
	bool ipll_enabled;
	unsigned int volt_offset;
	struct regulator *vdd_int;
	struct exynos5_ppmu_handle *ppmu;

	struct notifier_block tmu_notifier;
	int busy;
};

/* TOP 0 */
struct int_clk_info aclk_200_fsys[] = {
	/* Level, Freq, Parent_Pll */
	{LV_0,   200000, D_PLL},
	{LV_1,   200000, D_PLL},
	{LV_1_1, 200000, D_PLL},
	{LV_1_2, 200000, D_PLL},
	{LV_1_3, 200000, D_PLL},
	{LV_2,   200000, D_PLL},
	{LV_3,   200000, D_PLL},
	{LV_4,   150000, D_PLL},
	{LV_5,   100000, D_PLL},
#if !defined(CONFIG_SUPPORT_WQXGA)
	{LV_6,   100000, D_PLL},
#endif
};

struct int_clk_info pclk_200_fsys[] = {
	/* Level, Freq, Parent_Pll */
	{LV_0,   200000, D_PLL},
	{LV_1,   200000, D_PLL},
	{LV_1_1, 200000, D_PLL},
	{LV_1_2, 200000, D_PLL},
	{LV_1_3, 200000, D_PLL},
	{LV_2,   200000, D_PLL},
	{LV_3,   150000, D_PLL},
	{LV_4,   150000, D_PLL},
	{LV_5,   100000, D_PLL},
#if !defined(CONFIG_SUPPORT_WQXGA)
	{LV_6,   100000, D_PLL},
#endif
};

struct int_clk_info aclk_100_noc[] = {
	/* Level, Freq, Parent_Pll */
	{LV_0,   100000, D_PLL},
	{LV_1,   100000, D_PLL},
	{LV_1_1, 100000, D_PLL},
	{LV_1_2, 100000, D_PLL},
	{LV_1_3, 100000, D_PLL},
	{LV_2,   100000, D_PLL},
	{LV_3,    86000, D_PLL},
	{LV_4,    75000, D_PLL},
	{LV_5,    67000, M_PLL},
#if !defined(CONFIG_SUPPORT_WQXGA)
	{LV_6,    67000, M_PLL},
#endif
};

struct int_clk_info aclk_400_wcore[] = {
	/* Level, Freq, Parent_Pll */
	{LV_0,   400000, SW_MUX},
	{LV_1,   400000, SW_MUX},
	{LV_1_1, 400000, SW_MUX},
	{LV_1_2, 400000, SW_MUX},
	{LV_1_3, 400000, SW_MUX},
	{LV_2,   400000, SW_MUX},
	{LV_3,   333000, C_PLL},
	{LV_4,   222000, C_PLL},
#if !defined(CONFIG_SUPPORT_WQXGA)
	{LV_5,   111000, C_PLL},
	{LV_6,    84000, C_PLL},
#else
	{LV_5,   134000, C_PLL},
#endif
};

struct int_clk_info aclk_200_fsys2[] = {
	/* Level, Freq, Parent_Pll */
	{LV_0,   200000, D_PLL},
	{LV_1,   200000, D_PLL},
	{LV_1_1, 200000, D_PLL},
	{LV_1_2, 200000, D_PLL},
	{LV_1_3, 200000, D_PLL},
	{LV_2,   200000, D_PLL},
	{LV_3,   200000, D_PLL},
	{LV_4,   150000, D_PLL},
	{LV_5,   100000, D_PLL},
#if !defined(CONFIG_SUPPORT_WQXGA)
	{LV_6,   100000, D_PLL},
#endif
};

struct int_clk_info aclk_200_disp1[] = {
	/* Level, Freq, Parent_Pll */
	{LV_0,   200000, D_PLL},
	{LV_1,   200000, D_PLL},
	{LV_1_1, 200000, D_PLL},
	{LV_1_2, 200000, D_PLL},
	{LV_1_3, 200000, D_PLL},
	{LV_2,   200000, D_PLL},
#if !defined(CONFIG_SUPPORT_WQXGA)
	{LV_3,   150000, D_PLL},
#else
	{LV_3,   200000, D_PLL},
#endif
	{LV_4,   150000, D_PLL},
	{LV_5,   100000, D_PLL},
#if !defined(CONFIG_SUPPORT_WQXGA)
	{LV_6,   100000, D_PLL},
#endif
};

struct int_clk_info aclk_400_mscl[] = {
	/* Level, Freq, Parent_Pll */
	{LV_0,   400000, SW_MUX},
	{LV_1,   400000, SW_MUX},
	{LV_1_1, 400000, SW_MUX},
	{LV_1_2, 400000, SW_MUX},
	{LV_1_3, 400000, SW_MUX},
	{LV_2,   400000, SW_MUX},
	{LV_3,   333000, C_PLL},
	{LV_4,   222000, C_PLL},
#if !defined(CONFIG_SUPPORT_WQXGA)
	{LV_5,   167000, C_PLL},
	{LV_6,    84000, C_PLL},
#else
	{LV_5,    84000, C_PLL},
#endif
};

struct int_clk_info aclk_400_isp[] = {
	/* Level, Freq, Parent_Pll */
	{LV_0,   400000, SW_MUX},
	{LV_1,   400000, SW_MUX},
	{LV_1_1, 400000, SW_MUX},
	{LV_1_2, 400000, SW_MUX},
	{LV_1_3, 400000, SW_MUX},
	{LV_2,    67000, M_PLL},
	{LV_3,    67000, M_PLL},
	{LV_4,    67000, M_PLL},
	{LV_5,    67000, M_PLL},
#if !defined(CONFIG_SUPPORT_WQXGA)
	{LV_6,    67000, M_PLL},
#endif
};

/* TOP 1 */
struct int_clk_info aclk_166[] = {
	/* Level, Freq, Parent_Pll */
	{LV_0,   167000, C_PLL},
	{LV_1,   167000, C_PLL},
	{LV_1_1, 167000, C_PLL},
	{LV_1_2, 167000, C_PLL},
	{LV_1_3, 167000, C_PLL},
	{LV_2,   167000, C_PLL},
	{LV_3,   134000, C_PLL},
	{LV_4,   111000, C_PLL},
	{LV_5,    84000, C_PLL},
#if !defined(CONFIG_SUPPORT_WQXGA)
	{LV_6,    84000, C_PLL},
#endif
};

struct int_clk_info aclk_266[] = {
	/* Level, Freq, Parent_Pll */
	{LV_0,   266000, M_PLL},
	{LV_1,   133000, M_PLL},
	{LV_1_1, 178000, M_PLL},
	{LV_1_2,  76000, M_PLL},
	{LV_1_3,  76000, M_PLL},
	{LV_2,   266000, M_PLL},
	{LV_3,   178000, M_PLL},
	{LV_4,   133000, M_PLL},
#if !defined(CONFIG_SUPPORT_WQXGA)
	{LV_5,   133000, M_PLL},
	{LV_6,    89000, M_PLL},
#else
	{LV_5,    89000, M_PLL},
#endif
};

struct int_clk_info aclk_66[] = {
	/* Level, Freq, Parent_Pll */
	{LV_0,    67000, C_PLL},
	{LV_1,    67000, C_PLL},
	{LV_1_1,  67000, C_PLL},
	{LV_1_2,  67000, C_PLL},
	{LV_1_3,  67000, C_PLL},
	{LV_2,    67000, C_PLL},
	{LV_3,    67000, C_PLL},
	{LV_4,    67000, C_PLL},
	{LV_5,    67000, C_PLL},
#if !defined(CONFIG_SUPPORT_WQXGA)
	{LV_6,    67000, C_PLL},
#endif
};

struct int_clk_info aclk_333_432_isp0[] = {
	/* Level, Freq, Parent_Pll */
	{LV_0,   432000, I_PLL},
#if !defined(CONFIG_SUPPORT_WQXGA)
	{LV_1,   144000, I_PLL},
#else
	{LV_1,   432000, I_PLL},
#endif
	{LV_1_1, 216000, I_PLL},
	{LV_1_2,  87000, I_PLL},
	{LV_1_3,  87000, I_PLL},
	{LV_2,     3000, I_PLL},
	{LV_3,     3000, I_PLL},
	{LV_4,     3000, I_PLL},
	{LV_5,     3000, I_PLL},
#if !defined(CONFIG_SUPPORT_WQXGA)
	{LV_6,     3000, I_PLL},
#endif
};

struct int_clk_info aclk_333_432_isp[] = {
	/* Level, Freq, Parent_Pll */
	{LV_0,   432000, I_PLL},
#if !defined(CONFIG_SUPPORT_WQXGA)
	{LV_1,   144000, I_PLL},
#else
	{LV_1,   432000, I_PLL},
#endif
	{LV_1_1, 216000, I_PLL},
	{LV_1_2,  87000, I_PLL},
	{LV_1_3,  87000, I_PLL},
	{LV_2,     3000, I_PLL},
	{LV_3,     3000, I_PLL},
	{LV_4,     3000, I_PLL},
	{LV_5,     3000, I_PLL},
#if !defined(CONFIG_SUPPORT_WQXGA)
	{LV_6,     3000, I_PLL},
#endif
};

struct int_clk_info aclk_333_432_gscl[] = {
	/* Level, Freq, Parent_Pll */
	{LV_0,   432000, I_PLL},
	{LV_1,   432000, I_PLL},
	{LV_1_1, 216000, I_PLL},
	{LV_1_2, 432000, I_PLL},
	{LV_1_3,  87000, I_PLL},
	{LV_2,     3000, I_PLL},
	{LV_3,     3000, I_PLL},
	{LV_4,     3000, I_PLL},
	{LV_5,     3000, I_PLL},
#if !defined(CONFIG_SUPPORT_WQXGA)
	{LV_6,     3000, I_PLL},
#endif
};

/* TOP 2 */
struct int_clk_info aclk_300_gscl[] = {
	/* Level, Freq, Parent_Pll */
	{LV_0,   300000, D_PLL},
	{LV_1,   300000, D_PLL},
	{LV_1_1, 300000, D_PLL},
	{LV_1_2, 300000, D_PLL},
	{LV_1_3, 300000, D_PLL},
	{LV_2,   300000, D_PLL},
	{LV_3,   300000, D_PLL},
	{LV_4,   200000, D_PLL},
#if !defined(CONFIG_SUPPORT_WQXGA)
	{LV_5,   150000, D_PLL},
	{LV_6,    75000, D_PLL},
#else
	{LV_5,    75000, D_PLL},
#endif
};

struct int_clk_info aclk_300_disp1[] = {
	/* Level, Freq, Parent_Pll */
#if !defined(CONFIG_SUPPORT_WQXGA)
	{LV_0,   200000, D_PLL},
	{LV_1,   200000, D_PLL},
	{LV_1_1, 200000, D_PLL},
	{LV_1_2, 200000, D_PLL},
	{LV_1_3, 200000, D_PLL},
	{LV_2,   200000, D_PLL},
	{LV_3,   200000, D_PLL},
	{LV_4,   200000, D_PLL},
	{LV_5,   200000, D_PLL},
	{LV_6,   120000, D_PLL},
#else
	{LV_0,   300000, D_PLL},
	{LV_1,   300000, D_PLL},
	{LV_1_1, 300000, D_PLL},
	{LV_1_2, 300000, D_PLL},
	{LV_1_3, 300000, D_PLL},
	{LV_2,   300000, D_PLL},
	{LV_3,   300000, D_PLL},
	{LV_4,   300000, D_PLL},
	{LV_5,   200000, D_PLL},
#endif
};

struct int_clk_info aclk_400_disp1[] = {
	/* Level, Freq, Parent_Pll */
#if !defined(CONFIG_SUPPORT_WQXGA)
	{LV_0,   300000, D_PLL},
	{LV_1,   300000, D_PLL},
	{LV_1_1, 300000, D_PLL},
	{LV_1_2, 300000, D_PLL},
	{LV_1_3, 300000, D_PLL},
	{LV_2,   300000, D_PLL},
	{LV_3,   300000, D_PLL},
	{LV_4,   200000, D_PLL},
	{LV_5,   200000, D_PLL},
	{LV_6,   120000, D_PLL},
#else
	{LV_0,   400000, SW_MUX},
	{LV_1,   400000, SW_MUX},
	{LV_1_1, 400000, SW_MUX},
	{LV_1_2, 400000, SW_MUX},
	{LV_1_3, 400000, SW_MUX},
	{LV_2,   400000, SW_MUX},
	{LV_3,   400000, SW_MUX},
	{LV_4,   300000, D_PLL},
	{LV_5,   200000, D_PLL},
#endif
};

struct int_clk_info aclk_300_jpeg[] = {
	/* Level, Freq, Parent_Pll */
	{LV_0,   300000, D_PLL},
	{LV_1,   300000, D_PLL},
	{LV_1_1, 300000, D_PLL},
	{LV_1_2, 300000, D_PLL},
	{LV_1_3, 300000, D_PLL},
	{LV_2,   300000, D_PLL},
	{LV_3,   300000, D_PLL},
	{LV_4,   200000, D_PLL},
#if !defined(CONFIG_SUPPORT_WQXGA)
	{LV_5,   150000, D_PLL},
	{LV_6,    75000, D_PLL},
#else
	{LV_5,    75000, D_PLL},
#endif
};

struct int_clk_info aclk_266_g2d[] = {
	/* Level, Freq, Parent_Pll */
	{LV_0,   266000, M_PLL},
	{LV_1,   266000, M_PLL},
	{LV_1_1, 266000, M_PLL},
	{LV_1_2, 266000, M_PLL},
	{LV_1_3, 266000, M_PLL},
	{LV_2,   266000, M_PLL},
	{LV_3,   266000, M_PLL},
	{LV_4,   178000, M_PLL},
#if !defined(CONFIG_SUPPORT_WQXGA)
	{LV_5,   133000, M_PLL},
	{LV_6,    67000, M_PLL},
#else
	{LV_5,    67000, M_PLL},
#endif
};

struct int_clk_info aclk_333_g2d[] = {
	/* Level, Freq, Parent_Pll */
	{LV_0,   333000, C_PLL},
	{LV_1,   333000, C_PLL},
	{LV_1_1, 333000, C_PLL},
	{LV_1_2, 333000, C_PLL},
	{LV_1_3, 333000, C_PLL},
	{LV_2,   333000, C_PLL},
	{LV_3,   222000, C_PLL},
	{LV_4,   222000, C_PLL},
#if !defined(CONFIG_SUPPORT_WQXGA)
	{LV_5,   167000, C_PLL},
	{LV_6,    84000, C_PLL},
#else
	{LV_5,    84000, C_PLL},
#endif
};

#define EXYNOS5_INT_PM_CLK(NAME, CLK, PCLK, P_PCLK, CLK_INFO)		\
static struct int_pm_clks int_pm_clks_##NAME = {	\
	.clk_name = CLK,					\
	.parent_clk_name = PCLK,				\
	.p_parent_clk_name = P_PCLK,				\
	.clk_info = CLK_INFO,					\
}

EXYNOS5_INT_PM_CLK(aclk_200_fsys, "aclk_200_fsys_sw",
		"aclk_200_fsys_dout", NULL, aclk_200_fsys);
EXYNOS5_INT_PM_CLK(pclk_200_fsys, "pclk_200_fsys_sw",
		"pclk_200_fsys_dout", NULL, pclk_200_fsys);
EXYNOS5_INT_PM_CLK(aclk_100_noc, "aclk_100_noc_sw",
		"aclk_100_noc_dout", NULL, aclk_100_noc);
EXYNOS5_INT_PM_CLK(aclk_400_wcore, "aclk_400_wcore_sw",
		"aclk_400_wcore_dout", "aclk_400_wcore_mout", aclk_400_wcore);
EXYNOS5_INT_PM_CLK(aclk_200_fsys2, "aclk_200_fsys2_sw",
		"aclk_200_fsys2_dout", NULL, aclk_200_fsys2);
EXYNOS5_INT_PM_CLK(aclk_200_disp1, "aclk_200_sw",
		"aclk_200_dout", NULL, aclk_200_disp1);
EXYNOS5_INT_PM_CLK(aclk_400_mscl, "aclk_400_mscl_sw",
		"aclk_400_mscl_dout", NULL, aclk_400_mscl);
EXYNOS5_INT_PM_CLK(aclk_400_isp, "aclk_400_isp_sw",
		"aclk_400_isp_dout", NULL, aclk_400_isp);
EXYNOS5_INT_PM_CLK(aclk_166, "aclk_166_sw",
		"aclk_166_dout", NULL, aclk_166);
EXYNOS5_INT_PM_CLK(aclk_266, "aclk_266_sw",
		"aclk_266_dout", NULL, aclk_266);
EXYNOS5_INT_PM_CLK(aclk_66, "aclk_66_sw",
		"aclk_66_dout", NULL, aclk_66);
EXYNOS5_INT_PM_CLK(aclk_333_432_isp, "aclk_333_432_isp_sw",
		"aclk_333_432_isp_dout", NULL, aclk_333_432_isp);
EXYNOS5_INT_PM_CLK(aclk_333_432_isp0, "aclk_333_432_isp0_sw",
		"aclk_333_432_isp0_dout", NULL, aclk_333_432_isp0);
EXYNOS5_INT_PM_CLK(aclk_333_432_gscl, "aclk_333_432_gscl_sw",
		"aclk_333_432_gscl_dout", NULL, aclk_333_432_gscl);
EXYNOS5_INT_PM_CLK(aclk_300_gscl, "aclk_300_gscl_sw",
		"aclk_300_gscl_dout", NULL, aclk_300_gscl);
EXYNOS5_INT_PM_CLK(aclk_300_disp1, "aclk_300_disp1_sw",
		"aclk_300_disp1_dout", NULL, aclk_300_disp1);
EXYNOS5_INT_PM_CLK(aclk_300_jpeg, "aclk_300_jpeg_sw",
		"aclk_300_jpeg_dout", NULL, aclk_300_jpeg);
EXYNOS5_INT_PM_CLK(aclk_266_g2d, "aclk_266_g2d_sw",
		"aclk_266_g2d_dout", NULL, aclk_266_g2d);
EXYNOS5_INT_PM_CLK(aclk_333_g2d, "aclk_333_g2d_sw",
		"aclk_333_g2d_dout", NULL, aclk_333_g2d);
EXYNOS5_INT_PM_CLK(aclk_400_disp1, "aclk_400_disp1_sw",
		"aclk_400_disp1_dout", NULL, aclk_400_disp1);

static struct int_pm_clks *exynos5_int_pm_clks[] = {
	&int_pm_clks_aclk_200_fsys,
	&int_pm_clks_pclk_200_fsys,
	&int_pm_clks_aclk_100_noc,
	&int_pm_clks_aclk_400_wcore,
	&int_pm_clks_aclk_200_fsys2,
	&int_pm_clks_aclk_200_disp1,
	&int_pm_clks_aclk_400_mscl,
	&int_pm_clks_aclk_400_isp,
	&int_pm_clks_aclk_166,
	&int_pm_clks_aclk_266,
	&int_pm_clks_aclk_66,
	&int_pm_clks_aclk_333_432_isp,
	&int_pm_clks_aclk_333_432_isp0,
	&int_pm_clks_aclk_333_432_gscl,
	&int_pm_clks_aclk_300_gscl,
	&int_pm_clks_aclk_300_disp1,
	&int_pm_clks_aclk_300_jpeg,
	&int_pm_clks_aclk_266_g2d,
	&int_pm_clks_aclk_333_g2d,
	&int_pm_clks_aclk_400_disp1,
};

#ifdef CONFIG_EXYNOS_THERMAL
static unsigned int get_limit_voltage(unsigned int voltage, unsigned int volt_offset)
{
	if (voltage > LIMIT_COLD_VOLTAGE)
		return voltage;

	if (voltage + volt_offset > LIMIT_COLD_VOLTAGE)
		return LIMIT_COLD_VOLTAGE;

	if (volt_offset && (voltage + volt_offset < MIN_COLD_VOLTAGE))
		return MIN_COLD_VOLTAGE;

	return voltage + volt_offset;
}
#endif

static struct clk *exynos5_change_pll(struct busfreq_data_int *data,
					enum int_bus_pll target_pll)
{
	struct clk *target_src_clk = NULL;

	switch (target_pll) {
	case SW_MUX:
		target_src_clk = data->mout_spll;
		break;
	case C_PLL:
		target_src_clk = data->mout_cpll;
		break;
	case M_PLL:
		target_src_clk = data->mout_mpll;
		break;
	case D_PLL:
		target_src_clk = data->mout_dpll;
	default:
		break;
	}

	return target_src_clk;
}

static void exynos5_int_set_freq(struct busfreq_data_int *data,
					unsigned long target_freq, unsigned long pre_freq)
{
	unsigned int i;
	int target_idx = -EINVAL;
	int pre_idx = -EINVAL;
	struct int_pm_clks *int_clk;
	struct clk *temp_clk;

	sec_debug_aux_log(SEC_DEBUG_AUXLOG_CPU_BUS_CLOCK_CHANGE,
			"old:%7d new:%7d (INT)",
			pre_freq, target_freq);

	/* Find setting value with target and previous frequency */
	for (i = 0; i < LV_END; i++) {
		if (int_bus_opp_list[i].freq == target_freq)
			target_idx = int_bus_opp_list[i].idx;
		if (int_bus_opp_list[i].freq == pre_freq)
			pre_idx = int_bus_opp_list[i].idx;
	}

#if !defined(CONFIG_SUPPORT_WQXGA)
	if (target_idx <= LV_2 && !data->spll_enabled) {
#else
	if (target_idx <= LV_3 && !data->spll_enabled) {
#endif
		clk_enable(data->fout_spll);
		data->spll_enabled = true;
	}

	if (target_idx < LV_2 && !data->ipll_enabled) {
		clk_set_parent(data->fout_ipll, data->ipll);
		data->ipll_enabled = true;
	}

	list_for_each_entry(int_clk, &data->list, node) {

		if (int_clk->clk_info[pre_idx].src_pll !=
			int_clk->clk_info[target_idx].src_pll) {

			if (int_clk->clk_info[pre_idx].target_freq ==
				int_clk->clk_info[target_idx].target_freq)
				continue;

			temp_clk = exynos5_change_pll(data,
					int_clk->clk_info[target_idx].src_pll);

			if (int_clk->clk_info[target_idx].src_pll == SW_MUX) {
				clk_set_parent(int_clk->clk, temp_clk);
					continue;
			}

			if (pre_freq < target_freq) {
				if (int_clk->p_parent_clk)
					clk_set_parent(int_clk->p_parent_clk, temp_clk);
				else
					clk_set_parent(int_clk->parent_clk, temp_clk);
				clk_set_parent(int_clk->clk, int_clk->parent_clk);
				clk_set_rate(int_clk->parent_clk,
						int_clk->clk_info[target_idx].target_freq * 1000);
			} else {
				clk_set_rate(int_clk->parent_clk,
						int_clk->clk_info[target_idx].target_freq * 1000);
				clk_set_parent(int_clk->clk, int_clk->parent_clk);
				if (int_clk->p_parent_clk)
					clk_set_parent(int_clk->p_parent_clk, temp_clk);
				else
					clk_set_parent(int_clk->parent_clk, temp_clk);
				/*
				 * If the clock rate is set before setting the parent clock,
				 * the clock rate is incorrect after setting the parent clock
				 * by divider value. So, re-setting clock rate.
				 */
				clk_set_rate(int_clk->parent_clk,
						int_clk->clk_info[target_idx].target_freq * 1000);
			}
		} else {
			/* No need to change pll */
			clk_set_rate(int_clk->parent_clk,
				int_clk->clk_info[target_idx].target_freq * 1000);
		}
	}

	if (target_idx >= LV_2 && data->ipll_enabled) {
		clk_set_parent(data->fout_ipll, data->fin_ipll);
		data->ipll_enabled = false;
	}

#if !defined(CONFIG_SUPPORT_WQXGA)
	if (target_idx > LV_2 && data->spll_enabled) {
#else
	if (target_idx > LV_3 && data->spll_enabled) {
#endif
		clk_disable(data->fout_spll);
		data->spll_enabled = false;
	}
}

static void exynos5_int_update_state(unsigned int target_freq)
{
	cputime64_t cur_time = get_jiffies_64();
	cputime64_t tmp_cputime;
	unsigned int target_idx = LV_0;
	unsigned int i;

	/*
	 * Find setting value with target frequency
	 */
	for (i = LV_0; i < LV_END; i++) {
		if (int_bus_opp_list[i].freq == target_freq)
			target_idx = int_bus_opp_list[i].idx;
	}

	tmp_cputime = cur_time - int_pre_time;

	int_bus_opp_list[target_idx].time_in_state =
		int_bus_opp_list[target_idx].time_in_state + tmp_cputime;

	int_pre_time = cur_time;
}

unsigned long curr_int_freq;
static int exynos5_int_busfreq_target(struct device *dev,
				      unsigned long *_freq, u32 flags)
{
	int err = 0;
	struct platform_device *pdev = container_of(dev, struct platform_device, dev);
	struct busfreq_data_int *data = platform_get_drvdata(pdev);
	struct opp *opp;
	unsigned long freq;
	unsigned long old_freq;
	unsigned long target_volt;

	mutex_lock(&data->lock);

	/* get available opp information */
	rcu_read_lock();
	opp = devfreq_recommended_opp(dev, _freq, flags);
	if (IS_ERR(opp)) {
		rcu_read_unlock();
		dev_err(dev, "%s: Invalid OPP.\n", __func__);
		mutex_unlock(&data->lock);
		return PTR_ERR(opp);
	}

	freq = opp_get_freq(opp);
	target_volt = opp_get_voltage(opp);
	rcu_read_unlock();

	/* get olg opp information */
	rcu_read_lock();
	old_freq = opp_get_freq(data->curr_opp);
	rcu_read_unlock();

	exynos5_int_update_state(old_freq);

	if (old_freq == freq)
		goto out;

#ifdef CONFIG_EXYNOS_THERMAL
	if (data->volt_offset)
		target_volt = get_limit_voltage(target_volt, data->volt_offset);
#endif

	/*
	 * If target freq is higher than old freq
	 * after change voltage, setting freq ratio
	 */
	if (old_freq < freq) {
		regulator_set_voltage(data->vdd_int, target_volt, target_volt + INT_VOLT_STEP);

		exynos5_int_set_freq(data, freq, old_freq);
	} else {
		exynos5_int_set_freq(data, freq, old_freq);

		regulator_set_voltage(data->vdd_int, target_volt, target_volt + INT_VOLT_STEP);
	}

	curr_int_freq = freq;
	data->curr_opp = opp;
out:
	mutex_unlock(&data->lock);

	return err;
}

static int exynos5_int_bus_get_dev_status(struct device *dev,
				      struct devfreq_dev_status *stat)
{
	struct busfreq_data_int *data = dev_get_drvdata(dev);
	unsigned long busy_data;
	unsigned int int_ccnt = 0;
	unsigned long int_pmcnt = 0;


	rcu_read_lock();
	stat->current_frequency = opp_get_freq(data->curr_opp);
	rcu_read_unlock();

	/*
	 * Bandwidth of memory interface is 128bits
	 * So bus can transfer 16bytes per cycle
	 */

	busy_data = exynos5_ppmu_get_busy(data->ppmu, PPMU_SET_TOP,
					&int_ccnt, &int_pmcnt);

	stat->total_time = int_ccnt;
	stat->busy_time = int_pmcnt;

	return 0;
}

#if defined(CONFIG_DEVFREQ_GOV_SIMPLE_ONDEMAND)
static struct devfreq_simple_ondemand_data exynos5_int_governor_data = {
	.pm_qos_class		= PM_QOS_DEVICE_THROUGHPUT,
	.upthreshold		= 95,
	.cal_qos_max		= 400000,
};
#endif

static struct devfreq_dev_profile exynos5_int_devfreq_profile = {
	.initial_freq	= 400000,
	.polling_ms	= 100,
	.target		= exynos5_int_busfreq_target,
	.get_dev_status	= exynos5_int_bus_get_dev_status,
};

static int exynos5420_init_int_table(struct busfreq_data_int *data)
{
	unsigned int i;
	unsigned int ret;
	unsigned int asv_volt;

	/* will add code for ASV information setting function in here */

	for (i = 0; i < ARRAY_SIZE(int_bus_opp_list); i++) {
		asv_volt = get_match_volt(ID_INT, int_bus_opp_list[i].freq);

		if (!asv_volt)
			asv_volt = int_bus_opp_list[i].volt;

		pr_info("INT %luKhz ASV is %duV\n", int_bus_opp_list[i].freq, asv_volt);

		ret = opp_add(data->dev, int_bus_opp_list[i].freq, asv_volt);

		if (ret) {
			dev_err(data->dev, "Fail to add opp entries.\n");
			return ret;
		}
	}

	return 0;
}

static ssize_t int_show_state(struct device *dev, struct device_attribute *attr, char *buf)
{
	unsigned int i;
	ssize_t len = 0;
	ssize_t write_cnt = (ssize_t)((PAGE_SIZE / LV_END) - 2);

	for (i = LV_0; i < LV_END; i++)
		len += snprintf(buf + len, write_cnt, "%ld %llu\n", int_bus_opp_list[i].freq,
				(unsigned long long)int_bus_opp_list[i].time_in_state);

	return len;
}

static DEVICE_ATTR(int_time_in_state, 0644, int_show_state, NULL);


static struct attribute *busfreq_int_entries[] = {
	&dev_attr_int_time_in_state.attr,
	NULL,
};
static struct attribute_group busfreq_int_attr_group = {
	.name	= "time_in_state",
	.attrs	= busfreq_int_entries,
};

static ssize_t show_freq_table(struct device *dev, struct device_attribute *attr, char *buf)
{
	int i, count = 0;
	struct opp *opp;
	ssize_t write_cnt = (ssize_t)((PAGE_SIZE / ARRAY_SIZE(int_bus_opp_list)) - 2);
	struct device *int_dev = dev->parent;

	if (!unlikely(int_dev)) {
		pr_err("%s: device is not probed\n", __func__);
		return -ENODEV;
	}

	rcu_read_lock();
	for (i = 0; i < ARRAY_SIZE(int_bus_opp_list); i++) {
		opp = opp_find_freq_exact(int_dev, int_bus_opp_list[i].freq, true);
		if (!IS_ERR_OR_NULL(opp))
			count += snprintf(&buf[count], write_cnt, "%lu ", opp_get_freq(opp));
	}
	rcu_read_unlock();

	count += snprintf(&buf[count], 2, "\n");
	return count;
}

static DEVICE_ATTR(freq_table, S_IRUGO, show_freq_table, NULL);

static ssize_t show_volt_table(struct device *device,
		struct device_attribute *attr, char *buf)
{	
	struct device_opp *dev_opp = ERR_PTR(-ENODEV);
	struct opp *temp_opp;
	struct device *int_dev = device->parent;
	int len = 0;

	dev_opp = find_device_opp(int_dev);

	list_for_each_entry_rcu(temp_opp, &dev_opp->opp_list, node) {
		if (temp_opp->available)
			len += sprintf(buf + len, "%lu %lu\n",
					opp_get_freq(temp_opp),
					opp_get_voltage(temp_opp));
	}

	return len;
}

static ssize_t store_volt_table(struct device *device,
		struct device_attribute *attr,
		const char *buf, size_t count)
{
	struct device *int_dev = device->parent;
	struct device_opp *dev_opp = find_device_opp(int_dev);
	struct opp *temp_opp;
	int u[LV_END];
	int rest, t, i = 0;

	if ((t = read_into((int*)&u, LV_END, buf, count)) < 0)
		return -EINVAL;

	if (t == 2 && LV_END != 2) {
		temp_opp = opp_find_freq_exact(int_dev, u[0], true);
		if(IS_ERR(temp_opp))
			return -EINVAL;

		if ((rest = (u[1] % 6250)) != 0)
			u[1] += 6250 - rest;

		sanitize_min_max(u[1], 600000, 1300000);
		temp_opp->u_volt = u[1];
	} else {
		list_for_each_entry_rcu(temp_opp, &dev_opp->opp_list, node) {
			if (temp_opp->available) {
				if ((rest = (u[i] % 6250)) != 0)
					u[i] += 6250 - rest;

				sanitize_min_max(u[i], 600000, 1300000);
				temp_opp->u_volt = u[i++];
			}
		}
	}

	return count;
}

static DEVICE_ATTR(volt_table, S_IRUGO | S_IWUSR, show_volt_table, store_volt_table);


static struct exynos_devfreq_platdata default_qos_int_pd = {
	.default_qos = 400000,
};

static int exynos5_int_reboot_notifier_call(struct notifier_block *this,
				   unsigned long code, void *_cmd)
{
	pm_qos_update_request(&exynos5_int_qos,
			exynos5_int_devfreq_profile.initial_freq);

	return NOTIFY_DONE;
}

static struct notifier_block exynos5_int_reboot_notifier = {
	.notifier_call = exynos5_int_reboot_notifier_call,
};

#ifdef CONFIG_EXYNOS_THERMAL
static int exynos5_int_devfreq_tmu_notifier(struct notifier_block *notifier,
						unsigned long event, void *v)
{
	struct busfreq_data_int *data = container_of(notifier, struct busfreq_data_int,
								tmu_notifier);
	unsigned int prev_volt, set_volt;
	unsigned int *on = v;

	if (event != TMU_COLD)
		return NOTIFY_OK;

	mutex_lock(&data->lock);

	prev_volt = regulator_get_voltage(data->vdd_int);

	if (*on) {
		if (data->volt_offset != COLD_VOLT_OFFSET) {
			data->volt_offset = COLD_VOLT_OFFSET;
		} else {
			mutex_unlock(&data->lock);
			return NOTIFY_OK;
		}

		/* setting voltage for INT about cold temperature */
		set_volt = get_limit_voltage(prev_volt, data->volt_offset);
		regulator_set_voltage(data->vdd_int, set_volt, set_volt + INT_VOLT_STEP);
	} else {
		if (data->volt_offset != 0) {
			data->volt_offset = 0;
		} else {
			mutex_unlock(&data->lock);
			return NOTIFY_OK;
		}

		/* restore voltage for INT */
		set_volt = get_limit_voltage(prev_volt - COLD_VOLT_OFFSET, data->volt_offset);
		regulator_set_voltage(data->vdd_int, set_volt, set_volt + INT_VOLT_STEP);
	}

	mutex_unlock(&data->lock);

	return NOTIFY_OK;
}
#endif

static __devinit int exynos5_busfreq_int_probe(struct platform_device *pdev)
{
	struct busfreq_data_int *data;
	struct opp *opp;
	struct device *dev = &pdev->dev;
	struct exynos_devfreq_platdata *pdata;
	int err = 0;
	int nr_clk;
	struct clk *tmp_clk = NULL, *tmp_parent_clk = NULL, *tmp_p_parent_clk = NULL;
	struct int_pm_clks *int_clk;

	data = kzalloc(sizeof(struct busfreq_data_int), GFP_KERNEL);

	if (data == NULL) {
		dev_err(dev, "Cannot allocate memory for INT.\n");
		return -ENOMEM;
	}

	data->dev = dev;
	INIT_LIST_HEAD(&data->list);
	mutex_init(&data->lock);

	/* Setting table for int */
	exynos5420_init_int_table(data);

	data->vdd_int = regulator_get(dev, "vdd_int");
	if (IS_ERR(data->vdd_int)) {
		dev_err(dev, "Cannot get the regulator \"vdd_int\"\n");
		err = PTR_ERR(data->vdd_int);
		goto err_regulator;
	}

	data->fout_spll = clk_get(dev, "fout_spll");
	if (IS_ERR(data->fout_spll)) {
		dev_err(dev, "Cannot get clock \"fout_spll\"\n");
		err = PTR_ERR(data->fout_spll);
		goto err_fout_spll;
	}

	data->fout_ipll = clk_get(dev, "fout_ipll");
	if (IS_ERR(data->fout_ipll)) {
		dev_err(dev, "Cannot get clock \"fout_ipll\"\n");
		err = PTR_ERR(data->fout_ipll);
		goto err_fout_ipll;
	}

	data->ipll = clk_get(dev, "ipll");
	if (IS_ERR(data->ipll)) {
		dev_err(dev, "Cannot get clock \"ipll\"\n");
		err = PTR_ERR(data->ipll);
		goto err_ipll;
	}

	data->fin_ipll = clk_get(dev, "ext_xtal");
	if (IS_ERR(data->fin_ipll)) {
		dev_err(dev, "Cannot get clock \"fin_ipll\"\n");
		err = PTR_ERR(data->fin_ipll);
		goto err_fin_ipll;
	}

	data->mout_mpll = clk_get(dev, "mout_mpll");
	if (IS_ERR(data->mout_mpll)) {
		dev_err(dev, "Cannot get clock \"mout_mpll\"\n");
		err = PTR_ERR(data->mout_mpll);
		goto err_mout_mpll;
	}

	data->mout_dpll = clk_get(dev, "mout_dpll");
	if (IS_ERR(data->mout_dpll)) {
		dev_err(dev, "Cannot get clock \"mout_dpll\"\n");
		err = PTR_ERR(data->mout_dpll);
		goto err_mout_dpll;
	}

	data->mout_spll = clk_get(dev, "mout_spll");
	if (IS_ERR(data->mout_spll)) {
		dev_err(dev, "Cannot get clock \"mout_spll\"\n");
		err = PTR_ERR(data->mout_spll);
		goto err_mout_spll;
	}

	data->mout_cpll = clk_get(dev, "mout_cpll");
	if (IS_ERR(data->mout_cpll)) {
		dev_err(dev, "Cannot get clock \"mout_cpll\"\n");
		err = PTR_ERR(data->mout_cpll);
		goto err_mout_cpll;
	}

	/* Register and add int clocks to list */
	for (nr_clk = 0; nr_clk < ARRAY_SIZE(exynos5_int_pm_clks); nr_clk++) {
		int_clk = exynos5_int_pm_clks[nr_clk];

		tmp_clk = clk_get(dev, int_clk->clk_name);
		tmp_parent_clk = clk_get(dev, int_clk->parent_clk_name);
		if (int_clk->p_parent_clk_name)
			tmp_p_parent_clk = clk_get(dev, int_clk->p_parent_clk_name);
		else
			tmp_p_parent_clk = NULL;

		if ((!IS_ERR(tmp_clk)) && (!IS_ERR(tmp_parent_clk))) {
			int_clk->clk = tmp_clk;
			int_clk->parent_clk = tmp_parent_clk;
			if (int_clk->p_parent_clk_name) {
				if (!IS_ERR(tmp_p_parent_clk)) {
					int_clk->p_parent_clk = tmp_p_parent_clk;
				} else {
					dev_err(dev, "Failed to get %s clock\n", tmp_p_parent_clk->name);
					goto err_int_clk;
				}
			}
			list_add_tail(&int_clk->node, &data->list);
		} else {
			dev_err(dev, "Failed to get %s clock\n", tmp_clk->name);
			goto err_int_clk;
		}
	}

	rcu_read_lock();
	opp = opp_find_freq_floor(dev, &exynos5_int_devfreq_profile.initial_freq);
	if (IS_ERR(opp)) {
		rcu_read_unlock();
		dev_err(dev, "Invalid initial frequency %lu kHz.\n",
			       exynos5_int_devfreq_profile.initial_freq);
		err = PTR_ERR(opp);
		goto err_opp_add;
	}
	rcu_read_unlock();

	int_pre_time = get_jiffies_64();

	data->curr_opp = opp;
	data->volt_offset = 0;

	data->ipll_enabled = false;
#ifdef CONFIG_EXYNOS_THERMAL
	data->tmu_notifier.notifier_call = exynos5_int_devfreq_tmu_notifier;
#endif

	platform_set_drvdata(pdev, data);

	data->ppmu = exynos5_ppmu_get();
	if (!data->ppmu)
		goto err_opp_add;

#if defined(CONFIG_DEVFREQ_GOV_USERSPACE)
	data->devfreq = devfreq_add_device(dev, &exynos5_int_devfreq_profile,
						&devfreq_userspace, NULL);
#endif
#if defined(CONFIG_DEVFREQ_GOV_SIMPLE_ONDEMAND)
	data->devfreq = devfreq_add_device(dev, &exynos5_int_devfreq_profile,
					   &devfreq_simple_ondemand, &exynos5_int_governor_data);
#endif
	if (IS_ERR(data->devfreq)) {
		err = PTR_ERR(data->devfreq);
		goto err_opp_add;
	}

	devfreq_register_opp_notifier(dev, data->devfreq);

	/* Create file for time_in_state */
	err = sysfs_create_group(&data->devfreq->dev.kobj, &busfreq_int_attr_group);

	/* Add sysfs for freq_table */
	err = device_create_file(&data->devfreq->dev, &dev_attr_freq_table);
	if (err)
		pr_err("%s: Fail to create sysfs file\n", __func__);

	/* Add sysfs for volt_table */
	err = device_create_file(&data->devfreq->dev, &dev_attr_volt_table);
	if (err)
		pr_err("%s: Fail to create sysfs file\n", __func__);

	pdata = pdev->dev.platform_data;
	if (!pdata)
		pdata = &default_qos_int_pd;

	clk_enable(data->fout_spll);
	data->spll_enabled = true;

	pm_qos_add_request(&boot_int_qos, PM_QOS_DEVICE_THROUGHPUT,
				exynos5_int_devfreq_profile.initial_freq);
	pm_qos_update_request_timeout(&boot_int_qos,
			exynos5_int_devfreq_profile.initial_freq, 40000 * 1000);
	pm_qos_add_request(&exynos5_int_qos, PM_QOS_DEVICE_THROUGHPUT, pdata->default_qos);

	register_reboot_notifier(&exynos5_int_reboot_notifier);

#ifdef CONFIG_EXYNOS_THERMAL
	exynos_tmu_add_notifier(&data->tmu_notifier);
#endif

	return 0;

err_opp_add:
err_int_clk:
	clk_put(tmp_clk);
	clk_put(tmp_parent_clk);
	clk_put(data->mout_cpll);
err_mout_cpll:
	clk_put(data->mout_spll);
err_mout_spll:
	clk_put(data->mout_dpll);
err_mout_dpll:
	clk_put(data->mout_mpll);
err_mout_mpll:
	clk_put(data->fin_ipll);
err_fin_ipll:
	clk_put(data->ipll);
err_ipll:
	clk_put(data->fout_ipll);
err_fout_ipll:
	clk_put(data->fout_spll);
err_fout_spll:
	regulator_put(data->vdd_int);
err_regulator:
	kfree(data);

	return err;
}

static __devexit int exynos5_busfreq_int_remove(struct platform_device *pdev)
{
	struct busfreq_data_int *data = platform_get_drvdata(pdev);

	devfreq_remove_device(data->devfreq);

	pm_qos_remove_request(&exynos5_int_qos);

	clk_put(data->mout_cpll);
	clk_put(data->mout_spll);
	clk_put(data->mout_dpll);
	clk_put(data->mout_mpll);
	clk_put(data->fin_ipll);
	clk_put(data->ipll);
	clk_put(data->fout_ipll);
	clk_put(data->fout_spll);

	regulator_put(data->vdd_int);

	kfree(data);

	platform_set_drvdata(pdev, NULL);

	return 0;
}

static int exynos5_busfreq_int_suspend(struct device *dev)
{
	if (pm_qos_request_active(&exynos5_int_qos))
		pm_qos_update_request(&exynos5_int_qos,
				exynos5_int_devfreq_profile.initial_freq);

	return 0;
}

static int exynos5_busfreq_int_resume(struct device *dev)
{
	struct exynos_devfreq_platdata *pdata = dev->platform_data;

	if (pm_qos_request_active(&exynos5_int_qos))
		pm_qos_update_request(&exynos5_int_qos, pdata->default_qos);

	return 0;
}

static const struct dev_pm_ops exynos5_busfreq_int_pm = {
	.suspend	= exynos5_busfreq_int_suspend,
	.resume		= exynos5_busfreq_int_resume,
};

static struct platform_driver exynos5_busfreq_int_driver = {
	.probe	= exynos5_busfreq_int_probe,
	.remove	= __devexit_p(exynos5_busfreq_int_remove),
	.driver = {
		.name	= "exynos5-busfreq-int",
		.owner	= THIS_MODULE,
		.pm	= &exynos5_busfreq_int_pm,
	},
};

static int __init exynos5_busfreq_int_init(void)
{
	return platform_driver_register(&exynos5_busfreq_int_driver);
}
late_initcall(exynos5_busfreq_int_init);

static void __exit exynos5_busfreq_int_exit(void)
{
	platform_driver_unregister(&exynos5_busfreq_int_driver);
}
module_exit(exynos5_busfreq_int_exit);
