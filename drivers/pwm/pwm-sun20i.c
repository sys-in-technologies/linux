// SPDX-License-Identifier: GPL-2.0-only
/*
 * Allwinner sun20i (T113-S3/D1) PWM driver
 * Based on T113-S3 User Manual v1.1
 */

#include <linux/bitops.h>
#include <linux/clk.h>
#include <linux/err.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/pwm.h>
#include <linux/reset.h>

#define PWM_PCCR(ch)		(0x20 + ((ch) >> 1) * 0x4)
#define PWM_PCGR		0x40
#define PWM_PER			0x80
#define PWM_PCR(ch)		(0x100 + (ch) * 0x20)
#define PWM_PPR(ch)		(0x104 + (ch) * 0x20)

#define PCR_PRESCAL_K_MSK	GENMASK(7, 0)
#define PCR_ACT_STA		BIT(8)
#define PCR_MODE_PULSE		BIT(9)

#define PPR_ENTIRE_CYCLE_MSK	GENMASK(31, 16)
#define PPR_ACT_CYCLE_MSK	GENMASK(15, 0)

struct sun20i_pwm_chip {
	struct clk *bus_clk;
	struct clk *clk;
	struct reset_control *rst;
	void __iomem *base;
};

static inline struct sun20i_pwm_chip *to_sun20i_pwm_chip(struct pwm_chip *chip)
{
	return pwmchip_get_drvdata(chip);
}

static int sun20i_pwm_apply(struct pwm_chip *chip, struct pwm_device *pwm,
			    const struct pwm_state *state)
{
	struct sun20i_pwm_chip *sun20ichip = to_sun20i_pwm_chip(chip);
	unsigned int ch = pwm->hwpwm;
	u64 rate, cycle, duty;
	u32 val;
	int ret;

	if (!state->enabled) {
		val = readl(sun20ichip->base + PWM_PER);
		writel(val & ~BIT(ch), sun20ichip->base + PWM_PER);
		val = readl(sun20ichip->base + PWM_PCGR);
		writel(val & ~BIT(ch), sun20ichip->base + PWM_PCGR);
		clk_disable_unprepare(sun20ichip->clk);
		return 0;
	}

	ret = clk_prepare_enable(sun20ichip->clk);
	if (ret)
		return ret;

	rate = clk_get_rate(sun20ichip->clk);
	cycle = mul_u64_u32_div(state->period, (u32)rate, NSEC_PER_SEC);

	u32 k = 0;
	while (cycle > 65536 && k < 255) {
		k++;
		cycle = mul_u64_u32_div(state->period, (u32)rate / (k + 1), NSEC_PER_SEC);
	}

	if (cycle > 65536) cycle = 65536;
	if (cycle == 0) return -EINVAL;

	duty = mul_u64_u32_div(state->duty_cycle, (u32)rate / (k + 1), NSEC_PER_SEC);
	if (duty > cycle) duty = cycle;

	val = readl(sun20ichip->base + PWM_PCGR);
	writel(val | BIT(ch), sun20ichip->base + PWM_PCGR);

	writel(0, sun20ichip->base + PWM_PCCR(ch));

	val = k & PCR_PRESCAL_K_MSK;
	if (state->polarity == PWM_POLARITY_NORMAL)
		val |= PCR_ACT_STA;
	else
		val &= ~PCR_ACT_STA;
	writel(val, sun20ichip->base + PWM_PCR(ch));

	val = (((u32)cycle - 1) << 16) | (u32)duty;
	writel(val, sun20ichip->base + PWM_PPR(ch));

	val = readl(sun20ichip->base + PWM_PER);
	writel(val | BIT(ch), sun20ichip->base + PWM_PER);

	return 0;
}

static int sun20i_pwm_get_state(struct pwm_chip *chip, struct pwm_device *pwm,
				struct pwm_state *state)
{
	struct sun20i_pwm_chip *sun20ichip = to_sun20i_pwm_chip(chip);
	unsigned int ch = pwm->hwpwm;
	u64 rate, cycle, duty;
	u32 val, pcr;

	val = readl(sun20ichip->base + PWM_PER);
	state->enabled = !!(val & BIT(ch));

	pcr = readl(sun20ichip->base + PWM_PCR(ch));
	state->polarity = (pcr & PCR_ACT_STA) ? PWM_POLARITY_NORMAL : PWM_POLARITY_INVERSED;

	rate = clk_get_rate(sun20ichip->clk);
	if (!rate) return -EINVAL;

	u32 k = (pcr & PCR_PRESCAL_K_MSK) + 1;
	val = readl(sun20ichip->base + PWM_PPR(ch));
	cycle = (val >> 16) + 1;
	duty = val & 0xffff;

	state->period = DIV_ROUND_UP_ULL(cycle * k * NSEC_PER_SEC, rate);
	state->duty_cycle = DIV_ROUND_UP_ULL(duty * k * NSEC_PER_SEC, rate);

	return 0;
}

static const struct pwm_ops sun20i_pwm_ops = {
	.apply = sun20i_pwm_apply,
	.get_state = sun20i_pwm_get_state,
};

static int sun20i_pwm_probe(struct platform_device *pdev)
{
	struct sun20i_pwm_chip *sun20ichip;
	struct pwm_chip *chip;
	int ret;

	chip = devm_pwmchip_alloc(&pdev->dev, 8, sizeof(*sun20ichip));
	if (IS_ERR(chip)) return PTR_ERR(chip);
	sun20ichip = pwmchip_get_drvdata(chip);
	sun20ichip->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(sun20ichip->base)) return PTR_ERR(sun20ichip->base);

	sun20ichip->clk = devm_clk_get(&pdev->dev, "mod");
	sun20ichip->bus_clk = devm_clk_get(&pdev->dev, "bus");
	sun20ichip->rst = devm_reset_control_get_exclusive(&pdev->dev, NULL);
	if (IS_ERR(sun20ichip->clk) || IS_ERR(sun20ichip->bus_clk) || IS_ERR(sun20ichip->rst))
		return -EINVAL;

	ret = reset_control_deassert(sun20ichip->rst);
	if (ret) return ret;

	ret = clk_prepare_enable(sun20ichip->bus_clk);
	if (ret) goto err_reset;

	chip->ops = &sun20i_pwm_ops;
	ret = pwmchip_add(chip);
	if (ret) goto err_clk;

	platform_set_drvdata(pdev, chip);
	return 0;

err_clk: clk_disable_unprepare(sun20ichip->bus_clk);
err_reset: reset_control_assert(sun20ichip->rst);
	return ret;
}

static void sun20i_pwm_remove(struct platform_device *pdev)
{
	struct pwm_chip *chip = platform_get_drvdata(pdev);
	struct sun20i_pwm_chip *sun20ichip = pwmchip_get_drvdata(chip);
	pwmchip_remove(chip);
	clk_disable_unprepare(sun20ichip->bus_clk);
	reset_control_assert(sun20ichip->rst);
}

static const struct of_device_id sun20i_pwm_dt_ids[] = {
	{ .compatible = "allwinner,sun20i-d1-pwm" },
	{ }
};
MODULE_DEVICE_TABLE(of, sun20i_pwm_dt_ids);

static struct platform_driver sun20i_pwm_driver = {
	.driver = { .name = "sun20i-pwm", .of_match_table = sun20i_pwm_dt_ids },
	.probe = sun20i_pwm_probe,
	.remove_new = sun20i_pwm_remove,
};
module_platform_driver(sun20i_pwm_driver);
MODULE_LICENSE("GPL");
