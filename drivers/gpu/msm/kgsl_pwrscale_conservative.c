/* Copyright (c) 2010-2012, Code Aurora Forum. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

#include <linux/export.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/io.h>
#include <linux/spinlock.h>
#include <mach/socinfo.h>
#include <mach/scm.h>

#include "kgsl.h"
#include "kgsl_pwrscale.h"
#include "kgsl_device.h"

#define GPU_UP_THRESHOLD 90
#define GPU_DOWN_THRESHOLD 30
#define GPU_POLL_INTVL_MSEC 100

static unsigned long walltime_total = 0;
static unsigned long busytime_total = 0;

static void conservative_wake(struct kgsl_device *device, struct kgsl_pwrscale *pwrscale)
{
  struct kgsl_power_stats stats;
  printk("%s: GPU waking up\n",__func__);
  if (device->state != KGSL_STATE_NAP) {
    kgsl_pwrctrl_pwrlevel_change(device,
				 device->pwrctrl.default_pwrlevel);
    //reset the power stats counters;
    device->ftbl->power_stats(device,&stats);
    walltime_total = 0;
    busytime_total = 0;
  }
    
}

static void conservative_idle(struct kgsl_device *device, struct kgsl_pwrscale *pwrscale,
						unsigned int ignore_idle)
{
	struct kgsl_pwrctrl *pwr = &device->pwrctrl;
	struct kgsl_power_stats stats;
	int val = 0;
	unsigned int loadpct;

	if (ignore_idle)
		return;

	device->ftbl->power_stats(device, &stats);
	if (stats.total_time == 0)
		return;

	walltime_total += (unsigned long) stats.total_time;
	busytime_total += (unsigned long) stats.busy_time;
	if(walltime_total > (GPU_POLL_INTVL_MSEC*1000))
	  {
	    printk("%s: walltime_total = %lu, busytime_total = %lu\n",__func__,walltime_total,busytime_total);

	    loadpct = (100*busytime_total)/walltime_total;
	    printk("%s: loadpct = %d\n",__func__,loadpct);

	    walltime_total = busytime_total = 0;

	    if(loadpct < GPU_DOWN_THRESHOLD)
	      val = 1;
	    else if(loadpct > GPU_UP_THRESHOLD)
	      val = -1;

	    printk("%s: active_pwrlevel = %d, change = %d\n",__func__,pwr->active_pwrlevel,val);

	    if (val)
	      kgsl_pwrctrl_pwrlevel_change(device,
					   pwr->active_pwrlevel + val);
	  }
}

static void conservative_busy(struct kgsl_device *device,
	struct kgsl_pwrscale *pwrscale)
{
	device->on_time = ktime_to_us(ktime_get());
}

static void conservative_sleep(struct kgsl_device *device,
	struct kgsl_pwrscale *pwrscale)
{
  printk("%s: GPU going to sleep\n",__func__);
}

static int conservative_init(struct kgsl_device *device, struct kgsl_pwrscale *pwrscale)
{
	return 0;
}

static void conservative_close(struct kgsl_device *device, struct kgsl_pwrscale *pwrscale)
{

}

struct kgsl_pwrscale_policy kgsl_pwrscale_policy_conservative = {
	.name = "conservative",
	.init = conservative_init,
	.busy = conservative_busy,
	.idle = conservative_idle,
	.sleep = conservative_sleep,
	.wake = conservative_wake,
	.close = conservative_close
};
EXPORT_SYMBOL(kgsl_pwrscale_policy_conservative);
