// SPDX-License-Identifier: GPL-2.0+
/*
 * Minimal RK3588 DesignWare HDMI QP display driver
 *
 * Copyright (c) 2026 Yuri
 */

#include <clk.h>
#include <display.h>
#include <dm.h>
#include <edid.h>
#include <fdtdec.h>
#include <generic-phy.h>
#include <regmap.h>
#include <reset.h>
#include <syscon.h>
#include <asm/io.h>
#include <linux/bitops.h>
#include <linux/delay.h>
#include <linux/err.h>

#include "../../phy/rockchip/phy-rockchip-samsung-hdptx-hdmi.h"

#define I2CM_FM_SCL_CONFIG0			0xe4
#define I2CM_CONTROL0				0xec
#define I2CM_INTERFACE_CONTROL0			0xf4
#define  I2CM_ADDR				GENMASK(19, 12)
#define  I2CM_SLVADDR				GENMASK(11, 5)
#define  I2CM_WR_MASK				GENMASK(4, 1)
#define  I2CM_NBYTES_MASK			GENMASK(23, 20)
#define  I2CM_16BYTES				(0xf << 20)
#define  I2CM_FM_READ				BIT(2)
#define I2CM_INTERFACE_RDDATA_0_3		0x10c

#define TIMER_BASE_CONFIG0			0x80
#define VIDEO_INTERFACE_CONFIG0			0x800
#define FRAME_COMPOSER_CONFIG9			0x864
#define  KEEPOUT_REKEY_CFG			GENMASK(9, 8)
#define  KEEPOUT_REKEY_ALWAYS			(0x2 << 8)
#define HDCP2LOGIC_CONFIG0			0x8e0
#define  HDCP2_BYPASS				BIT(0)
#define SCRAMB_CONFIG0				0x960
#define LINK_CONFIG0				0x968
#define  OPMODE_FRL_4LANES			BIT(8)
#define  OPMODE_DVI				BIT(4)
#define  OPMODE_FRL				BIT(0)
#define PKTSCHED_PKT_CONFIG1			0xa9c
#define  PKTSCHED_AVI_FIELDRATE			BIT(12)
#define PKTSCHED_PKT_EN				0xaa8
#define  PKTSCHED_AVI_TX_EN			BIT(13)
#define  PKTSCHED_GCP_TX_EN			BIT(3)
#define PKTSCHED_PKT_CONTROL0			0xaac
#define PKT_AVI_CONTENTS0			0xbe0
#define PKT_AVI_CONTENTS1			0xbe4

#define MAINUNIT_0_INT_MASK_N			0x3014
#define MAINUNIT_1_INT_STATUS			0x3020
#define  I2CM_NACK_RCVD_IRQ			BIT(2)
#define  I2CM_OP_DONE_IRQ			BIT(0)
#define MAINUNIT_1_INT_MASK_N			0x3024
#define MAINUNIT_1_INT_CLEAR			0x3028

#define RK3588_GRF_SOC_CON7			0x031c
#define  RK3588_SET_HPD_PATH_MASK		GENMASK(13, 12)
#define RK3588_GRF_SOC_STATUS1			0x0384
#define  RK3588_HDMI0_LEVEL_INT			BIT(16)

#define RK3588_GRF_VO1_CON3			0x000c
#define  RK3588_COLOR_FORMAT_MASK		GENMASK(3, 0)
#define  RK3588_COLOR_DEPTH_MASK		GENMASK(7, 4)
#define  RK3588_SCLIN_MASK			BIT(9)
#define  RK3588_SDAIN_MASK			BIT(10)
#define  RK3588_MODE_MASK			BIT(11)
#define  RK3588_COMPRESS_MODE_MASK		BIT(12)
#define  RK3588_I2S_SEL_MASK			BIT(13)
#define RK3588_GRF_VO1_CON4			0x0010
#define  RK3588_HDMI21_MASK			BIT(0)
#define RK3588_GRF_VO1_CON9			0x0024
#define  RK3588_HDMI0_GRANT_SEL			BIT(10)

#define RK3588_HDMI0_BASE			0xfde80000UL
#define HDMI_QP_MAX_TMDS_CLOCK			340000000UL
#define HDMI_EDID_BLOCK_SIZE			128
#define DDC_ADDR				0x50

#define HIWORD_UPDATE(value, mask)		((value) | ((mask) << 16))

struct rk3588_hdmi_qp {
	void __iomem *regs;
	void __iomem *grf;
	void __iomem *vo_grf;
	struct phy phy;
	struct clk_bulk clocks;
	struct reset_ctl_bulk resets;
};

static void hdmi_qp_modb(struct rk3588_hdmi_qp *priv, u32 data,
			 u32 mask, u32 reg)
{
	u32 val = readl(priv->regs + reg);

	val &= ~mask;
	val |= data & mask;
	writel(val, priv->regs + reg);
}

static bool rk3588_hdmi_qp_hpd(struct rk3588_hdmi_qp *priv)
{
	u32 status = readl(priv->grf + RK3588_GRF_SOC_STATUS1);

	return status & RK3588_HDMI0_LEVEL_INT;
}

static int rk3588_hdmi_qp_wait_hpd(struct rk3588_hdmi_qp *priv)
{
	int i;

	for (i = 0; i < 10; i++) {
		if (rk3588_hdmi_qp_hpd(priv)) {
			if (i)
				printf("rk3588-hdmi-qp: HPD ready after %d ms\n",
				       i * 100);
			return 0;
		}
		mdelay(100);
	}

	printf("rk3588-hdmi-qp: HPD timeout\n");
	return -ENODEV;
}

static void rk3588_hdmi_qp_io_path_init(struct rk3588_hdmi_qp *priv)
{
	u32 val;

	val = HIWORD_UPDATE(RK3588_SCLIN_MASK, RK3588_SCLIN_MASK) |
	      HIWORD_UPDATE(RK3588_SDAIN_MASK, RK3588_SDAIN_MASK) |
	      HIWORD_UPDATE(RK3588_MODE_MASK, RK3588_MODE_MASK) |
	      HIWORD_UPDATE(RK3588_I2S_SEL_MASK, RK3588_I2S_SEL_MASK);
	writel(val, priv->vo_grf + RK3588_GRF_VO1_CON3);

	val = HIWORD_UPDATE(RK3588_SET_HPD_PATH_MASK,
			    RK3588_SET_HPD_PATH_MASK);
	writel(val, priv->grf + RK3588_GRF_SOC_CON7);

	val = HIWORD_UPDATE(RK3588_HDMI0_GRANT_SEL,
			    RK3588_HDMI0_GRANT_SEL);
	writel(val, priv->vo_grf + RK3588_GRF_VO1_CON9);
}

static void rk3588_hdmi_qp_ddc_init(struct rk3588_hdmi_qp *priv)
{
	writel(1, priv->regs + I2CM_CONTROL0);
	writel(0x085c085c, priv->regs + I2CM_FM_SCL_CONFIG0);
	hdmi_qp_modb(priv, 0, BIT(0), I2CM_INTERFACE_CONTROL0);
	writel(I2CM_OP_DONE_IRQ | I2CM_NACK_RCVD_IRQ,
	       priv->regs + MAINUNIT_1_INT_CLEAR);
}

static int rk3588_hdmi_qp_ddc_chunk(struct rk3588_hdmi_qp *priv,
				    u8 offset, u8 *buf)
{
	u32 intr, val;
	int retry, reg, i;

	hdmi_qp_modb(priv, DDC_ADDR << 5, I2CM_SLVADDR,
		     I2CM_INTERFACE_CONTROL0);
	hdmi_qp_modb(priv, offset << 12, I2CM_ADDR,
		     I2CM_INTERFACE_CONTROL0);
	hdmi_qp_modb(priv, I2CM_16BYTES, I2CM_NBYTES_MASK,
		     I2CM_INTERFACE_CONTROL0);

	for (retry = 0; retry < 3; retry++) {
		writel(I2CM_OP_DONE_IRQ | I2CM_NACK_RCVD_IRQ,
		       priv->regs + MAINUNIT_1_INT_CLEAR);
		hdmi_qp_modb(priv, I2CM_FM_READ, I2CM_WR_MASK,
			     I2CM_INTERFACE_CONTROL0);

		for (i = 0; i < 20; i++) {
			udelay(1000);
			intr = readl(priv->regs + MAINUNIT_1_INT_STATUS);
			intr &= I2CM_OP_DONE_IRQ | I2CM_NACK_RCVD_IRQ;
			if (intr)
				break;
		}

		hdmi_qp_modb(priv, 0, I2CM_WR_MASK,
			     I2CM_INTERFACE_CONTROL0);
		if (intr & I2CM_OP_DONE_IRQ)
			break;

		writel(1, priv->regs + I2CM_CONTROL0);
	}

	if (!(intr & I2CM_OP_DONE_IRQ)) {
		return -EIO;
	}

	writel(intr, priv->regs + MAINUNIT_1_INT_CLEAR);
	for (i = 0; i < 16; i++) {
		reg = I2CM_INTERFACE_RDDATA_0_3 + (i / 4) * 4;
		val = readl(priv->regs + reg);
		buf[i] = val >> ((i % 4) * 8);
	}

	return 0;
}

static int rk3588_hdmi_qp_ddc_block(struct rk3588_hdmi_qp *priv,
				    u8 offset, u8 *buf)
{
	int i, ret;

	for (i = 0; i < HDMI_EDID_BLOCK_SIZE; i += 16) {
		ret = rk3588_hdmi_qp_ddc_chunk(priv, offset + i, buf + i);
		if (ret)
			return ret;
	}

	return 0;
}

static int rk3588_hdmi_qp_read_edid(struct udevice *dev, u8 *buf,
				    int buf_size)
{
	struct rk3588_hdmi_qp *priv = dev_get_priv(dev);
	int blocks = 1;
	int i, ret;

	if (buf_size < HDMI_EDID_BLOCK_SIZE)
		return -ENOSPC;
	ret = rk3588_hdmi_qp_wait_hpd(priv);
	if (ret)
		return ret;

	for (i = 0; i < 10; i++) {
		ret = rk3588_hdmi_qp_ddc_block(priv, 0, buf);
		if (!ret &&
		    !edid_check_info((struct edid1_info *)buf) &&
		    !edid_check_checksum(buf))
			break;
		ret = -EIO;
		mdelay(100);
	}
	if (ret) {
		printf("rk3588-hdmi-qp: EDID base block failed: %d\n", ret);
		return ret;
	}
	if (i)
		printf("rk3588-hdmi-qp: EDID ready after %d ms\n", i * 100);

	if (buf_size >= 2 * HDMI_EDID_BLOCK_SIZE && buf[126]) {
		ret = rk3588_hdmi_qp_ddc_block(priv, 128,
					       buf + HDMI_EDID_BLOCK_SIZE);
		if (!ret &&
		    !edid_check_checksum(buf + HDMI_EDID_BLOCK_SIZE))
			blocks++;
	}

	return blocks * HDMI_EDID_BLOCK_SIZE;
}

static bool rk3588_hdmi_qp_mode_valid(struct udevice *dev,
				      const struct display_timing *timing)
{
	return timing->pixelclock.typ &&
	       timing->pixelclock.typ <= HDMI_QP_MAX_TMDS_CLOCK;
}

static void rk3588_hdmi_qp_set_avi(struct rk3588_hdmi_qp *priv,
				   const struct display_timing *timing)
{
	u32 avi1 = 0x0000006f;
	u32 avi2 = 0;

	if (!(timing->flags & DISPLAY_FLAGS_INTERLACED)) {
		if (timing->hactive.typ == 1920 &&
		    timing->vactive.typ == 1080 &&
		    timing->pixelclock.typ == 148500000) {
			avi1 = 0x08000255;
			avi2 = 0x00000010;
		} else if (timing->hactive.typ == 3840 &&
			   timing->vactive.typ == 2160 &&
			   timing->pixelclock.typ == 297000000) {
			avi1 = 0x04281231;
		} else if (timing->hactive.typ == 1920 &&
			   timing->vactive.typ == 1200 &&
			   timing->pixelclock.typ == 154000000) {
			avi1 = 0x08000265;
		}
	}

	writel(0x000d0200, priv->regs + PKT_AVI_CONTENTS0);
	writel(avi1, priv->regs + PKT_AVI_CONTENTS1);
	writel(avi2, priv->regs + PKT_AVI_CONTENTS1 + 4);
	writel(0, priv->regs + PKT_AVI_CONTENTS1 + 8);
	writel(0, priv->regs + PKT_AVI_CONTENTS1 + 12);
	hdmi_qp_modb(priv, 0, PKTSCHED_AVI_FIELDRATE,
		     PKTSCHED_PKT_CONFIG1);

	hdmi_qp_modb(priv, PKTSCHED_AVI_TX_EN, PKTSCHED_AVI_TX_EN,
		     PKTSCHED_PKT_EN);
}

static void rk3588_hdmi_qp_set_tmds(struct rk3588_hdmi_qp *priv,
				    const struct display_timing *timing)
{
	u32 mask;

	writel(HIWORD_UPDATE(0, RK3588_HDMI21_MASK),
	       priv->vo_grf + RK3588_GRF_VO1_CON4);

	mask = RK3588_COMPRESS_MODE_MASK | RK3588_COLOR_FORMAT_MASK |
	       RK3588_COLOR_DEPTH_MASK;
	writel(HIWORD_UPDATE(0, mask),
	       priv->vo_grf + RK3588_GRF_VO1_CON3);

	hdmi_qp_modb(priv, HDCP2_BYPASS, HDCP2_BYPASS,
		     HDCP2LOGIC_CONFIG0);
	hdmi_qp_modb(priv, KEEPOUT_REKEY_ALWAYS, KEEPOUT_REKEY_CFG,
		     FRAME_COMPOSER_CONFIG9);
	hdmi_qp_modb(priv, timing->hdmi_monitor ? 0 : OPMODE_DVI,
		     OPMODE_DVI | OPMODE_FRL |
		     OPMODE_FRL_4LANES, LINK_CONFIG0);
	writel(0, priv->regs + SCRAMB_CONFIG0);
	if (timing->hdmi_monitor) {
		rk3588_hdmi_qp_set_avi(priv, timing);
		hdmi_qp_modb(priv, PKTSCHED_GCP_TX_EN,
			     PKTSCHED_GCP_TX_EN, PKTSCHED_PKT_EN);
	} else {
		writel(2, priv->regs + PKTSCHED_PKT_CONTROL0);
		hdmi_qp_modb(priv, PKTSCHED_GCP_TX_EN,
			     PKTSCHED_AVI_TX_EN | PKTSCHED_GCP_TX_EN,
			     PKTSCHED_PKT_EN);
	}
	writel(BIT(21), priv->regs + VIDEO_INTERFACE_CONFIG0);

}

static int rk3588_hdmi_qp_enable(struct udevice *dev, int panel_bpp,
				 const struct display_timing *timing)
{
	struct rk3588_hdmi_qp *priv = dev_get_priv(dev);
	struct rk3588_hdptx_config config = {
		.pixel_clock = timing->pixelclock.typ,
		.bits_per_color = 8,
	};
	int ret;

	if (!rk3588_hdmi_qp_mode_valid(dev, timing))
		return -EINVAL;

	ret = generic_phy_configure(&priv->phy, &config);
	if (ret)
		return ret;

	rk3588_hdmi_qp_set_tmds(priv, timing);

	ret = generic_phy_power_on(&priv->phy);
	if (ret)
		return ret;

	mdelay(50);
	writel(2, priv->regs + PKTSCHED_PKT_CONTROL0);
	return 0;
}

static int rk3588_hdmi_qp_probe(struct udevice *dev)
{
	struct rk3588_hdmi_qp *priv = dev_get_priv(dev);
	struct regmap *map;
	phys_addr_t addr = dev_read_addr(dev);
	int ret;

	if (addr != RK3588_HDMI0_BASE)
		return -EOPNOTSUPP;

	priv->regs = dev_read_addr_ptr(dev);
	if (!priv->regs)
		return -ENOENT;

	map = syscon_regmap_lookup_by_phandle(dev, "rockchip,grf");
	if (IS_ERR(map))
		return PTR_ERR(map);
	priv->grf = regmap_get_range(map, 0);

	map = syscon_regmap_lookup_by_phandle(dev, "rockchip,vo-grf");
	if (IS_ERR(map))
		return PTR_ERR(map);
	priv->vo_grf = regmap_get_range(map, 0);

	ret = clk_get_bulk(dev, &priv->clocks);
	if (ret)
		return ret;
	ret = clk_enable_bulk(&priv->clocks);
	if (ret)
		return ret;
	ret = reset_get_bulk(dev, &priv->resets);
	if (ret)
		return ret;
	ret = reset_deassert_bulk(&priv->resets);
	if (ret)
		return ret;

	ret = generic_phy_get_by_index(dev, 0, &priv->phy);
	if (ret)
		return ret;

	rk3588_hdmi_qp_io_path_init(priv);
	writel(0, priv->regs + MAINUNIT_0_INT_MASK_N);
	writel(0, priv->regs + MAINUNIT_1_INT_MASK_N);
	writel(428571429, priv->regs + TIMER_BASE_CONFIG0);
	rk3588_hdmi_qp_ddc_init(priv);

	return 0;
}

static const struct dm_display_ops rk3588_hdmi_qp_ops = {
	.read_edid = rk3588_hdmi_qp_read_edid,
	.enable = rk3588_hdmi_qp_enable,
	.mode_valid = rk3588_hdmi_qp_mode_valid,
};

static const struct udevice_id rk3588_hdmi_qp_ids[] = {
	{ .compatible = "rockchip,rk3588-dw-hdmi-qp" },
	{ }
};

U_BOOT_DRIVER(rk3588_hdmi_qp) = {
	.name = "rk3588_hdmi_qp",
	.id = UCLASS_DISPLAY,
	.of_match = rk3588_hdmi_qp_ids,
	.probe = rk3588_hdmi_qp_probe,
	.ops = &rk3588_hdmi_qp_ops,
	.priv_auto = sizeof(struct rk3588_hdmi_qp),
};
