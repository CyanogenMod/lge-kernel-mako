/*
 * Copyright (c) 2012, The Linux Foundation. All rights reserved.
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
 * TODO:
 * Use standard cpufreq APIs to limit CPU min/max frequencies
 *
 */

#include <linux/cpu.h>
#include <linux/cpufreq.h>
#include <linux/module.h>
#include <mach/cpufreq.h>

#define CPUFREQ_LIMIT "cpufreq_limit"

static uint32_t limited_max_freq = MSM_CPUFREQ_NO_LIMIT;
static uint32_t limited_min_freq = MSM_CPUFREQ_NO_LIMIT;

static int update_cpu_freq_limits(unsigned int cpu,
			uint32_t min_freq, uint32_t max_freq)
{
	int ret;

	ret = msm_cpufreq_set_freq_limits(cpu, min_freq, max_freq);
	if (ret)
		goto err;

	ret = cpufreq_update_policy(cpu);

err:
	return ret;
}

static ssize_t show_limited_min_freq(struct kobject *kobj,
			struct attribute *attr, char *buf)
{
	return sprintf(buf, "%u\n", limited_min_freq);
}

static ssize_t store_limited_min_freq(struct kobject *kobj,
			struct attribute *attr, const char *buf, size_t count)
{
	int ret;
	unsigned long new_freq;
	uint32_t cpu;

	ret = kstrtoul(buf, 0, &new_freq);
	if (ret < 0)
		return ret;

	if (new_freq == 384000)
		new_freq = MSM_CPUFREQ_NO_LIMIT;

	for_each_possible_cpu(cpu) {
		ret = update_cpu_freq_limits(cpu, new_freq, limited_max_freq);
		if (ret)
			pr_debug("%s: Failed to limit cpu%u min freq to %lu\n",
				__func__, cpu, new_freq);
	}

	limited_min_freq = new_freq;

	return count;
}

static struct global_attr limited_min_freq_attr = __ATTR(limited_min_freq,
		S_IRUGO | S_IWUSR | S_IWGRP,
		show_limited_min_freq,
		store_limited_min_freq);

static ssize_t show_limited_max_freq(struct kobject *kobj,
			struct attribute *attr, char *buf)
{
	return sprintf(buf, "%u\n", limited_max_freq);
}

static ssize_t store_limited_max_freq(struct kobject *kobj,
			struct attribute *attr, const char *buf, size_t count)
{
	int ret;
	unsigned long new_freq;
	uint32_t cpu;

	ret = kstrtoul(buf, 0, &new_freq);
	if (ret < 0)
		return ret;

	if (new_freq == 1512000)
		new_freq = MSM_CPUFREQ_NO_LIMIT;

	for_each_possible_cpu(cpu) {
		ret = update_cpu_freq_limits(cpu, limited_min_freq, new_freq);
		if (ret)
			pr_debug("%s: Failed to limit cpu%u max freq to %lu\n",
				__func__, cpu, new_freq);
	}

	limited_max_freq = new_freq;

	return count;
}

static struct global_attr limited_max_freq_attr = __ATTR(limited_max_freq,
		S_IRUGO | S_IWUSR | S_IWGRP,
		show_limited_max_freq,
		store_limited_max_freq);

static struct attribute *cpufreq_limit_attributes[] = {
	&limited_max_freq_attr.attr,
	&limited_min_freq_attr.attr,
	NULL
};

static struct attribute_group cpufreq_limit_attr_group = {
	.attrs = cpufreq_limit_attributes,
	.name = CPUFREQ_LIMIT,
};

static int cpufreq_limit_init(void)
{
	int ret;

	ret = sysfs_create_group(kernel_kobj, &cpufreq_limit_attr_group);
	if (ret)
		pr_err("%s: sysfs_creation failed, ret=%d\n", __func__, ret);

	return ret;
}

static void cpufreq_limit_exit(void)
{
	sysfs_remove_group(kernel_kobj, &cpufreq_limit_attr_group);
}

module_init(cpufreq_limit_init);
module_exit(cpufreq_limit_exit);
