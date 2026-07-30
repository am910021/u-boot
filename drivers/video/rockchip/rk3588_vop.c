// SPDX-License-Identifier: GPL-2.0
/*
 * Rockchip RK3588 VOP2 support
 *
 * Hardware register programming is derived from Rockchip's vendor U-Boot.
 */

#include <clk.h>
#include <dm.h>
#include <log.h>
#include <regmap.h>
#include <syscon.h>
#include <video.h>
#include <asm/arch-rockchip/cru_rk3588.h>
#include <asm/global_data.h>
#include <asm/io.h>
#include <linux/bitfield.h>
#include <linux/err.h>
#include <linux/iopoll.h>

#include "rk_vop2.h"

DECLARE_GLOBAL_DATA_PTR;

#define RK3588_DSP_IF_EN			0x028
#define RK3588_DSP_IF_CTRL			0x02c
#define RK3588_DSP_IF_POL			0x030
#define RK3588_IF_CFG_DONE_IMMEDIATE		BIT(28)
#define RK3588_SYS_PD_CTRL			0x034
#define RK3588_SYS_STATUS0			0x060
#define RK3588_OVL_CTRL				0x600
#define RK3588_OVL_CTRL_ROUTE_CLEAR		(BIT(0) | BIT(28) | \
						 GENMASK(31, 30))
#define RK3588_OVL_LAYER_SEL			0x604
#define RK3588_OVL_PORT_SEL			0x608
#define RK3588_OVL_LAYER_SEL_CLUSTER0_VP0	0x76543210
#define RK3588_OVL_PORT_SEL_CLUSTER0_VP0		0xa0507873
#define RK3588_CLUSTER0_MIX_SRC_COLOR		0x610
#define RK3588_CLUSTER0_MIX_DST_COLOR		0x614
#define RK3588_CLUSTER0_MIX_SRC_ALPHA		0x618
#define RK3588_CLUSTER0_MIX_DST_ALPHA		0x61c
#define RK3588_CLUSTER0_SRC_COLOR_OPAQUE		0x000001a1
#define RK3588_CLUSTER0_DST_COLOR_OPAQUE		0x00ff0061
#define RK3588_CLUSTER0_SRC_ALPHA_OPAQUE		0x00000020
#define RK3588_CLUSTER0_DST_ALPHA_OPAQUE		0x00000074
#define RK3588_HDMI0_EN				BIT(3)
#define RK3588_HDMI0_MUX			GENMASK(17, 16)
#define RK3588_PD_ENABLE_MASK			0x8f
#define RK3588_PD_STATUS_MASK			0x8f00

#define RK3588_HDMI0_DCLK_DIV			GENMASK(17, 16)
#define RK3588_HDMI0_PIXCLK_DIV			GENMASK(19, 18)

#define RK3588_VP0_COLOR_BAR_CTRL		0xc08
#define RK3588_VP0_CLK_CTRL			0xc0c
#define RK3588_VP_CLK_CTRL_STRIDE		0x100
#define RK3588_DCLK_CORE_DIV			GENMASK(1, 0)
#define RK3588_DCLK_OUT_DIV			GENMASK(3, 2)
#define RK3588_VP0_DSP_BG			0xc2c

#define RK3588_ESMART0_CTRL1			0x1804
#define RK3588_ESMART0_AXI_CTRL			0x1808
#define RK3588_CLUSTER0_CTRL			0x1100
#define RK3588_CLUSTER0_CTRL2			0x1008
#define RK3588_CLUSTER0_SCL_FACTOR		0x1030
#define RK3588_CLUSTER0_AFBCD_OUTPUT		0x1050
#define RK3588_CLUSTER0_AFBCD_CTRL		0x106c
#define RK3588_CLUSTER_AXI_ID			BIT(13)
#define RK3588_CLUSTER_AXI_YRGB_ID		GENMASK(4, 0)
#define RK3588_CLUSTER_AXI_UV_ID		GENMASK(9, 5)
#define RK3588_CLUSTER_AFBCD_HALF_BLOCK		BIT(7)
#define RK3588_CLUSTER_DLY_NUM			0x6f0
#define RK3588_CLUSTER_DLY_VALUE			0x00000404
#define RK3588_VP0_BG_MIX_CTRL			0x6e0
#define RK3588_BG_DLY				GENMASK(31, 24)
#define RK3588_VP0_BG_DLY			54
#define RK3588_ESMART_AXI_ID			BIT(1)
#define RK3588_ESMART_AXI_YRGB_ID		GENMASK(8, 4)
#define RK3588_ESMART_AXI_UV_ID			GENMASK(16, 12)
#define RK3588_SMART_DLY_NUM			0x6f8
#define RK3588_SMART_DLY_VALUE			0x17170000
#define RK3588_IOMMU0_OFFSET			0x7e00
#define RK3588_IOMMU_STRIDE			0x100
#define RK3588_IOMMU_STATUS			0x04
#define RK3588_IOMMU_COMMAND			0x08
#define RK3588_IOMMU_PAGE_FAULT_ACTIVE		BIT(1)
#define RK3588_IOMMU_DISABLE_PAGING		1
#define RK3588_IOMMU_FORCE_RESET		6
#define RK3588_VOP_GRF_CON2			0x008
#define RK3588_HDMITX0_ENABLE			BIT(1)

#define RK3588_VO1_GRF_CON0			0x000
#define RK3588_HDMI0_SYNC_POL			GENMASK(6, 5)

#define RK3588_VOP_CLK_GATE			RK3588_CLKGATE_CON(52)
#define RK3588_VOP_BUS_CLOCKS			(GENMASK(3, 0) | GENMASK(9, 8))

#define RK3588_HSYNC_POSITIVE			BIT(0)
#define RK3588_VSYNC_POSITIVE			BIT(1)

static int rk3588_get_grf(struct udevice *dev, const char *name, void **base)
{
	struct regmap *map;

	map = syscon_regmap_lookup_by_phandle(dev, name);
	if (IS_ERR(map))
		return PTR_ERR(map);

	*base = regmap_get_range(map, 0);
	if (!*base)
		return -ENXIO;

	return 0;
}

static int rk3588_enable_vop_bus_clocks(struct udevice *dev)
{
	struct clk aclk;
	void *cru;
	int ret;

	ret = clk_get_by_name(dev, "aclk", &aclk);
	if (ret)
		return ret;

	cru = dev_read_addr_ptr(aclk.dev);
	if (!cru)
		return -ENXIO;

	writel(RK3588_VOP_BUS_CLOCKS << 16,
	       cru + RK3588_VOP_CLK_GATE);

	return 0;
}

static void rk3588_disable_vop_mmu(struct rk_vop2_priv *priv)
{
	int i;

	for (i = 0; i < 2; i++) {
		void *mmu = priv->regs + RK3588_IOMMU0_OFFSET +
			    i * RK3588_IOMMU_STRIDE;
		u32 status = readl(mmu + RK3588_IOMMU_STATUS);

		writel(RK3588_IOMMU_DISABLE_PAGING,
		       mmu + RK3588_IOMMU_COMMAND);
		if (status & RK3588_IOMMU_PAGE_FAULT_ACTIVE)
			writel(RK3588_IOMMU_FORCE_RESET,
			       mmu + RK3588_IOMMU_COMMAND);

	}
}

static void rk3588_enable_output(struct udevice *dev,
				 enum vop_modes mode, u32 port)
{
	struct rk_vop2_priv *priv = dev_get_priv(dev);
	u32 reg;

	if (mode != VOP_MODE_HDMI)
		return;

	reg = readl(priv->regs + RK3588_DSP_IF_EN);
	reg &= ~(RK3588_HDMI0_EN | RK3588_HDMI0_MUX);
	reg |= RK3588_HDMI0_EN | FIELD_PREP(RK3588_HDMI0_MUX, port);
	writel(reg, priv->regs + RK3588_DSP_IF_EN);

	reg = readl(priv->regs + RK3588_DSP_IF_CTRL);
	reg &= ~(RK3588_HDMI0_DCLK_DIV | RK3588_HDMI0_PIXCLK_DIV);
	reg |= FIELD_PREP(RK3588_HDMI0_DCLK_DIV, 2) |
	       FIELD_PREP(RK3588_HDMI0_PIXCLK_DIV, 1);
	writel(reg, priv->regs + RK3588_DSP_IF_CTRL);

	reg = readl(priv->regs + RK3588_VP0_CLK_CTRL +
		    port * RK3588_VP_CLK_CTRL_STRIDE);
	reg &= ~(RK3588_DCLK_CORE_DIV | RK3588_DCLK_OUT_DIV);
	reg |= FIELD_PREP(RK3588_DCLK_CORE_DIV, 2);
	writel(reg, priv->regs + RK3588_VP0_CLK_CTRL +
	       port * RK3588_VP_CLK_CTRL_STRIDE);

	writel((RK3588_HDMITX0_ENABLE << 16) | RK3588_HDMITX0_ENABLE,
	       priv->vop_grf + RK3588_VOP_GRF_CON2);

	writel(RK3588_OVL_LAYER_SEL_CLUSTER0_VP0,
	       priv->regs + RK3588_OVL_LAYER_SEL);
	writel(RK3588_OVL_PORT_SEL_CLUSTER0_VP0,
	       priv->regs + RK3588_OVL_PORT_SEL);
	clrbits_le32(priv->regs + RK3588_OVL_CTRL,
		     RK3588_OVL_CTRL_ROUTE_CLEAR);
	writel(0, priv->regs + RK3588_CLUSTER0_SCL_FACTOR);
	writel(BIT(4), priv->regs + RK3588_CLUSTER0_AFBCD_OUTPUT);
	writel(0, priv->regs + RK3588_VP0_DSP_BG);
	writel(0, priv->regs + RK3588_VP0_COLOR_BAR_CTRL);

}

static void rk3588_set_pin_polarity(struct udevice *dev,
				    enum vop_modes mode, u32 polarity)
{
	struct rk_vop2_priv *priv = dev_get_priv(dev);
	u32 val = 0;

	if (mode != VOP_MODE_HDMI)
		return;

	if (!(polarity & RK3588_HSYNC_POSITIVE))
		val |= BIT(0);
	if (!(polarity & RK3588_VSYNC_POSITIVE))
		val |= BIT(1);

	setbits_le32(priv->regs + RK3588_DSP_IF_POL,
		     RK3588_IF_CFG_DONE_IMMEDIATE);
	writel((RK3588_HDMI0_SYNC_POL << 16) |
	       FIELD_PREP(RK3588_HDMI0_SYNC_POL, val),
	       priv->vo_grf + RK3588_VO1_GRF_CON0);

}

static int rk3588_vop_initialize(struct udevice *dev)
{
	struct rk_vop2_priv *priv = dev_get_priv(dev);
	struct rk3568_vop_sysctrl *sysctrl;
	u32 reg;
	int ret;

	priv->regs = dev_read_addr_ptr(dev);
	if (!priv->regs)
		return -EINVAL;

	ret = rk3588_enable_vop_bus_clocks(dev);
	if (ret)
		return ret;

	rk3588_disable_vop_mmu(priv);

	ret = rk3588_get_grf(dev, "rockchip,vop-grf", &priv->vop_grf);
	if (ret)
		return ret;

	ret = rk3588_get_grf(dev, "rockchip,vo1-grf", &priv->vo_grf);
	if (ret)
		return ret;

	sysctrl = priv->regs + VOP2_SYSREG_OFFSET;

	clrbits_le32(priv->regs + RK3588_SYS_PD_CTRL,
		     RK3588_PD_ENABLE_MASK);
	ret = readl_poll_timeout(priv->regs + RK3588_SYS_STATUS0, reg,
				 !(reg & RK3588_PD_STATUS_MASK), 50000);
	if (ret) {
		log_err("window power-on timed out\n");
		return ret;
	}
	clrbits_le32(priv->regs + RK3588_CLUSTER0_CTRL,
		     RK3588_CLUSTER_AXI_ID);
	reg = readl(priv->regs + RK3588_CLUSTER0_CTRL2);
	reg &= ~(RK3588_CLUSTER_AXI_YRGB_ID | RK3588_CLUSTER_AXI_UV_ID);
	reg |= FIELD_PREP(RK3588_CLUSTER_AXI_YRGB_ID, 2) |
	       FIELD_PREP(RK3588_CLUSTER_AXI_UV_ID, 3);
	writel(reg, priv->regs + RK3588_CLUSTER0_CTRL2);
	setbits_le32(priv->regs + RK3588_CLUSTER0_AFBCD_CTRL,
		     RK3588_CLUSTER_AFBCD_HALF_BLOCK);
	writel(RK3588_CLUSTER_DLY_VALUE,
	       priv->regs + RK3588_CLUSTER_DLY_NUM);

	clrbits_le32(priv->regs + RK3588_ESMART0_AXI_CTRL,
		     RK3588_ESMART_AXI_ID);
	reg = readl(priv->regs + RK3588_ESMART0_CTRL1);
	reg &= ~(RK3588_ESMART_AXI_YRGB_ID | RK3588_ESMART_AXI_UV_ID);
	reg |= FIELD_PREP(RK3588_ESMART_AXI_YRGB_ID, 0x0a) |
	       FIELD_PREP(RK3588_ESMART_AXI_UV_ID, 0x0b);
	writel(reg, priv->regs + RK3588_ESMART0_CTRL1);
	writel(RK3588_SMART_DLY_VALUE,
	       priv->regs + RK3588_SMART_DLY_NUM);
	clrsetbits_le32(priv->regs + RK3588_VP0_BG_MIX_CTRL,
			RK3588_BG_DLY,
			FIELD_PREP(RK3588_BG_DLY, RK3588_VP0_BG_DLY));
	writel(RK3588_CLUSTER0_SRC_COLOR_OPAQUE,
	       priv->regs + RK3588_CLUSTER0_MIX_SRC_COLOR);
	writel(RK3588_CLUSTER0_DST_COLOR_OPAQUE,
	       priv->regs + RK3588_CLUSTER0_MIX_DST_COLOR);
	writel(RK3588_CLUSTER0_SRC_ALPHA_OPAQUE,
	       priv->regs + RK3588_CLUSTER0_MIX_SRC_ALPHA);
	writel(RK3588_CLUSTER0_DST_ALPHA_OPAQUE,
	       priv->regs + RK3588_CLUSTER0_MIX_DST_ALPHA);
	clrbits_le32(&sysctrl->autogating_ctrl, M_AUTO_GATING);
	writel(M_GLOBAL_REGDONE, &sysctrl->reg_cfg_done);

	return 0;
}

static int rk3588_vop_probe(struct udevice *dev)
{
	int ret;

	if (!(gd->flags & GD_FLG_RELOC))
		return 0;

	ret = rk3588_vop_initialize(dev);
	if (ret) {
		printf("rk3588-vop: initialize failed: %d\n", ret);
		return ret;
	}

	return rk_vop2_probe(dev);
}

static int rk3588_vop_remove(struct udevice *dev)
{
	if (CONFIG_IS_ENABLED(VIDEO_REMOVE)) {
		struct rk_vop2_priv *priv = dev_get_priv(dev);

		if (priv->layer < ROCKCHIP_VOP2_ESMART0) {
			struct rk3568_vop_cluster *cluster;

			cluster = priv->regs + VOP2_CLUSTER_OFFSET(priv->layer);
			writel(0, &cluster->win0_ctrl0);
			writel(0, priv->regs + VOP2_CLUSTER_OFFSET(priv->layer) +
			       VOP2_CLUSTER_CTRL_OFFSET);
		} else {
			struct rk3568_vop_esmart *esmart;

			esmart = priv->regs +
				 VOP2_ESMART_OFFSET(priv->layer - 4);
			writel(0, &esmart->esmart_region0_mst_ctl);
		}
		writel(M_GLOBAL_REGDONE | M_LOAD_GLOBAL(priv->vp) |
		       (M_LOAD_GLOBAL(priv->vp) << 16), priv->regs);
	}

	return 0;
}

static int rk3588_vop_bind(struct udevice *dev)
{
	struct video_uc_plat *plat = dev_get_uclass_plat(dev);
	int ret;

	ret = rk_vop2_bind(dev);
	if (ret)
		return ret;

	/* Keep the 4K framebuffer inside the first contiguous DRAM bank. */
	plat->base = 0x20000000;

	return 0;
}

static struct rkvop2_platdata rk3588_platdata = {
	.delay = 20,
	.bg_dly = {RK3588_VP0_BG_DLY, 54, 52, 52},
	.vp_lyr = {0, 3, 6, 7},
	.layers = {
		ROCKCHIP_VOP2_CLUSTER0,
		ROCKCHIP_VOP2_CLUSTER1,
		ROCKCHIP_VOP2_ESMART0,
		ROCKCHIP_VOP2_ESMART1,
		ROCKCHIP_VOP2_CLUSTER2,
		ROCKCHIP_VOP2_CLUSTER3,
		ROCKCHIP_VOP2_ESMART2,
		ROCKCHIP_VOP2_ESMART3,
	},
};

static struct rkvop2_driverdata rk3588_driverdata = {
	.set_pin_polarity = rk3588_set_pin_polarity,
	.enable_output = rk3588_enable_output,
	.platdata = &rk3588_platdata,
};

static const struct udevice_id rk3588_vop_ids[] = {
	{
		.compatible = "rockchip,rk3588-vop",
		.data = (ulong)&rk3588_driverdata,
	},
	{ }
};

static const struct video_ops rk3588_vop_ops = {
};

U_BOOT_DRIVER(rk3588_vop) = {
	.name = "rk3588_vop",
	.id = UCLASS_VIDEO,
	.of_match = rk3588_vop_ids,
	.ops = &rk3588_vop_ops,
	.bind = rk3588_vop_bind,
	.probe = rk3588_vop_probe,
	.remove = rk3588_vop_remove,
	.priv_auto = sizeof(struct rk_vop2_priv),
#if CONFIG_IS_ENABLED(VIDEO_REMOVE)
	.flags = DM_FLAG_PRE_RELOC | DM_FLAG_OS_PREPARE,
#else
	.flags = DM_FLAG_PRE_RELOC,
#endif
};
