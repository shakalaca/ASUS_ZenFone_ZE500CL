/*
 *  ctp_rhb_rt5647.c - ASoc Machine driver for Intel Cloverview MID platform
 *
 *  Copyright (C) 2011-13 Intel Corp
 *  Author: KP Jeeja<jeeja.kp@intel.com>
 *  Author: Vaibhav Agarwal <vaibhav.agarwal@intel.com>
 *  Author: Dharageswari.R<dharageswari.r@intel.com>
 *  ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; version 2 of the License.
 *
 *  This program is distributed in the hope that it will be useful, but
 *  WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License along
 *  with this program; if not, write to the Free Software Foundation, Inc.,
 *  59 Temple Place, Suite 330, Boston, MA 02111-1307 USA.
 *
 * ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
 */
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt
#define DEBUG 1

#include <linux/module.h>
#include <linux/init.h>
#include <linux/device.h>
#include <linux/slab.h>
#include <linux/io.h>
#include <linux/async.h>
#include <linux/delay.h>
#include <linux/gpio.h>
#include <asm/intel_sst_ctp.h>
#include <sound/pcm.h>
#include <sound/pcm_params.h>
#include <sound/soc.h>
#include <sound/jack.h>
#include "../../codecs/rt5647.h"
#include "ctp_rhb_rt5647.h"
#include "../ssp/mid_ssp.h"
#include "ctp_common_rt5647.h"

/*+++ ASUS_BSP : Eric +++*/
#include <linux/HWVersion.h>
extern int Read_HW_ID(void);
int rhb_HW_ID;
/*--- ASUS_BSP : Eric ---*/

/* ASUS_BSP Paul +++ */
#include <linux/switch.h>
static struct switch_dev *ringtone_preset_sdev;
int g_ringtone_preset_state;
int g_audiostream;
/* ASUS_BSP Paul --- */

/* As per the codec spec the mic2_sdet debounce delay is 20ms.
 * But having 20ms delay doesn't work */
#define MIC2SDET_DEBOUNCE_DELAY	50 /* 50 ms */
#define MICBIAS_NAME	"MIC2 Bias"

/* Headset jack detection gpios func(s) */
#define HPDETECT_DEBOUNCE_DELAY	50 /* 50 ms */

static unsigned int codec_clk_rate = DEFAULT_MCLK;
/* Configure I2S HW switch for audio route */
struct snd_soc_codec *rt5647_codec; // ASUS_BSP Paul +++

/* ALC5647 widgets */
static const struct snd_soc_dapm_widget ctp_dapm_widgets[] = {
	SND_SOC_DAPM_HP("Headphone", NULL),
	SND_SOC_DAPM_MIC("Headset Mic", NULL),
	SND_SOC_DAPM_MIC("Int Mic", NULL),
	SND_SOC_DAPM_SPK("Receiver", NULL),
	SND_SOC_DAPM_SPK("Ext Spk", ctp_amp_event),
	SND_SOC_DAPM_SPK("Int Spk", NULL),
};

/* ALC5647 Audio Map */
static const struct snd_soc_dapm_route ctp_sr_audio_map[] = {
	{"micbias2", NULL, "Headset Mic"},
	{"IN1P", NULL, "Headset Mic"},
	{"IN1N", NULL, "Headset Mic"},
	{"DMIC L2", NULL, "Int Mic"},
	{"DMIC R2", NULL, "Int Mic"},
	{"Headphone", NULL, "HPOL"},
	{"Headphone", NULL, "HPOR"},
	{"Int Spk", NULL, "SPOL"},
	{"Int Spk", NULL, "SPOR"},
	{"Receiver", NULL, "MonoP"},
	{"Receiver", NULL, "MonoN"},
};

static const struct snd_soc_dapm_route ctp_audio_map[] = {
	{"micbias2", NULL, "Headset Mic"},
	{"IN3P", NULL, "Headset Mic"},
	{"IN3N", NULL, "Headset Mic"},
	{"DMIC L2", NULL, "Int Mic"},
	{"DMIC R2", NULL, "Int Mic"},
	{"Headphone", NULL, "HPOL"},
	{"Headphone", NULL, "HPOR"},
	{"Ext Spk", NULL, "LOUTL"},
	{"Ext Spk", NULL, "LOUTR"},
	{"Receiver", NULL, "MonoP"},
	{"Receiver", NULL, "MonoN"},
};

#define SNDRV_BT_SCO_ENABLE		_IOW('S', 0x01, int)
#define SNDRV_VR_ACTIVE			_IOW('S', 0x02, int)
#define SNDRV_SET_AUDIO_MODE	_IOW('S', 0x03, int)
#define SNDRV_AUDIO_STREAM		_IOW('S', 0x04, int)
#define SNDRV_SET_RINGTONE_PRESET	_IOW('S', 0x05, int)
static int VRActive = -1;
static int audio_mode = -1;
static int stream = -1;

static int ctp_startup(struct snd_pcm_substream *substream)
{
	unsigned int device = substream->pcm->device;
	pr_debug("%s - applying rate constraint\n", __func__);
	switch (device) {
	case CTP_RHB_AUD_ASP_DEV:
	case CTP_RHB_AUD_PROBE_DEV:
	case CTP_RHB_AUD_VIRTUAL_ASP_DEV:
		snd_pcm_hw_constraint_list(substream->runtime, 0,
			SNDRV_PCM_HW_PARAM_RATE, &constraints_48000);
		break;
	case CTP_RHB_AUD_VSP_DEV:
		ctp_config_voicecall_flag(substream, true);
		break;
	default:
		pr_err("Invalid device\n");
		return -EINVAL;
	}
	return 0;
}


static int ctp_comms_dai_link_startup(struct snd_pcm_substream *substream)
{
	struct snd_pcm_runtime *str_runtime;

	str_runtime = substream->runtime;

	WARN(!substream->pcm, "CTP Comms Machine: ERROR NULL substream->pcm\n");

	if (!substream->pcm)
		return -EINVAL;

	/* set the runtime hw parameter with local snd_pcm_hardware struct */
	switch (substream->pcm->device) {
	case CTP_COMMS_BT_SCO_DEV:
		str_runtime->hw = BT_sco_hw_param;
		break;

	case CTP_COMMS_MSIC_VOIP_DEV:
		str_runtime->hw = VOIP_alsa_hw_param;
		break;

	case CTP_COMMS_IFX_MODEM_DEV:
		str_runtime->hw = IFX_modem_alsa_hw_param;
		break;
	default:
		pr_err("CTP Comms Machine: bad PCM Device = %d\n",
						substream->pcm->device);
	}
	return snd_pcm_hw_constraint_integer(str_runtime,
						SNDRV_PCM_HW_PARAM_PERIODS);
}

int ctp_startup_fm_xsp(struct snd_pcm_substream *substream)
{
	pr_debug("%s - applying rate constraint\n", __func__);
	snd_pcm_hw_constraint_list(substream->runtime, 0,
				SNDRV_PCM_HW_PARAM_RATE,
				&constraints_48000);
	return 0;
}

int ctp_set_asp_clk_fmt(struct snd_soc_dai *codec_dai)
{
	unsigned int fmt;
	int ret;

	/* ALC5647  Slave Mode`*/
	fmt =   SND_SOC_DAIFMT_I2S | SND_SOC_DAIFMT_NB_NF
		| SND_SOC_DAIFMT_CBS_CFS;

	/* Set codec DAI configuration */
	ret = snd_soc_dai_set_fmt(codec_dai, fmt);

	if (ret < 0) {
		pr_err("can't set codec DAI configuration %d\n", ret);
		return ret;
	}

	ret = snd_soc_dai_set_pll(codec_dai, 0, RT5647_PLL1_S_MCLK,
				  DEFAULT_MCLK, codec_clk_rate);
	if (ret < 0) {
		pr_err("can't set codec pll: %d\n", ret);
		return ret;
	}
	ret = snd_soc_dai_set_sysclk(codec_dai, RT5647_SCLK_S_PLL1,
					codec_clk_rate, SND_SOC_CLOCK_IN);

	if (ret < 0) {
		pr_err("can't set codec clock %d\n", ret);
		return ret;
	}
	return 0;
}

static int ctp_asp_hw_params(struct snd_pcm_substream *substream,
		struct snd_pcm_hw_params *params)
{
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct snd_soc_dai *codec_dai = rtd->codec_dai;
	codec_clk_rate = params_rate(params) * 512;
	return ctp_set_asp_clk_fmt(codec_dai);
}

static int clv_asp_set_params(struct snd_compr_stream *cstream)
{
	struct snd_soc_pcm_runtime *rtd = cstream->private_data;
	struct snd_soc_dai *codec_dai = rtd->codec_dai;

	return ctp_set_asp_clk_fmt(codec_dai);
}

static int ctp_vsp_hw_params(struct snd_pcm_substream *substream,
	struct snd_pcm_hw_params *params)
{
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct snd_soc_dai *codec_dai = rtd->codec_dai;
	unsigned int fmt;
	int ret , clk_source;

	pr_debug("Slave Mode selected\n");
	/* ALC5647 Slave Mode`*/
	fmt =   SND_SOC_DAIFMT_I2S | SND_SOC_DAIFMT_NB_NF
		| SND_SOC_DAIFMT_CBS_CFS;
	clk_source = SND_SOC_CLOCK_IN;

	/* Set codec DAI configuration */
	ret = snd_soc_dai_set_fmt(codec_dai, fmt);
	if (ret < 0) {
		pr_err("can't set codec DAI configuration %d\n", ret);
		return ret;
	}

	ret = snd_soc_dai_set_pll(codec_dai, 0, RT5647_PLL1_S_MCLK,
				  DEFAULT_MCLK, params_rate(params) * 512);
	if (ret < 0) {
		pr_err("can't set codec pll: %d\n", ret);
		return ret;
	}
	ret = snd_soc_dai_set_sysclk(codec_dai, RT5647_SCLK_S_PLL1,
		params_rate(params) * 512, clk_source);

	if (ret < 0) {
		pr_err("can't set codec clock %d\n", ret);
		return ret;
	}
	return 0;
}
static int ctp_comms_dai_link_hw_params(struct snd_pcm_substream *substream,
				      struct snd_pcm_hw_params *params)
{
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct snd_soc_dai *cpu_dai = rtd->cpu_dai;
	struct snd_soc_card *soc_card = rtd->card;
	struct ctp_mc_private *ctx = snd_soc_card_get_drvdata(soc_card);
	struct comms_mc_private *ctl = &(ctx->comms_ctl);

	int ret = 0;
	unsigned int tx_mask, rx_mask;
	unsigned int nb_slot = 0;
	unsigned int slot_width = 0;
	unsigned int tristate_offset = 0;
	unsigned int device = substream->pcm->device;


	pr_debug("ssp_bt_sco_master_mode %d\n", ctl->ssp_bt_sco_master_mode);
	pr_debug("ssp_voip_master_mode %d\n", ctl->ssp_voip_master_mode);
	pr_debug("ssp_modem_master_mode %d\n", ctl->ssp_modem_master_mode);

	pr_debug("\n ssp_voip_master_mode %d device %d\n",
		ctl->ssp_voip_master_mode, device);

	switch (device) {
	case CTP_COMMS_BT_SCO_DEV:
		/*
		 * set cpu DAI configuration
		 * frame_format = PSP_FORMAT
		 * ssp_serial_clk_mode = SSP_CLK_MODE_1
		 * ssp_frmsync_pol_bit = SSP_FRMS_ACTIVE_HIGH
		 */
		ret = snd_soc_dai_set_fmt(cpu_dai,
				SND_SOC_DAIFMT_I2S |
				SSP_DAI_SCMODE_1 |
				SND_SOC_DAIFMT_NB_NF |
				(ctl->ssp_bt_sco_master_mode ?
				SND_SOC_DAIFMT_CBM_CFM : SND_SOC_DAIFMT_CBS_CFS));

		if (ret < 0) {
			pr_err("MFLD Comms Machine: Set FMT Fails %d\n",
					ret);
			return -EINVAL;
		}

		/*
		 * BT SCO SSP Config
		 * ssp_active_tx_slots_map = 0x01
		 * ssp_active_rx_slots_map = 0x01
		 * frame_rate_divider_control = 1
		 * data_size = 16
		 * tristate = 1
		 * ssp_frmsync_timing_bit = 0
		 * (NEXT_FRMS_ASS_AFTER_END_OF_T4)
		 * ssp_frmsync_timing_bit = 1
		 * (NEXT_FRMS_ASS_WITH_LSB_PREVIOUS_FRM)
		 * ssp_psp_T2 = 1
		 * (Dummy start offset = 1 bit clock period)
		 */
		nb_slot = SSP_BT_SLOT_NB_SLOT;
		slot_width = SSP_BT_SLOT_WIDTH;
		tx_mask = SSP_BT_SLOT_TX_MASK;
		rx_mask = SSP_BT_SLOT_RX_MASK;

		if (ctl->ssp_bt_sco_master_mode)
			tristate_offset = BIT(TRISTATE_BIT);
		else
			tristate_offset = BIT(FRAME_SYNC_RELATIVE_TIMING_BIT);
		break;
	case CTP_COMMS_MSIC_VOIP_DEV:
		/*
		 * set cpu DAI configuration
		 * frame_format = PSP_FORMAT
		 * ssp_serial_clk_mode = SSP_CLK_MODE_0
		 * ssp_frmsync_pol_bit = SSP_FRMS_ACTIVE_LOW
		 */

		pr_debug("CTP_COMMS_MSIC_VOIP_:EV ssp_voip_master_mode %d\n",
			ctl->ssp_voip_master_mode);

		ret = snd_soc_dai_set_fmt(cpu_dai,
				SND_SOC_DAIFMT_I2S |
				SSP_DAI_SCMODE_0 |
				SND_SOC_DAIFMT_NB_IF |
				(ctl->ssp_voip_master_mode ?
				SND_SOC_DAIFMT_CBM_CFM : SND_SOC_DAIFMT_CBS_CFS));

		if (ret < 0) {
			pr_err("MFLD Comms Machine: Set FMT Fails %d\n",
							ret);
			return -EINVAL;
		}

		/*
		 * MSIC VOIP SSP Config
		 * ssp_active_tx_slots_map = 0x01
		 * ssp_active_rx_slots_map = 0x01
		 * frame_rate_divider_control = 1
		 * data_size = 32
		 * tristate = 1
		 * ssp_frmsync_timing_bit = 0, for SLAVE
		 * (NEXT_FRMS_ASS_AFTER_END_OF_T4)
		 * ssp_frmsync_timing_bit = 1, for MASTER
		 * (NEXT_FRMS_ASS_WITH_LSB_PREVIOUS_FRM)
		 *
		 *
		 */
		nb_slot = SSP_VOIP_SLOT_NB_SLOT;
		slot_width = SSP_VOIP_SLOT_WIDTH;
		tx_mask = SSP_VOIP_SLOT_TX_MASK;
		rx_mask = SSP_VOIP_SLOT_RX_MASK;

		tristate_offset = BIT(TRISTATE_BIT);
		break;

	case CTP_COMMS_IFX_MODEM_DEV:
		/*
		 * set cpu DAI configuration
		 * frame_format = PSP_FORMAT
		 * ssp_serial_clk_mode = SSP_CLK_MODE_0
		 * ssp_frmsync_pol_bit = SSP_FRMS_ACTIVE_HIGH
		 */
		ret = snd_soc_dai_set_fmt(cpu_dai,
				SND_SOC_DAIFMT_I2S |
				SSP_DAI_SCMODE_0 |
				SND_SOC_DAIFMT_NB_NF |
				(ctl->ssp_modem_master_mode ?
				SND_SOC_DAIFMT_CBM_CFM : SND_SOC_DAIFMT_CBS_CFS));
		if (ret < 0) {
			pr_err("MFLD Comms Machine:  Set FMT Fails %d\n", ret);
			return -EINVAL;
		}

		/*
		 * IFX Modem Mixing SSP Config
		 * ssp_active_tx_slots_map = 0x01
		 * ssp_active_rx_slots_map = 0x01
		 * frame_rate_divider_control = 1
		 * data_size = 32
		 * Master:
		 *	tristate = 3
		 *	ssp_frmsync_timing_bit = 1, for MASTER
		 *	(NEXT_FRMS_ASS_WITH_LSB_PREVIOUS_FRM)
		 * Slave:
		 *	tristate = 1
		 *	ssp_frmsync_timing_bit = 0, for SLAVE
		 *	(NEXT_FRMS_ASS_AFTER_END_OF_T4)
		 *
		 */
		nb_slot = SSP_IFX_SLOT_NB_SLOT;
		slot_width = SSP_IFX_SLOT_WIDTH;
		tx_mask = SSP_IFX_SLOT_TX_MASK;
		rx_mask = SSP_IFX_SLOT_RX_MASK;

		tristate_offset = BIT(TRISTATE_BIT) |
				BIT(FRAME_SYNC_RELATIVE_TIMING_BIT);

		break;
	default:
		pr_err("CTP Comms Machine: bad PCM Device ID = %d\n",
				device);
		return -EINVAL;
	}

	ret = snd_soc_dai_set_tdm_slot(cpu_dai, tx_mask,
			rx_mask, nb_slot, slot_width);

	if (ret < 0) {
		pr_err("CTP Comms Machine:  Set TDM Slot Fails %d\n",
				ret);
		return -EINVAL;
	}

	ret = snd_soc_dai_set_tristate(cpu_dai, tristate_offset);
	if (ret < 0) {
		pr_err("CTP Comms Machine: Set Tristate Fails %d\n",
				ret);
		return -EINVAL;
	}

	if (device == CTP_COMMS_MSIC_VOIP_DEV) {
		pr_debug("Call ctp_vsp_hw_params to enable the PLL Codec\n");
		ctp_vsp_hw_params(substream, params);
	}

	pr_debug("CTP Comms Machine: slot_width = %d\n",
			slot_width);
	pr_debug("CTP Comms Machine: tx_mask = %d\n",
			tx_mask);
	pr_debug("CTP Comms Machine: rx_mask = %d\n",
			rx_mask);
	pr_debug("CTP Comms Machine: tristate_offset = %d\n",
			tristate_offset);

	return 0;

} /* ctp_comms_dai_link_hw_params*/

static int ctp_comms_dai_link_prepare(struct snd_pcm_substream *substream)
{
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct snd_soc_dai *cpu_dai = rtd->cpu_dai;
	struct ctp_mc_private *ctx = snd_soc_card_get_drvdata(rtd->card);
	struct comms_mc_private *ctl = &(ctx->comms_ctl);

	unsigned int device = substream->pcm->device;

	pr_debug("%s substream->runtime->rate %d\n",
			__func__,
			substream->runtime->rate);

	/* select clock source (if master) */
	/* BT SCO: CPU DAI is master */
	/* FM: CPU DAI is master */
	/* BT_VOIP: CPU DAI is master */
	if (((device == CTP_COMMS_BT_SCO_DEV &&
		ctl->ssp_bt_sco_master_mode) ||
		((device == CTP_COMMS_MSIC_VOIP_DEV) &&
		ctl->ssp_voip_master_mode)) ||
		(device == CTP_COMMS_IFX_MODEM_DEV &&
		ctl->ssp_modem_master_mode)) {

		snd_soc_dai_set_sysclk(cpu_dai, SSP_CLK_ONCHIP,
				substream->runtime->rate, 0);

	}

	return 0;
} /* ctp_comms_dai_link_prepare */

static const struct snd_kcontrol_new ssp_comms_controls[] = {
		SOC_ENUM_EXT("SSP BT Master Mode",
				ssp_bt_sco_master_mode_enum,
				get_ssp_bt_sco_master_mode,
				set_ssp_bt_sco_master_mode),
		SOC_ENUM_EXT("SSP VOIP Master Mode",
				ssp_voip_master_mode_enum,
				get_ssp_voip_master_mode,
				set_ssp_voip_master_mode),
		SOC_ENUM_EXT("SSP Modem Master Mode",
				ssp_modem_master_mode_enum,
				get_ssp_modem_master_mode,
				set_ssp_modem_master_mode),
		SOC_DAPM_PIN_SWITCH("Headphone"),
		SOC_DAPM_PIN_SWITCH("Headset Mic"),
		SOC_DAPM_PIN_SWITCH("Ext Spk"),
		SOC_DAPM_PIN_SWITCH("Int Mic"),
		SOC_DAPM_PIN_SWITCH("Receiver"),
};

int switch_ctrl_open(struct inode *i_node, struct file *file_ptr)
{
	pr_debug("%s\n", __func__);
	return 0;
}

int switch_ctrl_release(struct inode *i_node, struct file *file_ptr)
{
	pr_debug("%s\n", __func__);
	return 0;
}

long switch_ctrl_ioctl(struct file *file_ptr,
		unsigned int cmd, unsigned long arg)
{
	pr_debug("%s\n", __func__);

	switch (_IOC_NR(cmd)) {
	case _IOC_NR(SNDRV_BT_SCO_ENABLE): {
		int bt_enable;
		if (copy_from_user(&bt_enable, (void __user *)arg,
				   sizeof(bt_enable)))
			return -EFAULT;
		pr_info("%s: BT SCO status %d\n", __func__, bt_enable);

		if (rt5647_codec)
			rt5647_i2s2_func_switch(rt5647_codec, !bt_enable);
		break;
	}
	case _IOC_NR(SNDRV_VR_ACTIVE): {
		int vr_active;

		if (copy_from_user(&vr_active,
			(void __user *)arg, sizeof(vr_active)))
			return -EFAULT;

		if (vr_active)
			VRActive = 1;
		else
			VRActive = 0;

		pr_debug("%s : VR status %d %d\n", __func__, vr_active, VRActive);
		break;
	}
	case _IOC_NR(SNDRV_SET_AUDIO_MODE): {
		int lmode;

		if (copy_from_user(&lmode,
			(void __user *)arg, sizeof(lmode)))
			return -EFAULT;

		audio_mode = lmode;

		pr_debug("%s : Audio Mode status %d %d\n", __func__, lmode, audio_mode);

		break;
	}
	case _IOC_NR(SNDRV_AUDIO_STREAM): {
		int lstream;

		if (copy_from_user(&lstream,
			(void __user *)arg, sizeof(lstream)))
			return -EFAULT;

		g_audiostream = lstream;
		rt5647_update_spk_drc();

		pr_debug("%s : AUDIO STREAM status %d\n", __func__, g_audiostream);

#if 0
		if (stream == 2) {		/* AudioSystem::RING == 2 */
			pr_debug("%s : Set PF450CL RingTone to 1\n", __func__);
			bRing_eq = 1;
			set_pf450cl_ring_eq();
		} else if (stream == 4) {		/* AudioSystem::ALARM == 4 */
			pr_debug("%s : Set PF450CL ALARM to 1\n", __func__);
			bAlarm_eq = 1;
			set_pf450cl_alarm_eq();
		} else {
			pr_debug("%s : Play others stream, restore normal EQ\n", __func__);
			bRing_eq = 0;
			bAlarm_eq = 0;
			set_pf450cl_normal_eq();
		}
#endif

		break;
	}
	/* ASUS_BSP Paul +++ */
	case _IOC_NR(SNDRV_SET_RINGTONE_PRESET): {
		int ringtone_preset_state;

		if (copy_from_user(&ringtone_preset_state,
			(void __user *)arg, sizeof(ringtone_preset_state)))
			return -EFAULT;

		switch_set_state(ringtone_preset_sdev, ringtone_preset_state);
		g_ringtone_preset_state = ringtone_preset_state;
		rt5647_update_spk_drc();

		pr_info("%s: set ringtone preset state %d\n", __func__, ringtone_preset_state);

		break;
	}
	/* ASUS_BSP Paul --- */
	default:
		pr_err("%s: command not supported.\n", __func__);
		return -EINVAL;
	}
	return 0;
}

int is_vr_active(void)
{
	pr_debug("%s() : VRActive %d\n", __func__, VRActive);
	return VRActive;
}
EXPORT_SYMBOL(is_vr_active);

int get_audiomode(void)
{
	pr_debug("%s() : Audio Mode %d\n", __func__, audio_mode);
	return audio_mode;
}
EXPORT_SYMBOL(get_audiomode);

static const struct file_operations switch_ctrl_fops = {
	.owner = THIS_MODULE,
	.open = switch_ctrl_open,
	.release = switch_ctrl_release,
	.unlocked_ioctl = switch_ctrl_ioctl,
};

static struct miscdevice switch_ctrl = {
	.minor = MISC_DYNAMIC_MINOR, /* dynamic allocation */
	.name = "switch_ctrl", /* /dev/bt_switch_ctrl */
	.fops = &switch_ctrl_fops
};



int ctp_init(struct snd_soc_pcm_runtime *runtime)
{
	int ret;
	struct snd_soc_codec *codec = runtime->codec;
	struct snd_soc_dapm_context *dapm = &codec->dapm;
	struct snd_soc_card *card = runtime->card;
	struct ctp_mc_private *ctx = snd_soc_card_get_drvdata(runtime->card);
	static int hp_enable;

	pr_debug("%s\n", __func__);

	VRActive = 0;
	audio_mode = 0;
	stream = 0;

	/* Set codec bias level */
	ctp_set_bias_level(card, dapm, SND_SOC_BIAS_OFF);
	card->dapm.idle_bias_off = true;

	/* Add Jack specific widgets */
	ret = snd_soc_dapm_new_controls(dapm, ctp_dapm_widgets,
					ARRAY_SIZE(ctp_dapm_widgets));
	if (ret)
		return ret;


       rhb_HW_ID = Read_HW_ID();
	if (rhb_HW_ID <= HW_ID_SR1) {
		/* Set up Jack specific audio path audio_map */
		snd_soc_dapm_add_routes(dapm, ctp_sr_audio_map,
						ARRAY_SIZE(ctp_sr_audio_map));
	} else {
		/* Set up Jack specific audio path audio_map */
		snd_soc_dapm_add_routes(dapm, ctp_audio_map,
						ARRAY_SIZE(ctp_audio_map));
	}

	/* Add Comms specefic controls */
	ctx->comms_ctl.ssp_bt_sco_master_mode = false;

/* Set ssp_voip to master mode by default */
#if 0
	ctx->comms_ctl.ssp_voip_master_mode = false;
#else
	ctx->comms_ctl.ssp_voip_master_mode = true;
	pr_debug("ctp_init ssp_voip_master_mode %d\n",
		ctx->comms_ctl.ssp_voip_master_mode);
#endif

	ctx->comms_ctl.ssp_modem_master_mode = false;

	ret = snd_soc_add_card_controls(card, ssp_comms_controls,
				ARRAY_SIZE(ssp_comms_controls));
	if (ret) {
		pr_err("Add Comms Controls failed %d",
				ret);
		return ret;
	}

	/* Keep the voice call paths active during
	suspend. Mark the end points ignore_suspend */
	snd_soc_dapm_ignore_suspend(dapm, "HPOL");
	snd_soc_dapm_ignore_suspend(dapm, "HPOR");

	snd_soc_dapm_ignore_suspend(dapm, "SPOL");
	snd_soc_dapm_ignore_suspend(dapm, "SPOR");

	snd_soc_dapm_ignore_suspend(dapm, "MonoP");
	snd_soc_dapm_ignore_suspend(dapm, "MonoN");
	snd_soc_dapm_ignore_suspend(dapm, "IN1P");
	snd_soc_dapm_ignore_suspend(dapm, "IN1N");
    snd_soc_dapm_ignore_suspend(dapm, "IN2P");/* ASUS_BSP Rice +++ */
    snd_soc_dapm_ignore_suspend(dapm, "IN2N");/* ASUS_BSP Rice +++ */
	snd_soc_dapm_ignore_suspend(dapm, "DMIC L1");
	snd_soc_dapm_ignore_suspend(dapm, "DMIC R1");
	snd_soc_dapm_ignore_suspend(dapm, "DMIC L2");
	snd_soc_dapm_ignore_suspend(dapm, "DMIC R2");
	snd_soc_dapm_ignore_suspend(dapm, "LOUTL");
	snd_soc_dapm_ignore_suspend(dapm, "LOUTR");

	snd_soc_dapm_ignore_suspend(dapm, "Headset Mic");
	snd_soc_dapm_ignore_suspend(dapm, "Headphone");
	snd_soc_dapm_ignore_suspend(dapm, "Ext Spk");
	snd_soc_dapm_ignore_suspend(dapm, "Int Spk");
	snd_soc_dapm_ignore_suspend(dapm, "Int Mic");
	snd_soc_dapm_ignore_suspend(dapm, "Receiver");
	snd_soc_dapm_ignore_suspend(dapm, "AIF2 Playback");
	snd_soc_dapm_ignore_suspend(dapm, "AIF2 Capture");
	snd_soc_dapm_ignore_suspend(dapm, "AIF2TX");
	snd_soc_dapm_ignore_suspend(dapm, "AIF2RX");

	snd_soc_dapm_enable_pin(dapm, "Headset Mic");
	snd_soc_dapm_enable_pin(dapm, "Headphone");
	snd_soc_dapm_enable_pin(dapm, "Ext Spk");
	snd_soc_dapm_enable_pin(dapm, "Int Mic");
	snd_soc_dapm_enable_pin(dapm, "Receiver");

	mutex_lock(&codec->mutex);
	snd_soc_dapm_sync(dapm);
	mutex_unlock(&codec->mutex);

	/*Register switch Control as misc driver*/
	ret = misc_register(&switch_ctrl);
	if (ret)
		pr_err("%s: couldn't register control device\n",
			__func__);

	rt5647_codec = codec;

	/* use hard-coded GPIO value before IFWI ready */
	/* hp_enable = get_gpio_by_name("AUDIO_DEBUG"); */
	hp_enable = 172;
	if (hp_enable > 0) {
		pr_info("Get AUDIO_DEBUG name!\n");
		ret = gpio_request_one(hp_enable, GPIOF_DIR_OUT, "AUDIO_DEBUG");
		if (ret)
			pr_err("gpio_request AUDIO_DEBUG failed!\n");

		/*Set GPIO O(H) to default => Low:UART; High:headset */
		gpio_direction_output(hp_enable, 0);
		pr_info("AUDIO_DEBUG value = %d\n", gpio_get_value(hp_enable));
	} else
		pr_err("get_gpio AUDIO_DEBUG failed!\n");

	/* ASUS_BSP Paul +++ */
	ringtone_preset_sdev = kzalloc(sizeof(struct switch_dev), GFP_KERNEL);
	if (!ringtone_preset_sdev)
		return -ENOMEM;

	ringtone_preset_sdev->name = "ringtone_preset";
	ringtone_preset_sdev->state = 0;

	ret = switch_dev_register(ringtone_preset_sdev);
	if (ret < 0)
		printk("Failed to register switch ringtone_preset\n");

	g_ringtone_preset_state = 0;
	g_audiostream = -1;
	/* ASUS_BSP Paul --- */

	return ret;
}

static struct snd_soc_ops ctp_asp_ops = {
	.startup = ctp_startup_asp,
	.hw_params = ctp_asp_hw_params,
};

static struct snd_soc_compr_ops ctp_asp_compr_ops = {
	.set_params = clv_asp_set_params,
};

static struct snd_soc_ops ctp_vsp_ops = {
	.hw_params = ctp_vsp_hw_params,
};
static struct snd_soc_ops ctp_comms_dai_link_ops = {
	.startup = ctp_comms_dai_link_startup,
	.hw_params = ctp_comms_dai_link_hw_params,
	.prepare = ctp_comms_dai_link_prepare,
};
static struct snd_soc_ops ctp_comms_voip_dai_link_ops = {
	.startup = ctp_comms_dai_link_startup,
	.hw_params = ctp_comms_dai_link_hw_params,
	.prepare = ctp_comms_dai_link_prepare,
};

static struct snd_soc_ops ctp_probe_ops = {
	.startup = ctp_startup,
};

static struct snd_soc_dai_link ctp_rhb_dailink[] = {
	[CTP_AUD_ASP_DEV] = {
		.name = "Cloverview ASP",
		.stream_name = "Audio",
		.cpu_dai_name = "Headset-cpu-dai",
		.codec_dai_name = "rt5647-aif1",
		.codec_name = "rt5647.1-001b",
		.platform_name = "sst-platform",
		.init = snd_ctp_init,
		.ignore_suspend = 1,
		.ops = &ctp_asp_ops,
		.playback_count = 2,
	},
	[CTP_AUD_VSP_DEV] = {
		.name = "Cloverview VSP",
		.stream_name = "Voice",
		.cpu_dai_name = "Voice-cpu-dai",
		.codec_dai_name = "rt5647-aif2",
		.codec_name = "rt5647.1-001b",
		.platform_name = "sst-platform",
		.init = NULL,
		.ignore_suspend = 1,
		.ops = &ctp_vsp_ops,
	},
	[CTP_AUD_COMP_ASP_DEV] = {
		.name = "Cloverview Comp ASP",
		.stream_name = "Compress-Audio",
		.cpu_dai_name = "Compress-cpu-dai",
		.codec_dai_name = "rt5647-aif1",
		.codec_name = "rt5647.1-001b",
		.platform_name = "sst-platform",
		.init = NULL,
		.ignore_suspend = 1,
		.compr_ops = &ctp_asp_compr_ops,
	},
	[CTP_COMMS_BT_SCO_DEV] = {
		.name = "Cloverview Comms BT SCO",
		.stream_name = "BTSCO",
		.cpu_dai_name = SSP_BT_DAI_NAME,
		.codec_dai_name = "snd-soc-dummy-dai",
		.codec_name = "snd-soc-dummy",
		.platform_name = "mid-ssp-dai",
		.init = NULL,
		.ops = &ctp_comms_dai_link_ops,
	},
	[CTP_COMMS_MSIC_VOIP_DEV] = {
		.name = "Cloverview Comms MSIC VOIP",
		.stream_name = "VOIP",
		.cpu_dai_name = SSP_BT_DAI_NAME,
		.codec_dai_name = "rt5647-aif2",
		.codec_name = "rt5647.1-001b",
		.platform_name = "mid-ssp-dai",
		.init = NULL,
		.ops = &ctp_comms_voip_dai_link_ops,
	},
	[CTP_COMMS_IFX_MODEM_DEV] = {
		.name = "Cloverview Comms IFX MODEM",
		.stream_name = "IFX_MODEM_MIXING",
		.cpu_dai_name = SSP_MODEM_DAI_NAME,
		.codec_dai_name = "snd-soc-dummy-dai",
		.codec_name = "snd-soc-dummy",
		.platform_name = "mid-ssp-dai",
		.init = NULL,
		.ops = &ctp_comms_dai_link_ops,
	},
	[CTP_AUD_VIRTUAL_ASP_DEV] = {
		.name = "Cloverview virtual-ASP",
		.stream_name = "virtual-stream",
		.cpu_dai_name = "Virtual-cpu-dai",
		.codec_dai_name = "rt5647-aif1",
		.codec_name = "rt5647.1-001b",
		.platform_name = "sst-platform",
		.init = NULL,
		.ignore_suspend = 1,
		.ops = &ctp_asp_ops,
	},
	[CTP_RHB_AUD_PROBE_DEV] = {
		.name = "Cloverview Probe",
		.stream_name = "CTP Probe",
		.cpu_dai_name = "Probe-cpu-dai",
		.codec_dai_name = "snd-soc-dummy-dai",
		.codec_name = "snd-soc-dummy",
		.platform_name = "sst-platform",
		.init = NULL,
		.ops = &ctp_probe_ops,
	},
};

int ctp_hp_detection(struct snd_soc_codec *codec,
			struct snd_soc_jack *jack, int enable)
{
	int status;
	/* +++ ASUS_BSP : Eric */
	/* use different type headset detect */
	if (rhb_HW_ID <= HW_ID_SR1)
		status = rt5647_headset_detect_sr(codec, enable);
	else
		status = rt5647_headset_detect(codec, enable);
	/* --- ASUS_BSP : Eric */


	if (SND_JACK_HEADSET == status) {
		msleep(200);
		rt5647_enable_push_button_irq(codec, jack);
	}

	return status;
}
#if 0 /* Hook detect by IA GPIO */
int ctp_bp_detection(struct snd_soc_codec *codec,
			struct snd_soc_jack *jack, int enable)
{
	int status;

	pr_debug("%s\n", __func__);

	if (rt5647_button_detect(codec))
		status = jack->status | SND_JACK_BTN_0;
	else
		status = jack->status & ~SND_JACK_BTN_0;

	pr_debug("%s, enable = 0x%x, status = 0x%x\n",
		 __func__, enable, status);
	return status;
}
#else
int ctp_bp_detection(struct snd_soc_codec *codec,
			struct snd_soc_jack *jack, int enable)
{
	int status = 0;

	status = rt5647_check_irq_event(codec);

	pr_debug("%s:status = 0x%x\n", __func__, status);

	return status;
}
#endif
int ctp_dai_link(struct snd_soc_card *card)
{
	pr_debug("%s\n", __func__);
	card->dai_link = ctp_rhb_dailink;
	card->num_links = ARRAY_SIZE(ctp_rhb_dailink);
	return 0;
}

static void ctp_rhb_card_name(struct snd_soc_card *card)
{
	card->name = "cloverview_audio";
}

struct snd_soc_machine_ops ctp_rhb_ops = {
	.card_name = ctp_rhb_card_name,
	.ctp_init = ctp_init,
	.dai_link = ctp_dai_link,
	.bp_detection = ctp_bp_detection,
	.hp_detection = ctp_hp_detection,
	.mclk_switch = NULL,
	.jack_support = true,
	.dmic3_support = false,
	.micsdet_debounce = MIC2SDET_DEBOUNCE_DELAY,
	.mic_bias = MICBIAS_NAME,
};
MODULE_DESCRIPTION("ASoC Intel(R) Cloverview MID Machine driver");
MODULE_AUTHOR("Jeeja KP<jeeja.kp@intel.com>");
MODULE_AUTHOR("Dharageswari R<dharageswari.r@intel.com>");
MODULE_LICENSE("GPL v2");
MODULE_ALIAS("ipc:ctprt5647-audio");
