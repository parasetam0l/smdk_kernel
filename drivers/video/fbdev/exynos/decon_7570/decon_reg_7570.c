/* drivers/video/fbdev/exynos/decon_7570/decon_reg_7570.c
 *
 * Copyright 2015 Samsung Electronics
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
*/

/* use this definition when you test CAL on firmware */
/* #define FW_TEST */
#ifdef FW_TEST
#include "decon_fw.h"
#else
#include "decon.h"
#endif

/******************* CAL raw functions implementation *************************/
int decon_reg_reset(u32 id)
{
	int tries;

	decon_write(id, VIDCON0, VIDCON0_SWRESET);
	for (tries = 2000; tries; --tries) {
		if (~decon_read(id, VIDCON0) & VIDCON0_SWRESET)
			break;
		udelay(10);
	}

	if (!tries) {
		decon_err("failed to reset Decon\n");
		return -EBUSY;
	}

	return 0;
}

void decon_reg_set_default_win_channel(u32 id)
{
	decon_write_mask(id, WINCHMAP0, WINCHMAP_DMA(0, 1), WINCHMAP_MASK(0));
	decon_write_mask(id, WINCHMAP0, WINCHMAP_DMA(1, 1), WINCHMAP_MASK(1));
	decon_write_mask(id, WINCHMAP0, WINCHMAP_DMA(2, 1), WINCHMAP_MASK(2));
}

void decon_reg_set_clkgate_mode(u32 id, u32 en)
{
}

void decon_reg_blend_alpha_bits(u32 id, u32 alpha_bits)
{
	decon_write(id, BLENDCON, alpha_bits);
}

void decon_reg_set_vidout(u32 id, struct decon_psr_info *psr,
		enum decon_dsi_mode dsi_mode, u32 en)
{
	if (psr->psr_mode == DECON_MIPI_COMMAND_MODE)
		decon_write_mask(id, VIDOUTCON0, VIDOUTCON0_I80IF_F,
					VIDOUTCON0_IF_MASK);
	else
		decon_write_mask(id, VIDOUTCON0, VIDOUTCON0_RGBIF_F,
					VIDOUTCON0_IF_MASK);

	decon_write_mask(id, VIDOUTCON0, en ? ~0 : 0, VIDOUTCON0_LCD_ON_F);
}

void decon_reg_set_crc(u32 id, u32 en)
{
	u32 val = en ? ~0 : 0;

	decon_write_mask(id, CRCCTRL, val,
			CRCCTRL_CRCCLKEN | CRCCTRL_CRCEN | CRCCTRL_CRCSTART_F);
}

void decon_reg_set_fixvclk(u32 id, int dsi_idx, enum decon_hold_scheme mode)
{
	u32 val = VIDCON1_VCLK_HOLD;

	switch (mode) {
	case DECON_VCLK_HOLD:
		val = VIDCON1_VCLK_HOLD;
		break;
	case DECON_VCLK_RUNNING:
		val = VIDCON1_VCLK_RUN;
		break;
	case DECON_VCLK_RUN_VDEN_DISABLE:
		val = VIDCON1_VCLK_RUN_VDEN_DISABLE;
		break;
	}

	decon_write_mask(id, VIDCON1(dsi_idx), val, VIDCON1_VCLK_MASK);
}

void decon_reg_clear_win(u32 id, int win_idx)
{
	decon_write(id, WINCON(win_idx), WINCON_RESET_VALUE);
	decon_write(id, VIDOSD_A(win_idx), 0);
	decon_write(id, VIDOSD_B(win_idx), 0);
	decon_write(id, VIDOSD_C(win_idx), 0);
	decon_write(id, VIDOSD_D(win_idx), 0);
}

void decon_reg_set_rgb_order(u32 id, int dsi_idx, enum decon_rgb_order order)
{
	u32 val = VIDCON1_RGB_ORDER_O_RGB;

	switch (order) {
	case DECON_RGB:
		val = VIDCON1_RGB_ORDER_O_RGB;
		break;
	case DECON_GBR:
		val = VIDCON1_RGB_ORDER_O_GBR;
		break;
	case DECON_BRG:
		val = VIDCON1_RGB_ORDER_O_BRG;
		break;
	case DECON_BGR:
		val = VIDCON1_RGB_ORDER_O_BGR;
		break;
	case DECON_RBG:
		val = VIDCON1_RGB_ORDER_O_RBG;
		break;
	case DECON_GRB:
		val = VIDCON1_RGB_ORDER_O_GRB;
		break;
	}

	decon_write_mask(id, VIDCON1(dsi_idx), val, VIDCON1_RGB_ORDER_O_MASK);
}

void decon_reg_set_porch(u32 id, int dsi_idx, struct decon_lcd *info)
{
	u32 val = 0;

	val = VIDTCON0_VBPD(info->decon_vbp - 1) | VIDTCON0_VFPD(info->decon_vfp - 1);
	decon_write(id, VIDTCON0(dsi_idx), val);

	val = VIDTCON1_VSPW(info->decon_vsa - 1);
	decon_write(id, VIDTCON1(dsi_idx), val);

	val = VIDTCON2_HBPD(info->decon_hbp - 1) | VIDTCON2_HFPD(info->decon_hfp - 1);
	decon_write(id, VIDTCON2(dsi_idx), val);

	val = VIDTCON3_HSPW(info->decon_hsa - 1);
	decon_write(id, VIDTCON3(dsi_idx), val);

	val = VIDTCON4_LINEVAL(info->yres - 1) |
			VIDTCON4_HOZVAL(info->xres - 1);
	decon_write(id, VIDTCON4(dsi_idx), val);
}


void decon_reg_set_resolution(u32 id, int dsi_idx, struct decon_lcd *info)
{
	u32 val = 0;

	/* from LCD info */
	/* These values are fixed considering to 320 pixel restriction */
	val = VIDTCON5_LINEVAL(info->dispif_h - 1) |
			VIDTCON5_HOZVAL(info->dispif_w - 1);
	decon_write(id, VIDTCON5(dsi_idx), val);
}

void decon_reg_set_linecnt_op_threshold(u32 id, int dsi_idx, u32 th)
{
	decon_write(id, LINECNT_OP_THRESHOLD(dsi_idx), th);
}

void decon_reg_set_clkval(u32 id, u32 clkdiv)
{
	decon_write_mask(id, VCLKCON0, ~0, VCLKCON0_CLKVALUP);
}

void decon_reg_direct_on_off(u32 id, u32 en)
{
	u32 val = en ? ~0 : 0;

	decon_write_mask(id, VIDCON0, val, VIDCON0_ENVID_F | VIDCON0_ENVID);
}

void decon_reg_per_frame_off(u32 id)
{
	decon_write_mask(id, VIDCON0, 0, VIDCON0_ENVID_F);
}

void decon_reg_set_freerun_mode(u32 id, u32 en)
{
	decon_write_mask(id, VCLKCON0, en ? ~0 : 0, VCLKCON0_VLCKFREE);
}

void decon_reg_update_standalone(u32 id)
{
	decon_write_mask(id, DECON_UPDATE, ~0, DECON_UPDATE_STANDALONE_F);
}

void decon_reg_configure_lcd(u32 id, enum decon_dsi_mode dsi_mode,
		struct decon_lcd *lcd_info)
{
	decon_reg_set_rgb_order(id, 0, DECON_RGB);
	decon_reg_set_porch(id, 0, lcd_info);
	decon_reg_set_resolution(id, 0, lcd_info);
	if (lcd_info->mic_enabled)
		decon_reg_config_mic(id, 0, lcd_info);

	if (lcd_info->mode == DECON_VIDEO_MODE)
		decon_reg_set_linecnt_op_threshold(id, 0, lcd_info->yres - 1);

	decon_reg_set_clkval(id, 0);

	decon_reg_set_freerun_mode(id, 1);
	decon_reg_direct_on_off(id, 0);
}

void decon_reg_configure_trigger(u32 id, enum decon_trig_mode mode)
{
	u32 val, mask;

	mask = TRIGCON_SWTRIGEN | TRIGCON_HWTRIGEN;
	if (mode == DECON_SW_TRIG) {
		val = TRIGCON_SWTRIGEN;
	} else {
		val = TRIGCON_HWTRIGEN | TRIGCON_HWTRIG_AUTO_MASK;
	}

	decon_write_mask(id, TRIGCON, val, mask);
}

void decon_reg_set_winmap(u32 id, u32 idx, u32 color, u32 en)
{
	u32 val = en ? WIN_MAP_MAP : 0;

	decon_reg_shadow_protect_win(id, idx, 1);
	val |= WIN_MAP_MAP_COLOUR(color);
	decon_write_mask(id, WIN_MAP(idx), val,
			WIN_MAP_MAP | WIN_MAP_MAP_COLOUR_MASK);
	decon_reg_shadow_protect_win(id, idx, 0);
}

u32 decon_reg_get_linecnt(u32 id, int dsi_idx)
{
	return VIDCON1_LINECNT_GET(decon_read(id, VIDCON1(dsi_idx)));
}

u32 decon_reg_get_vstatus(u32 id, int dsi_idx)
{
	return decon_read(id, VIDCON1(dsi_idx)) & VIDCON1_VSTATUS_MASK;
}

/* timeout : usec */
int decon_reg_wait_linecnt_is_zero_timeout(u32 id, int dsi_idx,
				unsigned long timeout)
{
	unsigned long delay_time = 10;
	unsigned long cnt = timeout / delay_time;
	u32 linecnt, vstatus;

	do {
		linecnt = decon_reg_get_linecnt(id, dsi_idx);
		if (!linecnt) {
			vstatus = decon_reg_get_vstatus(id, dsi_idx);
			if (vstatus == VIDCON1_VSTATUS_IDLE)
				break;
		}
		cnt--;
		udelay(delay_time);
	} while (cnt);

	if (!cnt) {
		decon_err("wait timeout linecount is zero(%u)\n", linecnt);
		return -EBUSY;
	}

	return 0;
}

u32 decon_reg_get_stop_status(u32 id)
{
	u32 val;

	val = decon_read(id, VIDCON0);
	if (val & VIDCON0_DECON_STOP_STATUS)
		return 1;

	return 0;
}

int decon_reg_wait_stop_status_timeout(u32 id, unsigned long timeout)
{
	unsigned long delay_time = 10;
	unsigned long cnt = timeout / delay_time;
	u32 status;

	do {
		status = decon_reg_get_stop_status(id);
		cnt--;
		udelay(delay_time);
	} while (status && cnt);

	if (!cnt) {
		decon_err("wait timeout decon stop status(%u)\n", status);
		return -EBUSY;
	}

	return 0;
}

int decon_reg_is_win_enabled(u32 id, int win_idx)
{
	if (decon_read(id, WINCON(win_idx)) & WINCON_ENWIN)
		return 1;

	return 0;
}

int decon_reg_is_shadow_updated(u32 id)
{
	return 0;
}

void decon_reg_config_mic(u32 id, int dsi_idx, struct decon_lcd *lcd_info)
{
}

void decon_reg_clear_int(u32 id)
{
	u32 mask;

	mask = VIDINTCON1_INT_I80 | VIDINTCON1_INT_FRAME | VIDINTCON1_INT_FIFO;
	decon_write_mask(id, VIDINTCON1, 0, mask);
}

void decon_reg_config_win_channel(u32 id, u32 win_idx,
			enum decon_idma_type type)
{
	switch (type) {
	case IDMA_G0:
	case IDMA_G1:
		decon_write_mask(id, WINCHMAP0, WINCHMAP_DMA(type + 1, win_idx),
				WINCHMAP_MASK(win_idx));
		break;
	case IDMA_VG0:
	case IDMA_VG1:
	case IDMA_VGR0:
	case IDMA_VGR1:
	case IDMA_G2:
	case IDMA_G3:
		decon_write_mask(id, WINCHMAP0, WINCHMAP_DMA(0, win_idx),
				WINCHMAP_MASK(win_idx));
		break;

	default:
		decon_err("channel(0x%x) is not valid\n", type);
		return;
	}

	decon_dbg("decon-%s win[%d]-type[%d] WINCHMAP:%#x\n", "int",
			win_idx, type, decon_read(id, WINCHMAP0));
}

/***************** CAL APIs implementation *******************/
void decon_reg_init(u32 id, enum decon_dsi_mode dsi_mode,
			struct decon_init_param *p)
{
	int win_idx;
	struct decon_lcd *lcd_info = p->lcd_info;
	struct decon_psr_info *psr = &p->psr;

	decon_reg_reset(id);
	decon_reg_set_clkgate_mode(id, 0);
	decon_reg_blend_alpha_bits(id, BLENDCON_NEW_8BIT_ALPHA_VALUE);
	decon_reg_set_vidout(id, psr, dsi_mode, 1);
	decon_reg_set_crc(id, 0);

	/* Does exynos7570 decon always use DECON_VCLK_HOLD ?  No */
	if (psr->psr_mode == DECON_MIPI_COMMAND_MODE)
		decon_reg_set_fixvclk(id, 0, DECON_VCLK_RUN_VDEN_DISABLE);
	else
		decon_reg_set_fixvclk(id, 0, DECON_VCLK_HOLD);

	for (win_idx = 0; win_idx < p->nr_windows; win_idx++)
		decon_reg_clear_win(id, win_idx);

	/* RGB order -> porch values -> LINECNT_OP_THRESHOLD -> clock divider
	 * -> freerun mode --> stop DECON */
	decon_reg_configure_lcd(id, dsi_mode, lcd_info);

	if (psr->psr_mode == DECON_MIPI_COMMAND_MODE)
		decon_reg_configure_trigger(id, psr->trig_mode);

	/* asserted interrupt should be cleared before initializing decon hw */
	decon_reg_clear_int(id);
}

void decon_reg_init_probe(u32 id, enum decon_dsi_mode dsi_mode,
				struct decon_init_param *p)
{
	struct decon_lcd *lcd_info = p->lcd_info;
	struct decon_psr_info *psr = &p->psr;

	decon_reg_set_clkgate_mode(id, 0);
	decon_reg_blend_alpha_bits(id, BLENDCON_NEW_8BIT_ALPHA_VALUE);
	decon_reg_set_vidout(id, psr, dsi_mode, 1);

	/* Does exynos7570 decon always use DECON_VCLK_HOLD ? */
	if (psr->psr_mode == DECON_MIPI_COMMAND_MODE)
		decon_reg_set_fixvclk(id, 0, DECON_VCLK_RUN_VDEN_DISABLE);
	else
		decon_reg_set_fixvclk(id, 0, DECON_VCLK_HOLD);

	decon_reg_set_rgb_order(id, 0, DECON_RGB);
	decon_reg_set_porch(id, 0, lcd_info);
	decon_reg_set_resolution(id, 0, lcd_info);
	if (lcd_info->mic_enabled)
		decon_reg_config_mic(id, 0, lcd_info);

	if (lcd_info->mode == DECON_VIDEO_MODE)
		decon_reg_set_linecnt_op_threshold(id, 0, lcd_info->yres - 1);

	decon_reg_set_clkval(id, 0);

	decon_reg_set_freerun_mode(id, 1);
	decon_reg_update_standalone(id);

	if (psr->psr_mode == DECON_MIPI_COMMAND_MODE)
		decon_reg_configure_trigger(id, psr->trig_mode);
}

void decon_reg_start(u32 id, enum decon_dsi_mode dsi_mode,
			struct decon_psr_info *psr)
{
	decon_reg_direct_on_off(id, 1);

	decon_reg_update_standalone(id);
	if ((psr->psr_mode == DECON_MIPI_COMMAND_MODE) &&
			(psr->trig_mode == DECON_HW_TRIG))
		decon_reg_set_trigger(id, dsi_mode, psr->trig_mode,
				DECON_TRIG_ENABLE);
}

int decon_reg_stop(u32 id, enum decon_dsi_mode dsi_mode,
				struct decon_psr_info *psr)
{
	int ret = 0;

	if ((psr->psr_mode == DECON_MIPI_COMMAND_MODE) &&
			(psr->trig_mode == DECON_HW_TRIG)) {
		decon_reg_set_trigger(id, dsi_mode, psr->trig_mode,
				DECON_TRIG_DISABLE);
	}

	if (psr->psr_mode == DECON_MIPI_COMMAND_MODE){
		/* timeout : 50ms */
		ret = decon_reg_wait_linecnt_is_zero_timeout(id, 0, 50 * 1000);
		if (ret < 0)
			goto err;

		decon_reg_direct_on_off(id, 0);
	} else {
		decon_reg_per_frame_off(id);
	}

	/* set update bit */
	decon_reg_update_standalone(id);

	/* timeout : 30ms */
	ret = decon_reg_wait_stop_status_timeout(id, 30 * 1000);
	if(ret < 0){
		goto err;
	}

	return 0;

err:
	decon_err("failed to decon stop\n");
	return ret;
}

void decon_reg_set_rgb_type(u32 id, int win_idx, u32 type)
{
	u32 csc_eq = 0;

	switch (type) {
	case BT_601_NARROW:
		csc_eq = WINCON_RGB_TYPE_BT601N;
		break;
	case BT_601_WIDE:
		csc_eq = WINCON_RGB_TYPE_BT601W;
		break;
	default:
		decon_err("Unsupported CSC Equation\n");
	}

	decon_dbg("win_idx: %d CSC mode : %x\n", win_idx, csc_eq);
	decon_write_mask(id, WINCON(win_idx), csc_eq, (0x1 << 26));
}

void decon_reg_set_regs_data(u32 id, int win_idx,
			struct decon_regs_data *regs)
{
	u32 val;

	if (regs->wincon & WINCON_ENWIN)
		decon_reg_config_win_channel(id, win_idx, regs->type);

	val = regs->wincon & WINCON_OUTSTAND_MAX_MASK;
	if (val < (WINCON_OUTSTAND_MAX_DEFAULT << WINCON_OUTSTAND_MAX_POS)) {
		val = regs->wincon & (~WINCON_OUTSTAND_MAX_MASK);
		regs->wincon = val | (WINCON_OUTSTAND_MAX_DEFAULT <<
					WINCON_OUTSTAND_MAX_POS);
	}
	decon_write(id, WINCON(win_idx), regs->wincon);
	decon_write(id, WIN_MAP(win_idx), regs->winmap);
	if (regs->winmap & WIN_MAP_MAP) {
		decon_write_mask(id, WINCHMAP0, WINCHMAP_DMA(0x7, win_idx),
				WINCHMAP_MASK(win_idx));
	}

	decon_write(id, VIDOSD_A(win_idx), regs->vidosd_a);
	decon_write(id, VIDOSD_B(win_idx), regs->vidosd_b);
	decon_write(id, VIDOSD_C(win_idx), regs->vidosd_c);
	decon_write(id, VIDOSD_D(win_idx), regs->vidosd_d);

	decon_write(id, VIDW_ADD0(win_idx), regs->vidw_buf_start);
	decon_write(id, VIDW_WHOLE_X(win_idx), regs->vidw_whole_w);
	decon_write(id, VIDW_WHOLE_Y(win_idx), regs->vidw_whole_h);
	decon_write(id, VIDW_OFFSET_X(win_idx), regs->vidw_offset_x);
	decon_write(id, VIDW_OFFSET_Y(win_idx), regs->vidw_offset_y);
	decon_write(id, VIDW_ADD2(win_idx), regs->vidw_plane2_buf_start);
	decon_write(id, VIDW_ADD3(win_idx), regs->vidw_plane3_buf_start);

	if (win_idx)
		decon_write(id, BLENDE(win_idx - 1), regs->blendeq);

	decon_dbg("%s: regs->type(%d)\n", __func__, regs->type);
}

void decon_reg_set_int(u32 id, struct decon_psr_info *psr,
			enum decon_dsi_mode dsi_mode, u32 en)
{
	u32 val;

	if (en) {
		val = VIDINTCON0_INT_ENABLE | VIDINTCON0_FIFOLEVEL_EMPTY;
		if (psr->psr_mode == DECON_MIPI_COMMAND_MODE) {
			decon_write_mask(id, VIDINTCON1, ~0,
						VIDINTCON1_INT_I80);
			val |= VIDINTCON0_INT_FIFO | VIDINTCON0_INT_I80_EN | VIDINTCON0_INT_FRAME
				| VIDINTCON0_FRAMESEL0_VSYNC;
		} else {
			val |= VIDINTCON0_INT_FIFO | VIDINTCON0_INT_FRAME
				| VIDINTCON0_FRAMESEL0_VSYNC;
		}
		decon_write_mask(id, VIDINTCON0, val, ~0);
	} else {
		decon_write_mask(id, VIDINTCON0, 0, VIDINTCON0_INT_ENABLE);
	}
}

void decon_enable_eclk_idle_gate(u32 id, enum decon_set_eclk_idle_gate en)
{
	u32 val = (en == DECON_ECLK_IDLE_GATE_ENABLE) ? ~0 : 0;

	decon_write_mask(id, VCLKCON0, val, ECLK_IDLE_GATE_EN);
}

/* It is needed to unmask hw trigger and mask asynchronously for dual DSI */
/* enable(unmask) / disable(mask) hw trigger */
void decon_reg_set_trigger(u32 id, enum decon_dsi_mode dsi_mode,
			enum decon_trig_mode trig, enum decon_set_trig en)
{
	u32 val = (en == DECON_TRIG_ENABLE) ? ~0 : 0;
	u32 mask;

	if (trig == DECON_SW_TRIG)
		mask = TRIGCON_SWTRIGCMD;
	else
		mask = TRIGCON_HWTRIGMASK_DISPIF0;
	decon_write_mask(id, TRIGCON, val, mask);
}

/* wait until shadow update is finished */
int decon_reg_wait_for_update_timeout(u32 id, unsigned long timeout)
{
	unsigned long delay_time = 100;
	unsigned long cnt = timeout / delay_time;

	while ((decon_read(id, DECON_UPDATE) & DECON_UPDATE_STANDALONE_F) &&
				--cnt)
		udelay(delay_time);

	if (!cnt) {
		decon_err("timeout of updating decon registers\n");
		return -EBUSY;
	}

	return 0;
}

/* prohibit shadow update during writing something to SFR */
void decon_reg_shadow_protect_win(u32 id, u32 win_idx, u32 protect)
{
	u32 val = protect ? ~0 : 0;

	decon_write_mask(id, SHADOWCON, val, SHADOWCON_WIN_PROTECT(win_idx));
}

/* enable each window */
void decon_reg_activate_window(u32 id, u32 index)
{
	decon_write_mask(id, WINCON(index), ~0, WINCON_ENWIN);
	decon_reg_update_standalone(id);
}

void decon_reg_set_block_mode(u32 id, u32 win_idx, u32 x, u32 y, u32 w,
					u32 h, u32 en)
{
	u32 val = en ? ~0 : 0;
	u32 blk_offset = 0, blk_size = 0;

	blk_offset = VIDW_BLKOFFSET_Y_F(y) | VIDW_BLKOFFSET_X_F(x);
	blk_size = VIDW_BLKSIZE_W_F(w) | VIDW_BLKSIZE_H_F(h);

	decon_write_mask(id, VIDW_BLKOFFSET(win_idx), blk_offset,
				VIDW_BLKOFFSET_MASK);
	decon_write_mask(id, VIDW_BLKSIZE(win_idx), blk_size,
				VIDW_BLKSIZE_MASK);
	decon_write_mask(id, WINCON(win_idx), val, WINCON_BLK_EN_F);
}

void decon_reg_set_tui_va(u32 id, u32 va)
{
	decon_write(id, VIDW_ADD2(6), va);
}

/* DECON disp i/f size is defined VIDTCON5 sfr instead of VIDTCON4 */
u32 decon_reg_get_lineval(u32 id, int dsi_idx, struct decon_lcd *lcd_info)
{
	u32 val;

	if (lcd_info->mode == DECON_VIDEO_MODE)
		val = decon_read(id, VIDTCON4(dsi_idx) + SHADOW_OFFSET);
	else
		val = decon_read(id, VIDTCON5(dsi_idx) + SHADOW_OFFSET);

	return VIDTCONx_LINEVAL_GET(val) + 1;
}

u32 decon_reg_get_hozval(u32 id, int dsi_idx, struct decon_lcd *lcd_info)
{
	u32 val;

	if (lcd_info->mode == DECON_VIDEO_MODE)
		val = decon_read(id, VIDTCON4(dsi_idx) + SHADOW_OFFSET);
	else
		val = decon_read(id, VIDTCON5(dsi_idx) + SHADOW_OFFSET);

	return VIDTCONx_HOZVAL_GET(val) + 1;
}
