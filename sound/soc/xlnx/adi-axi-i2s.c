/*
 *  Copyright (C) 2012, Analog Devices Inc.
 *	Author: Lars-Peter Clausen <lars@metafoo.de>
 *
 *  This program is free software; you can redistribute it and/or modify it
 *  under  the terms of the GNU General  Public License as published by the
 *  Free Software Foundation;  either version 2 of the License, or (at your
 *  option) any later version.
 *
 *  You should have received a copy of the GNU General Public License along
 *  with this program; if not, write to the Free Software Foundation, Inc.,
 *  675 Mass Ave, Cambridge, MA 02139, USA.
 *
 */

#include <linux/clk.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/slab.h>

#include <sound/core.h>
#include <sound/pcm.h>
#include <sound/pcm_params.h>
#include <sound/soc.h>

struct axi_i2s {
	struct regmap *regmap;
	struct clk *clk;
};

#define AXI_I2S_REG_RESET	0x00
#define AXI_I2S_REG_CTRL	0x04
#define AXI_I2S_REG_CLK_CTRL	0x08
#define AXI_I2S_REG_STATUS	0x10
#define AXI_I2S_REG_PERIOD_SIZE	0x18

#define AXI_I2S_RESET_GLOBAL	BIT(0)
#define AXI_I2S_RESET_TX_FIFO	BIT(1)
#define AXI_I2S_RESET_RX_FIFO	BIT(2)

#define AXI_I2S_CTRL_TX_EN	BIT(0)
#define AXI_I2S_CTRL_RX_EN	BIT(1)

static int axi_i2s_trigger(struct snd_pcm_substream *substream, int cmd,
	struct snd_soc_dai *dai)
{
	struct axi_i2s *i2s = snd_soc_dai_get_drvdata(dai);
	unsigned int mask, val;

	if (substream->stream == SNDRV_PCM_STREAM_CAPTURE)
		mask = AXI_I2S_CTRL_RX_EN;
	else
		mask = AXI_I2S_CTRL_TX_EN;

	switch (cmd) {
	case SNDRV_PCM_TRIGGER_START:
	case SNDRV_PCM_TRIGGER_RESUME:
	case SNDRV_PCM_TRIGGER_PAUSE_RELEASE:
		val = mask;
		break;
	case SNDRV_PCM_TRIGGER_STOP:
	case SNDRV_PCM_TRIGGER_SUSPEND:
	case SNDRV_PCM_TRIGGER_PAUSE_PUSH:
		val = 0;
		break;
	default:
		return -EINVAL;
	}

	regmap_update_bits(i2s->regmap, AXI_I2S_REG_CTRL, mask, val);

	return 0;
}

static int axi_i2s_hw_params(struct snd_pcm_substream *substream,
	struct snd_pcm_hw_params *params, struct snd_soc_dai *dai)
{
	struct axi_i2s *i2s = snd_soc_dai_get_drvdata(dai);
	unsigned int bclk_div, frame_size;

	frame_size = snd_soc_params_to_frame_size(params) / 2;
	bclk_div = DIV_ROUND_UP(clk_get_rate(i2s->clk),
				snd_soc_params_to_bclk(params)) / 2 - 1;
	regmap_write(i2s->regmap, AXI_I2S_REG_CLK_CTRL, (frame_size << 16) | bclk_div);

	if (substream->stream == SNDRV_PCM_STREAM_CAPTURE) {
	    unsigned int period_size = params_period_bytes(params) / 4 - 1;
	    regmap_write(i2s->regmap, AXI_I2S_REG_PERIOD_SIZE, period_size);
	}

	return 0;
}

static int axi_i2s_startup(struct snd_pcm_substream *substream,
	struct snd_soc_dai *dai)
{
	struct axi_i2s *i2s = snd_soc_dai_get_drvdata(dai);
	uint32_t mask;

	if (substream->stream == SNDRV_PCM_STREAM_CAPTURE)
		mask = AXI_I2S_RESET_RX_FIFO;
	else
		mask = AXI_I2S_RESET_TX_FIFO;

	regmap_write(i2s->regmap, AXI_I2S_REG_RESET, mask);

	return 0;
}

static const struct snd_soc_dai_ops axi_i2s_dai_ops = {
	.startup = axi_i2s_startup,
	.trigger = axi_i2s_trigger,
	.hw_params = axi_i2s_hw_params,
};

static struct snd_soc_dai_driver axi_i2s_dai = {
	.playback = {
		.channels_min = 2,
		.channels_max = 8,
		.rates = SNDRV_PCM_RATE_48000,
		.formats = SNDRV_PCM_FMTBIT_S32_LE,
	},
	.capture = {
		.channels_min = 2,
		.channels_max = 8,
		.rates = SNDRV_PCM_RATE_48000,
		.formats = SNDRV_PCM_FMTBIT_S32_LE,
	},
	.ops = &axi_i2s_dai_ops,
	.symmetric_rates = 1,
};

static const struct regmap_config axi_i2s_regmap_config = {
	.reg_bits = 32,
	.reg_stride = 4,
	.val_bits = 32,
	.max_register = AXI_I2S_REG_PERIOD_SIZE,
};

static int axi_i2s_probe(struct platform_device *pdev)
{
	struct resource *res;
	struct axi_i2s *i2s;
	void __iomem *base;
	int ret;

	i2s = devm_kzalloc(&pdev->dev, sizeof(*i2s), GFP_KERNEL);
	if (!i2s)
		return -ENOMEM;

	platform_set_drvdata(pdev, i2s);

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	base = devm_request_and_ioremap(&pdev->dev, res);
	if (!base)
		return -EBUSY;

	i2s->regmap = devm_regmap_init_mmio(&pdev->dev, base,
					    &axi_i2s_regmap_config);
	if (IS_ERR(i2s->regmap))
		return PTR_ERR(i2s->regmap);

	i2s->clk = devm_clk_get(&pdev->dev, NULL);
	if (IS_ERR(i2s->clk))
		return PTR_ERR(i2s->clk);

	ret = snd_soc_register_dai(&pdev->dev, &axi_i2s_dai);
	if (ret) {
		dev_err(&pdev->dev, "Failed to register DAI\n");
		return ret;
	}

	regmap_write(i2s->regmap, AXI_I2S_REG_RESET, AXI_I2S_RESET_GLOBAL);

	return 0;
}

static int axi_i2s_dev_remove(struct platform_device *pdev)
{
	snd_soc_unregister_dai(&pdev->dev);

	return 0;
}

static const struct of_device_id axi_i2s_of_match[] = {
	{ .compatible = "adi,axi-i2s-1.00.a", },
	{},
};
MODULE_DEVICE_TABLE(of, axi_i2s_of_match);

static struct platform_driver axi_i2s_driver = {
	.driver = {
		.name = "axi-i2s",
		.owner = THIS_MODULE,
		.of_match_table = axi_i2s_of_match,
	},
	.probe = axi_i2s_probe,
	.remove = axi_i2s_dev_remove,
};
module_platform_driver(axi_i2s_driver);

MODULE_AUTHOR("Lars-Peter Clausen <lars@metafoo.de>");
MODULE_DESCRIPTION("AXI I2S driver");
MODULE_LICENSE("GPL");
