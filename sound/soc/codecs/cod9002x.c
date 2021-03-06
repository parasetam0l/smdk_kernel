/*
 * Copyright (c) 2014 Samsung Electronics Co. Ltd.
 *
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/delay.h>
#include <linux/pm.h>
#include <linux/pm_runtime.h>
#include <linux/regmap.h>
#include <linux/slab.h>
#include <sound/core.h>
#include <sound/pcm.h>
#include <sound/pcm_params.h>
#include <sound/soc.h>
#include <sound/jack.h>
#include <sound/initval.h>
#include <sound/tlv.h>
#include <sound/exynos_regmap_fw.h>
#include <linux/i2c.h>
#include <linux/regulator/consumer.h>
#include <linux/gpio.h>
#include <linux/of.h>
#include <linux/of_gpio.h>
#include <linux/switch.h>
#include <linux/input.h>
#include <linux/completion.h>

#include <sound/exynos-audmixer.h>
#include <sound/cod9002x.h>
#include "cod9002x.h"

#define COD9002X_FIRMWARE_NAME	"cod9002x-s2803x-aud-fw.bin"

#define COD9002X_SAMPLE_RATE_48KHZ	48000
#define COD9002X_SAMPLE_RATE_192KHZ	192000

#define COD9002X_RESTORE_OTP_COUNT	5
#define COD9002X_RESTORE_REG_COUNT	16
#define COD9002X_OTP_R_OFFSET		0x0

#define COD9002X_MAX_IRQ_CHK_BITS	7
#define COD9002X_START_IRQ_CHK_BIT	2
#define COD9002X_MJ_DET_INVALID		(-1)

#ifdef CONFIG_SND_SOC_SAMSUNG_VERBOSE_DEBUG
#ifdef dev_dbg
#undef dev_dbg
#endif
#define dev_dbg dev_err
#endif

/* Forward Declarations */
static void cod9002x_save_otp_registers(struct snd_soc_codec *codec);
static void cod9002x_restore_otp_registers(struct snd_soc_codec *codec);
static void cod9002x_reset_io_selector_bits(struct snd_soc_codec *codec);
static void cod9002x_configure_mic_bias(struct snd_soc_codec *codec);
static int cod9002x_disable(struct device *dev);
static int cod9002x_enable(struct device *dev);

static inline void cod9002x_usleep(unsigned int u_sec)
{
	usleep_range(u_sec, u_sec + 10);
}

/**
 * Helper functions to read ADC value for button detection
 */

#define COD9002X_ADC_SAMPLE_SIZE	5

static void cod9002x_adc_start(struct cod9002x_priv *cod9002x)
{
	cod9002x->jack_adc = iio_channel_get_all(cod9002x->dev);
}

static void cod9002x_adc_stop(struct cod9002x_priv *cod9002x)
{
	iio_channel_release(cod9002x->jack_adc);
}

static int cod9002x_adc_get_value(struct cod9002x_priv *cod9002x)
{
	int adc_data = -1;
	int adc_max = 0;
	int adc_min = 0xFFFF;
	int adc_total = 0;
	int adc_retry_cnt = 0;
	int i;
	struct iio_channel *jack_adc = cod9002x->jack_adc;

	for (i = 0; i < COD9002X_ADC_SAMPLE_SIZE; i++) {
		iio_read_channel_raw(&jack_adc[0], &adc_data);
		/* if adc_data is negative, ignore */
		while (adc_data < 0) {
			adc_retry_cnt++;
			if (adc_retry_cnt > 10)
				return adc_data;
			iio_read_channel_raw(&jack_adc[0], &adc_data);
		}

		/* Update min/max values */
		if (adc_data > adc_max)
			adc_max = adc_data;
		if (adc_data < adc_min)
			adc_min = adc_data;

		adc_total += adc_data;
	}

	return (adc_total - adc_max - adc_min) / (COD9002X_ADC_SAMPLE_SIZE - 2);
}

/**
 * Return value:
 * true: if the register value cannot be cached, hence we have to read from the
 * hardware directly
 * false: if the register value can be read from cache
 */
static bool cod9002x_volatile_register(struct device *dev, unsigned int reg)
{
	/**
	 * For all the registers for which we want to restore the value during
	 * regcache_sync operation, we need to return true here. For registers
	 * whose value need not be cached and restored should return false here.
	 *
	 * For the time being, let us cache the value of all registers other
	 * than the IRQ pending and IRQ status registers.
	 */
	switch (reg) {
	case COD9002X_01_IRQ1PEND ... COD9002X_05_IRQ5PEND:
	case COD9002X_0B_STATUS1 ... COD9002X_0D_STATUS3:
	case COD9002X_61_RESERVED ... COD9002X_62_IRQ_R:
	case COD9002X_80_DET_PDB ... 0x97:
		return true;
	default:
		return false;
	}
}

/**
 * Return value:
 * true: if the register value can be read
 * flase: if the register cannot be read
 */
static bool cod9002x_readable_register(struct device *dev, unsigned int reg)
{
	switch (reg) {
	case COD9002X_01_IRQ1PEND ... COD9002X_0D_STATUS3:
	case COD9002X_10_PD_REF ... COD9002X_1C_SV_DA:
	case COD9002X_20_VOL_AD1 ... COD9002X_26_DSM_ADS:
	case COD9002X_30_VOL_HPL ... COD9002X_39_VOL_SPK:
	case COD9002X_40_DIGITAL_POWER ... COD9002X_44_ADC_R_VOL:
	case COD9002X_50_DAC1 ... COD9002X_5F_SPKLIMIT3:
	case COD9002X_60_OFFSET1 ... COD9002X_62_IRQ_R:
	case COD9002X_70_CLK1_AD ... COD9002X_7A_SL_DA2:
	case COD9002X_80_DET_PDB ... 0x97:
	case COD9002X_D0_CTRL_IREF1 ... COD9002X_DE_CTRL_SPKS2:
		return true;
	default:
		return false;
	}
}

static bool cod9002x_writeable_register(struct device *dev, unsigned int reg)
{
	switch (reg) {
	/* Reg-0x09 to Reg-0x0B are read-only status registers */
	case COD9002X_01_IRQ1PEND ... COD9002X_0A_IRQ5M:
	case COD9002X_10_PD_REF ... COD9002X_1C_SV_DA:
	case COD9002X_20_VOL_AD1 ... COD9002X_26_DSM_ADS:
	case COD9002X_30_VOL_HPL ... COD9002X_39_VOL_SPK:
	case COD9002X_40_DIGITAL_POWER ... COD9002X_44_ADC_R_VOL:
	case COD9002X_50_DAC1 ... COD9002X_5F_SPKLIMIT3:
	/* Reg-0x61 is reserved, Reg-0x62 is read-only */
	case COD9002X_60_OFFSET1:
	case COD9002X_70_CLK1_AD ... COD9002X_7A_SL_DA2:
	case COD9002X_80_DET_PDB ... 0x97:
	case COD9002X_D0_CTRL_IREF1 ... COD9002X_DE_CTRL_SPKS2:
		return true;
	default:
		return false;
	}
}

const struct regmap_config cod9002x_regmap = {
	.reg_bits = 8,
	.val_bits = 8,

	.max_register = COD9002X_MAX_REGISTER,
	.readable_reg = cod9002x_readable_register,
	.writeable_reg = cod9002x_writeable_register,
	.volatile_reg = cod9002x_volatile_register,

	.use_single_rw = true,
	.cache_type = REGCACHE_RBTREE,
};

/**
 * TLV_DB_SCALE_ITEM
 *
 * (TLV: Threshold Limit Value)
 *
 * For various properties, the dB values don't change linearly with respect to
 * the digital value of related bit-field. At most, they are quasi-linear,
 * that means they are linear for various ranges of digital values. Following
 * table define such ranges of various properties.
 *
 * TLV_DB_RANGE_HEAD(num)
 * num defines the number of linear ranges of dB values.
 *
 * s0, e0, TLV_DB_SCALE_ITEM(min, step, mute),
 * s0: digital start value of this range (inclusive)
 * e0: digital end valeu of this range (inclusive)
 * min: dB value corresponding to s0
 * step: the delta of dB value in this range
 * mute: ?
 *
 * Example:
 *	TLV_DB_RANGE_HEAD(3),
 *	0, 1, TLV_DB_SCALE_ITEM(-2000, 2000, 0),
 *	2, 4, TLV_DB_SCALE_ITEM(1000, 1000, 0),
 *	5, 6, TLV_DB_SCALE_ITEM(3800, 8000, 0),
 *
 * The above code has 3 linear ranges with following digital-dB mapping.
 * (0...6) -> (-2000dB, 0dB, 1000dB, 2000dB, 3000dB, 3800dB, 4600dB),
 *
 * DECLARE_TLV_DB_SCALE
 *
 * This macro is used in case where there is a linear mapping between
 * the digital value and dB value.
 *
 * DECLARE_TLV_DB_SCALE(name, min, step, mute)
 *
 * name: name of this dB scale
 * min: minimum dB value corresponding to digital 0
 * step: the delta of dB value
 * mute: ?
 *
 * NOTE: The information is mostly for user-space consumption, to be viewed
 * alongwith amixer.
 */

/**
 * cod9002x_ctvol_bst_tlv
 *
 * Range: 0dB to +20dB, step 2dB
 *
 * CTVOL_BST1, reg(0x20), shift(4), width(4), invert(0), max(20)
 * CTVOL_BST2, reg(0x21), shift(4), width(4), invert(0), max(20)
 * CTVOL_BST3, reg(0x22), shift(4), width(4), invert(0), max(20)
 */
static const DECLARE_TLV_DB_SCALE(cod9002x_ctvol_bst_tlv, 0, 200, 0);

/**
 * cod9002x_ctvol_bst_pga_tlv
 *
 * Range: 0dB to +30dB, step 6dB
 *
 * CTVOL_BST_PGA1, reg(0x20), shift(0), width(3), invert(0), max(30)
 * CTVOL_BST_PGA2, reg(0x21), shift(0), width(3), invert(0), max(30)
 * CTVOL_BST_PGA3, reg(0x22), shift(0), width(3), invert(0), max(30)
 */
static const DECLARE_TLV_DB_SCALE(cod9002x_ctvol_bst_pga_tlv, 0, 600, 0);

/**
 * cod9002x_ctvol_hp_tlv
 *
 * Range: -57dB to +6dB, step 1dB
 *
 * CTVOL_HPL, reg(0x30), shift(0), width(6), invert(1), max(63)
 * CTVOL_HPR, reg(0x31), shift(0), width(6), invert(1), max(63)
 */
static const DECLARE_TLV_DB_SCALE(cod9002x_ctvol_hp_tlv, -5700, 100, 0);

/**
 * cod3019_ctvol_ep_tlv
 *
 * Range: -6dB to +12dB, step 1dB
 *
 * CTVOL_EP, reg(0x32), shift(0), width(5), invert(0), max(18)
 */
static const DECLARE_TLV_DB_SCALE(cod9002x_ctvol_ep_tlv, -600, 100, 0);

/**
 * cod9002x_ctvol_spk_pga_tlv
 *
 * Range: -18dB to +6dB, step 1dB
 *
 * CTVOL_SPK_PGA, reg(0x39), shift(0), width(5), invert(1), max(24)
 */
static const DECLARE_TLV_DB_SCALE(cod9002x_ctvol_spk_pga_tlv, -1800, 100, 0);

/**
 * cod9002x_dvol_adc_tlv
 *
 * Map as per data-sheet:
 * (0x00 to 0x86) -> (+12dB to -55dB, step 0.5dB)
 * (0x87 to 0x91) -> (-56dB to -66dB, step 1dB)
 * (0x92 to 0x94) -> (-68dB to -72dB, step 2dB)
 * (0x95 to 0x96) -> (-78dB to -84dB, step 6dB)
 *
 * When the map is in descending order, we need to set the invert bit
 * and arrange the map in ascending order. The offsets are calculated as
 * (max - offset).
 *
 * offset_in_table = max - offset_actual;
 *
 * DVOL_ADL, reg(0x43), shift(0), width(8), invert(1), max(0x96)
 * DVOL_ADR, reg(0x44), shift(0), width(8), invert(1), max(0x96)
 * DVOL_DAL, reg(0x51), shift(0), width(8), invert(1), max(0x96)
 * DVOL_DAR, reg(0x52), shift(0), width(8), invert(1), max(0x96)
 */
static const unsigned int cod9002x_dvol_tlv[] = {
	TLV_DB_RANGE_HEAD(4),
	0x00, 0x01, TLV_DB_SCALE_ITEM(-8400, 600, 0),
	0x02, 0x04, TLV_DB_SCALE_ITEM(-7200, 200, 0),
	0x05, 0x09, TLV_DB_SCALE_ITEM(-6600, 100, 0),
	0x10, 0x96, TLV_DB_SCALE_ITEM(-5500, 50, 0),
};

/**
 * cod9002x_dnc_min_gain_tlv
 *
 * Range: -6dB to 0dB, step 1dB
 *
 * DNC_MINGAIN , reg(0x55), shift(5), width(3)
 */
static const unsigned int cod9002x_dnc_min_gain_tlv[] = {
	TLV_DB_RANGE_HEAD(1),
	0x00, 0x06, TLV_DB_SCALE_ITEM(-600, 0, 0),
};

/**
 * cod9002x_dnc_max_gain_tlv
 *
 * Range: 0dB to 24dB, step 1dB
 *
 * DNC_MAXGAIN , reg(0x55), shift(0), width(5)
 */
static const unsigned int cod9002x_dnc_max_gain_tlv[] = {
	TLV_DB_RANGE_HEAD(1),
	0x06, 0x1e, TLV_DB_SCALE_ITEM(0, 2400, 0),
};

/**
 * cod9002x_dnc_lvl_tlv
 *
 * Range: -10.5dB to 0dB, step 1.5dB
 *
 * DNCLVL_R/L, reg(0x55), shift(0/4), width(3), invert(0), max(7)
 */
static const DECLARE_TLV_DB_SCALE(cod9002x_dnc_lvl_tlv, -1050, 0, 0);

/**
 * mono_mix_mode
 *
 * Selecting the Mode of Mono Mixer (inside DAC block)
 */
static const char *cod9002x_mono_mix_mode_text[] = {
	"Disable", "R", "L", "LR-Invert",
	"(L+R)/2", "L+R"
};

static const struct soc_enum cod9002x_mono_mix_mode_enum =
	SOC_ENUM_SINGLE(COD9002X_50_DAC1, DAC1_MONOMIX_SHIFT,
			ARRAY_SIZE(cod9002x_mono_mix_mode_text),
			cod9002x_mono_mix_mode_text);

/**
 * chargepump_mode
 *
 * Selecting the chargepump mode
 */
static const char *cod9002x_chargepump_mode_text[] = {
	"VDD", "HALF-VDD", "CLASS-G-D", "CLASS-G-A"
};

static const struct soc_enum cod9002x_chargepump_mode_enum =
	SOC_ENUM_SINGLE(COD9002X_33_CTRL_EP, CTMV_CP_MODE_SHIFT,
			ARRAY_SIZE(cod9002x_chargepump_mode_text),
			cod9002x_chargepump_mode_text);


/**
 * struct snd_kcontrol_new cod9002x_snd_control
 *
 * Every distinct bit-fields within the CODEC SFR range may be considered
 * as a control elements. Such control elements are defined here.
 *
 * Depending on the access mode of these registers, different macros are
 * used to define these control elements.
 *
 * SOC_ENUM: 1-to-1 mapping between bit-field value and provided text
 * SOC_SINGLE: Single register, value is a number
 * SOC_SINGLE_TLV: Single register, value corresponds to a TLV scale
 * SOC_SINGLE_TLV_EXT: Above + custom get/set operation for this value
 * SOC_SINGLE_RANGE_TLV: Register value is an offset from minimum value
 * SOC_DOUBLE: Two bit-fields are updated in a single register
 * SOC_DOUBLE_R: Two bit-fields in 2 different registers are updated
 */

/**
 * All the data goes into cod9002x_snd_controls.
 * All path inter-connections goes into cod9002x_dapm_routes
 */
static const struct snd_kcontrol_new cod9002x_snd_controls[] = {
	SOC_SINGLE_TLV("MIC1 Boost Volume", COD9002X_20_VOL_AD1,
			VOLAD1_CTVOL_BST1_SHIFT,
			(BIT(VOLAD1_CTVOL_BST1_WIDTH) - 1), 0,
			cod9002x_ctvol_bst_tlv),

	SOC_SINGLE_TLV("MIC1 Volume", COD9002X_20_VOL_AD1,
			VOLAD1_CTVOL_BST_PGA1_SHIFT,
			(BIT(VOLAD1_CTVOL_BST_PGA1_WIDTH) - 1), 0,
			cod9002x_ctvol_bst_pga_tlv),

	SOC_SINGLE_TLV("MIC2 Boost Volume", COD9002X_21_VOL_AD2,
			VOLAD2_CTVOL_BST2_SHIFT,
			(BIT(VOLAD2_CTVOL_BST2_WIDTH) - 1), 0,
			cod9002x_ctvol_bst_tlv),

	SOC_SINGLE_TLV("MIC2 Volume", COD9002X_21_VOL_AD2,
			VOLAD2_CTVOL_BST_PGA2_SHIFT,
			(BIT(VOLAD2_CTVOL_BST_PGA2_WIDTH) - 1), 0,
			cod9002x_ctvol_bst_pga_tlv),

	SOC_SINGLE_TLV("MIC3 Boost Volume", COD9002X_22_VOL_AD3,
			VOLAD3_CTVOL_BST3_SHIFT,
			(BIT(VOLAD3_CTVOL_BST3_WIDTH) - 1), 0,
			cod9002x_ctvol_bst_tlv),

	SOC_SINGLE_TLV("MIC3 Volume", COD9002X_22_VOL_AD3,
			VOLAD3_CTVOL_BST_PGA3_SHIFT,
			(BIT(VOLAD3_CTVOL_BST_PGA3_WIDTH) - 1), 0,
			cod9002x_ctvol_bst_pga_tlv),

	SOC_DOUBLE_R_TLV("Headphone Volume", COD9002X_30_VOL_HPL,
			COD9002X_31_VOL_HPR, VOLHP_CTVOL_HP_SHIFT,
			(BIT(VOLHP_CTVOL_HP_WIDTH) - 1), 1,
			cod9002x_ctvol_hp_tlv),

	SOC_SINGLE_TLV("Earphone Volume", COD9002X_32_VOL_EP,
			CTVOL_EP_SHIFT,
			(BIT(CTVOL_EP_WIDTH) - 1), 0,
			cod9002x_ctvol_ep_tlv),

	SOC_SINGLE_TLV("Speaker Volume", COD9002X_39_VOL_SPK,
			CTVOL_SPK_PGA_SHIFT,
			(BIT(CTVOL_SPK_PGA_WIDTH) - 1), 1,
			cod9002x_ctvol_spk_pga_tlv),

	SOC_SINGLE_TLV("ADC Left Gain", COD9002X_43_ADC_L_VOL,
			AD_DA_DVOL_SHIFT,
			AD_DA_DVOL_MAXNUM, 1, cod9002x_dvol_tlv),

	SOC_SINGLE_TLV("ADC Right Gain", COD9002X_44_ADC_R_VOL,
			AD_DA_DVOL_SHIFT,
			AD_DA_DVOL_MAXNUM, 1, cod9002x_dvol_tlv),

	SOC_DOUBLE_R_TLV("DAC Gain", COD9002X_51_DAC_L_VOL,
			COD9002X_52_DAC_R_VOL, AD_DA_DVOL_SHIFT,
			AD_DA_DVOL_MAXNUM, 1, cod9002x_dvol_tlv),

	SOC_SINGLE_TLV("DNC Min Gain", COD9002X_55_DNC2,
			DNC_MIN_GAIN_SHIFT,
			(BIT(DNC_MIN_GAIN_WIDTH) - 2), 0,
			cod9002x_dnc_min_gain_tlv),

	SOC_SINGLE_TLV("DNC Max Gain", COD9002X_55_DNC2,
			DNC_MAX_GAIN_SHIFT,
			(BIT(DNC_MAX_GAIN_WIDTH) - 2), 0,
			cod9002x_dnc_max_gain_tlv),

	SOC_DOUBLE_R_TLV("DNC Level", COD9002X_56_DNC3,
			DNC_LVL_L_SHIFT, DNC_LVL_R_SHIFT,
			(BIT(DNC_LVL_L_WIDTH) - 1), 0, cod9002x_dnc_lvl_tlv),

	SOC_ENUM("MonoMix Mode", cod9002x_mono_mix_mode_enum),

	SOC_ENUM("Chargepump Mode", cod9002x_chargepump_mode_enum),
};

static int dac_ev(struct snd_soc_dapm_widget *w, struct snd_kcontrol *kcontrol,
		int event)
{
	dev_dbg(w->codec->dev, "%s called\n", __func__);

	switch (event) {
	case SND_SOC_DAPM_PRE_PMU:
		/* Default value of DIGITAL BLOCK */
		snd_soc_write(w->codec, COD9002X_40_DIGITAL_POWER, 0xf9);

		/* DAC digital power On */
		snd_soc_update_bits(w->codec, COD9002X_40_DIGITAL_POWER,
				PDB_DACDIG_MASK | RSTB_OVFW_DA_MASK,
				PDB_DACDIG_MASK);

		snd_soc_update_bits(w->codec, COD9002X_40_DIGITAL_POWER,
				RSTB_DAT_DA_MASK, 0x0);

		snd_soc_update_bits(w->codec, COD9002X_40_DIGITAL_POWER,
				RSTB_DAT_DA_MASK, RSTB_DAT_DA_MASK);

		break;

	case SND_SOC_DAPM_PRE_PMD:
		/* Default value of DIGITAL BLOCK during power off */
		snd_soc_write(w->codec, COD9002X_40_DIGITAL_POWER, 0xe9);
		break;

	default:
		break;
	}

	return 0;
}

static void cod9002x_adc_digital_mute(struct snd_soc_codec *codec, bool on)
{
	if (on)
		snd_soc_update_bits(codec, COD9002X_42_ADC1,
				ADC1_MUTE_AD_EN_MASK, ADC1_MUTE_AD_EN_MASK);
	else
		snd_soc_update_bits(codec, COD9002X_42_ADC1,
				ADC1_MUTE_AD_EN_MASK, 0);
}

static int cod9002x_capture_init_manual_mode(struct snd_soc_codec *codec)
{
	dev_dbg(codec->dev, "%s called\n", __func__);

	snd_soc_update_bits(codec, COD9002X_10_PD_REF,
			PDB_VMID_MASK, PDB_VMID_MASK);

	snd_soc_update_bits(codec, COD9002X_18_CTRL_REF,
					CTMF_VMID_MASK,
					CTMF_VMID_5K_OM << CTMF_VMID_SHIFT);

	snd_soc_update_bits(codec, COD9002X_11_PD_AD1,
			EN_DSMR_PREQ_MASK | EN_DSML_PREQ_MASK,
			EN_DSMR_PREQ_MASK | EN_DSML_PREQ_MASK);

	snd_soc_update_bits(codec, COD9002X_18_CTRL_REF,
					CTMF_VMID_MASK,
					CTMF_VMID_50K_OM << CTMF_VMID_SHIFT);

	snd_soc_update_bits(codec, COD9002X_10_PD_REF,
			PDB_IGEN_MASK, PDB_IGEN_MASK);

	return 0;
}

static int cod9002x_capture_init(struct snd_soc_codec *codec)
{
	dev_dbg(codec->dev, "%s called\n", __func__);

	/* enable ADC digital mute before configuring ADC */
	cod9002x_adc_digital_mute(codec, true);

	/* Recording Digital  Power on */
	snd_soc_update_bits(codec, COD9002X_40_DIGITAL_POWER,
			PDB_ADCDIG_MASK | RSTB_OVFW_DA_MASK,
			PDB_ADCDIG_MASK);

	/* Recording Digital Reset on/off */
	snd_soc_update_bits(codec, COD9002X_40_DIGITAL_POWER,
			RSTB_DAT_AD_MASK, 0x0);
	snd_soc_update_bits(codec, COD9002X_40_DIGITAL_POWER,
			RSTB_DAT_AD_MASK, RSTB_DAT_AD_MASK);

	cod9002x_capture_init_manual_mode(codec);

	return 0;
}

static void cod9002x_capture_deinit_manual_mode(struct snd_soc_codec *codec)
{
        snd_soc_update_bits(codec, COD9002X_10_PD_REF,
                        PDB_IGEN_MASK, 0);

        snd_soc_update_bits(codec, COD9002X_10_PD_REF,
                        PDB_VMID_MASK, 0);
}

static int cod9002x_capture_deinit(struct snd_soc_codec *codec)
{
        int dac_on;

        dac_on = snd_soc_read(codec, COD9002X_40_DIGITAL_POWER);

        cod9002x_capture_deinit_manual_mode(codec);

	if (PDB_DACDIG_MASK & dac_on)
                snd_soc_update_bits(codec,
                                COD9002X_40_DIGITAL_POWER,
                                RSTB_DAT_AD_MASK, 0x0);
        else
                snd_soc_update_bits(codec,
                                COD9002X_40_DIGITAL_POWER,
                                RSTB_DAT_AD_MASK | RSTB_OVFW_DA_MASK,
                                RSTB_OVFW_DA_MASK);

	/* disable ADC digital mute after configuring ADC */
        cod9002x_adc_digital_mute(codec, false);

	return 0;
}

static int adc_ev(struct snd_soc_dapm_widget *w, struct snd_kcontrol *kcontrol,
		int event)
{
	int dac_on;

	dev_dbg(w->codec->dev, "%s called, event = %d\n", __func__, event);

	dac_on = snd_soc_read(w->codec, COD9002X_40_DIGITAL_POWER);
	switch (event) {
	case SND_SOC_DAPM_PRE_PMU:
		break;

	case SND_SOC_DAPM_POST_PMU:
		/* disable ADC digital mute after configuring ADC */
		cod9002x_adc_digital_mute(w->codec, false);
		break;

	case SND_SOC_DAPM_PRE_PMD:
		/* disable ADC digital mute before configuring ADC */
		cod9002x_adc_digital_mute(w->codec, true);
		break;

	default:
		break;
	}

	return 0;
}



static int cod9002_power_on_mic1(struct snd_soc_codec *codec)
{
	unsigned int mix_val;

	dev_dbg(codec->dev, "%s called\n", __func__);

	mix_val = snd_soc_read(codec, COD9002X_23_MIX_AD1);
	mix_val &= EN_MIX_MIC1L_MASK | EN_MIX_MIC1R_MASK;

	/* Select default if no paths have been selected */
	if (!mix_val)
		mix_val = EN_MIX_MIC1L_MASK | EN_MIX_MIC1R_MASK;

	/* mic bias1 on */
	snd_soc_update_bits(codec, COD9002X_10_PD_REF,
			PDB_MCB1_MASK | PDB_MCB_LDO_CODEC_MASK,
			PDB_MCB1_MASK | PDB_MCB_LDO_CODEC_MASK);

	/* Reset the mixer-switches before powering on */
	snd_soc_update_bits(codec, COD9002X_23_MIX_AD1,
			EN_MIX_MIC1L_MASK | EN_MIX_MIC1R_MASK, 0x0);

	cod9002x_usleep(100);

	snd_soc_update_bits(codec, COD9002X_10_PD_REF,
                         PDB_IGEN_AD_MASK, PDB_IGEN_AD_MASK);

	cod9002x_usleep(100);

	snd_soc_update_bits(codec, COD9002X_12_PD_AD2,
			PDB_MIC_PGA1_MASK | PDB_MIC_BST1_MASK ,
			PDB_MIC_PGA1_MASK | PDB_MIC_BST1_MASK );

	cod9002x_usleep(100);

	snd_soc_update_bits(codec, COD9002X_11_PD_AD1,
			PDB_MIXL_MASK | PDB_MIXR_MASK ,
			PDB_MIXL_MASK | PDB_MIXR_MASK );

	cod9002x_usleep(100);
	snd_soc_update_bits(codec, COD9002X_11_PD_AD1,
			PDB_DSML_MASK | PDB_DSMR_MASK ,
			PDB_DSML_MASK | PDB_DSMR_MASK );

	cod9002x_usleep(100);
	snd_soc_update_bits(codec, COD9002X_23_MIX_AD1,
			EN_MIX_MIC1L_MASK | EN_MIX_MIC1R_MASK, mix_val);

	snd_soc_update_bits(codec, COD9002X_11_PD_AD1,
			EN_DSMR_PREQ_MASK | EN_DSML_PREQ_MASK, 0);

	cod9002x_usleep(100);
	snd_soc_update_bits(codec, COD9002X_11_PD_AD1,
			RESETB_DSML_MASK | RESETB_DSMR_MASK,
			RESETB_DSML_MASK | RESETB_DSMR_MASK);

	cod9002x_usleep(100);

	return 0;
}

static int cod9002_power_off_mic1(struct snd_soc_codec *codec)
{
	dev_dbg(codec->dev, "%s called\n", __func__);

	snd_soc_update_bits(codec, COD9002X_11_PD_AD1,
			EN_DSMR_PREQ_MASK | EN_DSML_PREQ_MASK,
			EN_DSMR_PREQ_MASK | EN_DSML_PREQ_MASK);

	cod9002x_usleep(100);

	snd_soc_update_bits(codec, COD9002X_23_MIX_AD1,
			EN_MIX_MIC1L_MASK | EN_MIX_MIC1R_MASK, 0);

	cod9002x_usleep(100);

	snd_soc_update_bits(codec, COD9002X_11_PD_AD1,
			PDB_DSML_MASK | PDB_DSMR_MASK, 0);

	cod9002x_usleep(100);

	snd_soc_update_bits(codec, COD9002X_11_PD_AD1,
			PDB_MIXL_MASK | PDB_MIXR_MASK, 0);

	cod9002x_usleep(100);

	snd_soc_update_bits(codec, COD9002X_12_PD_AD2,
			PDB_MIC_PGA1_MASK | PDB_MIC_BST1_MASK, 0);

	cod9002x_usleep(100);

	snd_soc_update_bits(codec, COD9002X_10_PD_REF,
                         PDB_IGEN_AD_MASK, 0);

	cod9002x_usleep(100);

	/* mic bias1 off */
	snd_soc_update_bits(codec, COD9002X_10_PD_REF,
			PDB_MCB1_MASK | PDB_MCB_LDO_CODEC_MASK,
			0);

	return 0;
}
static int cod9002_power_on_mic2(struct snd_soc_codec *codec)
{
	unsigned int mix_val;

	dev_info(codec->dev, "%s called\n", __func__);

	mix_val = snd_soc_read(codec, COD9002X_23_MIX_AD1);
	mix_val &= EN_MIX_MIC2L_MASK | EN_MIX_MIC2R_MASK;

	/* Select default if no paths have been selected */
	if (!mix_val)
		mix_val = EN_MIX_MIC2L_MASK | EN_MIX_MIC2R_MASK;


	/* mic bias1 On */
	snd_soc_update_bits(codec, COD9002X_10_PD_REF,
			PDB_MCB1_MASK | PDB_MCB_LDO_CODEC_MASK,
			PDB_MCB1_MASK | PDB_MCB_LDO_CODEC_MASK);

	/* Reset the mixer-switches before powering on */
	snd_soc_update_bits(codec, COD9002X_23_MIX_AD1,
			EN_MIX_MIC2L_MASK | EN_MIX_MIC2R_MASK, 0x0);

	cod9002x_usleep(100);

	snd_soc_update_bits(codec, COD9002X_10_PD_REF,
                         PDB_IGEN_AD_MASK, PDB_IGEN_AD_MASK);

	cod9002x_usleep(100);

	snd_soc_update_bits(codec, COD9002X_12_PD_AD2,
			PDB_MIC_PGA2_MASK | PDB_MIC_BST2_MASK ,
			PDB_MIC_PGA2_MASK | PDB_MIC_BST2_MASK );

	cod9002x_usleep(100);

	snd_soc_update_bits(codec, COD9002X_11_PD_AD1,
			PDB_MIXL_MASK | PDB_MIXR_MASK ,
			PDB_MIXL_MASK | PDB_MIXR_MASK );

	cod9002x_usleep(100);
	snd_soc_update_bits(codec, COD9002X_11_PD_AD1,
			PDB_DSML_MASK | PDB_DSMR_MASK ,
			PDB_DSML_MASK | PDB_DSMR_MASK );

	cod9002x_usleep(100);
	snd_soc_update_bits(codec, COD9002X_23_MIX_AD1,
			EN_MIX_MIC2L_MASK | EN_MIX_MIC2R_MASK, mix_val);

	snd_soc_update_bits(codec, COD9002X_11_PD_AD1,
			EN_DSMR_PREQ_MASK | EN_DSML_PREQ_MASK, 0);

	cod9002x_usleep(100);
	snd_soc_update_bits(codec, COD9002X_11_PD_AD1,
			RESETB_DSML_MASK | RESETB_DSMR_MASK,
			RESETB_DSML_MASK | RESETB_DSMR_MASK);

	cod9002x_usleep(100);

	return 0;
}



static int cod9002_power_off_mic2(struct snd_soc_codec *codec)
{
	dev_dbg(codec->dev, "%s called\n", __func__);

	snd_soc_update_bits(codec, COD9002X_11_PD_AD1,
			EN_DSMR_PREQ_MASK | EN_DSML_PREQ_MASK,
			EN_DSMR_PREQ_MASK | EN_DSML_PREQ_MASK);

	cod9002x_usleep(100);

	snd_soc_update_bits(codec, COD9002X_23_MIX_AD1,
			EN_MIX_MIC2L_MASK | EN_MIX_MIC2R_MASK, 0);

	cod9002x_usleep(100);

	snd_soc_update_bits(codec, COD9002X_11_PD_AD1,
			PDB_DSML_MASK | PDB_DSMR_MASK, 0);

	cod9002x_usleep(100);

	snd_soc_update_bits(codec, COD9002X_11_PD_AD1,
			PDB_MIXL_MASK | PDB_MIXR_MASK, 0);

	cod9002x_usleep(100);

	snd_soc_update_bits(codec, COD9002X_12_PD_AD2,
			PDB_MIC_PGA2_MASK | PDB_MIC_BST2_MASK, 0);

	cod9002x_usleep(100);

	snd_soc_update_bits(codec, COD9002X_10_PD_REF,
                         PDB_IGEN_AD_MASK, 0);

	cod9002x_usleep(100);

	/* mic bias1 Off */
	snd_soc_update_bits(codec, COD9002X_10_PD_REF,
			PDB_MCB1_MASK | PDB_MCB_LDO_CODEC_MASK,
			0);

	return 0;
}

static int cod9002_power_on_mic3(struct snd_soc_codec *codec)
{
	unsigned int mix_val;

	dev_info(codec->dev, "%s called\n", __func__);

	mix_val = snd_soc_read(codec, COD9002X_23_MIX_AD1);
	mix_val &= EN_MIX_MIC3L_MASK | EN_MIX_MIC3R_MASK;

	/* Select default if no paths have been selected */
	if (!mix_val)
		mix_val = EN_MIX_MIC3L_MASK | EN_MIX_MIC3R_MASK;

	/* should be remove */
	snd_soc_write(codec, 0x81, 0x03);

	/* mic bias2 on */
	snd_soc_update_bits(codec, COD9002X_10_PD_REF,
			PDB_MCB2_MASK | PDB_MCB_LDO_CODEC_MASK,
			PDB_MCB2_MASK | PDB_MCB_LDO_CODEC_MASK);

	/* Reset the mixer-switches before powering on */
	snd_soc_update_bits(codec, COD9002X_23_MIX_AD1,
			EN_MIX_MIC3L_MASK | EN_MIX_MIC3R_MASK, 0x0);

	cod9002x_usleep(100);

	snd_soc_update_bits(codec, COD9002X_10_PD_REF,
                         PDB_IGEN_AD_MASK, PDB_IGEN_AD_MASK);

	cod9002x_usleep(100);

	snd_soc_update_bits(codec, COD9002X_12_PD_AD2,
			PDB_MIC_PGA3_MASK | PDB_MIC_BST3_MASK ,
			PDB_MIC_PGA3_MASK | PDB_MIC_BST3_MASK );

	cod9002x_usleep(100);

	snd_soc_update_bits(codec, COD9002X_11_PD_AD1,
			PDB_MIXL_MASK | PDB_MIXR_MASK ,
			PDB_MIXL_MASK | PDB_MIXR_MASK );

	cod9002x_usleep(100);
	snd_soc_update_bits(codec, COD9002X_11_PD_AD1,
			PDB_DSML_MASK | PDB_DSMR_MASK ,
			PDB_DSML_MASK | PDB_DSMR_MASK );

	cod9002x_usleep(100);
	snd_soc_update_bits(codec, COD9002X_23_MIX_AD1,
			EN_MIX_MIC3L_MASK | EN_MIX_MIC3R_MASK, mix_val);

	snd_soc_update_bits(codec, COD9002X_11_PD_AD1,
			EN_DSMR_PREQ_MASK | EN_DSML_PREQ_MASK, 0);

	cod9002x_usleep(100);
	snd_soc_update_bits(codec, COD9002X_11_PD_AD1,
			RESETB_DSML_MASK | RESETB_DSMR_MASK,
			RESETB_DSML_MASK | RESETB_DSMR_MASK);

	cod9002x_usleep(100);

	return 0;
}

static int cod9002_power_on_linein(struct snd_soc_codec *codec)
{
	unsigned int mix_val;

	dev_info(codec->dev, "%s called\n", __func__);

	mix_val = snd_soc_read(codec, COD9002X_23_MIX_AD1);
	mix_val &= EN_MIX_LNLL_MASK | EN_MIX_LNLR_MASK;

	/* Select default if no paths have been selected */
	if (!mix_val)
		mix_val = EN_MIX_LNLL_MASK | EN_MIX_LNLR_MASK;

	/* Reset the mixer-switches before powering on */
	snd_soc_update_bits(codec, COD9002X_23_MIX_AD1,
			EN_MIX_LNLL_MASK | EN_MIX_LNLR_MASK, 0x0);

	cod9002x_usleep(100);

	snd_soc_update_bits(codec, COD9002X_10_PD_REF,
                         PDB_IGEN_AD_MASK, PDB_IGEN_AD_MASK);

	cod9002x_usleep(100);

	snd_soc_update_bits(codec, COD9002X_12_PD_AD2,
			PDB_LNL_MASK | PDB_LNR_MASK ,
			PDB_LNL_MASK | PDB_LNR_MASK );

	cod9002x_usleep(100);

	snd_soc_update_bits(codec, COD9002X_11_PD_AD1,
			PDB_MIXL_MASK | PDB_MIXR_MASK ,
			PDB_MIXL_MASK | PDB_MIXR_MASK );

	cod9002x_usleep(100);
	snd_soc_update_bits(codec, COD9002X_11_PD_AD1,
			PDB_DSML_MASK | PDB_DSMR_MASK ,
			PDB_DSML_MASK | PDB_DSMR_MASK );

	cod9002x_usleep(100);
	snd_soc_update_bits(codec, COD9002X_23_MIX_AD1,
			EN_MIX_LNLL_MASK | EN_MIX_LNLR_MASK, mix_val);

	snd_soc_update_bits(codec, COD9002X_11_PD_AD1,
			EN_DSMR_PREQ_MASK | EN_DSML_PREQ_MASK, 0);

	cod9002x_usleep(100);
	snd_soc_update_bits(codec, COD9002X_11_PD_AD1,
			RESETB_DSML_MASK | RESETB_DSMR_MASK,
			RESETB_DSML_MASK | RESETB_DSMR_MASK);

	cod9002x_usleep(100);

	return 0;
}

static int cod9002_power_off_linein(struct snd_soc_codec *codec)
{
	dev_dbg(codec->dev, "%s called\n", __func__);

	snd_soc_update_bits(codec, COD9002X_11_PD_AD1,
			RESETB_DSML_MASK | RESETB_DSMR_MASK, 0);

	cod9002x_usleep(100);

	snd_soc_update_bits(codec, COD9002X_23_MIX_AD1,
			EN_MIX_LNLL_MASK | EN_MIX_LNLR_MASK, 0);

	cod9002x_usleep(100);

	snd_soc_update_bits(codec, COD9002X_11_PD_AD1,
			PDB_DSML_MASK | PDB_DSMR_MASK, 0);

	cod9002x_usleep(100);

	snd_soc_update_bits(codec, COD9002X_11_PD_AD1,
			PDB_MIXL_MASK | PDB_MIXR_MASK, 0);

	cod9002x_usleep(100);

	snd_soc_update_bits(codec, COD9002X_12_PD_AD2,
			PDB_LNL_MASK | PDB_LNR_MASK, 0);

	cod9002x_usleep(100);

	snd_soc_update_bits(codec, COD9002X_10_PD_REF,
                         PDB_IGEN_AD_MASK, 0);

	cod9002x_usleep(100);


	return 0;
}
static int cod9002_power_off_mic3(struct snd_soc_codec *codec)
{
	dev_dbg(codec->dev, "%s called\n", __func__);

	snd_soc_update_bits(codec, COD9002X_11_PD_AD1,
			RESETB_DSML_MASK | RESETB_DSMR_MASK, 0);

	cod9002x_usleep(100);

	snd_soc_update_bits(codec, COD9002X_23_MIX_AD1,
			EN_MIX_MIC3L_MASK | EN_MIX_MIC3R_MASK, 0);

	cod9002x_usleep(100);

	snd_soc_update_bits(codec, COD9002X_11_PD_AD1,
			PDB_DSML_MASK | PDB_DSMR_MASK, 0);

	cod9002x_usleep(100);

	snd_soc_update_bits(codec, COD9002X_11_PD_AD1,
			PDB_MIXL_MASK | PDB_MIXR_MASK, 0);

	cod9002x_usleep(100);

	snd_soc_update_bits(codec, COD9002X_12_PD_AD2,
			PDB_MIC_PGA3_MASK | PDB_MIC_BST3_MASK, 0);

	cod9002x_usleep(100);

	snd_soc_update_bits(codec, COD9002X_10_PD_REF,
                         PDB_IGEN_AD_MASK, 0);

	cod9002x_usleep(100);

	/* mic bias2 off */
	snd_soc_update_bits(codec, COD9002X_10_PD_REF,
			PDB_MCB2_MASK | PDB_MCB_LDO_CODEC_MASK,
			0);

	return 0;
}

static int vmid_ev(struct snd_soc_dapm_widget *w,
		struct snd_kcontrol *kcontrol, int event)
{
	dev_dbg(w->codec->dev, "%s called\n", __func__);

	switch (event) {
	case SND_SOC_DAPM_PRE_PMU:
		cod9002x_capture_init(w->codec);
		break;

	case SND_SOC_DAPM_POST_PMU:
		break;

	case SND_SOC_DAPM_PRE_PMD:
		cod9002x_capture_deinit(w->codec);
		break;

	default:
		break;
	}

	return 0;
}

/**
 * SFR revision 1.14 recommends following settings.
 * If playback is only HP mode, write f/w OTP value at 0xD4 and 0xD5 and enable
 * DNC.
 * If playback is not only HP mode, write all zero data value at 0xD4 and 0xD5
 * with DNC disabled.
 */
static void cod9002x_update_playback_otp(struct snd_soc_codec *codec)
{
	int hp_on, spk_on, ep_on;
	int chop_val;
	int offset;
	struct cod9002x_priv *cod9002x = snd_soc_codec_get_drvdata(codec);

	chop_val = snd_soc_read(codec, COD9002X_76_CHOP_DA);
	hp_on = chop_val & EN_HP_CHOP_MASK;
	spk_on = chop_val & EN_SPK_PGA_CHOP_MASK;
	ep_on = chop_val & EN_EP_CHOP_MASK;

	if (!hp_on && !spk_on && !ep_on) {
		dev_warn(codec->dev, "None of the output paths selected.\n");
		return;
	}

	if (hp_on && !spk_on && !ep_on) {
		/* We are in HP only mode */
		/* Updating OTP register 0xD4 */
		offset = COD9002X_D4_OFFSET_DAL - COD9002X_OTP_REG_WRITE_START;
		snd_soc_write(codec, COD9002X_D4_OFFSET_DAL,
				cod9002x->otp_reg[offset]);

		/* Updating OTP register 0xD5 */
		offset = COD9002X_D5_OFFSET_DAR - COD9002X_OTP_REG_WRITE_START;
		snd_soc_write(codec, COD9002X_D5_OFFSET_DAR,
				cod9002x->otp_reg[offset]);

	} else {
		/* This is not-only HP mode */
		snd_soc_write(codec, COD9002X_D4_OFFSET_DAL, 0x0);
		snd_soc_write(codec, COD9002X_D5_OFFSET_DAR, 0x0);

		/* Disable DNC */
		snd_soc_update_bits(codec, COD9002X_54_DNC1,
				EN_DNC_MASK , 0x0);

		cod9002x_usleep(100);
	}

	if (ep_on) {
		/**
		 * When EP path is enabled, update 0xDC as 0x58.
		 * CTMI_EP_A: 0x1 (2.0 uA)
		 * CTMI_EP_P: 0x3 (4.0 uA)
		 * CTMI_EP_D: 0x0 (2.0 uA)
		 */
		snd_soc_write(codec, COD9002X_DC_CTRL_EPS,
				(CTMI_EP_A_1_UA << CTMI_EP_A_SHIFT) |
				(CTMI_EP_P_D_4_UA << CTMI_EP_P_SHIFT) |
				(CTMI_EP_P_D_2_UA << CTMI_EP_D_SHIFT));
	}
}

static int cod9002x_hp_playback_init(struct snd_soc_codec *codec)
{
	int mcq_on;
	unsigned char ctrl_hps;
	dev_dbg(codec->dev, "%s called\n", __func__);

	/* Increase HP current to 4uA in MCQ mode(192Khz), 2uA otherwise */
	mcq_on = snd_soc_read(codec, COD9002X_53_MQS);
	ctrl_hps = snd_soc_read(codec, COD9002X_DB_CTRL_HPS);
	ctrl_hps &= ~CTMI_HP_A_MASK;

	if ((mcq_on & MQS_MODE_MASK) == MQS_MODE_MASK)
		ctrl_hps |= (CTMI_HP_4_UA << CTMI_HP_A_SHIFT);
	else
		ctrl_hps |= (CTMI_HP_2_UA << CTMI_HP_A_SHIFT);

	snd_soc_write(codec, COD9002X_DB_CTRL_HPS, ctrl_hps);
	cod9002x_usleep(100);

	snd_soc_update_bits(codec, COD9002X_D7_CTRL_CP1,
			CTRV_CP_NEGREF_MASK, 0x00);
	/* Enable DNC Start gain*/
	snd_soc_update_bits(codec, COD9002X_54_DNC1,
				DNC_START_GAIN_MASK, DNC_START_GAIN_MASK);

	/* Set DNC Start gain value*/
	snd_soc_write(codec, COD9002X_5A_DNC7, 0x18);

	/* set HP volume Level */
	snd_soc_write(codec, COD9002X_30_VOL_HPL, 0x26);
	snd_soc_write(codec, COD9002X_31_VOL_HPR, 0x26);

	/* Update OTP configuration */
	cod9002x_update_playback_otp(codec);

	/* DNC Target level selection */
	snd_soc_write(codec, COD9002X_56_DNC3, 0x33);

	/* DNC Window selection set to 20Hz time window */
	snd_soc_update_bits(codec, COD9002X_57_DNC4, DNC_WINSEL_MASK,
				(DNC_WIN_SIZE_20HZ << DNC_WINSEL_SHIFT));

	snd_soc_update_bits(codec, COD9002X_36_MIX_DA1,
			EN_HP_MIXL_DCTL_MASK | EN_HP_MIXR_DCTR_MASK,
			EN_HP_MIXL_DCTL_MASK | EN_HP_MIXR_DCTR_MASK);

	cod9002x_usleep(100);

	return 0;
}

static int spkdrv_ev(struct snd_soc_dapm_widget *w,
		struct snd_kcontrol *kcontrol, int event)
{
	unsigned int spk_on;
	unsigned int spk_gain;
	unsigned int hp_on;
	int offset;
	struct cod9002x_priv *cod9002x = snd_soc_codec_get_drvdata(w->codec);

	spk_on = snd_soc_read(w->codec, COD9002X_76_CHOP_DA);
	if (!(spk_on & EN_SPK_PGA_CHOP_MASK)) {
		dev_dbg(w->codec->dev, "%s called but speaker not enabled\n",
				__func__);
		return 0;
	}
	dev_dbg(w->codec->dev, "%s called event=%d\n", __func__, event);

	switch (event) {
	case SND_SOC_DAPM_PRE_PMU:
		/* Update OTP configuration */
		cod9002x_update_playback_otp(w->codec);

		snd_soc_update_bits(w->codec, COD9002X_37_MIX_DA2,
				EN_SPK_MIX_DCTL_MASK | EN_SPK_MIX_DCTR_MASK, 0);

		spk_gain = snd_soc_read(w->codec, COD9002X_39_VOL_SPK);

		snd_soc_update_bits(w->codec, COD9002X_39_VOL_SPK,
				CTVOL_SPK_PGA_MASK,
				0x3 << CTVOL_SPK_PGA_SHIFT);

		snd_soc_update_bits(w->codec, COD9002X_17_PWAUTO_DA,
				PW_AUTO_DA_MASK | APW_SPK_MASK,
				PW_AUTO_DA_MASK | APW_SPK_MASK);

		snd_soc_update_bits(w->codec, COD9002X_37_MIX_DA2,
				EN_SPK_MIX_DCTL_MASK | EN_SPK_MIX_DCTR_MASK,
				EN_SPK_MIX_DCTL_MASK | EN_SPK_MIX_DCTR_MASK);

		msleep(135);

		snd_soc_update_bits(w->codec, COD9002X_39_VOL_SPK,
				CTVOL_SPK_PGA_MASK, spk_gain);

		break;

	case SND_SOC_DAPM_PRE_PMD:
		snd_soc_update_bits(w->codec, COD9002X_39_VOL_SPK,
				CTVOL_SPK_PGA_MASK, 0x3);

		snd_soc_update_bits(w->codec, COD9002X_37_MIX_DA2,
				EN_SPK_MIX_DCTL_MASK | EN_SPK_MIX_DCTR_MASK, 0);

		snd_soc_update_bits(w->codec, COD9002X_17_PWAUTO_DA,
				APW_SPK_MASK, 0);

		cod9002x_usleep(500);
		/* Check HP is ON */
		hp_on = snd_soc_read(w->codec, COD9002X_76_CHOP_DA);
		if ((hp_on & EN_HP_CHOP_MASK)) {
			/* We are in HP only mode */
			/* Updating OTP register 0xD4 */
			offset = COD9002X_D4_OFFSET_DAL - COD9002X_OTP_REG_WRITE_START;
			snd_soc_write(w->codec, COD9002X_D4_OFFSET_DAL,
				cod9002x->otp_reg[offset]);

			/* Updating OTP register 0xD5 */
			offset = COD9002X_D5_OFFSET_DAR - COD9002X_OTP_REG_WRITE_START;
			snd_soc_write(w->codec, COD9002X_D5_OFFSET_DAR,
				cod9002x->otp_reg[offset]);
			snd_soc_write(w->codec, COD9002X_30_VOL_HPL, 0x18);
			snd_soc_write(w->codec, COD9002X_31_VOL_HPR, 0x18);
			msleep(6);
			/* enable DNC */
			snd_soc_update_bits(w->codec,
				COD9002X_54_DNC1,EN_DNC_MASK, EN_DNC_MASK);
		}
		break;
	default:
		break;
	}

	return 0;
}

static int hpdrv_ev(struct snd_soc_dapm_widget *w,
		struct snd_kcontrol *kcontrol, int event)
{
	int hp_on, spk_on, ep_on;
	int chop_val;
	unsigned char ctrl_hps;
	unsigned int detb_period = CTMF_DETB_PERIOD_2048;
	struct cod9002x_priv *cod9002x = snd_soc_codec_get_drvdata(w->codec);
	struct cod9002x_jack_det *jackdet = &cod9002x->jack_det;

	if (cod9002x->use_external_jd || cod9002x->use_btn_adc_mode)
		detb_period = CTMF_DETB_PERIOD_8;

	chop_val = snd_soc_read(w->codec, COD9002X_76_CHOP_DA);
	hp_on = chop_val & EN_HP_CHOP_MASK;
	spk_on = chop_val & EN_SPK_PGA_CHOP_MASK;
	ep_on = chop_val & EN_EP_CHOP_MASK;

	if (!hp_on) {
		dev_dbg(w->codec->dev, "%s called but headphone not enabled\n",
				__func__);
		return 0;
	}

	dev_dbg(w->codec->dev, "%s called, event = %d\n", __func__, event);

	switch (event) {
	case SND_SOC_DAPM_PRE_PMU:
		cod9002x->vol_hpl = snd_soc_read(w->codec, COD9002X_30_VOL_HPL);
		cod9002x->vol_hpr = snd_soc_read(w->codec, COD9002X_31_VOL_HPR);

		/* enable soft mute */
		snd_soc_update_bits(w->codec, COD9002X_50_DAC1,
			DAC1_SOFT_MUTE_MASK, DAC1_SOFT_MUTE_MASK);

		cod9002x_hp_playback_init(w->codec);
		break;

	case SND_SOC_DAPM_POST_PMU:
		if (cod9002x->use_external_jd == true ){
			snd_soc_update_bits(w->codec, COD9002X_86_DET_TIME,
					CTMF_DETB_PERIOD_MASK, 0x0);
		} else {
			/*
			 * Using codec internal jack detection, there is some noise issue.
			 * So , 0x86 detection time set to 0xff when insert 3pole jack.
			 */
			if(jackdet->jack_det && !jackdet->mic_det) {
				snd_soc_update_bits(w->codec, COD9002X_86_DET_TIME,
					CTMF_DETB_PERIOD_MASK ,
					0x0f << CTMF_DETB_PERIOD_SHIFT );
			} else {
				snd_soc_update_bits(w->codec, COD9002X_86_DET_TIME,
					CTMF_DETB_PERIOD_MASK, 0x0);
			}
		}
		cod9002x_usleep(100);

		snd_soc_update_bits(w->codec, COD9002X_17_PWAUTO_DA,
				PW_AUTO_DA_MASK | APW_HP_MASK,
				PW_AUTO_DA_MASK | APW_HP_MASK);
		msleep(160);

		snd_soc_update_bits(w->codec, COD9002X_D7_CTRL_CP1,
			CTRV_CP_NEGREF_MASK, 0x04);

		snd_soc_update_bits(w->codec, COD9002X_1C_SV_DA,
				EN_HP_SV_MASK, 0);
		cod9002x_usleep(100);

		snd_soc_write(w->codec, COD9002X_30_VOL_HPL, 0x1E);
		snd_soc_write(w->codec, COD9002X_31_VOL_HPR, 0x1E);
		cod9002x_usleep(100);

		snd_soc_update_bits(w->codec, COD9002X_1C_SV_DA,
				EN_HP_SV_MASK, EN_HP_SV_MASK);
		cod9002x_usleep(100);

		if (!spk_on && !ep_on) {
			/* Only HP is on, enable DNC and set default analog HP
			 * volume
			 */
			snd_soc_write(w->codec, COD9002X_30_VOL_HPL, 0x18);
			snd_soc_write(w->codec, COD9002X_31_VOL_HPR, 0x18);
			msleep(6);

			/* Limiter level selection -0.2dB (defult) */
			snd_soc_update_bits(w->codec, COD9002X_54_DNC1,
					EN_DNC_MASK, EN_DNC_MASK);
		} else {
			/* Either SPK or EP is on, disable DNC and set given
			 * analog HP volume
			 */
			snd_soc_write(w->codec, COD9002X_30_VOL_HPL,
							cod9002x->vol_hpl);
			snd_soc_write(w->codec, COD9002X_31_VOL_HPR,
							cod9002x->vol_hpr);
		}

		/* diable soft mute */
		snd_soc_update_bits(w->codec, COD9002X_50_DAC1,
			DAC1_SOFT_MUTE_MASK, 0x0);
		break;

	case SND_SOC_DAPM_PRE_PMD:
		/* enable soft mute */
		snd_soc_update_bits(w->codec, COD9002X_50_DAC1,
			DAC1_SOFT_MUTE_MASK, DAC1_SOFT_MUTE_MASK);
		msleep(24);

		snd_soc_update_bits(w->codec, COD9002X_54_DNC1,
				EN_DNC_MASK , 0);
		cod9002x_usleep(100);

		snd_soc_write(w->codec, COD9002X_30_VOL_HPL, 0x1E);
		snd_soc_write(w->codec, COD9002X_31_VOL_HPR, 0x1E);
		cod9002x_usleep(6000);

		snd_soc_update_bits(w->codec, COD9002X_1C_SV_DA,
				EN_HP_SV_MASK, 0);
		cod9002x_usleep(100);

		snd_soc_write(w->codec, COD9002X_30_VOL_HPL, 0x26);
		snd_soc_write(w->codec, COD9002X_31_VOL_HPR, 0x26);
		cod9002x_usleep(100);

		snd_soc_update_bits(w->codec, COD9002X_1C_SV_DA,
				EN_HP_SV_MASK,
				EN_HP_SV_MASK);
		cod9002x_usleep(100);

		snd_soc_update_bits(w->codec, COD9002X_D7_CTRL_CP1,
			CTRV_CP_NEGREF_MASK, 0x0f);

		snd_soc_update_bits(w->codec, COD9002X_17_PWAUTO_DA,
				APW_HP_MASK, 0);
		msleep(40);

		snd_soc_update_bits(w->codec, COD9002X_36_MIX_DA1,
				EN_HP_MIXL_DCTL_MASK | EN_HP_MIXR_DCTR_MASK, 0);
		cod9002x_usleep(100);
		if (cod9002x->use_external_jd == true ) {
			snd_soc_update_bits(w->codec, COD9002X_86_DET_TIME,
				CTMF_DETB_PERIOD_MASK,
				(detb_period << CTMF_DETB_PERIOD_SHIFT));
		} else {
			/*
			 * Using codec internal jack detection, there is some noise issue.
			 * So , 0x86 detection time set to 0xff when insert 3pole jack.
			 */
			if(jackdet->jack_det && !jackdet->mic_det) {
				snd_soc_update_bits(w->codec, COD9002X_86_DET_TIME,
					CTMF_DETB_PERIOD_MASK ,
					0x0f << CTMF_DETB_PERIOD_SHIFT );
			} else {
				snd_soc_update_bits(w->codec, COD9002X_86_DET_TIME,
					CTMF_DETB_PERIOD_MASK,
					(detb_period << CTMF_DETB_PERIOD_SHIFT));
			}
		}
		cod9002x_usleep(100);

		/* set to default HP current value */
		ctrl_hps = snd_soc_read(w->codec, COD9002X_DB_CTRL_HPS);
		ctrl_hps &= ~CTMI_HP_A_MASK;
		ctrl_hps |= (CTMI_HP_2_UA << CTMI_HP_A_SHIFT);
		snd_soc_write(w->codec, COD9002X_DB_CTRL_HPS, ctrl_hps);

		/* diable soft mute */
		snd_soc_update_bits(w->codec, COD9002X_50_DAC1,
			DAC1_SOFT_MUTE_MASK, 0x0);
		break;

	default:
		break;
	}

	return 0;
}

static int epdrv_ev(struct snd_soc_dapm_widget *w,
		struct snd_kcontrol *kcontrol, int event)
{
	unsigned int ep_on;

	ep_on = snd_soc_read(w->codec, COD9002X_76_CHOP_DA);
	if (!(ep_on & EN_EP_CHOP_MASK)) {
		dev_dbg(w->codec->dev, "%s called but ear-piece not enabled\n",
				__func__);
		return 0;
	}
	dev_dbg(w->codec->dev, "%s called, event = %d\n", __func__, event);

	switch (event) {
	case SND_SOC_DAPM_PRE_PMU:
		/* Update OTP configuration */
		cod9002x_update_playback_otp(w->codec);
		/* enable soft mute */
		snd_soc_update_bits(w->codec, COD9002X_50_DAC1,
			DAC1_SOFT_MUTE_MASK, DAC1_SOFT_MUTE_MASK);

		snd_soc_update_bits(w->codec, COD9002X_D7_CTRL_CP1,
			CTRV_CP_NEGREF_MASK, 0x00);

		snd_soc_update_bits(w->codec, COD9002X_10_PD_REF,
					PDB_VMID_MASK, PDB_VMID_MASK);

		snd_soc_update_bits(w->codec, COD9002X_13_PD_DA1,
					EN_DCTL_PREQ_MASK | EN_DCTR_PREQ_MASK,
					EN_DCTL_PREQ_MASK | EN_DCTR_PREQ_MASK);

		snd_soc_update_bits(w->codec, COD9002X_10_PD_REF,
					PDB_IGEN_MASK, PDB_IGEN_MASK);

		snd_soc_update_bits(w->codec, COD9002X_13_PD_DA1,
					PDB_DCTL_MASK, PDB_DCTL_MASK);

		snd_soc_update_bits(w->codec, COD9002X_13_PD_DA1,
					EN_DCTL_PREQ_MASK |
					EN_DCTR_PREQ_MASK, 0);

		snd_soc_update_bits(w->codec, COD9002X_13_PD_DA1,
					RESETB_DCTL_MASK, RESETB_DCTL_MASK);

		snd_soc_update_bits(w->codec, COD9002X_15_PD_DA3,
					PDB_DOUBLER_MASK | PDB_CP_MASK,
					PDB_DOUBLER_MASK | PDB_CP_MASK);
		msleep(1);

		snd_soc_update_bits(w->codec, COD9002X_D7_CTRL_CP1,
			CTRV_CP_NEGREF_MASK, 0x04);

		snd_soc_update_bits(w->codec, COD9002X_15_PD_DA3,
					PDB_EP_CORE_MASK, PDB_EP_CORE_MASK);

		snd_soc_update_bits(w->codec, COD9002X_15_PD_DA3,
					PDB_EP_DRV_MASK, PDB_EP_DRV_MASK);

		snd_soc_update_bits(w->codec, COD9002X_33_CTRL_EP,
					EN_EP_PRT_MASK | EN_EP_IDET_MASK,
					EN_EP_PRT_MASK | EN_EP_IDET_MASK);


		snd_soc_update_bits(w->codec, COD9002X_37_MIX_DA2,
				EN_EP_MIX_DCTL_MASK | EN_EP_MIX_DCTR_MASK,
				EN_EP_MIX_DCTL_MASK | EN_EP_MIX_DCTR_MASK);
		cod9002x_usleep(100);
		/* disable_soft_mute */
		snd_soc_update_bits(w->codec, COD9002X_50_DAC1,
			DAC1_SOFT_MUTE_MASK, 0);
		break;
	case SND_SOC_DAPM_PRE_PMD:
		/* enable soft mute */
		snd_soc_update_bits(w->codec, COD9002X_50_DAC1,
			DAC1_SOFT_MUTE_MASK, DAC1_SOFT_MUTE_MASK);
		msleep(24);

		snd_soc_update_bits(w->codec, COD9002X_D7_CTRL_CP1,
			CTRV_CP_NEGREF_MASK, 0x0f);

		snd_soc_update_bits(w->codec, COD9002X_33_CTRL_EP,
					CTMV_CP_MODE_MASK,
					CTMV_CP_MODE_HALF_VDD << CTMV_CP_MODE_SHIFT);

		snd_soc_update_bits(w->codec, COD9002X_37_MIX_DA2,
				EN_EP_MIX_DCTL_MASK | EN_EP_MIX_DCTR_MASK, 0x0);
		cod9002x_usleep(100);

		snd_soc_update_bits(w->codec, COD9002X_33_CTRL_EP,
					EN_EP_PRT_MASK | EN_EP_IDET_MASK, 0);

		snd_soc_update_bits(w->codec, COD9002X_15_PD_DA3,
					PDB_DOUBLER_MASK | PDB_CP_MASK |
					PDB_EP_CORE_MASK |
					PDB_EP_DRV_MASK, 0);

		snd_soc_update_bits(w->codec, COD9002X_13_PD_DA1,
						PDB_DCTL_MASK |
						RESETB_DCTL_MASK, 0);

		snd_soc_update_bits(w->codec, COD9002X_10_PD_REF,
					PDB_VMID_MASK | PDB_IGEN_MASK, 0);

		snd_soc_update_bits(w->codec, COD9002X_33_CTRL_EP,
					CTMV_CP_MODE_MASK,
					CTMV_CP_MODE_ANALOG << CTMV_CP_MODE_SHIFT);

		/* disable_soft_mute */
		snd_soc_update_bits(w->codec, COD9002X_50_DAC1,
			DAC1_SOFT_MUTE_MASK, 0);
	default:
		break;
	}

	return 0;
}

static int mic2_pga_ev(struct snd_soc_dapm_widget *w,
		struct snd_kcontrol *kcontrol, int event)
{
	int mic_on;

	dev_dbg(w->codec->dev, "%s called, event = %d\n", __func__, event);

	mic_on = snd_soc_read(w->codec, COD9002X_75_CHOP_AD);
	if (!(mic_on & EN_MCB2_CHOP_MASK)) {
		dev_dbg(w->codec->dev, "%s: MIC2 is not enabled, returning.\n",
								__func__);
		return 0;
	}

	switch (event) {
	case SND_SOC_DAPM_PRE_PMU:
		cod9002_power_on_mic2(w->codec);
		break;

	case SND_SOC_DAPM_PRE_PMD:
		cod9002_power_off_mic2(w->codec);
		break;
	default:
		break;
	}

	return 0;
}

static int mic1_pga_ev(struct snd_soc_dapm_widget *w,
		struct snd_kcontrol *kcontrol, int event)
{
	int mic_on;

	dev_dbg(w->codec->dev, "%s called, event = %d\n", __func__, event);

	mic_on = snd_soc_read(w->codec, COD9002X_75_CHOP_AD);
	if (!(mic_on & EN_MCB1_CHOP_MASK)) {
		dev_dbg(w->codec->dev, "%s: MIC1 is not enabled, returning.\n",
								__func__);
		return 0;
	}

	switch (event) {
	case SND_SOC_DAPM_PRE_PMU:
		cod9002_power_on_mic1(w->codec);
		break;

	case SND_SOC_DAPM_PRE_PMD:
		cod9002_power_off_mic1(w->codec);
		break;

	default:
		break;
	}

	return 0;
}

static int mic3_pga_ev(struct snd_soc_dapm_widget *w,
		struct snd_kcontrol *kcontrol, int event)
{
//	int mic_on;
pr_err("[DEBUG] %s called , event = %d\n", __func__, event);
	dev_dbg(w->codec->dev, "%s called, event = %d\n", __func__, event);

/*	mic_on = snd_soc_read(w->codec, COD9002X_75_CHOP_AD);
	if (!(mic_on & EN_LN_CHOP_MASK)) {
		dev_dbg(w->codec->dev, "%s: MIC3 is not enabled, returning.\n",
								__func__);
		return 0;
	}
*/
	switch (event) {
	case SND_SOC_DAPM_PRE_PMU:
		cod9002_power_on_mic3(w->codec);
		break;

	case SND_SOC_DAPM_PRE_PMD:
		cod9002_power_off_mic3(w->codec);
		break;

	default:
		break;
	}

	return 0;
}

static int linein_pga_ev(struct snd_soc_dapm_widget *w,
		struct snd_kcontrol *kcontrol, int event)
{
	int linein_on;

	dev_dbg(w->codec->dev, "%s called, event = %d\n", __func__, event);

	linein_on = snd_soc_read(w->codec, COD9002X_75_CHOP_AD);
	if (!(linein_on & EN_LN_CHOP_MASK)) {
		dev_dbg(w->codec->dev, "%s: LINE IN is not enabled, returning.\n",
								__func__);
		return 0;
	}

	switch (event) {
	case SND_SOC_DAPM_PRE_PMU:
		cod9002_power_on_linein(w->codec);
		break;

	case SND_SOC_DAPM_PRE_PMD:
		cod9002_power_off_linein(w->codec);
		break;

	default:
		break;
	}

	return 0;
}

static const struct snd_kcontrol_new adcl_mix[] = {
	SOC_DAPM_SINGLE("MIC1L Switch", COD9002X_23_MIX_AD1,
			EN_MIX_MIC1L_SHIFT, 1, 0),
	SOC_DAPM_SINGLE("MIC2L Switch", COD9002X_23_MIX_AD1,
			EN_MIX_MIC2L_SHIFT, 1, 0),
	SOC_DAPM_SINGLE("MIC3L Switch", COD9002X_23_MIX_AD1,
			EN_MIX_MIC3L_SHIFT, 1, 0),
	SOC_DAPM_SINGLE("LINELL Switch", COD9002X_23_MIX_AD1,
			EN_MIX_LNLL_SHIFT, 1, 0),
	SOC_DAPM_SINGLE("LINERL Switch", COD9002X_24_MIX_AD2,
			EN_MIX_LNRL_SHIFT, 1, 0),
};

static const struct snd_kcontrol_new adcr_mix[] = {
	SOC_DAPM_SINGLE("MIC1R Switch", COD9002X_23_MIX_AD1,
			EN_MIX_MIC1R_SHIFT, 1, 0),
	SOC_DAPM_SINGLE("MIC2R Switch", COD9002X_23_MIX_AD1,
			EN_MIX_MIC2R_SHIFT, 1, 0),
	SOC_DAPM_SINGLE("MIC3R Switch", COD9002X_23_MIX_AD1,
			EN_MIX_MIC3R_SHIFT, 1, 0),
	SOC_DAPM_SINGLE("LINELR Switch", COD9002X_24_MIX_AD2,
			EN_MIX_LNLR_SHIFT, 1, 0),
	SOC_DAPM_SINGLE("LINERR Switch", COD9002X_23_MIX_AD1,
			EN_MIX_LNRR_SHIFT, 1, 0),
};

static const struct snd_kcontrol_new spk_on[] = {
	SOC_DAPM_SINGLE("SPK On", COD9002X_76_CHOP_DA,
				EN_SPK_PGA_CHOP_SHIFT, 1, 0),
};

static const struct snd_kcontrol_new hp_on[] = {
	SOC_DAPM_SINGLE("HP On", COD9002X_76_CHOP_DA, EN_HP_CHOP_SHIFT, 1, 0),
};

static const struct snd_kcontrol_new ep_on[] = {
	SOC_DAPM_SINGLE("EP On", COD9002X_76_CHOP_DA, EN_EP_CHOP_SHIFT, 1, 0),
};

static const struct snd_kcontrol_new mic1_on[] = {
	SOC_DAPM_SINGLE("MIC1 On", COD9002X_75_CHOP_AD,
					EN_MCB1_CHOP_SHIFT, 1, 0),
};

static const struct snd_kcontrol_new mic2_on[] = {
	SOC_DAPM_SINGLE("MIC2 On", COD9002X_75_CHOP_AD,
					EN_MCB2_CHOP_SHIFT, 1, 0),
};

static const struct snd_kcontrol_new mic3_on[] = {
	SOC_DAPM_SINGLE("MIC3 On", COD9002X_75_CHOP_AD,
					EN_MIC3_CHOP_SHIFT, 1, 0),
};

static const struct snd_kcontrol_new linein_on[] = {
	SOC_DAPM_SINGLE("LINEIN On", COD9002X_75_CHOP_AD,
					EN_LN_CHOP_SHIFT, 1, 0),
};

static const char * const cod9002x_fm_texts[] = {
	"None",
	"FM On",
};

static SOC_ENUM_SINGLE_VIRT_DECL(cod9002x_fm_enum, cod9002x_fm_texts);

static const struct snd_kcontrol_new cod9002x_fm_mux[] = {
	SOC_DAPM_ENUM("FM Link", cod9002x_fm_enum),
};

static const struct snd_soc_dapm_widget cod9002x_dapm_widgets[] = {
	SND_SOC_DAPM_SWITCH("SPK", SND_SOC_NOPM, 0, 0, spk_on),
	SND_SOC_DAPM_SWITCH("HP", SND_SOC_NOPM, 0, 0, hp_on),
	SND_SOC_DAPM_SWITCH("EP", SND_SOC_NOPM, 0, 0, ep_on),
	SND_SOC_DAPM_SWITCH("MIC1", SND_SOC_NOPM, 0, 0, mic1_on),
	SND_SOC_DAPM_SWITCH("MIC2", SND_SOC_NOPM, 0, 0, mic2_on),
	SND_SOC_DAPM_SWITCH("MIC3", SND_SOC_NOPM, 0, 0, mic3_on),
	SND_SOC_DAPM_SWITCH("LINEIN", SND_SOC_NOPM, 0, 0, linein_on),

	SND_SOC_DAPM_SUPPLY("VMID", SND_SOC_NOPM, 0, 0, vmid_ev,
			SND_SOC_DAPM_PRE_PMU),

	SND_SOC_DAPM_OUT_DRV_E("SPKDRV", SND_SOC_NOPM, 0, 0, NULL, 0,
			spkdrv_ev, SND_SOC_DAPM_PRE_PMU | SND_SOC_DAPM_PRE_PMD),
	SND_SOC_DAPM_OUT_DRV_E("EPDRV", SND_SOC_NOPM, 0, 0, NULL, 0,
			epdrv_ev, SND_SOC_DAPM_PRE_PMU | SND_SOC_DAPM_PRE_PMD),
	SND_SOC_DAPM_OUT_DRV_E("HPDRV", SND_SOC_NOPM, 0, 0, NULL, 0,
			hpdrv_ev, SND_SOC_DAPM_PRE_PMU |
			SND_SOC_DAPM_POST_PMU | SND_SOC_DAPM_PRE_PMD),
	SND_SOC_DAPM_MUX("FM Link", SND_SOC_NOPM, 0, 0,
			cod9002x_fm_mux),
	SND_SOC_DAPM_PGA_E("MIC1_PGA", SND_SOC_NOPM, 0, 0,
			NULL, 0, mic1_pga_ev,
			SND_SOC_DAPM_PRE_PMU | SND_SOC_DAPM_POST_PMU |
			SND_SOC_DAPM_PRE_PMD),
	SND_SOC_DAPM_PGA_E("MIC2_PGA", SND_SOC_NOPM, 0, 0,
			NULL, 0, mic2_pga_ev,
			SND_SOC_DAPM_PRE_PMU | SND_SOC_DAPM_POST_PMU |
			SND_SOC_DAPM_PRE_PMD),
	SND_SOC_DAPM_PGA_E("MIC3_PGA", SND_SOC_NOPM, 0, 0,
			NULL, 0, mic3_pga_ev,
			SND_SOC_DAPM_PRE_PMU | SND_SOC_DAPM_POST_PMU |
			SND_SOC_DAPM_PRE_PMD),
	SND_SOC_DAPM_PGA_E("LINEIN_PGA", SND_SOC_NOPM, 0, 0,
			NULL, 0, linein_pga_ev,
			SND_SOC_DAPM_PRE_PMU | SND_SOC_DAPM_POST_PMU |
			SND_SOC_DAPM_PRE_PMD),
	SND_SOC_DAPM_MIXER("ADCL Mixer", SND_SOC_NOPM, 0, 0, adcl_mix,
			ARRAY_SIZE(adcl_mix)),
	SND_SOC_DAPM_MIXER("ADCR Mixer", SND_SOC_NOPM, 0, 0, adcr_mix,
			ARRAY_SIZE(adcr_mix)),

	SND_SOC_DAPM_DAC_E("DAC", "AIF Playback", SND_SOC_NOPM, 0, 0,
			dac_ev, SND_SOC_DAPM_PRE_PMU | SND_SOC_DAPM_PRE_PMD),
	SND_SOC_DAPM_DAC_E("DAC", "AIF2 Playback", SND_SOC_NOPM, 0, 0,
			dac_ev, SND_SOC_DAPM_PRE_PMU | SND_SOC_DAPM_PRE_PMD),

	SND_SOC_DAPM_ADC_E("ADC", "AIF Capture", SND_SOC_NOPM, 0, 0,
			adc_ev, SND_SOC_DAPM_PRE_PMU |
			SND_SOC_DAPM_POST_PMU | SND_SOC_DAPM_PRE_PMD),
	SND_SOC_DAPM_ADC_E("ADC", "AIF2 Capture", SND_SOC_NOPM, 0, 0,
			adc_ev, SND_SOC_DAPM_PRE_PMU |
			SND_SOC_DAPM_POST_PMU | SND_SOC_DAPM_PRE_PMD),

	SND_SOC_DAPM_OUTPUT("SPKOUTLN"),
	SND_SOC_DAPM_OUTPUT("HPOUTLN"),
	SND_SOC_DAPM_OUTPUT("EPOUTN"),
	SND_SOC_DAPM_OUTPUT("AIF4OUT"),

	SND_SOC_DAPM_INPUT("IN1L"),
	SND_SOC_DAPM_INPUT("IN2L"),
	SND_SOC_DAPM_INPUT("IN3L"),
	SND_SOC_DAPM_INPUT("IN4L"),

	SND_SOC_DAPM_INPUT("AIF4IN"),
};

static const struct snd_soc_dapm_route cod9002x_dapm_routes[] = {
	/* Sink, Control, Source */
	{"SPKDRV", NULL, "DAC"},
	{"SPK" , "SPK On", "SPKDRV"},
	{"SPKOUTLN", NULL, "SPK"},

	{"EPDRV", NULL, "DAC"},
	{"EP", "EP On", "EPDRV"},
	{"EPOUTN", NULL, "EP"},

	{"HPDRV", NULL, "DAC"},
	{"HP", "HP On", "HPDRV"},
	{"HPOUTLN", NULL, "HP"},

	{"DAC" , NULL, "AIF Playback"},
	{"DAC" , NULL, "AIF2 Playback"},

	{"MIC1_PGA", NULL, "IN1L"},
	{"MIC1_PGA", NULL, "VMID"},
	{"MIC1", "MIC1 On", "MIC1_PGA"},

	{"ADCL Mixer", "MIC1L Switch", "MIC1"},
	{"ADCR Mixer", "MIC1R Switch", "MIC1"},

	{"MIC2_PGA", NULL, "IN2L"},
	{"MIC2_PGA", NULL, "VMID"},
	{"MIC2", "MIC2 On", "MIC2_PGA"},

	{"ADCL Mixer", "MIC2L Switch", "MIC2"},
	{"ADCR Mixer", "MIC2R Switch", "MIC2"},

	{"MIC3_PGA", NULL, "IN3L"},
	{"MIC3_PGA", NULL, "VMID"},
	{"MIC3", "MIC3 On", "MIC3_PGA"},

	{"ADCL Mixer", "MIC3L Switch", "MIC3"},
	{"ADCR Mixer", "MIC3R Switch", "MIC3"},

	{"LINEIN_PGA", NULL, "IN4L"},
	{"LINEIN_PGA", NULL, "VMID"},
	{"LINEIN", "LINEIN On", "LINEIN_PGA"},

	{"ADCL Mixer", "LINELL Switch", "LINEIN"},
	{"ADCL Mixer", "LINERL Switch", "LINEIN"},
	{"ADCR Mixer", "LINELR Switch", "LINEIN"},
	{"ADCR Mixer", "LINERR Switch", "LINEIN"},

	{"ADC", NULL, "ADCL Mixer"},
	{"ADC", NULL, "ADCR Mixer"},

	{"AIF Capture", NULL, "ADC"},
	{"AIF2 Capture", NULL, "ADC"},

	{"FM Link", "FM On", "ADC"},
	{"DAC", NULL, "FM Link"},
};

static int cod9002x_dai_set_fmt(struct snd_soc_dai *dai, unsigned int fmt)
{
	struct snd_soc_codec *codec = dai->codec;
	int bclk = 0, lrclk = 0;

	dev_dbg(codec->dev, "%s called\n", __func__);

	switch (fmt & SND_SOC_DAIFMT_FORMAT_MASK) {
	case SND_SOC_DAIFMT_LEFT_J:
		fmt = LRJ_AUDIO_FORMAT_MASK;
		break;

	case SND_SOC_DAIFMT_I2S:
		fmt = I2S_AUDIO_FORMAT_MASK;
		break;

	default:
		pr_err("Unsupported DAI format %d\n",
				fmt & SND_SOC_DAIFMT_FORMAT_MASK);
		return -EINVAL;
	}

	snd_soc_update_bits(codec, COD9002X_41_FORMAT,
			I2S_AUDIO_FORMAT_MASK | LRJ_AUDIO_FORMAT_MASK, fmt);

	switch (fmt & SND_SOC_DAIFMT_INV_MASK) {
	case SND_SOC_DAIFMT_NB_NF:
		break;
	case SND_SOC_DAIFMT_IB_IF:
		bclk = BCLK_POL_MASK;
		lrclk = LRCLK_POL_MASK;
		break;
	case SND_SOC_DAIFMT_IB_NF:
		bclk = BCLK_POL_MASK;
		break;
	case SND_SOC_DAIFMT_NB_IF:
		lrclk = LRCLK_POL_MASK;
		break;
	default:
		pr_err("Unsupported Polartiy selection %d\n",
				fmt & SND_SOC_DAIFMT_INV_MASK);
		return -EINVAL;
	}

	snd_soc_update_bits(codec, COD9002X_41_FORMAT,
			BCLK_POL_MASK | LRCLK_POL_MASK, bclk | lrclk);
	return 0;
}

int cod9002x_set_externel_jd(struct snd_soc_codec *codec)
{
	int ret;
	struct cod9002x_priv *cod9002x;

	if (codec == NULL) {
		pr_err("Initilaise codec, before calling %s\n", __func__);
		return -1;
	}

	dev_dbg(codec->dev, "%s called\n", __func__);

	cod9002x = snd_soc_codec_get_drvdata(codec);

	cod9002x->use_external_jd = true;

#ifdef CONFIG_PM_RUNTIME
	pm_runtime_get_sync(codec->dev);
#else
	cod9002x_enable(codec->dev);
#endif
	/* Enable External jack detecter */
	ret = snd_soc_update_bits(codec, COD9002X_83_JACK_DET1,
			CTMP_JD_MODE_MASK, CTMP_JD_MODE_MASK);

	/* Disable Internel Jack detecter */
	ret |= snd_soc_update_bits(codec, COD9002X_81_DET_ON,
			EN_PDB_JD_CLK_MASK | EN_PDB_JD_MASK,
			EN_PDB_JD_CLK_MASK);

	/* Keep mic2 bias always high */
	snd_soc_update_bits(codec, COD9002X_86_DET_TIME,
			CTMD_BTN_DBNC_MASK | CTMF_BTN_ON_MASK |
			CTMF_DETB_PERIOD_MASK,
			((CTMD_BTN_DBNC_5 << CTMD_BTN_DBNC_SHIFT) |
			(CTMF_BTN_ON_14_CLK << CTMF_BTN_ON_SHIFT) |
			(CTMF_DETB_PERIOD_8 << CTMF_DETB_PERIOD_SHIFT)));

	snd_soc_update_bits(codec, COD9002X_06_IRQ1M,
				IRQ1M_MASK_ALL, 0xFF);
	snd_soc_update_bits(codec, COD9002X_07_IRQ2M,
				IRQ2M_MASK_ALL, 0xFF);

	/* Set Jack debounce time */
	snd_soc_write(codec, COD9002X_84_JACK_DET2, 0x37);
	snd_soc_write(codec, COD9002X_87_LDO_DIG, 0x03);
#ifdef CONFIG_PM_RUNTIME
	pm_runtime_put_sync(codec->dev);
#else
	cod9002x_disable(codec->dev);
#endif

	return ret;
}
EXPORT_SYMBOL_GPL(cod9002x_set_externel_jd);

static int cod9002x_dai_startup(struct snd_pcm_substream *substream,
		struct snd_soc_dai *dai)
{
	struct snd_soc_codec *codec = dai->codec;

	dev_dbg(codec->dev, "(%s) %s completed\n",
			substream->stream ? "C" : "P", __func__);

	return 0;
}

static void cod9002x_dai_shutdown(struct snd_pcm_substream *substream,
		struct snd_soc_dai *dai)
{
	struct snd_soc_codec *codec = dai->codec;

	dev_dbg(codec->dev, "(%s) %s completed\n",
			substream->stream ? "C" : "P", __func__);
}

static void cod9002x_sys_reset(struct snd_soc_codec *codec)
{
	unsigned char otp_addr[COD9002X_RESTORE_OTP_COUNT] = {
					0xd4, 0xd5, 0xd6, 0xdb, 0xdc
					};

	unsigned char reg_addr[COD9002X_RESTORE_REG_COUNT]= {
					0x20, 0x22, 0x30, 0x31, 0x32,
					0x36, 0X37, 0x42, 0x44, 0X71,
					0x75, 0x5a, 0x54, 0x40, 0x16,
					0x17};

	unsigned char otp_val[COD9002X_RESTORE_OTP_COUNT];
	unsigned char reg_val[COD9002X_RESTORE_REG_COUNT];
	unsigned int i;

	dev_dbg(codec->dev, "%s called\n", __func__);

	/* TODO: Check if we can use cod3022x_{restore/save}_otp_registers() */
	/* OTP register values are read from 0xF* and written to 0xD* */
	for(i = 0; i < COD9002X_RESTORE_OTP_COUNT; i++)
		otp_val[i] = snd_soc_read(codec,
				(otp_addr[i] + COD9002X_OTP_R_OFFSET));

	for(i = 0; i < COD9002X_RESTORE_REG_COUNT; i++)
		reg_val[i] = snd_soc_read(codec, reg_addr[i]);

	snd_soc_update_bits(codec, COD9002X_40_DIGITAL_POWER,
						SYS_RSTB_MASK, 0);
	mdelay(1);
	snd_soc_update_bits(codec, COD9002X_40_DIGITAL_POWER,
					SYS_RSTB_MASK, SYS_RSTB_MASK);
	mdelay(1);

	for(i = 0; i < COD9002X_RESTORE_OTP_COUNT; i++)
		snd_soc_write(codec, otp_addr[i], otp_val[i]);

	for(i = 0; i < COD9002X_RESTORE_REG_COUNT; i++)
		snd_soc_write(codec, reg_addr[i], reg_val[i]);
}

static int cod9002x_dai_hw_params(struct snd_pcm_substream *substream,
		struct snd_pcm_hw_params *params,
		struct snd_soc_dai *dai)
{
	struct snd_soc_codec *codec = dai->codec;
	struct cod9002x_priv *cod9002x = snd_soc_codec_get_drvdata(codec);
	unsigned int cur_aifrate;
	int dnc;
	unsigned char ctrl_hps, hp_current_val = CTMI_HP_2_UA;
	int ret;

	dev_dbg(codec->dev, "%s called\n", __func__);

	/* 192 KHz support */
	cur_aifrate = params_rate(params);
	if (cod9002x->aifrate != cur_aifrate) {
		/* DNC needs to be disabled while switching samplerate */
		dnc = snd_soc_read(codec, COD9002X_54_DNC1);
		snd_soc_write(codec, COD9002X_54_DNC1, 0);

		/* Need to reset H/W while switching from 192KHz to 48KHz */
		if (cur_aifrate == COD9002X_SAMPLE_RATE_192KHZ) {
			snd_soc_update_bits(codec, COD9002X_53_MQS,
					MQS_MODE_MASK, MQS_MODE_MASK);
			hp_current_val = CTMI_HP_4_UA;
		} else if (cod9002x->aifrate == COD9002X_SAMPLE_RATE_192KHZ) {
			snd_soc_update_bits(codec, COD9002X_53_MQS,
					MQS_MODE_MASK, 0);
			hp_current_val = CTMI_HP_2_UA;
			cod9002x_sys_reset(codec);
		}

		/*
		 * If HP is already on, then change the 'current' setting based
		 * on samplerate
		 */
		if (APW_HP_MASK == (snd_soc_read(codec, COD9002X_17_PWAUTO_DA)
							& APW_HP_MASK)) {
			ctrl_hps = snd_soc_read(codec, COD9002X_DB_CTRL_HPS);
			ctrl_hps &= ~CTMI_HP_A_MASK;
			ctrl_hps |= hp_current_val << CTMI_HP_A_SHIFT;
			snd_soc_write(codec, COD9002X_DB_CTRL_HPS, ctrl_hps);
		}

		/* DNC mode can be restored after the samplerate switch */
		snd_soc_write(codec, COD9002X_54_DNC1, dnc);
		cod9002x->aifrate = cur_aifrate;
	}

	/*
	 * Codec supports only 24bits per sample, Mixer performs the required
	 * conversion to 24 bits. BFS is fixed at 64fs for mixer<->codec
	 * interface.
	 */
	ret = snd_soc_update_bits(codec, COD9002X_41_FORMAT,
			DATA_WORD_LENGTH_MASK,
			(DATA_WORD_LENGTH_24 << DATA_WORD_LENGTH_SHIFT));
	if (ret < 0) {
		dev_err(codec->dev, "%s failed to set bits per sample\n",
				__func__);
		return ret;
	}

	return 0;
}

static void cod9002x_jack_det_work(struct work_struct *work)
{
	struct cod9002x_priv *cod9002x =
		container_of(work, struct cod9002x_priv, jack_det_work.work);
	struct cod9002x_jack_det *jackdet = &cod9002x->jack_det;
	struct snd_soc_codec *codec = cod9002x->codec;
	int adc;

	dev_err(cod9002x->dev, " %s(%d) jackdet: %d \n" , __func__, __LINE__, jackdet->jack_det);
	mutex_lock(&cod9002x->jackdet_lock);

	if(jackdet->jack_det == true){
	/* read adc for mic detect */
		adc = cod9002x_adc_get_value(cod9002x);
		dev_err(cod9002x->dev, " %s mic det adc  %d \n" , __func__, adc);

		if ( adc > cod9002x->mic_adc_range )
			jackdet->mic_det = true;
		else
			jackdet->mic_det = false;
	} else {
		/* jack/mic out */
		jackdet->mic_det = false;
	}

	if (jackdet->jack_det && jackdet->mic_det)
		switch_set_state(&cod9002x->sdev, 1);
	else if (jackdet->jack_det)
		switch_set_state(&cod9002x->sdev, 2);
	else
		switch_set_state(&cod9002x->sdev, 0);

	if (cod9002x->is_suspend)
		regcache_cache_only(cod9002x->regmap, false);

	if (jackdet->jack_det && jackdet->mic_det)
		snd_soc_write(codec, 0x97, 0x32);
	else if (jackdet->jack_det)
		snd_soc_write(codec, 0x97, 0x12);
	else
		snd_soc_write(codec, 0x97, 0x02);



	if (cod9002x->is_suspend)
		regcache_cache_only(cod9002x->regmap, true);

	dev_dbg(cod9002x->codec->dev, "Jack %s, Mic %s\n",
				jackdet->jack_det ? "inserted" : "removed",
				jackdet->mic_det ? "inserted" : "removed");

	mutex_unlock(&cod9002x->jackdet_lock);
}

#define ADC_TRACE_NUM		5
#define ADC_TRACE_NUM2		2
#define ADC_READ_DELAY_US	500
#define ADC_READ_DELAY_MS	1
#define ADC_DEVI_THRESHOLD	18000

#define BUTTON_PRESS 1
#define BUTTON_RELEASE 0

static int get_adc_avg(int* adc_values)
{
	int i;
	int adc_sum=0;
	for ( i=0; i<ADC_TRACE_NUM; i++) {
		adc_sum += adc_values[i];
	}
	adc_sum = adc_sum / ADC_TRACE_NUM;
	return adc_sum;
}

static int get_adc_devi(int avg , int* adc_values)
{
	int i;
	int devi=0, diff;
	for ( i=0; i<ADC_TRACE_NUM; i++) {
		diff = adc_values[i]-avg;
		devi += (diff*diff);
	}
	return devi;
}

static void cod9002x_buttons_work(struct work_struct *work)
{
	struct cod9002x_priv *cod9002x =
		container_of(work, struct cod9002x_priv, buttons_work.work);
	struct cod9002x_jack_det *jd = &cod9002x->jack_det;
	struct jack_buttons_zone *btn_zones = cod9002x->jack_buttons_zones;
	int num_buttons_zones = ARRAY_SIZE(cod9002x->jack_buttons_zones);
	int adc_values[ADC_TRACE_NUM];
	int current_button_state;
	int adc;
	int i, avg, devi;
	int adc_final_values[ADC_TRACE_NUM2];
	int j;
	int adc_final = 0;
	int adc_max = 0;

	if (!jd->jack_det) {
		dev_err(cod9002x->dev, "Skip button events for jack_out\n");
		return;
	}
	if (!jd->mic_det) {
		dev_err(cod9002x->dev, "Skip button events for 3-pole jack\n");
		return;
	}

	for ( j=0; j<ADC_TRACE_NUM2; j++) {
		/* read GPADC for button */
		for ( i=0; i<ADC_TRACE_NUM; i++) {
			adc = cod9002x_adc_get_value(cod9002x);
			adc_values[i] = adc;
			udelay(ADC_READ_DELAY_US);
		}

		/*
		 * check avg/devi value is proper
		 * if not read adc after 5 ms
		 */
		avg = get_adc_avg(adc_values);
		devi = get_adc_devi(avg,adc_values);
		dev_err(cod9002x->dev, ":button adc avg: %d, devi: %d\n",avg, devi);

		if (devi > ADC_DEVI_THRESHOLD ) {
			queue_delayed_work(cod9002x->buttons_wq,
					&cod9002x->buttons_work, 5);
			for ( i=0; i<ADC_TRACE_NUM; ){
				dev_err(cod9002x->dev, ":retry button_work :  %d %d %d %d %d\n",
				adc_values[i+ 0], adc_values[i+ 1], adc_values[i+ 2], adc_values[i+ 3], adc_values[i+ 4]);
				i += 5;
			}
			return;
		}
		adc_final_values[j] = avg;

		if (avg > adc_max)
			adc_max = avg;
		mdelay(ADC_READ_DELAY_MS);
	}
	adc_final = adc_max;

	/* check button press/release */
	if(adc_final > cod9002x->btn_release_value)
		current_button_state = BUTTON_RELEASE;
	else
		current_button_state = BUTTON_PRESS;

	if( jd->privious_button_state == current_button_state) {
		return;
	}

	jd->privious_button_state = current_button_state;

	adc = adc_final;
	jd->adc_val = adc_final;

	for (i = 0; i < 4; i++)
			pr_err("[DEBUG]: buttons: code(%d), low(%d), high(%d)\n",
				cod9002x->jack_buttons_zones[i].code,
				cod9002x->jack_buttons_zones[i].adc_low,
				cod9002x->jack_buttons_zones[i].adc_high);

	/* determine which button press or release */
	if(current_button_state == BUTTON_PRESS) {
		for (i = 0; i < num_buttons_zones; i++)
			if (adc >= btn_zones[i].adc_low &&
				adc <= btn_zones[i].adc_high) {
				jd->button_code = btn_zones[i].code;
				input_report_key(cod9002x->input, jd->button_code, 1);
				input_sync(cod9002x->input);
				jd->button_det = true;
				dev_err(cod9002x->dev, ":key %d is pressed, adc %d\n",
						 btn_zones[i].code, adc);
				return;
		}

		dev_err(cod9002x->dev, ":key skipped. ADC %d\n", adc);
	} else {
		jd->button_det = false;
		input_report_key(cod9002x->input, jd->button_code, 0);
		input_sync(cod9002x->input);
		dev_err(cod9002x->dev, ":key %d released\n", jd->button_code);
	}

	return;
}

#define MIC_DETECT_DELAY 150

static irqreturn_t cod9002x_threaded_isr(int irq, void *data)
{
	struct cod9002x_priv *cod9002x = data;
	struct snd_soc_codec *codec = cod9002x->codec;
	struct cod9002x_jack_det *jd = &cod9002x->jack_det;
	unsigned int  stat1, pend1, pend2, pend3;
	int jackdet = COD9002X_MJ_DET_INVALID;
	bool det_status_change = false;

	mutex_lock(&cod9002x->key_lock);

	if (cod9002x->is_suspend)
		regcache_cache_only(cod9002x->regmap, false);

	pend1 = snd_soc_read(codec, COD9002X_01_IRQ1PEND);
	pend2 = snd_soc_read(codec, COD9002X_02_IRQ2PEND);
	pend3 = snd_soc_read(codec, COD9002X_03_IRQ3PEND);
	stat1 = snd_soc_read(codec, COD9002X_0B_STATUS1);

	pr_err("[DEBUG] %s , line %d 01: %02x, 02:%02x, 03:%02x \n",__func__, __LINE__
					, pend1, pend2, pend3);
	/*
	 * Sequence for Jack/Mic detection
	 *
	 * (JACK bit 0, MIC bit 1)
	 *
	 * 1. Check bits in IRQ2PEND and IRQ3PEND.
	 * 2. If either of them is 1, then the STATUS1 register tells current
	 * status of Jack/Mic. Connected if bit value is 1, removed otherwise.
	 */
	if ((pend2 & IRQ2_JACK_DET_R) || (pend3 & IRQ3_JACK_DET_F)) {
		det_status_change = true;
		jackdet = stat1 & BIT(STATUS1_JACK_DET_SHIFT);
		jd->jack_det = jackdet ? true : false;
	}


	if (det_status_change) {
		queue_delayed_work(cod9002x->jack_det_wq,
			&cod9002x->jack_det_work, msecs_to_jiffies(MIC_DETECT_DELAY));
		mutex_unlock(&cod9002x->key_lock);
		goto out;
	}

	if (cod9002x->use_btn_adc_mode) {
		/* start button work */
		queue_delayed_work(cod9002x->buttons_wq,
					&cod9002x->buttons_work, 5);
	} else {
		pr_err("[DEBUG] %s , line %d \n",__func__, __LINE__);
		/* need to implement button detection */
	}

	mutex_unlock(&cod9002x->key_lock);

out:
	if (cod9002x->is_suspend)
		regcache_cache_only(cod9002x->regmap, true);

	return IRQ_HANDLED;
}

int cod9002x_jack_mic_register(struct snd_soc_codec *codec)
{
	struct cod9002x_priv *cod9002x = snd_soc_codec_get_drvdata(codec);
	int i, ret;

	cod9002x->sdev.name = "h2w";

	ret = switch_dev_register(&cod9002x->sdev);
	if (ret < 0)
		dev_err(codec->dev, "Switch registration failed\n");

	cod9002x->input = devm_input_allocate_device(codec->dev);
	if (!cod9002x->input) {
		dev_err(codec->dev, "Failed to allocate input device\n");
		return -ENOMEM;
	}

	/* Not handling Headset events for now.Headset event handling
	 * registered as Input device, causing some conflict with Keyboard Input
	 * device.So, temporarily not handling Headset event, it will be enabled
	 * after proper fix.
	 */
	cod9002x->input->name = "Codec9002 Headset Events";
	cod9002x->input->phys = dev_name(codec->dev);
	cod9002x->input->id.bustype = BUS_I2C;

	cod9002x->input->evbit[0] = BIT_MASK(EV_KEY);
	for (i = 0; i < 4; i++)
		set_bit(cod9002x->jack_buttons_zones[i].code, cod9002x->input->keybit);
	cod9002x->input->dev.parent = codec->dev;
	input_set_drvdata(cod9002x->input, codec);

	ret = input_register_device(cod9002x->input);
	if (ret != 0) {
		cod9002x->input = NULL;
		dev_err(codec->dev, "Failed to register 9002 input device\n");
	}

#ifdef CONFIG_PM_RUNTIME
	pm_runtime_get_sync(codec->dev);
#else
	cod9002x_enable(codec->dev);
#endif




#ifdef CONFIG_PM_RUNTIME
	pm_runtime_put_sync(codec->dev);
#else
	cod9002x_disable(codec->dev);
#endif
	return 0;
}
EXPORT_SYMBOL_GPL(cod9002x_jack_mic_register);

static const struct snd_soc_dai_ops cod9002x_dai_ops = {
	.set_fmt = cod9002x_dai_set_fmt,
	.startup = cod9002x_dai_startup,
	.shutdown = cod9002x_dai_shutdown,
	.hw_params = cod9002x_dai_hw_params,
};

#define COD9002X_RATES		SNDRV_PCM_RATE_8000_192000

#define COD9002X_FORMATS	(SNDRV_PCM_FMTBIT_S16_LE |		\
				SNDRV_PCM_FMTBIT_S20_3LE |	\
				SNDRV_PCM_FMTBIT_S24_LE |	\
				SNDRV_PCM_FMTBIT_S32_LE)

static struct snd_soc_dai_driver cod9002x_dai[] = {
	{
		.name = "cod9002x-aif",
		.id = 1,
		.playback = {
			.stream_name = "AIF Playback",
			.channels_min = 1,
			.channels_max = 8,
			.rates = COD9002X_RATES,
			.formats = COD9002X_FORMATS,
		},
		.capture = {
			.stream_name = "AIF Capture",
			.channels_min = 1,
			.channels_max = 8,
			.rates = COD9002X_RATES,
			.formats = COD9002X_FORMATS,
		},
		.ops = &cod9002x_dai_ops,
		.symmetric_rates = 1,
	},
	{
		.name = "cod9002x-aif2",
		.id = 2,
		.playback = {
			.stream_name = "AIF2 Playback",
			.channels_min = 1,
			.channels_max = 2,
			.rates = COD9002X_RATES,
			.formats = COD9002X_FORMATS,
		},
		.capture = {
			.stream_name = "AIF2 Capture",
			.channels_min = 1,
			.channels_max = 2,
			.rates = COD9002X_RATES,
			.formats = COD9002X_FORMATS,
		},
		.ops = &cod9002x_dai_ops,
		.symmetric_rates = 1,
	},
};

static int cod9002x_regulators_enable(struct snd_soc_codec *codec)
{
	struct cod9002x_priv *cod9002x = snd_soc_codec_get_drvdata(codec);
	int ret;

	ret = regulator_enable(cod9002x->vdd);

	return ret;
}

static void cod9002x_regulators_disable(struct snd_soc_codec *codec)
{
	struct cod9002x_priv *cod9002x = snd_soc_codec_get_drvdata(codec);

	regulator_disable(cod9002x->vdd);
}

/* The clock for COD9002X is provided by the Audio sub-system. Hence we need to
 * ensure that the audio subsystem is active during codec operation. The
 * easiest way to do this is by calling s2803x_{get/put}_sync() helper
 * functions.
 */
static void cod9002x_clock_enable(struct snd_soc_codec *codec)
{
//	s2803x_get_sync();  //$$$_kjc
}

static void cod9002x_clock_disable(struct snd_soc_codec *codec)
{
//	s2803x_put_sync();  //$$$_kjc
}

static void cod9002x_save_otp_registers(struct snd_soc_codec *codec)
{
	int i;
	struct cod9002x_priv *cod9002x = snd_soc_codec_get_drvdata(codec);

	dev_dbg(codec->dev, "%s called\n", __func__);
	for (i = 0; i < COD9002X_OTP_MAX_REG; i++) {
		cod9002x->otp_reg[i] = (unsigned char) snd_soc_read(codec,
				(COD9002X_D0_CTRL_IREF1 + i));
	}
}

static void cod9002x_restore_otp_registers(struct snd_soc_codec *codec)
{
	int i;
	struct cod9002x_priv *cod9002x = snd_soc_codec_get_drvdata(codec);

	dev_dbg(codec->dev, "%s called\n", __func__);
	for (i = 0; i < COD9002X_OTP_MAX_REG; i++) {
		snd_soc_write(codec, (COD9002X_D0_CTRL_IREF1 + i),
					cod9002x->otp_reg[i]);
	}
}

static void cod9002x_reset_io_selector_bits(struct snd_soc_codec *codec)
{
	/* Reset input selector bits */
	snd_soc_update_bits(codec, COD9002X_75_CHOP_AD,
			EN_MIC_CHOP_MASK | EN_MCB1_CHOP_MASK |
			EN_MCB2_CHOP_MASK | EN_MIC3_CHOP_MASK,
			EN_MIC_CHOP_MASK);

	/* Reset output selector bits */
	snd_soc_update_bits(codec, COD9002X_76_CHOP_DA,
			EN_HP_CHOP_MASK | EN_EP_CHOP_MASK |
			EN_SPK_PGA_CHOP_MASK, 0x0);
}

/*
 * Configure the mic1 and mic2 bias voltages with default value or the value
 * received from the device tree.
 * Also configure the internal LDO voltage.
 */
static void cod9002x_configure_mic_bias(struct snd_soc_codec *codec)
{
	struct cod9002x_priv *cod9002x = snd_soc_codec_get_drvdata(codec);

	/* Configure Mic1 Bias Voltage */
	snd_soc_update_bits(codec, COD9002X_18_CTRL_REF,
			CTRV_MCB1_MASK,
			(cod9002x->mic_bias1_voltage << CTRV_MCB1_SHIFT));

	/* Configure Mic2 Bias Voltage */
	snd_soc_update_bits(codec, COD9002X_82_MIC_BIAS,
			CTRV_MCB2_MASK,
			(cod9002x->mic_bias2_voltage << CTRV_MCB2_SHIFT));

	/* Configure Mic Bias LDO Voltage */
	snd_soc_update_bits(codec, COD9002X_82_MIC_BIAS,
			CTRV_MCB_LDO_MASK,
			(cod9002x->mic_bias_ldo_voltage << CTRV_MCB_LDO_SHIFT));
}

/**
 * cod9002x_post_fw_update_failure: To be called if f/w update fails
 *
 * In case the firmware is not present or corrupt, we should still be able to
 * run the codec with decent parameters. This values are updated as per the
 * latest stable firmware.
 *
 * The values provided in this function are hard-coded register values, and we
 * need not update these values as per bit-fields.
 */
static void cod9002x_post_fw_update_failure(void *context)
{
	struct snd_soc_codec *codec = (struct snd_soc_codec *)context;
	struct cod9002x_priv *cod9002x = snd_soc_codec_get_drvdata(codec);
	unsigned int detb_period = CTMF_DETB_PERIOD_2048;
	unsigned int jd_mask = EN_PDB_JD_MASK;

	dev_dbg(codec->dev, "%s called, setting defaults\n", __func__);

	if (cod9002x->use_external_jd) {
		detb_period = CTMF_DETB_PERIOD_8;
		jd_mask = 0;
	} else if (cod9002x->use_btn_adc_mode)
		detb_period = CTMF_DETB_PERIOD_8;

#ifdef CONFIG_PM_RUNTIME
	pm_runtime_get_sync(codec->dev);
#else
	cod9002x_enable(codec->dev);
#endif

	if (cod9002x->use_external_jd) {
		snd_soc_update_bits(codec, 0x04,
				IRQ1M_MASK_ALL, 0xff);
		snd_soc_update_bits(codec, 0x05,
				IRQ2M_MASK_ALL, 0xff);
		snd_soc_update_bits(codec, 0x06,
				IRQ3M_MASK_ALL, 0xff);
	} else {
		snd_soc_update_bits(codec, 0x04,
				IRQ1M_MASK_ALL, 0xC0);
		snd_soc_update_bits(codec, 0x05,
				IRQ2M_MASK_ALL, 0xff);
		snd_soc_update_bits(codec, 0x06,
				IRQ3M_MASK_ALL, 0xff);
	}

	snd_soc_write(codec, 0x81, 0x02);
	mdelay(1);
	snd_soc_write(codec, 0x81, 0x03);
	snd_soc_write(codec, 0x83, 0x00);
	snd_soc_write(codec, 0x84, 0x51);
	snd_soc_write(codec, 0x85, 0xff);
	snd_soc_write(codec, 0x86, 0x04);
	snd_soc_write(codec, 0x96, 0x47);
	snd_soc_write(codec, 0x97, 0x02);

	/* Default value, enabling HPF and setting freq at 100Hz */
	snd_soc_write(codec, COD9002X_42_ADC1, 0x0c);

	snd_soc_update_bits(codec, COD9002X_71_CLK1_DA,
			SEL_CHCLK_DA_MASK | EN_HALF_CHOP_HP_MASK |
			EN_HALF_CHOP_DA_MASK,
			(DAC_CHOP_CLK_1_BY_32 << SEL_CHCLK_DA_SHIFT) |
			(DAC_HP_PHASE_SEL_3_BY_4 << EN_HALF_CHOP_HP_SHIFT) |
			(DAC_PHASE_SEL_1_BY_4 << EN_HALF_CHOP_DA_SHIFT));

	snd_soc_update_bits(codec, COD9002X_D0_CTRL_IREF1,
			CTMI_VCM_MASK | CTMI_MIX_MASK,
			(CTMI_VCM_4U << CTMI_VCM_SHIFT) | CTMI_MIX_2U);

	snd_soc_update_bits(codec, COD9002X_D1_CTRL_IREF2,
			CTMI_INT1_MASK, CTMI_INT1_4U);

	snd_soc_update_bits(codec, COD9002X_D2_CTRL_IREF3,
			CTMI_MIC2_MASK | CTMI_MIC1_MASK,
			(CTMI_MIC2_2U << CTMI_MIC2_SHIFT) | CTMI_MIC1_2U);

	snd_soc_update_bits(codec, COD9002X_D3_CTRL_IREF4,
		CTMI_MIC_BUFF_MASK | CTMI_MIC3_MASK,
		(CTMI_MIC_BUFF_2U << CTMI_MIC_BUFF_SHIFT) | CTMI_MIC3_2U);

	snd_soc_write(codec, COD9002X_43_ADC_L_VOL, 0x18);
	snd_soc_write(codec, COD9002X_44_ADC_R_VOL, 0x18);
	/* Boost 20 dB, Gain 0 dB for MIC1 */
	snd_soc_write(codec, COD9002X_20_VOL_AD1, 0x54);
	snd_soc_write(codec, COD9002X_21_VOL_AD2, 0x54);
	snd_soc_write(codec, COD9002X_22_VOL_AD3, 0x54);
	/* Gain 6dB for Line-in */
	snd_soc_write(codec, COD9002X_5A_DNC7, 0x18);

	/* Reset input/output selector bits */
	cod9002x_reset_io_selector_bits(codec);

	/* Configure mic bias voltage */
	cod9002x_configure_mic_bias(codec);

	/* All boot time hardware access is done. Put the device to sleep. */
#ifdef CONFIG_PM_RUNTIME
	pm_runtime_put_sync(codec->dev);
#else
	cod9002x_disable(codec->dev);
#endif
}

/**
 * cod9002x_post_fw_update_success: To be called after f/w update
 *
 * The firmware may be enabling some of the path and power registers which are
 * used during path enablement. We need to keep the values of these registers
 * consistent so that the functionality of the codec driver doesn't change
 * because of the firmware.
 */
static void cod9002x_post_fw_update_success(void *context)
{
	struct snd_soc_codec *codec = (struct snd_soc_codec *)context;
	struct cod9002x_priv *cod9002x = snd_soc_codec_get_drvdata(codec);
	unsigned int detb_period = CTMF_DETB_PERIOD_2048;
	unsigned int jd_mask = EN_PDB_JD_MASK;

	dev_dbg(codec->dev, "%s called\n", __func__);

	if (cod9002x->use_external_jd) {
		detb_period = CTMF_DETB_PERIOD_8;
		jd_mask = 0;
	} else if (cod9002x->use_btn_adc_mode)
		detb_period = CTMF_DETB_PERIOD_8;

#ifdef CONFIG_PM_RUNTIME
	pm_runtime_get_sync(codec->dev);
#else
	cod9002x_enable(codec->dev);
#endif

	if (cod9002x->use_external_jd) {
		snd_soc_update_bits(codec, COD9002X_06_IRQ1M,
				IRQ1M_MASK_ALL, 0xff);
		snd_soc_update_bits(codec, COD9002X_07_IRQ2M,
				IRQ2M_MASK_ALL, 0xff);
	} else {
		snd_soc_update_bits(codec, COD9002X_06_IRQ1M,
				IRQ1M_MASK_ALL, 0);
		snd_soc_update_bits(codec, COD9002X_07_IRQ2M,
				IRQ2M_MASK_ALL, 0);
	}

	/* Update for 3-pole jack detection */
	snd_soc_write(codec, COD9002X_85_MIC_DET, 0x03);

	/* Reset input/output selector bits */
	cod9002x_reset_io_selector_bits(codec);

	/* Reset the mixer switches for AD and DA */
	snd_soc_write(codec, COD9002X_23_MIX_AD1, 0x0);
	snd_soc_write(codec, COD9002X_36_MIX_DA1, 0x0);
	snd_soc_write(codec, COD9002X_37_MIX_DA2, 0x0);

	/* Reset the auto power bits for AD */
	snd_soc_update_bits(codec, COD9002X_16_PWAUTO_AD,
			APW_AUTO_AD_MASK | APW_MIC3_MASK |
			APW_MIC1_MASK | APW_MIC2_MASK,
			0x0);

	/* Reset the auto power bits for DA */
	snd_soc_update_bits(codec, COD9002X_17_PWAUTO_DA,
			PW_AUTO_DA_MASK | APW_SPK_MASK |
			APW_HP_MASK | APW_EP_MASK,
			0x0);

	/* Configure mic bias voltage */
	cod9002x_configure_mic_bias(codec);

	/*
	 * Need to restore back the device specific OTP values as the firmware
	 * binary might have corrupted the OTP values
	 */
	cod9002x_restore_otp_registers(codec);

	/* All boot time hardware access is done. Put the device to sleep. */
#ifdef CONFIG_PM_RUNTIME
	pm_runtime_put_sync(codec->dev);
#else
	cod9002x_disable(codec->dev);
#endif
}

static void cod9002x_regmap_sync(struct device *dev)
{
	struct cod9002x_priv *cod9002x = dev_get_drvdata(dev);
	unsigned char reg[COD9002X_MAX_REGISTER];
	int i;

	/* Read from Cache */
	for (i = 0; i <COD9002X_REGCACHE_SYNC_END_REG ; i++)
		if (cod9002x_readable_register(dev, i) &&
				(!cod9002x_volatile_register(dev,i)))
			reg[i] = (unsigned char)
				snd_soc_read(cod9002x->codec, i);

	snd_soc_write(cod9002x->codec, COD9002X_40_DIGITAL_POWER,
					reg[COD9002X_40_DIGITAL_POWER]);

	regcache_sync(cod9002x->regmap);
}

static void cod9002x_reg_restore(struct snd_soc_codec *codec)
{
	struct cod9002x_priv *cod9002x = snd_soc_codec_get_drvdata(codec);

	snd_soc_update_bits(codec, COD9002X_81_DET_ON,
			EN_PDB_JD_CLK_MASK, EN_PDB_JD_CLK_MASK);

	/* Give 15ms delay before storing the otp values */
	usleep_range(15000, 15000 + 1000);

	/*
	 * The OTP values are the boot-time values. For registers D0-DE, we need
	 * to save these register values during boot time. After system reset,
	 * these values are lost and we need to restore them using saved values.
	 */
	if (!cod9002x->is_probe_done) {
		cod9002x_regmap_sync(codec->dev);
		cod9002x_reset_io_selector_bits(codec);
		cod9002x_save_otp_registers(codec);
	} else {
		cod9002x_regmap_sync(codec->dev);
		cod9002x_restore_otp_registers(cod9002x->codec);
	}
}
static void cod9002x_i2c_parse_dt(struct cod9002x_priv *cod9002x)
{
	/* todo .. Need to add DT parsing for 9002 */
	struct device *dev = cod9002x->dev;
	struct device_node *np = dev->of_node;
	unsigned int bias_v_conf;
	int mic_range, mic_delay, btn_rel_val;
	struct of_phandle_args args;
	int i = 0;
	int ret;

	cod9002x->int_gpio = of_get_gpio(np, 0);

	if (cod9002x->int_gpio < 0)
		dev_err(dev, "(*)Error in getting Codec-9002 Interrupt gpio\n");

	/* Default Bias Voltages */
	cod9002x->mic_bias1_voltage = MIC_BIAS1_VO_3_0V;
	cod9002x->mic_bias2_voltage = MIC_BIAS2_VO_3_0V;
	cod9002x->mic_bias_ldo_voltage = MIC_BIAS_LDO_VO_3_3V;

	ret = of_property_read_u32(dev->of_node,
				"mic-bias1-voltage", &bias_v_conf);
	if ((!ret) && ((bias_v_conf >= MIC_BIAS1_VO_2_5V) &&
			(bias_v_conf <= MIC_BIAS1_VO_3_0V)))
		cod9002x->mic_bias1_voltage = bias_v_conf;
	else
		dev_dbg(dev, "Property 'mic-bias1-voltage' %s",
				ret ? "not found" : "invalid value (use:1-3)");

	ret = of_property_read_u32(dev->of_node,
				"mic-bias2-voltage", &bias_v_conf);
	if ((!ret) && ((bias_v_conf >= MIC_BIAS2_VO_2_5V) &&
			(bias_v_conf <= MIC_BIAS2_VO_3_0V)))
		cod9002x->mic_bias2_voltage = bias_v_conf;
	else
		dev_dbg(dev, "Property 'mic-bias2-voltage' %s",
				ret ? "not found" : "invalid value (use:1-3)");

	ret = of_property_read_u32(dev->of_node, "mic-adc-range", &mic_range);
	if (!ret)
		cod9002x->mic_adc_range = mic_range;
	else
		cod9002x->mic_adc_range = 1120;

	ret = of_property_read_u32(dev->of_node, "mic-det-delay", &mic_delay);
	if (!ret)
		cod9002x->mic_det_delay = mic_delay;
	else
		cod9002x->mic_det_delay = 50;

	ret = of_property_read_u32(dev->of_node, "btn-release-value", &btn_rel_val);
	if (!ret)
		cod9002x->btn_release_value = btn_rel_val;
	else
		cod9002x->btn_release_value = 1100;

	ret = of_property_read_u32(dev->of_node,
				"mic-bias-ldo-voltage", &bias_v_conf);
	if ((!ret) && ((bias_v_conf >= MIC_BIAS_LDO_VO_2_8V) &&
			(bias_v_conf <= MIC_BIAS_LDO_VO_3_3V)))
		cod9002x->mic_bias_ldo_voltage = bias_v_conf;
	else
		dev_dbg(dev, "Property 'mic-bias-ldo-voltage' %s",
				ret ? "not found" : "invalid value (use:1-3)");

	dev_dbg(dev, "Bias voltage values: bias1 = %d, bias2= %d, ldo = %d\n",
			cod9002x->mic_bias1_voltage,
			cod9002x->mic_bias2_voltage,
			cod9002x->mic_bias_ldo_voltage);

	if (of_find_property(dev->of_node,
				"update-firmware", NULL))
		cod9002x->update_fw = true;
	else
		cod9002x->update_fw = false;

	if (of_find_property(dev->of_node, "use-btn-adc-mode", NULL) != NULL)
		cod9002x->use_btn_adc_mode = true;

	dev_err(dev, "Using %s for button detection\n",
			cod9002x->use_btn_adc_mode ? "GPADC" : "internal h/w");

	if (cod9002x->use_btn_adc_mode) {
		/* Parsing but-zones, a maximum of 4 buttons are supported */
		for (i = 0; i < 4; i++) {
			if (of_parse_phandle_with_args(dev->of_node,
				"but-zones-list", "#list-but-cells", i, &args))
				break;

			cod9002x->jack_buttons_zones[i].code = args.args[0];
			cod9002x->jack_buttons_zones[i].adc_low = args.args[1];
			cod9002x->jack_buttons_zones[i].adc_high = args.args[2];
		}
		/* initialize button status */
		cod9002x->jack_det.privious_button_state = BUTTON_RELEASE;

		for (i = 0; i < 4; i++)
			dev_err(dev, "[DEBUG]: buttons: code(%d), low(%d), high(%d)\n",
				cod9002x->jack_buttons_zones[i].code,
				cod9002x->jack_buttons_zones[i].adc_low,
				cod9002x->jack_buttons_zones[i].adc_high);
	}
}

struct codec_notifier_struct {
	struct cod9002x_priv *cod9002x;
};
static struct codec_notifier_struct codec_notifier_t;

static int cod9002x_notifier_handler(struct notifier_block *nb , unsigned long insert, void *data)
{
	struct codec_notifier_struct *codec_notifier_data = data;
	struct cod9002x_priv *cod9002x = codec_notifier_data->cod9002x;
	struct cod9002x_jack_det *jd = &cod9002x->jack_det;
	unsigned int  stat1, pend1, pend2, pend3;
	int jackdet = COD9002X_MJ_DET_INVALID;
	bool det_status_change = false;

	pr_err("[DEBUG]%s codec notifier event call from mfd\n" , __func__);
	mutex_lock(&cod9002x->key_lock);

	if (cod9002x->is_suspend)
		regcache_cache_only(cod9002x->regmap, false);

	pend1 = cod9002x->irq_val[0];
	pend2 = cod9002x->irq_val[1];
	pend3 = cod9002x->irq_val[2];
	stat1 = cod9002x->irq_val[3];

	pr_err("[DEBUG] %s , line %d 01: %02x, 02:%02x, 03:%02x \n",__func__, __LINE__
					, pend1, pend2, pend3);
	/*
	 * Sequence for Jack/Mic detection
	 *
	 * (JACK bit 0, MIC bit 1)
	 *
	 * 1. Check bits in IRQ2PEND and IRQ3PEND.
	 * 2. If either of them is 1, then the STATUS1 register tells current
	 * status of Jack/Mic. Connected if bit value is 1, removed otherwise.
	 */
	if ((pend1 & IRQ2_JACK_DET_R) || (pend1 & IRQ3_JACK_DET_F)) {
		det_status_change = true;
		jackdet = stat1 & BIT(STATUS1_JACK_DET_SHIFT);
		jd->jack_det = jackdet ? true : false;
	}

	if (det_status_change) {
		queue_delayed_work(cod9002x->jack_det_wq,
			&cod9002x->jack_det_work, msecs_to_jiffies(MIC_DETECT_DELAY));
		mutex_unlock(&cod9002x->key_lock);
		goto out;
	}

	if (cod9002x->use_btn_adc_mode) {
		/* start button work */
		queue_delayed_work(cod9002x->buttons_wq,
					&cod9002x->buttons_work, 5);
	} else {
		pr_err("[DEBUG] %s , line %d \n",__func__, __LINE__);
		/* need to implement button detection */
	}

	mutex_unlock(&cod9002x->key_lock);

out:
	if (cod9002x->is_suspend)
		regcache_cache_only(cod9002x->regmap, true);

	return IRQ_HANDLED;
}
static BLOCKING_NOTIFIER_HEAD(cod9002x_notifier);

int cod9002x_register_notifier(struct notifier_block *n, struct cod9002x_priv *cod9002x)
{
	int ret;
	pr_err("[DEBUG] %s(%d)regiseter  \n", __func__, __LINE__);

	codec_notifier_t.cod9002x = cod9002x;
	ret = blocking_notifier_chain_register(&cod9002x_notifier, n);
	if(ret<0)
		pr_err("[DEBUG] %s(%d) \n", __func__, __LINE__);
	return ret;
}

void cod9002x_call_notifier(int irq1, int irq2, int irq3, int status1)
{
	struct cod9002x_priv *cod9002x = codec_notifier_t.cod9002x;

	pr_err("[DEBUG] %s(%d)  0x1: %02x 0x2: %02x 0x3: %02x \n", __func__, __LINE__, irq1, irq2, irq3);

	cod9002x->irq_val[0] = irq1;
	cod9002x->irq_val[1] = irq2;
	cod9002x->irq_val[2] = irq3;
	cod9002x->irq_val[3] = status1;

	blocking_notifier_call_chain(&cod9002x_notifier,0, &codec_notifier_t);
}
EXPORT_SYMBOL(cod9002x_call_notifier);

struct notifier_block codec_notifier;

static int cod9002x_codec_probe(struct snd_soc_codec *codec)
{
	int ret;
	struct cod9002x_priv *cod9002x = snd_soc_codec_get_drvdata(codec);

	int temp_irq;
	dev_dbg(codec->dev, "(*) %s\n", __func__);
	cod9002x->codec = codec;

	cod9002x->vdd = devm_regulator_get(codec->dev, "vdd");
	if (IS_ERR(cod9002x->vdd)) {
		dev_warn(codec->dev, "failed to get regulator vdd\n");
		return PTR_ERR(cod9002x->vdd);
	}

#ifdef CONFIG_PM_RUNTIME
	pm_runtime_get_sync(codec->dev);
#else
	cod9002x_enable(codec->dev);
#endif

	cod9002x->is_probe_done = true;

	/* Initialize work queue for button handling */
	INIT_DELAYED_WORK(&cod9002x->buttons_work, cod9002x_buttons_work);

	cod9002x->buttons_wq = create_singlethread_workqueue("buttons_wq");
	if (cod9002x->buttons_wq == NULL) {
		dev_err(codec->dev, "Failed to create buttons_wq\n");
		return -ENOMEM;
	}

	INIT_DELAYED_WORK(&cod9002x->jack_det_work , cod9002x_jack_det_work);

	cod9002x->jack_det_wq = create_singlethread_workqueue("jack_det_wq");
	if (cod9002x->jack_det_wq == NULL) {
		dev_err(codec->dev, "Failed to create jack_det_wq\n");
		return -ENOMEM;
	}

	cod9002x_adc_start(cod9002x);

	cod9002x->aifrate = COD9002X_SAMPLE_RATE_48KHZ;

	cod9002x_i2c_parse_dt(cod9002x);

	mutex_init(&cod9002x->jackdet_lock);
	mutex_init(&cod9002x->key_lock);

	/*
	 * interrupt pin should be shared with pmic.
	 * so codec driver use notifier because of knowing
	 * the interrupt status from mfd.
	 */
	codec_notifier.notifier_call = cod9002x_notifier_handler,
	cod9002x_register_notifier(&codec_notifier, cod9002x);

	if (cod9002x->update_fw)
		exynos_regmap_update_fw(COD9002X_FIRMWARE_NAME,
			codec->dev, cod9002x->regmap, cod9002x->i2c_addr,
			cod9002x_post_fw_update_success, codec,
			cod9002x_post_fw_update_failure, codec);
	else
		cod9002x_post_fw_update_failure(codec);

	// it should be modify to move machine driver
	cod9002x_jack_mic_register(codec);

	if (cod9002x->int_gpio > 0 ) {
		dev_err(codec->dev, "[DEBUG]%s : int_gpio %d\n",
					__func__, (int)cod9002x->int_gpio);
		ret = gpio_request(cod9002x->int_gpio, "cod9002x_irq");
		if (ret < 0) {
			dev_err(codec->dev, "%s : Request for %d GPIO failed\n",
					__func__, (int)cod9002x->int_gpio);
		}

		ret = gpio_direction_input(cod9002x->int_gpio);
		if (ret < 0) {
			dev_err(codec->dev,
			"Setting 9002 interrupt GPIO direction to input :failed\n");
		}
		temp_irq = gpio_to_irq(cod9002x->int_gpio);
		dev_err(codec->dev, "[DEBUG]%s : int_gpio %d\n",
					__func__, (int)temp_irq);
		ret = request_threaded_irq(
				temp_irq,
				NULL, cod9002x_threaded_isr,
	IRQF_TRIGGER_LOW | IRQF_SHARED,
				"cod9002_theaded_isr", cod9002x);
		if (ret < 0) {
			dev_err(codec->dev,
			"Error %d in requesting 9002 interrupt line:%d\n",
					ret, cod9002x->int_gpio);
		}

		ret = irq_set_irq_wake(gpio_to_irq(cod9002x->int_gpio), 1);
		if (ret < 0)
			dev_err(codec->dev, "cannot set 9002 irq_set_irq_wake\n");

	}

	snd_soc_dapm_ignore_suspend(&codec->dapm, "SPKOUTLN");
	snd_soc_dapm_ignore_suspend(&codec->dapm, "HPOUTLN");
	snd_soc_dapm_ignore_suspend(&codec->dapm, "EPOUTN");
	snd_soc_dapm_ignore_suspend(&codec->dapm, "IN1L");
	snd_soc_dapm_ignore_suspend(&codec->dapm, "IN2L");
	snd_soc_dapm_ignore_suspend(&codec->dapm, "IN3L");
	snd_soc_dapm_ignore_suspend(&codec->dapm, "IN4L");
	snd_soc_dapm_ignore_suspend(&codec->dapm, "AIF Playback");
	snd_soc_dapm_ignore_suspend(&codec->dapm, "AIF Capture");
	snd_soc_dapm_ignore_suspend(&codec->dapm, "AIF2 Playback");
	snd_soc_dapm_ignore_suspend(&codec->dapm, "AIF2 Capture");
	snd_soc_dapm_sync(&codec->dapm);


#ifdef CONFIG_PM_RUNTIME
	pm_runtime_put_sync(codec->dev);
#else
	cod9002x_disable(codec->dev);
#endif

	return 0;
}

static int cod9002x_codec_remove(struct snd_soc_codec *codec)
{
	struct cod9002x_priv *cod9002x = snd_soc_codec_get_drvdata(codec);
	dev_dbg(codec->dev, "(*) %s called\n", __func__);

	cancel_delayed_work_sync(&cod9002x->key_work);
	if (cod9002x->int_gpio) {
		free_irq(gpio_to_irq(cod9002x->int_gpio), cod9002x);
		gpio_free(cod9002x->int_gpio);
	}

	cod9002x_regulators_disable(codec);

	destroy_workqueue(cod9002x->buttons_wq);

	destroy_workqueue(cod9002x->jack_det_wq);

	cod9002x_adc_stop(cod9002x);

	return 0;
}

static struct snd_soc_codec_driver soc_codec_dev_cod9002x = {
	.probe = cod9002x_codec_probe,
	.remove = cod9002x_codec_remove,
	.controls = cod9002x_snd_controls,
	.num_controls = ARRAY_SIZE(cod9002x_snd_controls),
	.dapm_widgets = cod9002x_dapm_widgets,
	.num_dapm_widgets = ARRAY_SIZE(cod9002x_dapm_widgets),
	.dapm_routes = cod9002x_dapm_routes,
	.num_dapm_routes = ARRAY_SIZE(cod9002x_dapm_routes),
	.ignore_pmdown_time = true,
	.idle_bias_off = true,
};

static int cod9002x_i2c_probe(struct i2c_client *i2c,
		const struct i2c_device_id *id)
{
	struct cod9002x_priv *cod9002x;
	struct pinctrl *pinctrl;
	int ret;

	cod9002x = kzalloc(sizeof(struct cod9002x_priv), GFP_KERNEL);
	if (cod9002x == NULL)
		return -ENOMEM;
	cod9002x->dev = &i2c->dev;
	cod9002x->i2c_addr = i2c->addr;
	cod9002x->use_external_jd = false;
	cod9002x->is_probe_done = false;
	cod9002x->use_btn_adc_mode = false;

	cod9002x->regmap = devm_regmap_init_i2c(i2c, &cod9002x_regmap);
	if (IS_ERR(cod9002x->regmap)) {
		dev_err(&i2c->dev, "Failed to allocate regmap: %li\n",
				PTR_ERR(cod9002x->regmap));
		return PTR_ERR(cod9002x->regmap);
	}

	regcache_mark_dirty(cod9002x->regmap);

	pinctrl = devm_pinctrl_get(&i2c->dev);
	if (IS_ERR(pinctrl)) {
		dev_warn(&i2c->dev, "did not get pins for codec: %li\n",
							PTR_ERR(pinctrl));
	} else {
		cod9002x->pinctrl = pinctrl;
	}

	i2c_set_clientdata(i2c, cod9002x);

	ret = snd_soc_register_codec(&i2c->dev, &soc_codec_dev_cod9002x,
			cod9002x_dai, ARRAY_SIZE(cod9002x_dai));
	if (ret < 0)
		dev_err(&i2c->dev, "Failed to register codec: %d\n", ret);
#ifdef CONFIG_PM_RUNTIME
	pm_runtime_enable(cod9002x->dev);
#endif

	return ret;
}

static int cod9002x_i2c_remove(struct i2c_client *i2c)
{
	snd_soc_unregister_codec(&i2c->dev);
	return 0;
}

static void cod9002x_cfg_gpio(struct device *dev, const char *name)
{
	struct pinctrl_state *pin_state;
	struct cod9002x_priv *cod9002x = dev_get_drvdata(dev);

	pin_state = pinctrl_lookup_state(cod9002x->pinctrl, name);
	if (IS_ERR(pin_state))
		goto err;

	if (pinctrl_select_state(cod9002x->pinctrl, pin_state) < 0)
		goto err;

	return;
err:
	dev_err(dev, "Unable to configure codec gpio as %s\n", name);
	return;
}

static int cod9002x_enable(struct device *dev)
{
	struct cod9002x_priv *cod9002x = dev_get_drvdata(dev);

	dev_dbg(dev, "(*) %s\n", __func__);


	cod9002x_cfg_gpio(dev, "default");
	cod9002x_regulators_enable(cod9002x->codec);
	cod9002x_clock_enable(cod9002x->codec);
	/*
	 * Below sequence should be maintained, so that even the jd interupt
	 * changes the cache mode between below two line should not cause
	 * issue
	 */
	cod9002x->is_suspend = false;

	/* Disable cache_only feature and sync the cache with h/w */
	regcache_cache_only(cod9002x->regmap, false);
	cod9002x_reg_restore(cod9002x->codec);

	return 0;
}

static int cod9002x_disable(struct device *dev)
{
	struct cod9002x_priv *cod9002x = dev_get_drvdata(dev);

	dev_dbg(dev, "(*) %s\n", __func__);

	/*
	 * Below sequence should be maintained, so that even the jd interupt
	 * changes the cache mode between below two line should not cause
	 * issue
	 */
	cod9002x->is_suspend = true;

	/* As device is going to suspend-state, limit the writes to cache */
	regcache_cache_only(cod9002x->regmap, true);

	cod9002x_clock_disable(cod9002x->codec);
	cod9002x_regulators_disable(cod9002x->codec);
	cod9002x_cfg_gpio(dev, "idle");

	return 0;
}

static int cod9002x_sys_suspend(struct device *dev)
{
#ifndef CONFIG_PM_RUNTIME
	//$$$_kjc struct cod9002x_priv *cod9002x = dev_get_drvdata(dev);

	if (is_cp_aud_enabled()) {
		dev_dbg(dev, "(*)Don't suspend Codec-9002, cp functioning\n");
		return 0;
	}
	dev_dbg(dev, "(*) %s\n", __func__);
	cod9002x_disable(dev);
#endif

	return 0;
}

static int cod9002x_sys_resume(struct device *dev)
{
#ifndef CONFIG_PM_RUNTIME
	struct cod9002x_priv *cod9002x = dev_get_drvdata(dev);

	if (!cod9002x->is_suspend) {
		dev_dbg(dev, "(*)Codec-9002 not resuming, cp functioning\n");
		return 0;
	}
	dev_dbg(dev, "(*) %s\n", __func__);
	cod9002x_enable(dev);
#endif

	return 0;
}

#ifdef CONFIG_PM_RUNTIME
static int cod9002x_runtime_resume(struct device *dev)
{
	dev_dbg(dev, "(*) %s\n", __func__);

	cod9002x_enable(dev);

	return 0;
}

static int cod9002x_runtime_suspend(struct device *dev)
{
	dev_dbg(dev, "(*) %s\n", __func__);

	cod9002x_disable(dev);

	return 0;
}
#endif

static const struct dev_pm_ops cod9002x_pm = {
	SET_SYSTEM_SLEEP_PM_OPS(
			cod9002x_sys_suspend,
			cod9002x_sys_resume
	)
	SET_RUNTIME_PM_OPS(
			cod9002x_runtime_suspend,
			cod9002x_runtime_resume,
			NULL
	)
};

static const struct i2c_device_id cod9002x_i2c_id[] = {
	{ "cod9002x", 9002 },
	{ }
};
MODULE_DEVICE_TABLE(i2c, cod9002x_i2c_id);

const struct of_device_id cod9002x_of_match[] = {
	{ .compatible = "codec,cod9002x",},
	{},
};

static struct i2c_driver cod9002x_i2c_driver = {
	.driver = {
		.name = "cod9002x",
		.owner = THIS_MODULE,
		.pm = &cod9002x_pm,
		.of_match_table = of_match_ptr(cod9002x_of_match),
	},
	.probe = cod9002x_i2c_probe,
	.remove = cod9002x_i2c_remove,
	.id_table = cod9002x_i2c_id,
};

module_i2c_driver(cod9002x_i2c_driver);

MODULE_DESCRIPTION("ASoC COD9002X driver");
MODULE_LICENSE("GPL v2");
MODULE_ALIAS("platform:COD9002X-codec");
MODULE_FIRMWARE(COD9002X_FIRMWARE_NAME);
