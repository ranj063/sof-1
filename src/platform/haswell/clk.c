/*
 * Copyright (c) 2016, Intel Corporation
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *   * Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *   * Neither the name of the Intel Corporation nor the
 *     names of its contributors may be used to endorse or promote products
 *     derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 *
 * Author: Liam Girdwood <liam.r.girdwood@linux.intel.com>
 */

#include <sof/clock.h>
#include <sof/io.h>
#include <sof/sof.h>
#include <sof/list.h>
#include <sof/alloc.h>
#include <sof/notifier.h>
#include <sof/lock.h>
#include <platform/clk.h>
#include <platform/shim.h>
#include <platform/timer.h>
#include <config.h>
#include <stdint.h>
#include <limits.h>

#define NUM_CLOCKS	2

struct clk_data {
	uint32_t freq;
	uint32_t ticks_per_usec;
	spinlock_t lock;
};

struct clk_pdata {
	struct clk_data clk[NUM_CLOCKS];
};

struct freq_table {
	uint32_t freq;
	uint32_t ticks_per_usec;
	uint32_t fabric;
	uint32_t enc;
};

static struct clk_pdata *clk_pdata;

/* increasing frequency order */
static const struct freq_table cpu_freq[] = {
	{32000000, 80, 32000000, 0x6},
	{80000000, 80, 80000000, 0x2},
	{160000000, 160, 80000000, 0x1},
	{320000000, 320, 160000000, 0x4},/* default */
	{320000000, 320, 80000000, 0x0},
	{160000000, 160, 160000000, 0x5},
};

static const struct freq_table ssp_freq[] = {
	{24000000, 24, 0, 0},	/* default */
};

#define CPU_DEFAULT_IDX		3
#define SSP_DEFAULT_IDX		0

static inline uint32_t get_freq(const struct freq_table *table, int size,
	unsigned int hz)
{
	uint32_t i;

	/* find lowest available frequency that is >= requested Hz */
	for (i = 0; i < size; i++) {
		if (hz <= table[i].freq)
			return i;
	}

	/* not found, so return max frequency */
	return size - 1;
}

void clock_enable(int clock)
{
	switch (clock) {
	case CLK_CPU:
		break;
	case CLK_SSP:
	default:
		break;
	}
}

void clock_disable(int clock)
{
	switch (clock) {
	case CLK_CPU:
		break;
	case CLK_SSP:
	default:
		break;
	}
}

uint32_t clock_set_freq(int clock, uint32_t hz)
{
	struct clock_notify_data notify_data;
	uint32_t idx;
	uint32_t flags;

	notify_data.old_freq = clk_pdata->clk[clock].freq;
	notify_data.old_ticks_per_usec = clk_pdata->clk[clock].ticks_per_usec;

	/* atomic context for chaining clocks */
	spin_lock_irq(&clk_pdata->clk[clock].lock, flags);

	switch (clock) {
	case CLK_CPU:

		/* get nearest frequency that is >= requested Hz */
		idx = get_freq(cpu_freq, ARRAY_SIZE(cpu_freq), hz);
		notify_data.freq = cpu_freq[idx].freq;

		/* tell anyone interested we are about to change CPU freq */
		notifier_event(NOTIFIER_ID_CPU_FREQ, CLOCK_NOTIFY_PRE,
			&notify_data);

		/* set CPU frequency request for CCU */
		io_reg_update_bits(SHIM_BASE + SHIM_CSR,
						SHIM_CSR_DCS_MASK,
						SHIM_CSR_DCS(cpu_freq[idx].enc));

		/* tell anyone interested we have now changed CPU freq */
		notifier_event(NOTIFIER_ID_CPU_FREQ, CLOCK_NOTIFY_POST,
			&notify_data);
		break;
	case CLK_SSP:
	default:
		break;
	}

	spin_unlock_irq(&clk_pdata->clk[clock].lock, flags);
	return clk_pdata->clk[clock].freq;
}

uint32_t clock_get_freq(int clock)
{
	return clk_pdata->clk[clock].freq;
}

uint64_t clock_us_to_ticks(int clock, uint64_t us)
{
	return clk_pdata->clk[clock].ticks_per_usec * us;
}

uint64_t clock_time_elapsed(int clock, uint64_t previous, uint64_t *current)
{
	uint64_t _current;

	// TODO: change timer APIs to clk APIs ??
	switch (clock) {
	case CLK_CPU:
		_current = arch_timer_get_system(NULL);
		break;
	case CLK_SSP:
		_current = platform_timer_get(platform_timer);
		break;
	default:
		return 0;
	}

	*current = _current;
	if (_current >= previous)
		return (_current - previous) /
			clk_pdata->clk[clock].ticks_per_usec;
	else
		return (_current + (ULONG_LONG_MAX - previous)) /
			clk_pdata->clk[clock].ticks_per_usec;
}

void init_platform_clocks(void)
{
	clk_pdata = rzalloc(RZONE_SYS, SOF_MEM_CAPS_RAM, sizeof(*clk_pdata));

	spinlock_init(&clk_pdata->clk[0].lock);
	spinlock_init(&clk_pdata->clk[1].lock);

	/* set defaults */
	clk_pdata->clk[CLK_CPU].freq = cpu_freq[CPU_DEFAULT_IDX].freq;
	clk_pdata->clk[CLK_CPU].ticks_per_usec =
			cpu_freq[CPU_DEFAULT_IDX].ticks_per_usec;
	clk_pdata->clk[CLK_SSP].freq = ssp_freq[SSP_DEFAULT_IDX].freq;
	clk_pdata->clk[CLK_SSP].ticks_per_usec =
			ssp_freq[SSP_DEFAULT_IDX].ticks_per_usec;
}
