/*
 * Samsung EXYNOS FIMC-IS (Imaging Subsystem) driver
 *
 * Copyright (C) 2014 Samsung Electronics Co., Ltd.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/kernel.h>

#include "fimc-is-err.h"
#include "fimc-is-core.h"
#include "fimc-is-hw-control.h"

#include "fimc-is-hw-3aa.h"
#include "fimc-is-hw-isp.h"
#include "fimc-is-hw-tpu.h"
#if defined(CONFIG_CAMERA_FIMC_SCALER_USE)
#include "fimc-is-hw-scp.h"
#elif defined(CONFIG_CAMERA_MC_SCALER_VER1_USE)
#include "fimc-is-hw-mcscaler-v1.h"
#elif defined(CONFIG_CAMERA_MC_SCALER_VER2_USE)
#include "fimc-is-hw-mcscaler-v2.h"
#endif
#include "fimc-is-hw-vra.h"
#include "fimc-is-hw-dm.h"

#define INTERNAL_SHOT_EXIST	(1)

void framemgr_e_barrier_common(struct fimc_is_framemgr *this, u32 index, ulong flag)
{
	if (in_interrupt()) {
		framemgr_e_barrier(this, index);
	} else {
		framemgr_e_barrier_irqs(this, index, flag);
	}

	return;
}

void framemgr_x_barrier_common(struct fimc_is_framemgr *this, u32 index, ulong flag)
{
	if (in_interrupt()) {
		framemgr_x_barrier(this, index);
	} else {
		framemgr_x_barrier_irqr(this, index, flag);
	}

	return;
}

static int get_free_work_irq(struct fimc_is_work_list *this,
	struct fimc_is_work **work)
{
	int ret = 0;

	if (work) {
		spin_lock(&this->slock_free);

		if (this->work_free_cnt) {
			*work = container_of(this->work_free_head.next,
					struct fimc_is_work, list);
			list_del(&(*work)->list);
			this->work_free_cnt--;
		} else
			*work = NULL;

		spin_unlock(&this->slock_free);
	} else {
		ret = -EFAULT;
		err_hw("item is null ptr");
	}

	return ret;
}

static int set_req_work_irq(struct fimc_is_work_list *this,
	struct fimc_is_work *work)
{
	int ret = 0;

	if (work) {
		spin_lock(&this->slock_request);
		list_add_tail(&work->list, &this->work_request_head);
		this->work_request_cnt++;
#ifdef TRACE_WORK
		print_req_work_list(this);
#endif

		spin_unlock(&this->slock_request);
	} else {
		ret = -EFAULT;
		err_hw("item is null ptr");
	}

	return ret;
}

static inline void wq_func_schedule(struct fimc_is_interface *itf,
	struct work_struct *work_wq)
{
	if (itf->workqueue)
		queue_work(itf->workqueue, work_wq);
	else
		schedule_work(work_wq);
}

static void prepare_sfr_dump(struct fimc_is_hardware *hardware)
{
	int hw_slot = -1;
	int reg_size = 0;
	struct fimc_is_hw_ip *hw_ip = NULL;

	if (!hardware) {
		err_hw("hardware is null\n");
		return;
	}

	for (hw_slot = 0; hw_slot < HW_SLOT_MAX; hw_slot++) {
		hw_ip = &hardware->hw_ip[hw_slot];

		if (hw_ip->id == DEV_HW_END || hw_ip->id == 0)
		       continue;

		if (IS_ERR_OR_NULL(hw_ip->regs) ||
			(hw_ip->regs_start == 0) ||
			(hw_ip->regs_end == 0)) {
			warn_hw("[ID:%d] reg iomem is invalid", hw_ip->id);
			continue;
		}

		/* alloc sfr dump memory */
		reg_size = (hw_ip->regs_end - hw_ip->regs_start + 1);
		hw_ip->sfr_dump = (u8 *)kzalloc(reg_size, GFP_KERNEL);
		if (IS_ERR_OR_NULL(hw_ip->sfr_dump))
			err_hw("[ID:%d] sfr dump memory alloc fail", hw_ip->id);
		else
			info_hw("[ID:%d] sfr dump memory (V/P/S):(%p/%p/0x%X)[0x%llX~0x%llX]",
				hw_ip->id, hw_ip->sfr_dump, (void *)virt_to_phys(hw_ip->sfr_dump),
				reg_size, hw_ip->regs_start, hw_ip->regs_end);

		if (IS_ERR_OR_NULL(hw_ip->regs_b) ||
			(hw_ip->regs_b_start == 0) ||
			(hw_ip->regs_b_end == 0))
			continue;

		/* alloc sfr B dump memory */
		reg_size = (hw_ip->regs_b_end - hw_ip->regs_b_start + 1);
		hw_ip->sfr_b_dump = (u8 *)kzalloc(reg_size, GFP_KERNEL);
		if (IS_ERR_OR_NULL(hw_ip->sfr_b_dump))
			err_hw("[ID:%d] sfr B dump memory alloc fail", hw_ip->id);
		else
			info_hw("[ID:%d] sfr B dump memory (V/P/S):(%p/%p/0x%X)[0x%llX~0x%llX]",
				hw_ip->id, hw_ip->sfr_b_dump, (void *)virt_to_phys(hw_ip->sfr_b_dump),
				reg_size, hw_ip->regs_b_start, hw_ip->regs_b_end);
	}
}

void print_hw_frame_count(struct fimc_is_hw_ip *hw_ip)
{
	int f_index, p_index;
	struct hw_debug_info *debug_info;
	ulong usec[DEBUG_FRAME_COUNT][DEBUG_POINT_MAX];

	if (!hw_ip) {
		err_hw("hw_ip is null\n");
		return;
	}

	/* skip printing frame count, if hw_ip wasn't opened */
	if (!test_bit(HW_OPEN, &hw_ip->state))
		return;

	info_hw("[ID:%d] fs(%d), cl(%d), fe(%d), dma(%d)\n", hw_ip->id,
			atomic_read(&hw_ip->count.fs),
			atomic_read(&hw_ip->count.cl),
			atomic_read(&hw_ip->count.fe),
			atomic_read(&hw_ip->count.dma));

	for (f_index = 0; f_index < DEBUG_FRAME_COUNT; f_index++) {
		debug_info = &hw_ip->debug_info[f_index];
		for (p_index = 0 ; p_index < DEBUG_POINT_MAX; p_index++)
			usec[f_index][p_index]  = do_div(debug_info->time[p_index], NSEC_PER_SEC);

		info_hw("[%d][F:%d] shot[%5lu.%06lu], fs[c%d][%5lu.%06lu], fe[c%d][%5lu.%06lu], dma[c%d][%5lu.%06lu], \n",
				f_index, debug_info->fcount,
				(ulong)debug_info->time[DEBUG_POINT_HW_SHOT], usec[f_index][DEBUG_POINT_HW_SHOT] / NSEC_PER_USEC,
				debug_info->cpuid[DEBUG_POINT_FRAME_START],
				(ulong)debug_info->time[DEBUG_POINT_FRAME_START], usec[f_index][DEBUG_POINT_FRAME_START] / NSEC_PER_USEC,
				debug_info->cpuid[DEBUG_POINT_FRAME_END],
				(ulong)debug_info->time[DEBUG_POINT_FRAME_END], usec[f_index][DEBUG_POINT_FRAME_END] / NSEC_PER_USEC,
				debug_info->cpuid[DEBUG_POINT_FRAME_DMA_END],
				(ulong)debug_info->time[DEBUG_POINT_FRAME_DMA_END], usec[f_index][DEBUG_POINT_FRAME_DMA_END] / NSEC_PER_USEC);
	}
}

void print_all_hw_frame_count(struct fimc_is_hardware *hardware)
{
	int hw_slot = -1;
	struct fimc_is_hw_ip *_hw_ip = NULL;

	if (!hardware) {
		err_hw("hardware is null\n");
		return;
	}

	for (hw_slot = 0; hw_slot < HW_SLOT_MAX; hw_slot++) {
		_hw_ip = &hardware->hw_ip[hw_slot];
		print_hw_frame_count(_hw_ip);
	}
}

void fimc_is_hardware_flush_frame(struct fimc_is_hw_ip *hw_ip,
	enum fimc_is_hw_frame_state state,
	enum ShotErrorType done_type)
{
	int ret = 0;
	struct fimc_is_framemgr *framemgr;
	struct fimc_is_frame *frame;
	ulong flags;

	BUG_ON(!hw_ip);

	framemgr = hw_ip->framemgr;

	framemgr_e_barrier_irqs(framemgr, 0, flags);
	while (state <  FS_HW_WAIT_DONE) {
		frame = peek_frame(framemgr, state);
		while (frame) {
			trans_frame(framemgr, frame, state + 1);
			frame = peek_frame(framemgr, state);
		}
		state++;
	}
	frame = peek_frame(framemgr, FS_HW_WAIT_DONE);
	framemgr_x_barrier_irqr(framemgr, 0, flags);

	while (frame) {
		if (done_type == IS_SHOT_TIMEOUT) {
			err_hw("[ID:%d]hardware is timeout\n", hw_ip->id);
			fimc_is_hardware_size_dump(hw_ip);
		}

		ret = fimc_is_hardware_frame_ndone(hw_ip, frame, atomic_read(&hw_ip->instance), done_type);
		if (ret)
			err_hw("[%d][ID:%d] %s: hardware_frame_ndone fail",
				atomic_read(&hw_ip->instance), hw_ip->id, __func__);

		framemgr_e_barrier_irqs(framemgr, 0, flags);
		frame = peek_frame(framemgr, FS_HW_WAIT_DONE);
		framemgr_x_barrier_irqr(framemgr, 0, flags);
	}
}

u32 get_hw_id_from_group(u32 group_id)
{
	u32 hw_id = DEV_HW_END;

	switch(group_id) {
	case GROUP_ID_3AA0:
		hw_id = DEV_HW_3AA0;
		break;
	case GROUP_ID_3AA1:
		hw_id = DEV_HW_3AA1;
		break;
	case GROUP_ID_ISP0:
		hw_id = DEV_HW_ISP0;
		break;
	case GROUP_ID_ISP1:
		hw_id = DEV_HW_ISP1;
		break;
	case GROUP_ID_DIS0:
		hw_id = DEV_HW_TPU;
		break;
	case GROUP_ID_MCS0:
		hw_id = DEV_HW_MCSC0;
		break;
	case GROUP_ID_MCS1:
		hw_id = DEV_HW_MCSC1;
		break;
	case GROUP_ID_VRA0:
		hw_id = DEV_HW_VRA;
		break;
	default:
		hw_id = DEV_HW_END;
		err_hw("invalid group(%d)", group_id);
		break;
	}

	return hw_id;
}

int fimc_is_hardware_probe(struct fimc_is_hardware *hardware,
	struct fimc_is_interface *itf, struct fimc_is_interface_ischain *itfc)
{
	int ret = 0;
	int i, hw_slot = -1;
	enum fimc_is_hardware_id hw_id = DEV_HW_END;

	BUG_ON(!hardware);
	BUG_ON(!itf);
	BUG_ON(!itfc);

	for (i = 0; i < HW_SLOT_MAX; i++) {
		hardware->hw_ip[i].id = DEV_HW_END;
		hardware->hw_ip[i].priv_info = NULL;

	}

#if defined(SOC_3AAISP)
	hw_id = DEV_HW_3AA0;
	hw_slot = fimc_is_hw_slot_id(hw_id);
	if (!valid_hw_slot_id(hw_slot)) {
		err_hw("invalid slot (%d,%d)", hw_id, hw_slot);
		return -EINVAL;
	}
	ret = fimc_is_hw_3aa_probe(&(hardware->hw_ip[hw_slot]), itf, itfc, hw_id);
	if (ret) {
		err_hw("probe fail (%d,%d)", hw_id, hw_slot);
		return ret;
	}
#endif

#if (defined(SOC_30S) && !defined(SOC_3AAISP))
	hw_id = DEV_HW_3AA0;
	hw_slot = fimc_is_hw_slot_id(hw_id);
	if (!valid_hw_slot_id(hw_slot)) {
		err_hw("invalid slot (%d,%d)", hw_id, hw_slot);
		return -EINVAL;
	}
	ret = fimc_is_hw_3aa_probe(&(hardware->hw_ip[hw_slot]), itf, itfc, hw_id);
	if (ret) {
		err_hw("probe fail (%d,%d)", hw_id, hw_slot);
		return ret;
	}
#endif

#if (defined(SOC_31S) && !defined(SOC_3AAISP))
	hw_id = DEV_HW_3AA1;
	hw_slot = fimc_is_hw_slot_id(hw_id);
	if (!valid_hw_slot_id(hw_slot)) {
		err_hw("invalid slot (%d,%d)", hw_id, hw_slot);
		return -EINVAL;
	}
	ret = fimc_is_hw_3aa_probe(&(hardware->hw_ip[hw_slot]), itf, itfc, hw_id);
	if (ret) {
		err_hw("probe fail (%d,%d)", hw_id, hw_slot);
		return ret;
	}
#endif

#if (defined(SOC_I0S) && !defined(SOC_3AAISP))
	hw_id = DEV_HW_ISP0;
	hw_slot = fimc_is_hw_slot_id(hw_id);
	if (!valid_hw_slot_id(hw_slot)) {
		err_hw("invalid slot (%d,%d)", hw_id, hw_slot);
		return -EINVAL;
	}
	ret = fimc_is_hw_isp_probe(&(hardware->hw_ip[hw_slot]), itf, itfc, hw_id);
	if (ret) {
		err_hw("probe fail (%d,%d)", hw_id, hw_slot);
		return ret;
	}
#endif

#if (defined(SOC_I1S) && !defined(SOC_3AAISP))
	hw_id = DEV_HW_ISP1;
	hw_slot = fimc_is_hw_slot_id(hw_id);
	if (!valid_hw_slot_id(hw_slot)) {
		err_hw("invalid slot (%d,%d)", hw_id, hw_slot);
		return -EINVAL;
	}
	ret = fimc_is_hw_isp_probe(&(hardware->hw_ip[hw_slot]), itf, itfc, hw_id);
	if (ret) {
		err_hw("probe fail (%d,%d)", hw_id, hw_slot);
		return ret;
	}
#endif

#if defined(SOC_DIS)
	hw_id = DEV_HW_TPU;
	hw_slot = fimc_is_hw_slot_id(hw_id);
	if (!valid_hw_slot_id(hw_slot)) {
		err_hw("invalid slot (%d,%d)", hw_id, hw_slot);
		return -EINVAL;
	}
	ret = fimc_is_hw_tpu_probe(&(hardware->hw_ip[hw_slot]), itf, itfc, hw_id);
	if (ret) {
		err_hw("probe fail (%d,%d)", hw_id, hw_slot);
		return ret;
	}
#endif

#if (defined(SOC_SCP) && !defined(MCS_USE_SCP_PARAM))
	hw_id = DEV_HW_SCP;
	hw_slot = fimc_is_hw_slot_id(hw_id);
	if (!valid_hw_slot_id(hw_slot)) {
		err_hw("invalid slot (%d,%d)", hw_id, hw_slot);
		return -EINVAL;
	}

	ret = fimc_is_hw_scp_probe(&(hardware->hw_ip[hw_slot]), itf, itfc, hw_id);
	if (ret) {
		err_hw("probe fail (%d,%d)", hw_id, hw_slot);
		return ret;
	}
#endif

#if (defined(SOC_SCP) && defined(MCS_USE_SCP_PARAM))
	hw_id = DEV_HW_MCSC0;
	hw_slot = fimc_is_hw_slot_id(hw_id);
	if (!valid_hw_slot_id(hw_slot)) {
		err_hw("invalid slot (%d,%d)", hw_id, hw_slot);
		return -EINVAL;
	}

	ret = fimc_is_hw_mcsc_probe(&(hardware->hw_ip[hw_slot]), itf, itfc, hw_id);
	if (ret) {
		err_hw("probe fail (%d,%d)", hw_id, hw_slot);
		return ret;
	}
#endif

#if (defined(SOC_MCS) && defined(SOC_MCS0))
	hw_id = DEV_HW_MCSC0;
	hw_slot = fimc_is_hw_slot_id(hw_id);
	if (!valid_hw_slot_id(hw_slot)) {
		err_hw("invalid slot (%d,%d)", hw_id, hw_slot);
		return -EINVAL;
	}
	ret = fimc_is_hw_mcsc_probe(&(hardware->hw_ip[hw_slot]), itf, itfc, hw_id);
	if (ret) {
		err_hw("probe fail (%d,%d)", hw_id, hw_slot);
		return ret;
	}
#endif

#if (defined(SOC_MCS) && defined(SOC_MCS1))
	hw_id = DEV_HW_MCSC1;
	hw_slot = fimc_is_hw_slot_id(hw_id);
	if (!valid_hw_slot_id(hw_slot)) {
		err_hw("invalid slot (%d,%d)", hw_id, hw_slot);
		return -EINVAL;
	}
	ret = fimc_is_hw_mcsc_probe(&(hardware->hw_ip[hw_slot]), itf, itfc, hw_id);
	if (ret) {
		err_hw("probe fail (%d,%d)", hw_id, hw_slot);
		return ret;
	}
#endif

#if defined(SOC_VRA)
	hw_id = DEV_HW_VRA;
	hw_slot = fimc_is_hw_slot_id(hw_id);
	if (!valid_hw_slot_id(hw_slot)) {
		err_hw("invalid slot (%d,%d)", hw_id, hw_slot);
		return -EINVAL;
	}
	ret = fimc_is_hw_vra_probe(&(hardware->hw_ip[hw_slot]), itf, itfc, hw_id);
	if (ret) {
		err_hw("probe fail (%d,%d)", hw_id, hw_slot);
		return ret;
	}
#endif
	hardware->base_addr_mcuctl = itfc->regs_mcuctl;

	for (i = 0; i < FIMC_IS_STREAM_COUNT; i++) {
		hardware->hw_map[i] = 0;
		hardware->sensor_position[i] = 0;
		atomic_set(&hardware->streaming[i], 0);
	}

	atomic_set(&hardware->rsccount, 0);
	atomic_set(&hardware->bug_count, 0);
	atomic_set(&hardware->log_count, 0);

	prepare_sfr_dump(hardware);

	return ret;
}

int fimc_is_hardware_set_param(struct fimc_is_hardware *hardware, u32 instance,
	struct is_region *region, u32 lindex, u32 hindex, ulong hw_map)
{
	int ret = 0;
	int hw_slot = -1;
	struct fimc_is_hw_ip *hw_ip = NULL;

	BUG_ON(!hardware);
	BUG_ON(!region);

	for (hw_slot = 0; hw_slot < HW_SLOT_MAX; hw_slot++) {
		hw_ip = &hardware->hw_ip[hw_slot];

		CALL_HW_OPS(hw_ip, clk_gate, instance, true, false);
		ret = CALL_HW_OPS(hw_ip, set_param, region, lindex, hindex,
				instance, hw_map);
		CALL_HW_OPS(hw_ip, clk_gate, instance, false, false);
		if (ret) {
			err_hw("[%d]set_param fail (%d,%d)", instance,
				hw_ip->id, hw_slot);
			return -EINVAL;
		}
	}

	dbg_hw("[%d]set_param hw_map[0x%lx]\n", instance, hw_map);

	return ret;
}

static void fimc_is_hardware_shot_timer(unsigned long data)
{
	struct fimc_is_hw_ip *hw_ip = (struct fimc_is_hw_ip *)data;

	BUG_ON(!hw_ip);

	fimc_is_hardware_flush_frame(hw_ip, FS_HW_REQUEST, IS_SHOT_TIMEOUT);
}

int fimc_is_hardware_shot(struct fimc_is_hardware *hardware, u32 instance,
	struct fimc_is_group *group, struct fimc_is_frame *frame,
	struct fimc_is_framemgr *framemgr, ulong hw_map, u32 framenum)
{
	int ret = 0;
	struct fimc_is_hw_ip *hw_ip = NULL;
	enum fimc_is_hardware_id hw_id = DEV_HW_END;
	struct fimc_is_group *child = NULL;
	ulong flags = 0;
	int hw_list[GROUP_HW_MAX], hw_index, hw_slot;
	int hw_maxnum = 0;
	u32 index;

	BUG_ON(!hardware);
	BUG_ON(!frame);

	/* do the other device's group shot */
	ret = fimc_is_devicemgr_shot_callback(group, frame, frame->fcount, FIMC_IS_DEVICE_ISCHAIN);
	if (ret) {
		err_hw("fimc_is_devicemgr_shot_callback fail(%d).", frame->fcount);
		return -EINVAL;
	}

	framemgr_e_barrier_common(framemgr, 0, flags);
	put_frame(framemgr, frame, FS_HW_CONFIGURE);
	framemgr_x_barrier_common(framemgr, 0, flags);

	child = group->tail;

	while (child && (child->device_type == FIMC_IS_DEVICE_ISCHAIN)) {
		hw_maxnum = fimc_is_get_hw_list(child->id, hw_list);
		for (hw_index = hw_maxnum - 1; hw_index >= 0; hw_index--) {
			hw_id = hw_list[hw_index];
			hw_slot = fimc_is_hw_slot_id(hw_id);
			if (!valid_hw_slot_id(hw_slot)) {
				err_hw("[%d]invalid slot (%d,%d)", instance,
					hw_id, hw_slot);
				return -EINVAL;
			}

			hw_ip = &hardware->hw_ip[hw_slot];
			/* hw_ip->fcount : frame number of current frame in Vvalid  @ OTF *
			 * hw_ip->fcount is the frame number of next FRAME END interrupt  *
			 * In OTF scenario, hw_ip->fcount is not same as frame->fcount    */
			atomic_set(&hw_ip->fcount, framenum);
			atomic_set(&hw_ip->instance, instance);

			if (hw_ip->id != DEV_HW_VRA)
				CALL_HW_OPS(hw_ip, clk_gate, instance, true, false);

			ret = CALL_HW_OPS(hw_ip, shot, frame, hw_map);
			index = frame->fcount % DEBUG_FRAME_COUNT;
			hw_ip->debug_index[0] = frame->fcount;
			hw_ip->debug_info[index].cpuid[DEBUG_POINT_HW_SHOT] = raw_smp_processor_id();
			hw_ip->debug_info[index].time[DEBUG_POINT_HW_SHOT] = cpu_clock(raw_smp_processor_id());
			if (ret) {
				err_hw("[%d]shot fail (%d,%d)[F:%d]", instance,
					hw_id, hw_slot, frame->fcount);
				return -EINVAL;
			}
		}
		child = child->parent;
	}

#ifdef DBG_HW
	if (!atomic_read(&hardware->streaming[hardware->sensor_position[instance]])
		&& (hw_ip && atomic_read(&hw_ip->status.otf_start)))
		info_hw("[%d]shot [F:%d][G:0x%x][B:0x%lx][O:0x%lx][C:0x%lx][HWF:%d]\n",
			instance, frame->fcount, GROUP_ID(group->id),
			frame->bak_flag, frame->out_flag, frame->core_flag, framenum);
#endif

	return ret;
}

int fimc_is_hardware_get_meta(struct fimc_is_hw_ip *hw_ip, struct fimc_is_frame *frame,
	u32 instance, ulong hw_map, u32 output_id, enum ShotErrorType done_type)
{
	int ret = 0;

	BUG_ON(!hw_ip);

	if ((output_id != FIMC_IS_HW_CORE_END)
		&& (done_type == IS_SHOT_SUCCESS)
		&& (test_bit(hw_ip->id, &frame->core_flag))) {
		/* FIMC-IS v3.x only
		 * There is a chance that the DMA done interrupt occurred before
		 * the core done interrupt. So we skip to call the get_meta function.
		 */
		dbg_hw("%s: skip to get_meta [ID:%d][F:%d][B:0x%lx][C:0x%lx][O:0x%lx]\n",
			__func__, hw_ip->id, frame->fcount,
			frame->bak_flag, frame->core_flag, frame->out_flag);
		return 0;
	}

	switch (hw_ip->id) {
	case DEV_HW_3AA0:
	case DEV_HW_3AA1:
	case DEV_HW_ISP0:
	case DEV_HW_ISP1:
		copy_ctrl_to_dm(frame->shot);

		ret = CALL_HW_OPS(hw_ip, get_meta, frame, hw_map);
		if (ret) {
			err_hw("[%d][ID:%d][F:%d] get_meta fail", instance, hw_ip->id, frame->fcount);
			return 0;
		}
		break;
	case DEV_HW_TPU:
	case DEV_HW_VRA:
		ret = CALL_HW_OPS(hw_ip, get_meta, frame, hw_map);
		if (ret) {
			err_hw("[%d][ID:%d][F:%d] get_meta fail", instance, hw_ip->id, frame->fcount);
			return 0;
		}
		break;
	default:
		return 0;
		break;
	}

	dbg_hw("[%d]get_meta [ID:%d][G:0x%x][F:%d]\n", instance, hw_ip->id,
		GROUP_ID(hw_ip->group[instance]->id), frame->fcount);

	return ret;
}

int check_shot_exist(struct fimc_is_framemgr *framemgr, u32 fcount)
{
	struct fimc_is_frame *frame;

	if (framemgr->queued_count[FS_HW_WAIT_DONE]) {
		frame = find_frame(framemgr, FS_HW_WAIT_DONE, frame_fcount,
					(void *)(ulong)fcount);
		if (frame) {
			info_hw("[F:%d]is in complete_list\n", fcount);
			return INTERNAL_SHOT_EXIST;
		}
	}

	if (framemgr->queued_count[FS_HW_CONFIGURE]) {
		frame = find_frame(framemgr, FS_HW_CONFIGURE, frame_fcount,
					(void *)(ulong)fcount);
		if (frame) {
			info_hw("[F:%d]is in process_list\n", fcount);
			return INTERNAL_SHOT_EXIST;
		}
	}

	return 0;
}

int fimc_is_hardware_grp_shot(struct fimc_is_hardware *hardware, u32 instance,
	struct fimc_is_group *group, struct fimc_is_frame *frame, ulong hw_map)
{
	int ret = 0;
	int i, hw_slot = -1;
	struct fimc_is_hw_ip *hw_ip = NULL;
	enum fimc_is_hardware_id hw_id = DEV_HW_END;
	struct fimc_is_frame *hw_frame;
	struct fimc_is_framemgr *framemgr;
	struct fimc_is_group *head;
	ulong flags;

	BUG_ON(!hardware);
	BUG_ON(!frame);

	head = GET_HEAD_GROUP_IN_DEVICE(FIMC_IS_DEVICE_ISCHAIN, group);

	hw_id = get_hw_id_from_group(head->id);
	if (hw_id == DEV_HW_END)
		return -EINVAL;

	hw_slot = fimc_is_hw_slot_id(hw_id);
	if (!valid_hw_slot_id(hw_slot)) {
		err_hw("[%d]invalid slot (%d,%d)", instance, hw_id, hw_slot);
		return -EINVAL;
	}

	hw_ip = &hardware->hw_ip[hw_slot];
	if (hw_ip == NULL) {
		err_hw("[%d][G:0x%d]hw_ip(null) (%d,%d)", instance,
			GROUP_ID(head->id), hw_id, hw_slot);
		return -EINVAL;
	}

	if (!atomic_read(&hardware->streaming[hardware->sensor_position[instance]]))
		info_hw("[%d]grp_shot [F:%d][G:0x%x][B:0x%lx][O:0x%lx][IN:0x%x]\n",
			instance, frame->fcount, GROUP_ID(head->id),
			frame->bak_flag, frame->out_flag, frame->dvaddr_buffer[0]);

	framemgr = hw_ip->framemgr;
	framemgr_e_barrier_irqs(framemgr, 0, flags);
	ret = check_shot_exist(framemgr, frame->fcount);

	/* check late shot */
	if (hw_ip->internal_fcount >= frame->fcount || ret == INTERNAL_SHOT_EXIST) {
		info_hw("[%d]LATE_SHOT (%d)[F:%d][G:0x%x][B:0x%lx][O:0x%lx][C:0x%lx]\n",
			instance, hw_ip->internal_fcount, frame->fcount, GROUP_ID(head->id),
			frame->bak_flag, frame->out_flag, frame->core_flag);
		frame->type = SHOT_TYPE_LATE;
		/* unlock previous framemgr */
		framemgr_x_barrier_irqr(framemgr, 0, flags);
		framemgr = hw_ip->framemgr_late;
		/* lock by late framemgr */
		framemgr_e_barrier_irqs(framemgr, 0, flags);
		if (framemgr->queued_count[FS_HW_REQUEST] > 0) {
			warn_hw("[%d]LATE_SHOT REQ(%d) > 0, PRO(%d)",
				instance,
				framemgr->queued_count[FS_HW_REQUEST],
				framemgr->queued_count[FS_HW_CONFIGURE]);
		}

		if (frame->lindex || frame->hindex)
			set_bit(FIMC_IS_SUBDEV_FORCE_SET, &head->leader.state);

		ret = 0;
	} else {
		frame->type = SHOT_TYPE_EXTERNAL;
	}

	hw_frame = get_frame(framemgr, FS_HW_FREE);
	if (hw_frame == NULL) {
		framemgr_x_barrier_irqr(framemgr, 0, flags);
		err_hw("[%d][G:0x%x]free_head(NULL)", instance, GROUP_ID(head->id));
		return -EINVAL;
	}

	hw_frame->groupmgr	= frame->groupmgr;
	hw_frame->group		= frame->group;
	hw_frame->shot		= frame->shot;
	hw_frame->shot_ext	= frame->shot_ext;
	hw_frame->kvaddr_shot	= frame->kvaddr_shot;
	hw_frame->dvaddr_shot	= frame->dvaddr_shot;
	hw_frame->shot_size	= frame->shot_size;
	hw_frame->fcount	= frame->fcount;
	hw_frame->rcount	= frame->rcount;
	hw_frame->bak_flag	= GET_OUT_FLAG_IN_DEVICE(FIMC_IS_DEVICE_ISCHAIN, frame->bak_flag);
	hw_frame->out_flag	= GET_OUT_FLAG_IN_DEVICE(FIMC_IS_DEVICE_ISCHAIN, frame->out_flag);
	hw_frame->core_flag	= 0;
	atomic_set(&hw_frame->shot_done_flag, 1);

	for (i = 0; i < FIMC_IS_MAX_PLANES; i++) {
		hw_frame->dvaddr_buffer[i] = frame->dvaddr_buffer[i];
		hw_frame->kvaddr_buffer[i] = frame->kvaddr_buffer[i];
	}
	hw_frame->instance = instance;
	hw_frame->type = frame->type;

	if (test_bit(FIMC_IS_GROUP_OTF_INPUT, &hw_ip->group[instance]->state)) {
		if (!atomic_read(&hw_ip->status.otf_start)) {
			atomic_set(&hw_ip->status.otf_start, 1);
			info_hw("[%d]OTF start [F:%d][G:0x%x][B:0x%lx][O:0x%lx]\n",
				instance, hw_frame->fcount, GROUP_ID(head->id),
				hw_frame->bak_flag, hw_frame->out_flag);

			for (hw_slot = 0; hw_slot < HW_SLOT_MAX; hw_slot++) {
				hw_ip = &hardware->hw_ip[hw_slot];
				if (test_bit(hw_ip->id, &hw_map)) {
					dbg_hw("[%d][ID:%d]count clear\n", instance, hw_ip->id);
					atomic_set(&hw_ip->count.fs, (frame->fcount - 1));
					atomic_set(&hw_ip->count.cl, (frame->fcount - 1));
					atomic_set(&hw_ip->count.fe, (frame->fcount - 1));
					atomic_set(&hw_ip->count.dma, (frame->fcount - 1));
				}
			}

			if (frame->type == SHOT_TYPE_LATE) {
				put_frame(framemgr, hw_frame, FS_HW_REQUEST);
				framemgr_x_barrier_irqr(framemgr, 0, flags);
				return ret;
			}
		} else {
			atomic_set(&hw_ip->hardware->log_count, 0);
			put_frame(framemgr, hw_frame, FS_HW_REQUEST);
			framemgr_x_barrier_irqr(framemgr, 0, flags);

			mod_timer(&hw_ip->shot_timer, jiffies + msecs_to_jiffies(FIMC_IS_SHOT_TIMEOUT));

			return ret;
		}
	} else {
		mod_timer(&hw_ip->shot_timer, jiffies + msecs_to_jiffies(FIMC_IS_SHOT_TIMEOUT));
	}

	framemgr_x_barrier_irqr(framemgr, 0, flags);

	ret = fimc_is_hardware_shot(hardware, instance, head, hw_frame, framemgr,
			hw_map, frame->fcount);
	if (ret) {
		err_hw("hardware_shot fail [G:0x%x](%d)", GROUP_ID(head->id),
			hw_ip->id);
		return -EINVAL;
	}

	return ret;
}

int make_internal_shot(struct fimc_is_hw_ip *hw_ip, u32 instance, u32 fcount,
	struct fimc_is_frame **in_frame, struct fimc_is_framemgr *framemgr)
{
	int ret = 0;
	int i = 0;
	struct fimc_is_frame *frame;

	BUG_ON(!hw_ip);
	BUG_ON(!framemgr);

	if (framemgr->queued_count[FS_HW_FREE] < 3) {
		warn_hw("[%d][ID:%d] Free frame is less than 3", instance, hw_ip->id);
		check_hw_bug_count(hw_ip->hardware, 10);
	}

	ret = check_shot_exist(framemgr, fcount);
	if (ret == INTERNAL_SHOT_EXIST)
		return ret;

	frame = get_frame(framemgr, FS_HW_FREE);
	if (frame == NULL) {
		err_hw("[%d]config_lock: frame(null)", instance);
		frame_manager_print_info_queues(framemgr);
		return -EINVAL;
	}
	frame->groupmgr		= NULL;
	frame->group		= NULL;
	frame->shot		= NULL;
	frame->shot_ext		= NULL;
	frame->kvaddr_shot	= 0;
	frame->dvaddr_shot	= 0;
	frame->shot_size	= 0;
	frame->fcount		= fcount;
	frame->rcount		= 0;
	frame->bak_flag		= 0;
	frame->out_flag		= 0;
	frame->core_flag	= 0;
	atomic_set(&frame->shot_done_flag, 1);

	for (i = 0; i < FIMC_IS_MAX_PLANES; i++)
		frame->dvaddr_buffer[i]	= 0;

	frame->type = SHOT_TYPE_INTERNAL;
	frame->instance = instance;
	*in_frame = frame;

	mod_timer(&hw_ip->shot_timer, jiffies + msecs_to_jiffies(FIMC_IS_SHOT_TIMEOUT));

	return ret;
}

int fimc_is_hardware_config_lock(struct fimc_is_hw_ip *hw_ip, u32 instance, u32 framenum)
{
	int ret = 0;
	struct fimc_is_frame *frame;
	struct fimc_is_framemgr *framemgr;
	struct fimc_is_hardware *hardware;
	struct fimc_is_device_sensor *sensor;
	u32 sensor_fcount;
	u32 fcount = framenum + 1;
	u32 log_count;

	BUG_ON(!hw_ip);

	hardware = hw_ip->hardware;

	if (!test_bit(FIMC_IS_GROUP_OTF_INPUT, &hw_ip->group[instance]->state))
		return ret;

	dbg_hw("C.L [F:%d]\n", framenum);

	sensor = hw_ip->group[instance]->device->sensor;
	sensor_fcount = sensor->fcount;
	if (framenum < sensor_fcount) {
		warn_hw("[%d]fcount mismatch(%d, %d)\n", instance, framenum, sensor_fcount);
		framenum = sensor_fcount;
		fcount = framenum + 1;
		atomic_set(&hw_ip->count.fs, sensor_fcount);
	}

	framemgr = hw_ip->framemgr;

retry_get_frame:
	framemgr_e_barrier(framemgr, 0);
	if (framemgr->queued_count[FS_HW_REQUEST]) {
		frame = get_frame(framemgr, FS_HW_REQUEST);
		if (unlikely(frame->fcount <= framenum)) {
			put_frame(framemgr, frame, FS_HW_WAIT_DONE);
			framemgr_x_barrier(framemgr, 0);
			fimc_is_hardware_frame_ndone(hw_ip, frame, instance, IS_SHOT_UNPROCESSED);
			goto retry_get_frame;
		}
	} else {
		ret = make_internal_shot(hw_ip, instance, fcount, &frame, framemgr);
		if (ret == INTERNAL_SHOT_EXIST) {
			framemgr_x_barrier(framemgr, 0);
			return ret;
		}
		if (ret) {
			framemgr_x_barrier(framemgr, 0);
			print_all_hw_frame_count(hardware);
			BUG_ON(1);
		}
		log_count = atomic_read(&hardware->log_count);
		if ((log_count <= 20) || !(log_count % 100))
			info_hw("config_lock: INTERNAL_SHOT [F:%d](%d) count(%d)\n",
				fcount, frame->index, log_count);
	}
	frame->frame_info[INFO_CONFIG_LOCK].cpu = raw_smp_processor_id();
	frame->frame_info[INFO_CONFIG_LOCK].pid = current->pid;
	frame->frame_info[INFO_CONFIG_LOCK].when = cpu_clock(raw_smp_processor_id());

	framemgr_x_barrier(framemgr, 0);

	ret = fimc_is_hardware_shot(hardware, instance, hw_ip->group[instance],
			frame, framemgr, hardware->hw_map[instance], framenum);
	if (ret) {
		err_hw("hardware_shot fail [G:0x%x](%d)",
			GROUP_ID(hw_ip->group[instance]->id), hw_ip->id);
		return -EINVAL;
	}

	return ret;
}

void check_late_shot(struct fimc_is_hw_ip *hw_ip)
{
	int ret = 0;
	struct fimc_is_frame *frame;
	struct fimc_is_framemgr *framemgr;

	/* check LATE_FRAME */
	framemgr = hw_ip->framemgr_late;
	framemgr_e_barrier(framemgr, 0);
	if (!framemgr->queued_count[FS_HW_REQUEST]) {
		framemgr_x_barrier(framemgr, 0);
		return;
	}
	frame = get_frame(framemgr, FS_HW_REQUEST);
	framemgr_x_barrier(framemgr, 0);

	if (frame == NULL)
		return;

	framemgr_e_barrier(framemgr, 0);
	put_frame(framemgr, frame, FS_HW_WAIT_DONE);
	framemgr_x_barrier(framemgr, 0);

	ret = fimc_is_hardware_frame_ndone(hw_ip, frame, frame->instance, IS_SHOT_LATE_FRAME);
	if (ret)
		err_hw("[%d]F_NDONE fail (%d)", frame->instance, hw_ip->id);

	return;
}

void fimc_is_hardware_size_dump(struct fimc_is_hw_ip *hw_ip)
{
	int hw_slot = -1;
	struct fimc_is_hardware *hardware;
	u32 instance;

	BUG_ON(!hw_ip);
	BUG_ON(!hw_ip->hardware);

	instance = atomic_read(&hw_ip->instance);
	hardware = hw_ip->hardware;

	for (hw_slot = 0; hw_slot < HW_SLOT_MAX; hw_slot++) {
		hw_ip = &hardware->hw_ip[hw_slot];
		if (hw_ip->ops->size_dump) {
			CALL_HW_OPS(hw_ip, clk_gate, instance, true, false);
			hw_ip->ops->size_dump(hw_ip);;
			CALL_HW_OPS(hw_ip, clk_gate, instance, false, false);
		}
	}

	return;
}

void fimc_is_hardware_clk_gate_dump(struct fimc_is_hardware *hardware)
{
#ifdef ENABLE_DIRECT_CLOCK_GATE
	int hw_slot = -1;
	struct fimc_is_hw_ip *hw_ip;
	struct fimc_is_clk_gate *clk_gate;

	for (hw_slot = 0; hw_slot < HW_SLOT_MAX; hw_slot++) {
		hw_ip = &hardware->hw_ip[hw_slot];
		if (hw_ip && hw_ip->clk_gate) {
			clk_gate = hw_ip->clk_gate;
			info_hw("[ID:%d] CLOCK_ENABLE(0x%08X) ref(%d)\n",
					hw_ip->id, __raw_readl(clk_gate->regs), clk_gate->refcnt[hw_slot]);

			/* do clock on for later other dump */
			FIMC_IS_CLOCK_ON(clk_gate->regs, clk_gate->bit[hw_ip->clk_gate_idx]);
		}
	}
#endif
	return;
}

void fimc_is_hardware_frame_start(struct fimc_is_hw_ip *hw_ip, u32 instance)
{
	struct fimc_is_frame *frame;
	struct fimc_is_framemgr *framemgr;
	struct fimc_is_group *head;

	BUG_ON(!hw_ip);

	head = GET_HEAD_GROUP_IN_DEVICE(FIMC_IS_DEVICE_ISCHAIN,
			hw_ip->group[instance]);
	BUG_ON(!head);

	framemgr = hw_ip->framemgr;
	framemgr_e_barrier(framemgr, 0);

	/*
	 * If there are hw_ips having framestart processing
	 * and they are bound by OTF, the problem that same action was duplicated
	 * maybe happened.
	 * ex. 1) 3A0* => ISP* -> MCSC0* : no problem
	 *     2) 3A0* -> ISP  -> MCSC0* : problem happened!!
	 *      (* : called fimc_is_hardware_frame_start)
	 * Only leader group in OTF groups can control frame.
	 */
	if (hw_ip->group[instance]->id == head->id) {
		if (framemgr->queued_count[FS_HW_CONFIGURE]) {
			frame = get_frame(framemgr, FS_HW_CONFIGURE);
		} else {
			/* error happened..print the frame info */
			frame_manager_print_info_queues(framemgr);
			print_all_hw_frame_count(hw_ip->hardware);
			framemgr_x_barrier(framemgr, 0);
			err_hw("[%d]FSTART frame null [ID:%d](%d) (%d != %d)",
					instance,
					hw_ip->id, hw_ip->internal_fcount,
					hw_ip->group[instance]->id, head->id);
			return;
		}
	} else {
		goto check;
	}

	if (atomic_read(&hw_ip->status.otf_start)
		&& frame->fcount != atomic_read(&hw_ip->count.fs)) {
		/* error handling */
		info_hw("frame_start_isr (%d, %d)\n", frame->fcount,
			atomic_read(&hw_ip->count.fs));
	}
	/* TODO: multi-instance */
	frame->frame_info[INFO_FRAME_START].cpu = raw_smp_processor_id();
	frame->frame_info[INFO_FRAME_START].pid = current->pid;
	frame->frame_info[INFO_FRAME_START].when = cpu_clock(raw_smp_processor_id());
	put_frame(framemgr, frame, FS_HW_WAIT_DONE);
check:
	clear_bit(HW_CONFIG, &hw_ip->state);
	atomic_set(&hw_ip->status.Vvalid, V_VALID);
	framemgr_x_barrier(framemgr, 0);

	if (test_bit(FIMC_IS_GROUP_OTF_INPUT, &hw_ip->group[instance]->state))
		check_late_shot(hw_ip);

	return;
}

int fimc_is_hardware_sensor_start(struct fimc_is_hardware *hardware, u32 instance,
	ulong hw_map)
{
	int ret = 0;
	int hw_slot = -1;
	struct fimc_is_hw_ip *hw_ip;
	enum fimc_is_hardware_id hw_id = DEV_HW_END;

	BUG_ON(!hardware);

	if (test_bit(DEV_HW_3AA0, &hw_map)) {
		hw_id = DEV_HW_3AA0;
	} else if (test_bit(DEV_HW_3AA1, &hw_map)) {
		hw_id = DEV_HW_3AA1;
	} else {
		warn_hw("[%d]invalid state hw_map[0x%lx]\n", instance, hw_map);
		return 0;
	}

	hw_slot = fimc_is_hw_slot_id(hw_id);
	if (!valid_hw_slot_id(hw_slot)) {
		err_hw("[%d]invalid slot (%d,%d)", instance, hw_id, hw_slot);
		return -EINVAL;
	}

	hw_ip = &hardware->hw_ip[hw_slot];
	if (hw_ip == NULL) {
		err_hw("[%d]hw_ip(null) (%d,%d)", instance, hw_id, hw_slot);
		return -EINVAL;
	}

	ret = fimc_is_hw_3aa_mode_change(hw_ip, instance, hw_map);
	if (ret) {
		err_hw("[%d]mode_change fail (%d,%d)", instance, hw_ip->id, hw_slot);
		return -EINVAL;
	}

	atomic_set(&hardware->streaming[hardware->sensor_position[instance]], 1);
	atomic_set(&hardware->bug_count, 0);
	atomic_set(&hardware->log_count, 0);

	info_hw("[%d]hw_sensor_start [P:0x%lx]\n", instance, hw_map);

	return ret;
}

int fimc_is_hardware_sensor_stop(struct fimc_is_hardware *hardware, u32 instance,
	ulong hw_map)
{
	int ret = 0;
	int hw_slot = -1;
	int retry;
	struct fimc_is_frame *frame;
	struct fimc_is_framemgr *framemgr;
	struct fimc_is_group *group;
	struct fimc_is_hw_ip *hw_ip = NULL;
	enum fimc_is_hardware_id hw_id = DEV_HW_END;

	BUG_ON(!hardware);

	atomic_set(&hardware->streaming[hardware->sensor_position[instance]], 0);
	atomic_set(&hardware->bug_count, 0);
	atomic_set(&hardware->log_count, 0);

	if (test_bit(DEV_HW_3AA0, &hw_map)) {
		hw_id = DEV_HW_3AA0;
	} else if (test_bit(DEV_HW_3AA1, &hw_map)) {
		hw_id = DEV_HW_3AA1;
	} else {
		warn_hw("[%d]invalid state hw_map[0x%lx]\n", instance, hw_map);
		return 0;
	}

	hw_slot = fimc_is_hw_slot_id(hw_id);
	if (!valid_hw_slot_id(hw_slot)) {
		err_hw("[%d]invalid slot (%d,%d)", instance,
			hw_id, hw_slot);
		return -EINVAL;
	}

	hw_ip = &hardware->hw_ip[hw_slot];
	group = hw_ip->group[instance];
	framemgr = hw_ip->framemgr;
	if (test_bit(FIMC_IS_GROUP_OTF_INPUT, &group->state)) {
		retry = 99;
		while (--retry && framemgr->queued_count[FS_HW_WAIT_DONE]) {
			frame = peek_frame(framemgr, FS_HW_WAIT_DONE);
			if (frame == NULL)
				break;

			info_hw("hw_sensor_stop: com_list: [F:%d][%d][O:0x%lx][C:0x%lx][(%d)",
				frame->fcount, frame->type, frame->out_flag, frame->core_flag,
				framemgr->queued_count[FS_HW_WAIT_DONE]);
			warn_hw(" %d com waiting...", framemgr->queued_count[FS_HW_WAIT_DONE]);
			usleep_range(1000, 1000);
		}

		if (!retry) {
			ret = fimc_is_hardware_frame_ndone(hw_ip, frame, instance, IS_SHOT_UNPROCESSED);
			if (ret)
				err_hw("[%d]hardware_frame_ndone fail (%d)", instance, hw_ip->id);
		}

		/* for last fcount */
		print_all_hw_frame_count(hardware);
	}

	info_hw("[%d]hw_sensor_stop: done[P:0x%lx]\n", instance, hw_map);

	return ret;
}

int fimc_is_hardware_process_start(struct fimc_is_hardware *hardware, u32 instance,
	u32 group_id)
{
	int ret = 0;
	int hw_slot = -1;
	ulong hw_map;
	int hw_list[GROUP_HW_MAX];
	int hw_index, hw_maxnum;
	enum fimc_is_hardware_id hw_id = DEV_HW_END;
	struct fimc_is_hw_ip *hw_ip = NULL;

	BUG_ON(!hardware);

	dbg_hw("[%d]process_start [G:0x%x]\n", instance, GROUP_ID(group_id));

	hw_map = hardware->hw_map[instance];
	hw_maxnum = fimc_is_get_hw_list(group_id, hw_list);
	for (hw_index = 0; hw_index < hw_maxnum; hw_index++) {
		hw_id = hw_list[hw_index];
		hw_slot = fimc_is_hw_slot_id(hw_id);
		if (!valid_hw_slot_id(hw_slot)) {
			err_hw("[%d]invalid slot (%d,%d)", instance,
				hw_id, hw_slot);
			return -EINVAL;
		}

		hw_ip = &hardware->hw_ip[hw_slot];

		CALL_HW_OPS(hw_ip, clk_gate, instance, true, false);
		ret = CALL_HW_OPS(hw_ip, enable, instance, hw_map);
		CALL_HW_OPS(hw_ip, clk_gate, instance, false, false);
		if (ret) {
			err_hw("[%d]enable fail (%d,%d)", instance, hw_ip->id, hw_slot);
			return -EINVAL;
		}
		hw_ip->internal_fcount = 0;
	}

	return ret;
}

void fimc_is_hardware_force_stop(struct fimc_is_hardware *hardware,
	struct fimc_is_hw_ip *hw_ip, u32 instance)
{
	int ret = 0;
	int retry, list_index;
	struct fimc_is_frame *frame;
	struct fimc_is_framemgr *framemgr;
	struct fimc_is_framemgr *framemgr_late;

	BUG_ON(!hw_ip);

	framemgr = hw_ip->framemgr;
	framemgr_late = hw_ip->hardware->framemgr_late;

	pr_info("[@][HW][%d]complete_list (%d)(%d)(%d)\n", instance,
		framemgr->queued_count[FS_HW_WAIT_DONE],
		framemgr->queued_count[FS_HW_CONFIGURE],
		framemgr->queued_count[FS_HW_REQUEST]);

	retry = 150;
	while (--retry && framemgr->queued_count[FS_HW_WAIT_DONE]) {
		frame = peek_frame(framemgr, FS_HW_WAIT_DONE);
		if (frame == NULL)
			break;

		info_hw("complete_list [F:%d][%d][O:0x%lx][C:0x%lx][(%d)",
			frame->fcount, frame->type, frame->out_flag, frame->core_flag,
			framemgr->queued_count[FS_HW_WAIT_DONE]);
		ret = fimc_is_hardware_frame_ndone(hw_ip, frame, instance, IS_SHOT_UNPROCESSED);
		if (ret) {
			err_hw("[%d]hardware_frame_ndone fail (%d)", instance, hw_ip->id);
			return;
		}
		warn_hw(" %d com waiting...", framemgr->queued_count[FS_HW_WAIT_DONE]);
		msleep(1);
	}

	info_hw("[%d]process_list (%d)\n", instance, framemgr->queued_count[FS_HW_CONFIGURE]);
	retry = 150;
	while (--retry && framemgr->queued_count[FS_HW_CONFIGURE]) {
		frame = peek_frame(framemgr, FS_HW_CONFIGURE);
		if (frame == NULL)
			break;

		info_hw("process_list [F:%d][%d][O:0x%lx][C:0x%lx][(%d)",
			frame->fcount, frame->type, frame->out_flag, frame->core_flag,
			framemgr->queued_count[FS_HW_CONFIGURE]);

		set_bit(hw_ip->id, &frame->core_flag);

		ret = fimc_is_hardware_frame_ndone(hw_ip, frame, instance, IS_SHOT_UNPROCESSED);
		if (ret) {
			err_hw("[%d]hardware_frame_ndone fail (%d)", instance, hw_ip->id);
			return;
		}
		warn_hw(" %d pro waiting...", framemgr->queued_count[FS_HW_CONFIGURE]);
		msleep(1);
	}

	info_hw("[%d]request_list (%d)\n", instance, framemgr->queued_count[FS_HW_REQUEST]);
	retry = 150;
	while (--retry && framemgr->queued_count[FS_HW_REQUEST]) {
		frame = peek_frame(framemgr, FS_HW_REQUEST);
		if (frame == NULL)
			break;

		info_hw("request_list [F:%d](%d)", frame->fcount,
			framemgr->queued_count[FS_HW_REQUEST]);

		set_bit(hw_ip->id, &frame->core_flag);

		ret = fimc_is_hardware_frame_ndone(hw_ip, frame, instance, IS_SHOT_UNPROCESSED);
		if (ret) {
			err_hw("[%d]hardware_frame_ndone fail (%d)", instance, hw_ip->id);
			return;
		}
		warn_hw(" %d req waiting...", framemgr->queued_count[FS_HW_REQUEST]);
		msleep(1);
	}

	pr_info("[@][HW][%d]late_list (%d)(%d)(%d)\n",
		instance, framemgr_late->queued_count[FS_HW_WAIT_DONE],
		framemgr_late->queued_count[FS_HW_CONFIGURE],
		framemgr_late->queued_count[FS_HW_REQUEST]);
	for (list_index = FS_HW_REQUEST; list_index < FS_HW_INVALID; list_index++) {
		info_hw("[%d]late_list[%d] (%d)\n",
			instance, list_index, framemgr_late->queued_count[list_index]);
		retry = 150;
		while (--retry && framemgr_late->queued_count[list_index]) {
			frame = peek_frame(framemgr_late, list_index);
			if (frame == NULL)
				break;

			info_hw("late_list[%d] [F:%d][%d][O:0x%lx][C:0x%lx][(%d)",
				list_index, frame->fcount, frame->type,
				frame->out_flag, frame->core_flag,
				framemgr_late->queued_count[list_index]);

			set_bit(hw_ip->id, &frame->core_flag);

			ret = fimc_is_hardware_frame_ndone(hw_ip, frame, instance, IS_SHOT_LATE_FRAME);
			if (ret) {
				err_hw("[%d]hardware_frame_ndone fail (%d)", instance, hw_ip->id);
				return;
			}
			warn_hw(" %d late waiting...", framemgr_late->queued_count[list_index]);
			msleep(1);
		}
	}

	return;
}

void fimc_is_hardware_process_stop(struct fimc_is_hardware *hardware, u32 instance,
	u32 group_id, u32 mode)
{
	int ret;
	int hw_slot = -1;
	int hw_list[GROUP_HW_MAX];
	int hw_index, hw_maxnum;
	ulong hw_map;
	struct fimc_is_hw_ip *hw_ip;
	enum fimc_is_hardware_id hw_id = DEV_HW_END;
	struct fimc_is_framemgr *framemgr;
	int retry;

	BUG_ON(!hardware);

	dbg_hw("[%d]process_stop [G:0x%x](%d)\n", instance, GROUP_ID(group_id), mode);

	hw_id = get_hw_id_from_group(group_id);
	hw_slot = fimc_is_hw_slot_id(hw_id);
	if (!valid_hw_slot_id(hw_slot)) {
		err_hw("[%d]invalid slot (%d,%d)", instance,
			hw_id, hw_slot);
		return;
	}
	hw_ip = &hardware->hw_ip[hw_slot];
	BUG_ON(!hw_ip);

	framemgr = hw_ip->framemgr;
	BUG_ON(!framemgr);

	retry = 50;
	while (--retry && framemgr->queued_count[FS_HW_WAIT_DONE]) {
		warn_hw("[%d][ID:%d]HW_WAIT_DONE(%d) com waiting...", instance, hw_ip->id,
			framemgr->queued_count[FS_HW_WAIT_DONE]);
		usleep_range(1000, 1000);
	}
	if (!retry)
		warn_hw("[ID:%d]waiting(until frame empty) is fail", hw_ip->id);

	hw_map = hardware->hw_map[instance];
	hw_maxnum = fimc_is_get_hw_list(group_id, hw_list);
	for (hw_index = 0; hw_index < hw_maxnum; hw_index++) {
		hw_id = hw_list[hw_index];
		hw_slot = fimc_is_hw_slot_id(hw_id);
		if (!valid_hw_slot_id(hw_slot)) {
			err_hw("[%d]invalid slot (%d,%d)", instance,
				hw_id, hw_slot);
			return;
		}

		hw_ip = &hardware->hw_ip[hw_slot];

		CALL_HW_OPS(hw_ip, clk_gate, instance, true, false);
		ret = CALL_HW_OPS(hw_ip, disable, instance, hw_map);
		CALL_HW_OPS(hw_ip, clk_gate, instance, false, false);
		if (ret) {
			err_hw("[%d]disable fail (%d,%d)", instance, hw_ip->id, hw_slot);
		}
	}

	hw_id = get_hw_id_from_group(group_id);
	if (hw_id == DEV_HW_END)
		return;

	hw_slot = fimc_is_hw_slot_id(hw_id);
	if (!valid_hw_slot_id(hw_slot)) {
		if (test_bit(hw_id, &hw_map))
			err_hw("[%d]invalid slot (%d,%d)", instance, hw_id, hw_slot);
		return;
	}

	hw_ip = &hardware->hw_ip[hw_slot];
	if (hw_ip == NULL) {
		err_hw("[%d][G:0x%d]hw_ip(null) (%d,%d)", instance,
			group_id, hw_id, hw_slot);
		return;
	}

	if (mode == 0)
		return;

	CALL_HW_OPS(hw_ip, clk_gate, instance, true, false);
	fimc_is_hardware_force_stop(hardware, hw_ip, instance);
	CALL_HW_OPS(hw_ip, clk_gate, instance, false, false);
	atomic_set(&hw_ip->status.otf_start, 0);

	/* reset shot timer after force stop */
	del_timer_sync(&hw_ip->shot_timer);
	setup_timer(&hw_ip->shot_timer, fimc_is_hardware_shot_timer, (unsigned long)hw_ip);

	return;
}

int fimc_is_hardware_open(struct fimc_is_hardware *hardware, u32 hw_id,
	struct fimc_is_group *group, u32 instance, bool rep_flag, u32 module_id)
{
	int ret = 0;
	int hw_slot = -1;
	struct fimc_is_hw_ip *hw_ip = NULL;
	struct fimc_is_group *head;
	u32 size = 0;

	BUG_ON(!hardware);

	head = GET_HEAD_GROUP_IN_DEVICE(FIMC_IS_DEVICE_ISCHAIN,
			group);
	BUG_ON(!head);

	hw_slot = fimc_is_hw_slot_id(hw_id);
	if (!valid_hw_slot_id(hw_slot))
		return 0;

	hw_ip = &(hardware->hw_ip[hw_slot]);
	hw_ip->group[instance] = group;

	/* HACK : VRA open skip if it's already opened */
	if (hw_id == DEV_HW_VRA &&
		test_bit(HW_OPEN, &hw_ip->state))
		return 0;

	hw_ip->hardware = hardware;
	hw_ip->framemgr = &hardware->framemgr[head->id];
	hw_ip->framemgr_late = &hardware->framemgr_late[head->id];

	CALL_HW_OPS(hw_ip, clk_gate, instance, true, false);
	ret = CALL_HW_OPS(hw_ip, open, instance, &size);
	CALL_HW_OPS(hw_ip, clk_gate, instance, false, false);
	if (ret) {
		err_hw("[%d]open fail (%d)", instance, hw_ip->id);
		return ret;
	}
	if (size) {
		hw_ip->priv_info = kzalloc(size, GFP_KERNEL);
		if(!hw_ip->priv_info) {
			err_hw("hw_ip->priv_info(null) (%d)", hw_ip->id);
			return -EINVAL;
		}
	}

	CALL_HW_OPS(hw_ip, clk_gate, instance, true, false);
	ret = CALL_HW_OPS(hw_ip, init, group, rep_flag, module_id);
	CALL_HW_OPS(hw_ip, clk_gate, instance, false, false);
	if (ret) {
		err_hw("[%d]init fail (%d)", instance, hw_ip->id);
		return ret;
	}

	memset(hw_ip->debug_info, 0x00, sizeof(struct hw_debug_info) * DEBUG_FRAME_COUNT);
	memset(&hw_ip->setfile, 0x00, sizeof(struct fimc_is_hw_ip_setfile));

	if (!test_bit(HW_OPEN, &hw_ip->state)) {
		setup_timer(&hw_ip->shot_timer, fimc_is_hardware_shot_timer, (unsigned long)hw_ip);
	}

	set_bit(HW_OPEN, &hw_ip->state);
	set_bit(HW_INIT, &hw_ip->state);
	atomic_inc(&hw_ip->rsccount);

	if (!rep_flag) {
		hw_ip->debug_index[0] = 0;
		hw_ip->debug_index[1] = 0;
		atomic_set(&hw_ip->count.fs, 0);
		atomic_set(&hw_ip->count.cl, 0);
		atomic_set(&hw_ip->count.fe, 0);
		atomic_set(&hw_ip->count.dma, 0);
		atomic_set(&hw_ip->status.Vvalid, V_BLANK);
	}
	set_bit(hw_id, &hardware->hw_map[instance]);

	/* HACK: */
	if (atomic_read(&hardware->rsccount) == 0)
		fimc_is_hw_s_ctrl(hw_ip->itfc, DEV_HW_TPU, HW_S_CTRL_FULL_BYPASS, (void *)true);

	info_hw("[%d]open (%d)(%d)\n", instance, hw_ip->id, atomic_read(&hw_ip->rsccount));

	return ret;
}

int fimc_is_hardware_close(struct fimc_is_hardware *hardware,u32 hw_id, u32 instance)
{
	int ret = 0;
	int hw_slot = -1;
	struct fimc_is_hw_ip *hw_ip = NULL;

	BUG_ON(!hardware);

	hw_slot = fimc_is_hw_slot_id(hw_id);
	if (!valid_hw_slot_id(hw_slot))
		return 0;

	if (!test_bit(hw_id, &hardware->hw_map[instance]))
		return 0;

	hw_ip = &(hardware->hw_ip[hw_slot]);

	switch (hw_id) {
	case DEV_HW_3AA0:
	case DEV_HW_3AA1:
		fimc_is_hw_3aa_object_close(hw_ip, instance);
		break;
	case DEV_HW_ISP0:
	case DEV_HW_ISP1:
		fimc_is_hw_isp_object_close(hw_ip, instance);
		break;
	case DEV_HW_TPU:
		fimc_is_hw_tpu_object_close(hw_ip, instance);
		break;
	case DEV_HW_VRA:
		break;
		/* TODO */
	default:
		break;
	}

	if (!atomic_dec_and_test(&hw_ip->rsccount)) {
		info_hw("[%d][ID:%d] rsccount(%d)\n", instance, hw_ip->id,
			atomic_read(&hw_ip->rsccount));
		clear_bit(hw_id, &hardware->hw_map[instance]);
		return 0;
	}

	CALL_HW_OPS(hw_ip, clk_gate, instance, true, true);
	ret = CALL_HW_OPS(hw_ip, close, instance);
	if (ret) {
		err_hw("[%d]close fail (%d)", instance, hw_ip->id);
		return 0;
	}

	kfree(hw_ip->priv_info);
	clear_bit(hw_id, &hardware->hw_map[instance]);

	memset(hw_ip->debug_info, 0x00, sizeof(struct hw_debug_info) * DEBUG_FRAME_COUNT);
	hw_ip->debug_index[0] = 0;
	hw_ip->debug_index[1] = 0;
	clear_bit(HW_OPEN, &hw_ip->state);
	clear_bit(HW_INIT, &hw_ip->state);
	clear_bit(HW_CONFIG, &hw_ip->state);
	clear_bit(HW_RUN, &hw_ip->state);
	clear_bit(HW_TUNESET, &hw_ip->state);
	atomic_set(&hw_ip->status.otf_start, 0);
	atomic_set(&hw_ip->fcount, 0);
	atomic_set(&hw_ip->instance, 0);
	hw_ip->internal_fcount = 0;

	del_timer_sync(&hw_ip->shot_timer);

	return ret;
}

int do_frame_done_work_func(struct fimc_is_interface *itf, int wq_id, u32 instance,
	u32 group_id, u32 fcount, u32 rcount, u32 status)
{
	int ret = 0;
	bool retry_flag = false;
	struct work_struct *work_wq;
	struct fimc_is_work_list *work_list;
	struct fimc_is_work *work;

	work_wq   = &itf->work_wq[wq_id];
	work_list = &itf->work_list[wq_id];
retry:
	get_free_work_irq(work_list, &work);
	if (work) {
		work->msg.id		= 0;
		work->msg.command	= IHC_FRAME_DONE;
		work->msg.instance	= instance;
		work->msg.group		= GROUP_ID(group_id);
		work->msg.param1	= fcount;
		work->msg.param2	= rcount;
		work->msg.param3	= status; /* status: enum ShotErrorType */
		work->msg.param4	= 0;

		work->fcount = work->msg.param1;
		set_req_work_irq(work_list, work);

		if (!work_pending(work_wq))
			wq_func_schedule(itf, work_wq);
	} else {
		err_hw("free work item is empty (%d)", (int)retry_flag);
		if (retry_flag == false) {
			retry_flag = true;
			goto retry;
		}
		ret = -EINVAL;
	}

	return ret;
}

int check_core_end(struct fimc_is_hw_ip *hw_ip, u32 hw_fcount,
	struct fimc_is_frame **in_frame, struct fimc_is_framemgr *framemgr,
	u32 output_id, enum ShotErrorType done_type)
{
	int ret = 0;
	struct fimc_is_frame *frame = *in_frame;

	BUG_ON(!hw_ip);
	BUG_ON(!frame);
	BUG_ON(!framemgr);

	if (frame->fcount != hw_fcount) {
		if ((hw_ip->is_leader) && (hw_fcount - frame->fcount >= 2)) {
			dbg_hw("[%d]LATE  CORE END [ID:%d][F:%d][0x%x][C:0x%lx]" \
				"[O:0x%lx][END:%d]\n",
				frame->instance, hw_ip->id, frame->fcount,
				output_id, frame->core_flag, frame->out_flag,
				hw_fcount);

			info_hw("%d: force_done for LATE FRAME [ID:%d][F:%d]\n",
				__LINE__, hw_ip->id, frame->fcount);
			ret = fimc_is_hardware_frame_ndone(hw_ip, frame,
					frame->instance, IS_SHOT_UNPROCESSED);
			if (ret) {
				err_hw("[%d]hardware_frame_ndone fail (%d)",
					frame->instance, hw_ip->id);
				return -EINVAL;
			}
		}

		framemgr_e_barrier(framemgr, 0);
		*in_frame = find_frame(framemgr, FS_HW_WAIT_DONE, frame_fcount,
					(void *)(ulong)hw_fcount);
		framemgr_x_barrier(framemgr, 0);
		frame = *in_frame;

		if (frame == NULL) {
			err_hw("[ID:%d][F:%d]frame(null)!!(%d)", hw_ip->id,
				hw_fcount, done_type);
			framemgr_e_barrier(framemgr, 0);
			frame_manager_print_info_queues(framemgr);
			print_all_hw_frame_count(hw_ip->hardware);
			framemgr_x_barrier(framemgr, 0);
			return -EINVAL;
		}

		if (!test_bit_variables(hw_ip->id, &frame->core_flag)) {
			info_hw("[%d]invalid core_flag [ID:%d][F:%d][0x%x][C:0x%lx]" \
				"[O:0x%lx]",
				frame->instance, hw_ip->id, frame->fcount,
				output_id, frame->core_flag, frame->out_flag);
			return -EINVAL;
		}
	} else {
		dbg_hw("[ID:%d][%d,F:%d]FRAME COUNT invalid",
			hw_ip->id, frame->fcount, hw_fcount);
	}

	return ret;
}

int check_frame_end(struct fimc_is_hw_ip *hw_ip, u32 hw_fcount,
	struct fimc_is_frame **in_frame, struct fimc_is_framemgr *framemgr,
	u32 output_id, enum ShotErrorType done_type)
{
	int ret = 0;
	struct fimc_is_frame *frame = *in_frame;

	BUG_ON(!hw_ip);
	BUG_ON(!frame);
	BUG_ON(!framemgr);

	if (frame->fcount != hw_fcount) {
		dbg_hw("[%d]LATE FRAME END [ID:%d][F:%d][0x%x][C:0x%lx][O:0x%lx]" \
			"[END:%d]\n",
			frame->instance, hw_ip->id, frame->fcount, output_id,
			frame->core_flag, frame->out_flag, hw_fcount);

		framemgr_e_barrier(framemgr, 0);
		*in_frame = find_frame(framemgr, FS_HW_WAIT_DONE, frame_fcount,
					(void *)(ulong)hw_fcount);
		framemgr_x_barrier(framemgr, 0);
		frame = *in_frame;
		if (frame == NULL) {
			err_hw("[ID:%d][F:%d]frame(null)!!(%d)", hw_ip->id,
				hw_fcount, done_type);
			framemgr_e_barrier(framemgr, 0);
			frame_manager_print_info_queues(framemgr);
			print_all_hw_frame_count(hw_ip->hardware);
			framemgr_x_barrier(framemgr, 0);
			return -EINVAL;
		}

		if (!test_bit_variables(output_id, &frame->out_flag)) {
			info_hw("[%d]invalid output_id [ID:%d][F:%d][0x%x][C:0x%lx]" \
				"[O:0x%lx]",
				frame->instance, hw_ip->id, frame->fcount,
				output_id, frame->core_flag, frame->out_flag);
			return -EINVAL;
		}
	}

	return ret;
}

int fimc_is_hardware_frame_done(struct fimc_is_hw_ip *hw_ip, struct fimc_is_frame *frame,
	int wq_id, u32 output_id, enum ShotErrorType done_type)
{
	int ret = 0;
	struct fimc_is_framemgr *framemgr;
	struct fimc_is_group *head;

	BUG_ON(!hw_ip);

	framemgr = hw_ip->framemgr;

	switch (done_type) {
	case IS_SHOT_SUCCESS:
		if (frame == NULL) {
			framemgr_e_barrier(framemgr, 0);
			frame = peek_frame(framemgr, FS_HW_WAIT_DONE);
			framemgr_x_barrier(framemgr, 0);
		} else {
			warn_hw("[ID:%d]frame NOT null!!(%d)", hw_ip->id, done_type);
		}
		break;
	case IS_SHOT_LATE_FRAME:
		framemgr = hw_ip->framemgr_late;
		if (frame == NULL) {
			warn_hw("[ID:%d]frame null!!(%d)", hw_ip->id, done_type);
			framemgr_e_barrier(framemgr, 0);
			frame = peek_frame(framemgr, FS_HW_WAIT_DONE);
			framemgr_x_barrier(framemgr, 0);
		}

		if (frame!= NULL && frame->type != SHOT_TYPE_LATE) {
			warn_hw("invalid frame type");
			frame->type = SHOT_TYPE_LATE;
		}
		break;
	case IS_SHOT_UNPROCESSED:
	case IS_SHOT_UNKNOWN:
	case IS_SHOT_BAD_FRAME:
	case IS_SHOT_GROUP_PROCESSSTOP:
	case IS_SHOT_INVALID_FRAMENUMBER:
	case IS_SHOT_OVERFLOW:
	case IS_SHOT_TIMEOUT:
		break;
	default:
		err_hw("[ID:%d]invalid done_type(%d)", hw_ip->id, done_type);
		return -EINVAL;
	}

	if (frame == NULL) {
		err_hw("[ID:%d][F:%d]frame_done: frame(null)!!(%d)(0x%x)",
			hw_ip->id, atomic_read(&hw_ip->fcount), done_type, output_id);
		framemgr_e_barrier(framemgr, 0);
		frame_manager_print_info_queues(framemgr);
		print_all_hw_frame_count(hw_ip->hardware);
		framemgr_x_barrier(framemgr, 0);
		return -EINVAL;
	}

	head = GET_HEAD_GROUP_IN_DEVICE(FIMC_IS_DEVICE_ISCHAIN,
			hw_ip->group[frame->instance]);

	dbg_hw("[%d][ID:%d][0x%x]frame_done [F:%d][G:0x%x][B:0x%lx][C:0x%lx][O:0x%lx]\n",
		frame->instance, hw_ip->id, output_id, frame->fcount,
		GROUP_ID(head->id), frame->bak_flag, frame->core_flag, frame->out_flag);

	/* check core_done */
	if (output_id == FIMC_IS_HW_CORE_END) {
		switch (done_type) {
		case IS_SHOT_SUCCESS:
			if (!test_bit_variables(hw_ip->id, &frame->core_flag)) {
				ret = check_core_end(hw_ip, atomic_read(&hw_ip->fcount), &frame,
					framemgr, output_id, done_type);
				if (ret)
					return ret;

			}
			break;
		case IS_SHOT_UNPROCESSED:
		case IS_SHOT_LATE_FRAME:
		case IS_SHOT_UNKNOWN:
		case IS_SHOT_BAD_FRAME:
		case IS_SHOT_GROUP_PROCESSSTOP:
		case IS_SHOT_INVALID_FRAMENUMBER:
		case IS_SHOT_OVERFLOW:
		case IS_SHOT_TIMEOUT:
			goto shot_done;
			break;
		default:
			break;
		}

		if (hw_ip->is_leader) {
			frame->frame_info[INFO_FRAME_END_PROC].cpu = raw_smp_processor_id();
			frame->frame_info[INFO_FRAME_END_PROC].pid = current->pid;
			frame->frame_info[INFO_FRAME_END_PROC].when = cpu_clock(raw_smp_processor_id());
		}

	} else {
		if (frame->type == SHOT_TYPE_INTERNAL)
			goto shot_done;

		switch(done_type) {
		case IS_SHOT_SUCCESS:
			if (!test_bit_variables(output_id, &frame->out_flag)) {
				ret = check_frame_end(hw_ip, atomic_read(&hw_ip->fcount), &frame,
					framemgr, output_id, done_type);
				if (ret)
					return ret;
			}
			break;
		case IS_SHOT_UNPROCESSED:
			if (!test_bit_variables(output_id, &frame->out_flag))
				goto shot_done;
			break;
		case IS_SHOT_LATE_FRAME:
		case IS_SHOT_UNKNOWN:
		case IS_SHOT_BAD_FRAME:
		case IS_SHOT_GROUP_PROCESSSTOP:
		case IS_SHOT_INVALID_FRAMENUMBER:
		case IS_SHOT_OVERFLOW:
		case IS_SHOT_TIMEOUT:
			break;
		default:
			break;
		}

		ret = do_frame_done_work_func(hw_ip->itf,
				wq_id,
				frame->instance,
				head->id,
				frame->fcount,
				frame->rcount,
				done_type);
		if (ret)
			BUG_ON(1);

		clear_bit(output_id, &frame->out_flag);
	}

	if (frame->shot)
	    fimc_is_hardware_get_meta(hw_ip, frame,
			frame->instance, hw_ip->hardware->hw_map[frame->instance],
			output_id, done_type);

shot_done:
	if (output_id == FIMC_IS_HW_CORE_END)
		clear_bit(hw_ip->id, &frame->core_flag);

	framemgr_e_barrier(framemgr, 0);
	if (!OUT_FLAG(frame->out_flag, head->leader.id)
		&& !frame->core_flag
		&& atomic_dec_and_test(&frame->shot_done_flag)) {
		framemgr_x_barrier(framemgr, 0);
		ret = fimc_is_hardware_shot_done(hw_ip, frame, framemgr, done_type);
		return ret;
	}
	framemgr_x_barrier(framemgr, 0);

	return ret;
}

int fimc_is_hardware_shot_done(struct fimc_is_hw_ip *hw_ip, struct fimc_is_frame *frame,
	struct fimc_is_framemgr *framemgr, enum ShotErrorType done_type)
{
	int ret = 0;
	struct work_struct *work_wq;
	struct fimc_is_work_list *work_list;
	struct fimc_is_work *work;
	struct fimc_is_group *head;
	u32  req_id;

	BUG_ON(!hw_ip);

	if (frame == NULL) {
		err_hw("[ID:%d]frame(null)!!", hw_ip->id);
		framemgr_e_barrier(framemgr, 0);
		frame_manager_print_info_queues(framemgr);
		print_all_hw_frame_count(hw_ip->hardware);
		framemgr_x_barrier(framemgr, 0);
		BUG_ON(!frame);
	}

	head = GET_HEAD_GROUP_IN_DEVICE(FIMC_IS_DEVICE_ISCHAIN,
			hw_ip->group[frame->instance]);

	dbg_hw("[%d][ID:%d]shot_done [F:%d][G:0x%x][B:0x%lx][C:0x%lx][O:0x%lx]\n",
		frame->instance, hw_ip->id, frame->fcount, GROUP_ID(head->id),
		frame->bak_flag, frame->core_flag, frame->out_flag);

	if (frame->type == SHOT_TYPE_INTERNAL)
		goto free_frame;

	switch (head->id) {
	case GROUP_ID_3AA0:
	case GROUP_ID_3AA1:
	case GROUP_ID_ISP0:
	case GROUP_ID_ISP1:
	case GROUP_ID_DIS0:
	case GROUP_ID_MCS0:
	case GROUP_ID_MCS1:
	case GROUP_ID_VRA0:
		req_id = head->leader.id;
		break;
	default:
		err_hw("invalid group (%d)", head->id);
		goto exit;
		break;
	}

	if (!test_bit_variables(req_id, &frame->out_flag)) {
		err_hw("[%d]invalid bak_flag [ID:%d][F:%d][0x%x][B:0x%lx][O:0x%lx]",
			frame->instance, hw_ip->id, frame->fcount, req_id,
			frame->bak_flag, frame->out_flag);
		goto free_frame;
	}

	work_wq   = &hw_ip->itf->work_wq[INTR_SHOT_DONE];
	work_list = &hw_ip->itf->work_list[INTR_SHOT_DONE];

	get_free_work_irq(work_list, &work);
	if (work) {
		work->msg.id		= 0;
		work->msg.command	= IHC_FRAME_DONE;
		work->msg.instance	= frame->instance;
		work->msg.group		= GROUP_ID(head->id);
		work->msg.param1	= frame->fcount;
		work->msg.param2	= done_type; /* status: enum ShotErrorType */
		work->msg.param3	= 0;
		work->msg.param4	= 0;

		work->fcount = work->msg.param1;
		set_req_work_irq(work_list, work);

		if (!work_pending(work_wq))
			wq_func_schedule(hw_ip->itf, work_wq);
	} else {
		err_hw("free work item is empty\n");
	}
	clear_bit(req_id, &frame->out_flag);

free_frame:
	if (done_type) {
		info_hw("[%d]SHOT_NDONE [E%d][ID:%d][F:%d][G:0x%x]\n",
			frame->instance, done_type, hw_ip->id, frame->fcount, GROUP_ID(head->id));
		goto exit;
	}

	if (frame->type == SHOT_TYPE_INTERNAL) {
		dbg_hw("[%d]INTERNAL_SHOT_DONE [ID:%d][F:%d][G:0x%x]\n",
			frame->instance, hw_ip->id, frame->fcount, GROUP_ID(head->id));
		atomic_inc(&hw_ip->hardware->log_count);
	} else {
		dbg_hw("[%d]SHOT_DONE [ID:%d][F:%d][G:0x%x]\n",
			frame->instance, hw_ip->id, frame->fcount, GROUP_ID(head->id));
		atomic_set(&hw_ip->hardware->log_count, 0);
	}
exit:
	framemgr_e_barrier(framemgr, 0);
	trans_frame(framemgr, frame, FS_HW_FREE);
	framemgr_x_barrier(framemgr, 0);
	atomic_set(&frame->shot_done_flag, 0);
	if (framemgr->queued_count[FS_HW_FREE] > 10)
		atomic_set(&hw_ip->hardware->bug_count, 0);

	return ret;
}

int fimc_is_hardware_frame_ndone(struct fimc_is_hw_ip *ldr_hw_ip,
	struct fimc_is_frame *frame, u32 instance,
	enum ShotErrorType done_type)
{
	int ret = 0;
	int hw_slot = -1;
	struct fimc_is_hw_ip *hw_ip = NULL;
	struct fimc_is_group *group = NULL;
	struct fimc_is_group *head = NULL;
	struct fimc_is_hardware *hardware;
	enum fimc_is_hardware_id hw_id = DEV_HW_END;
	int hw_list[GROUP_HW_MAX], hw_index;
	int hw_maxnum = 0;

	if (!frame) {
		err_hw("%s[%d][ID:%d] ndone frame is NULL(%d)", __func__,
				instance, ldr_hw_ip->id, done_type);
		return -EINVAL;
	} else {
		info_hw("%s[F:%d][E%d][O:0x%lx][C:0x%lx]\n", __func__,
				frame->fcount, done_type,
				frame->out_flag, frame->core_flag);
	}

	group = ldr_hw_ip->group[instance];
	head = GET_HEAD_GROUP_IN_DEVICE(FIMC_IS_DEVICE_ISCHAIN, group);

	hardware = ldr_hw_ip->hardware;

	/* if there is not any out_flag without leader, forcely set the core flag */
	if (!OUT_FLAG(frame->out_flag, group->leader.id))
		set_bit(ldr_hw_ip->id, &frame->core_flag);

	while (head) {
		hw_maxnum = fimc_is_get_hw_list(head->id, hw_list);
		for (hw_index = 0; hw_index < hw_maxnum; hw_index++) {
			hw_id = hw_list[hw_index];
			hw_slot = fimc_is_hw_slot_id(hw_id);
			if (!valid_hw_slot_id(hw_slot)) {
				err_hw("[%d]invalid slot (%d,%d)", instance, hw_id, hw_slot);
				return -EINVAL;
			}

			hw_ip = &(hardware->hw_ip[hw_slot]);
			ret = CALL_HW_OPS(hw_ip, frame_ndone, frame, instance, done_type);
			if (ret) {
				err_hw("[%d]frame_ndone fail (%d,%d)", instance,
					hw_id, hw_slot);
				return -EINVAL;
			}
		}
		head = head->child;
	}

	return ret;
}

static int parse_setfile_header(ulong addr, struct fimc_is_setfile_header *header)
{
	union __setfile_header *file_header;

	/* 1. check setfile version */
	/* 2. load version specific header information */
	file_header = (union __setfile_header *)addr;
	if (file_header->magic_number == (SET_FILE_MAGIC_NUMBER - 1)) {
		header->version = SETFILE_V2;

		header->num_ips = file_header->ver_2.subip_num;
		header->num_scenarios = file_header->ver_2.scenario_num;

		header->scenario_table_base = addr + sizeof(struct __setfile_header_ver_2);
		header->setfile_entries_base = addr + file_header->ver_2.setfile_offset;

		header->designed_bits = 0;
		memset(header->version_code, 0, 5);
		memset(header->revision_code, 0, 5);
	} else if (file_header->magic_number == SET_FILE_MAGIC_NUMBER) {
		header->version = SETFILE_V3;

		header->num_ips = file_header->ver_3.subip_num;
		header->num_scenarios = file_header->ver_3.scenario_num;

		header->scenario_table_base = addr + sizeof(struct __setfile_header_ver_3);
		header->setfile_entries_base = addr + file_header->ver_3.setfile_offset;

		header->designed_bits = file_header->ver_3.designed_bit;
		memcpy(header->version_code, file_header->ver_3.version_code, 4);
		header->version_code[4] = 0;
		memcpy(header->revision_code, file_header->ver_3.revision_code, 4);
		header->revision_code[4] = 0;
	} else {
		err_hw("invalid magic number[0x%08x]", file_header->magic_number);
		return -EINVAL;
	}

	/* 3. process more header information */
	header->num_setfile_base = header->scenario_table_base
		+ (header->num_ips * header->num_scenarios * sizeof(u32));
	header->setfile_table_base = header->num_setfile_base
		+ (header->num_ips * sizeof(u32));

	dbg_hw("%s: version: %d\n", __func__, header->version);
	dbg_hw("%s: number of IPs: %d\n", __func__, header->num_ips);
	dbg_hw("%s: number of scenario: %d\n", __func__, header->num_scenarios);
	dbg_hw("%s: scenario table base: 0x%lx\n", __func__, header->scenario_table_base);
	dbg_hw("%s: number of setfile base: 0x%lx\n", __func__, header->num_setfile_base);
	dbg_hw("%s: setfile table base: 0x%lx\n", __func__, header->setfile_table_base);
	dbg_hw("%s: setfile entries base: 0x%lx\n", __func__, header->setfile_entries_base);

	return 0;
}

static void set_hw_slots_bit(unsigned long *slots, int nslots, int hw_id)
{
	int hw_slot;

	switch (hw_id) {
	/* setfile chain (3AA0, 3AA1, ISP0, ISP1) */
	case DEV_HW_3AA0:
		hw_slot = fimc_is_hw_slot_id(hw_id);
		if (valid_hw_slot_id(hw_slot))
			set_bit(hw_slot, slots);
		hw_id = DEV_HW_3AA1;
	case DEV_HW_3AA1:
		hw_slot = fimc_is_hw_slot_id(hw_id);
		if (valid_hw_slot_id(hw_slot))
			set_bit(hw_slot, slots);
		hw_id = DEV_HW_ISP0;
	case DEV_HW_ISP0:
		hw_slot = fimc_is_hw_slot_id(hw_id);
		if (valid_hw_slot_id(hw_slot))
			set_bit(hw_slot, slots);
		hw_id = DEV_HW_ISP1;
		break;

	/* setfile chain (MCSC0, MCSC1) */
	case DEV_HW_MCSC0:
		hw_slot = fimc_is_hw_slot_id(hw_id);
		if (valid_hw_slot_id(hw_slot))
			set_bit(hw_slot, slots);
		hw_id = DEV_HW_MCSC1;
		break;
	}

	switch (hw_id) {
	/* every leaf of each setfile chain */
	case DEV_HW_ISP1:

	case DEV_HW_DRC:
	case DEV_HW_DIS:
	case DEV_HW_3DNR:
	case DEV_HW_SCP:
	case DEV_HW_FD:
	case DEV_HW_VRA:

	case DEV_HW_MCSC1:
		hw_slot = fimc_is_hw_slot_id(hw_id);
		if (valid_hw_slot_id(hw_slot))
			set_bit(hw_slot, slots);
		break;
	}
}

static void get_setfile_hw_slots_no_hint(unsigned long *slots, int ip, u32 num_ips)
{
	int hw_id = 0;
	bool has_mcsc;

	bitmap_zero(slots, HW_SLOT_MAX);

	if (num_ips == 3) {
		/* ISP, DRC, VRA */
		switch (ip) {
		case 0:
			hw_id = DEV_HW_3AA0;
			break;
		case 1:
			hw_id = DEV_HW_DRC;
			break;
		case 2:
			hw_id = DEV_HW_VRA;
			break;
		}
	} else if (num_ips == 4) {
		/* ISP, DRC, TDNR, VRA */
		switch (ip) {
		case 0:
			hw_id = DEV_HW_3AA0;
			break;
		case 1:
			hw_id = DEV_HW_DRC;
			break;
		case 2:
			hw_id = DEV_HW_3DNR;
			break;
		case 3:
			hw_id = DEV_HW_VRA;
			break;
		}
	} else if (num_ips == 5) {
		/* ISP, DRC, DIS, TDNR, VRA */
		switch (ip) {
		case 0:
			hw_id = DEV_HW_3AA0;
			break;
		case 1:
			hw_id = DEV_HW_DRC;
			break;
		case 2:
			hw_id = DEV_HW_DIS;
			break;
		case 3:
			hw_id = DEV_HW_3DNR;
			break;
		case 4:
			hw_id = DEV_HW_VRA;
			break;
		}
	} else if (num_ips == 6) {
		/* ISP, DRC, DIS, TDNR, MCSC, VRA */
		switch (ip) {
		case 0:
			hw_id = DEV_HW_3AA0;
			break;
		case 1:
			hw_id = DEV_HW_DRC;
			break;
		case 2:
			hw_id = DEV_HW_DIS;
			break;
		case 3:
			hw_id = DEV_HW_3DNR;
			break;
		case 4:
			fimc_is_hw_g_ctrl(NULL, 0, HW_G_CTRL_HAS_MCSC, (void *)&has_mcsc);
			hw_id = has_mcsc ? DEV_HW_MCSC0 : DEV_HW_SCP;
			break;
		case 5:
			hw_id = DEV_HW_VRA;
			break;
		}
	}

	dbg_hw("%s: hw_id: %d, IP: %d, number of IPs: %d\n", __func__, hw_id, ip, num_ips);

	if (hw_id > 0)
		set_hw_slots_bit(slots, HW_SLOT_MAX, hw_id);
}

static void get_setfile_hw_slots(unsigned long *slots, unsigned long *hint)
{
	bool has_mcsc;

	dbg_hw("%s: designed bits(0x%lx) ", __func__, *hint);

	bitmap_zero(slots, HW_SLOT_MAX);

	if (test_and_clear_bit(SETFILE_DESIGN_BIT_3AA_ISP, hint)) {
		set_hw_slots_bit(slots, HW_SLOT_MAX, DEV_HW_3AA0);

	} else if (test_and_clear_bit(SETFILE_DESIGN_BIT_DRC, hint)) {
		set_hw_slots_bit(slots, HW_SLOT_MAX, DEV_HW_DRC);

	} else if (test_and_clear_bit(SETFILE_DESIGN_BIT_SCC, hint)) {
		/* not supported yet */
		/* set_hw_slots_bit(slots, HW_SLOT_MAX, DEV_HW_SCC); */

	} else if (test_and_clear_bit(SETFILE_DESIGN_BIT_ODC, hint)) {
		/* not supported yet */
		/* set_hw_slots_bit(slots, HW_SLOT_MAX, DEV_HW_ODC); */

	} else if (test_and_clear_bit(SETFILE_DESIGN_BIT_VDIS, hint)) {
		set_hw_slots_bit(slots, HW_SLOT_MAX, DEV_HW_DIS);

	} else if (test_and_clear_bit(SETFILE_DESIGN_BIT_TDNR, hint)) {
		set_hw_slots_bit(slots, HW_SLOT_MAX, DEV_HW_3DNR);

	} else if (test_and_clear_bit(SETFILE_DESIGN_BIT_SCX_MCSC, hint)) {
		fimc_is_hw_g_ctrl(NULL, 0, HW_G_CTRL_HAS_MCSC, (void *)&has_mcsc);
		set_hw_slots_bit(slots, HW_SLOT_MAX,
				has_mcsc ? DEV_HW_MCSC0 : DEV_HW_SCP);

	} else if (test_and_clear_bit(SETFILE_DESIGN_BIT_FD_VRA, hint)) {
		set_hw_slots_bit(slots, HW_SLOT_MAX, DEV_HW_VRA);

	}

	dbg_hw("              -> (0x%lx)\n", *hint);
}

int fimc_is_hardware_load_setfile(struct fimc_is_hardware *hardware, ulong addr,
	u32 instance, ulong hw_map)
{
	struct fimc_is_setfile_header header;
	struct __setfile_table_entry *setfile_table_entry;
	unsigned long slots[DIV_ROUND_UP(HW_SLOT_MAX, BITS_PER_LONG)];
	struct fimc_is_hw_ip *hw_ip;
	unsigned long hw_slot;
	unsigned long hint;
	u32 ip, idx;
	u32 blk_size;
	unsigned long base;
	int ret = 0;

	ret = parse_setfile_header(addr, &header);
	if (ret) {
		err_hw("failed to parse setfile header(%d)", ret);
		return ret;
	}

	if (header.num_scenarios > FIMC_IS_MAX_SCENARIO) {
		err_hw("too many scenarios: %d", header.num_scenarios);
		return -EINVAL;
	}

	hint = header.designed_bits;
	setfile_table_entry = (struct __setfile_table_entry *)header.setfile_table_base;

	for (ip = 0; ip < header.num_ips; ip++) {
		if (header.version == SETFILE_V3)
			get_setfile_hw_slots(slots, &hint);
		else
			get_setfile_hw_slots_no_hint(slots, ip, header.num_ips);

		hw_ip = NULL;

		hw_slot = find_first_bit(slots, HW_SLOT_MAX);
		while (hw_slot != HW_SLOT_MAX) {
			hw_ip = &hardware->hw_ip[hw_slot];

			/* set version */
			hw_ip->setfile.version = header.version;

			/* set what setfile index is used at each scenario */
			base = header.scenario_table_base;
			blk_size = header.num_scenarios * sizeof(u32);
			memcpy(hw_ip->setfile.index, (void *)(base + (ip * blk_size)), blk_size);

			/* fill out-of-range index for each not-used scenario to check sanity */
			memset((u32 *)&hw_ip->setfile.index[header.num_scenarios],
				0xff, (FIMC_IS_MAX_SCENARIO - header.num_scenarios) * sizeof(u32));
#if defined(DBG_HW)
			for (idx = 0; idx < header.num_scenarios; idx++)
				dbg_hw("[ID:%d] scenario table [%d:%d]\n", hw_ip->id, idx,
							hw_ip->setfile.index[idx]);
#endif

			/* set the number of setfile at each sub IP */
			base = header.num_setfile_base;
			blk_size = sizeof(u32);
			hw_ip->setfile.using_count = (u32)*(ulong *)(base + (ip * sizeof(u32)));

			if (hw_ip->setfile.using_count > FIMC_IS_MAX_SETFILE) {
				err_hw("too many setfile entries: %d", hw_ip->setfile.using_count);
				return -EINVAL;
			}

			dbg_hw("[ID:%d] number of setfile: %d\n", hw_ip->id, hw_ip->setfile.using_count);

			/* set each setfile address and size */
			for (idx = 0; idx < hw_ip->setfile.using_count; idx++) {
				hw_ip->setfile.table[idx].addr =
					(ulong)(header.setfile_entries_base + setfile_table_entry[idx].offset),
				hw_ip->setfile.table[idx].size = setfile_table_entry[idx].size;

				dbg_hw("[ID:%d] setfile[%d] addr: 0x%lx, size: %x\n", hw_ip->id, idx,
					hw_ip->setfile.table[idx].addr, hw_ip->setfile.table[idx].size);

				ret = CALL_HW_OPS(hw_ip, load_setfile, idx, instance, hw_map);
				if (ret) {
					err_hw("[ID:%d] failed to load setfile[%d] addr: 0x%lx, size: %x (%d)",
							hw_ip->id, idx, hw_ip->setfile.table[idx].addr,
							hw_ip->setfile.table[idx].size, ret);

					return ret;
				}
			}

			clear_bit(hw_slot, slots);
			hw_slot = find_first_bit(slots, HW_SLOT_MAX);
		}

		/* increase setfile table base even though there is no valid HW slot */
		if (hw_ip)
			setfile_table_entry += hw_ip->setfile.using_count;
		else
			setfile_table_entry++;
	}

	return ret;
};

int fimc_is_hardware_apply_setfile(struct fimc_is_hardware *hardware, u32 instance,
	u32 scenario, ulong hw_map)
{
	struct fimc_is_hw_ip *hw_ip = NULL;
	int hw_id = 0;
	int ret = 0;
	int hw_slot = -1;

	BUG_ON(!hardware);

	if (FIMC_IS_MAX_SCENARIO <= scenario) {
		err_hw("%s: invalid scenario id: scenario(%d)", __func__, scenario);
		return -EINVAL;
	}

	info_hw("[%d]apply_setfile: hw_map (0x%lx)\n", instance, hw_map);

	for (hw_slot = 0; hw_slot < HW_SLOT_MAX; hw_slot++) {
		hw_ip = &hardware->hw_ip[hw_slot];
		hw_id = hw_ip->id;
		ret = CALL_HW_OPS(hw_ip, apply_setfile, scenario, instance, hw_map);
		if (ret) {
			err_hw("[%d][ID:%d] apply_setfile fail (%d)", instance, hw_id, ret);
			return -EINVAL;
		}
	}

	return ret;
}

int fimc_is_hardware_delete_setfile(struct fimc_is_hardware *hardware, u32 instance,
	ulong hw_map)
{
	int ret = 0;
	int hw_slot = -1;
	struct fimc_is_hw_ip *hw_ip = NULL;
	enum fimc_is_hardware_id hw_id = DEV_HW_END;

	BUG_ON(!hardware);

	info_hw("[%d]delete_setfile: hw_map (0x%lx)\n", instance, hw_map);
	for (hw_slot = 0; hw_slot < HW_SLOT_MAX; hw_slot++) {
		hw_ip = &hardware->hw_ip[hw_slot];
		ret = CALL_HW_OPS(hw_ip, delete_setfile, instance, hw_map);
		if (ret) {
			err_hw("[%d]delete_setfile fail (%d)", instance, hw_id);
			return -EINVAL;
		}
	}

	return ret;
}

int fimc_is_hardware_runtime_resume(struct fimc_is_hardware *hardware)
{
	int ret = 0;
#ifdef ENABLE_DIRECT_CLOCK_GATE
	int hw_slot = -1;
	struct fimc_is_hw_ip *hw_ip = NULL;

	BUG_ON(!hardware);

	for (hw_slot = 0; hw_slot < HW_SLOT_MAX; hw_slot++) {
		hw_ip = &hardware->hw_ip[hw_slot];
		/* init clk gating variable */
		if (hw_ip->clk_gate != NULL)
			memset(hw_ip->clk_gate->refcnt, 0x0, sizeof(int) * HW_SLOT_MAX);
	}
#endif
	return ret;
}

int fimc_is_hardware_runtime_suspend(struct fimc_is_hardware *hardware)
{
	return 0;
}

void fimc_is_hardware_clk_gate(struct fimc_is_hw_ip *hw_ip, u32 instance,
	bool on, bool close)
{
#ifdef ENABLE_DIRECT_CLOCK_GATE
	struct fimc_is_group *head;
	struct fimc_is_clk_gate *clk_gate;
	u32 idx;
	ulong flag;

	BUG_ON(!hw_ip);

	if (!sysfs_debug.en_clk_gate || hw_ip->clk_gate == NULL)
		return;

	clk_gate = hw_ip->clk_gate;
	idx = hw_ip->clk_gate_idx;

	if (close) {
		spin_lock_irqsave(&clk_gate->slock, flag);
		FIMC_IS_CLOCK_ON(clk_gate->regs, clk_gate->bit[idx]);
		spin_unlock_irqrestore(&clk_gate->slock, flag);
		return;
	}

	if (!hw_ip->group[instance])
		return;

	head = hw_ip->group[instance]->head;

	if (test_bit(FIMC_IS_GROUP_OTF_INPUT, &head->state))
		return;

	spin_lock_irqsave(&clk_gate->slock, flag);

	if(on) {
		clk_gate->refcnt[idx]++;

		if (clk_gate->refcnt[idx] > 1)
			goto exit;

		FIMC_IS_CLOCK_ON(clk_gate->regs, clk_gate->bit[idx]);
	} else {
		clk_gate->refcnt[idx]--;

		if (clk_gate->refcnt[idx] >= 1)
			goto exit;

		if(clk_gate->refcnt[idx] < 0){
			warn("[%d][ID:%d] clock is already disable(%d)", instance, hw_ip->id, clk_gate->refcnt[idx]);
			clk_gate->refcnt[idx] = 0;
			goto exit;
		}

		FIMC_IS_CLOCK_OFF(clk_gate->regs, clk_gate->bit[idx]);
	}
exit:
	spin_unlock_irqrestore(&clk_gate->slock, flag);

	if (clk_gate->refcnt[idx] > FIMC_IS_STREAM_COUNT)
		warn("[%d][ID:%d] abnormal clk_gate refcnt(%d)", instance, hw_ip->id, clk_gate->refcnt[idx]);

	return;
#endif
}

void fimc_is_hardware_sfr_dump(struct fimc_is_hardware *hardware)
{
	int hw_slot = -1;
	int reg_size = 0;
	struct fimc_is_hw_ip *hw_ip = NULL;

	if (!hardware) {
		err_hw("hardware is null\n");
		return;
	}

	for (hw_slot = 0; hw_slot < HW_SLOT_MAX; hw_slot++) {
		hw_ip = &hardware->hw_ip[hw_slot];

		if (!test_bit(HW_OPEN, &hw_ip->state))
			continue;

		if (IS_ERR_OR_NULL(hw_ip->sfr_dump)) {
			warn_hw("[ID:%d] sfr_dump memory is invalid", hw_ip->id);
			continue;
		}

		/* dump reg */
		reg_size = (hw_ip->regs_end - hw_ip->regs_start + 1);
		memcpy(hw_ip->sfr_dump, hw_ip->regs, reg_size);

		info_hw("[ID:%d] ##### SFR DUMP(V/P/S):(%p/%p/0x%X)[0x%llX~0x%llX]\n",
				hw_ip->id, hw_ip->sfr_dump, (void *)virt_to_phys(hw_ip->sfr_dump),
				reg_size, hw_ip->regs_start, hw_ip->regs_end);
#ifdef ENABLE_PANIC_SFR_PRINT
		print_hex_dump(KERN_INFO, "", DUMP_PREFIX_OFFSET, 32, 4,
				hw_ip->regs, reg_size, false);
#endif
		if (IS_ERR_OR_NULL(hw_ip->sfr_b_dump))
			continue;

		/* dump reg B */
		reg_size = (hw_ip->regs_b_end - hw_ip->regs_b_start + 1);
		memcpy(hw_ip->sfr_b_dump, hw_ip->regs_b, reg_size);

		info_hw("[ID:%d] ##### SFR B DUMP(V/P/S):(%p/%p/0x%X)[0x%llX~0x%llX]\n",
				hw_ip->id, hw_ip->sfr_b_dump, (void *)virt_to_phys(hw_ip->sfr_b_dump),
				reg_size, hw_ip->regs_b_start, hw_ip->regs_b_end);
#ifdef ENABLE_PANIC_SFR_PRINT
		print_hex_dump(KERN_INFO, "", DUMP_PREFIX_OFFSET, 32, 4,
				hw_ip->regs_b, reg_size, false);
#endif
	}
}
