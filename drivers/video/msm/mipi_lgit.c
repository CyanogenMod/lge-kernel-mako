/*
 * Copyright (C) 2013 The CyanogenMod Project
 * Copyright (C) 2011-2012, LG Eletronics,Inc. All rights reserved.
 *     LGIT LCD device driver
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
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301, USA.
 *
 */
#include <linux/string.h>
#include <linux/gpio.h>
#include <linux/syscore_ops.h>

#include "msm_fb.h"
#include "mipi_dsi.h"
#include "mipi_lgit.h"
#include "mdp4.h"

static struct msm_panel_common_pdata *mipi_lgit_pdata;

#ifdef CONFIG_LGIT_VIDEO_WXGA_CABC
struct dsi_cmd_desc local_power_on_set_1[33];
#else
struct dsi_cmd_desc local_power_on_set_1[28];
#endif

static struct dsi_buf lgit_tx_buf;
static struct dsi_buf lgit_rx_buf;
static struct msm_fb_data_type *local_mfd;
static int skip_init;
static int lcd_isactive = 0;

#define DSV_ONBST 57

static int lgit_external_dsv_onoff(uint8_t on_off)
{
	int ret =0;
	static int init_done=0;

	if (!init_done) {
		ret = gpio_request(DSV_ONBST,"DSV_ONBST_en");
		if (ret) {
			pr_err("%s: failed to request DSV_ONBST gpio \n", __func__);
			goto out;
		}
		ret = gpio_direction_output(DSV_ONBST, 1);
		if (ret) {
			pr_err("%s: failed to set DSV_ONBST direction\n", __func__);
			goto err_gpio;
		}
		init_done = 1;
	}

	gpio_set_value(DSV_ONBST, on_off);
	mdelay(20);
	goto out;

err_gpio:
	gpio_free(DSV_ONBST);
out:
	return ret;
}

static int mipi_lgit_lcd_on(struct platform_device *pdev)
{
	struct msm_fb_data_type *mfd;
	int ret = 0;

	pr_info("%s started\n", __func__);

	mfd = platform_get_drvdata(pdev);
	local_mfd = mfd;
	if (!mfd)
		return -ENODEV;
	if (mfd->key != MFD_KEY)
		return -EINVAL;

	lcd_isactive = 1;
	MIPI_OUTP(MIPI_DSI_BASE + 0x38, 0x10000000);
	ret = mipi_dsi_cmds_tx(&lgit_tx_buf,
			local_power_on_set_1,
			mipi_lgit_pdata->power_on_set_size_1);
	MIPI_OUTP(MIPI_DSI_BASE + 0x38, 0x14000000);
	if (ret < 0) {
		pr_err("%s: failed to transmit power_on_set_1 cmds\n", __func__);
		return ret;
	}

	if(!skip_init){
		MIPI_OUTP(MIPI_DSI_BASE + 0x38, 0x10000000);
		ret = mipi_dsi_cmds_tx(&lgit_tx_buf,
				mipi_lgit_pdata->power_on_set_2,
				mipi_lgit_pdata->power_on_set_size_2);
		MIPI_OUTP(MIPI_DSI_BASE + 0x38, 0x14000000);
		if (ret < 0) {
			pr_err("%s: failed to transmit power_on_set_2 cmds\n", __func__);
			return ret;
		}
	}
	skip_init = false;

	ret = lgit_external_dsv_onoff(1);
	if (ret < 0) {
		pr_err("%s: failed to turn on external dsv\n", __func__);
		return ret;
	}

	MIPI_OUTP(MIPI_DSI_BASE + 0x38, 0x10000000);
	ret = mipi_dsi_cmds_tx(&lgit_tx_buf,
			mipi_lgit_pdata->power_on_set_3,
			mipi_lgit_pdata->power_on_set_size_3);
	MIPI_OUTP(MIPI_DSI_BASE + 0x38, 0x14000000);
	if (ret < 0) {
		pr_err("%s: failed to transmit power_on_set_3 cmds\n", __func__);
		return ret;
	}

	pr_info("%s finished\n", __func__);
	return 0;
}

static int mipi_lgit_lcd_off(struct platform_device *pdev)
{
	struct msm_fb_data_type *mfd;
	int ret = 0;

	pr_info("%s started\n", __func__);

	if (mipi_lgit_pdata->bl_pwm_disable)
		mipi_lgit_pdata->bl_pwm_disable();

	mfd = platform_get_drvdata(pdev);

	if (!mfd)
		return -ENODEV;

	if (mfd->key != MFD_KEY)
		return -EINVAL;

	lcd_isactive = 0;

	MIPI_OUTP(MIPI_DSI_BASE + 0x38, 0x10000000);
	ret = mipi_dsi_cmds_tx(&lgit_tx_buf,
			mipi_lgit_pdata->power_off_set_1,
			mipi_lgit_pdata->power_off_set_size_1);
	MIPI_OUTP(MIPI_DSI_BASE + 0x38, 0x14000000);
	if (ret < 0) {
		pr_err("%s: failed to transmit power_off_set_1 cmds\n", __func__);
		return ret;
	}

	ret = lgit_external_dsv_onoff(0);
	if (ret < 0) {
		pr_err("%s: failed to turn off external dsv\n", __func__);
		return ret;
	}

	MIPI_OUTP(MIPI_DSI_BASE + 0x38, 0x10000000);
	ret = mipi_dsi_cmds_tx(&lgit_tx_buf,
			mipi_lgit_pdata->power_off_set_2,
			mipi_lgit_pdata->power_off_set_size_2);
	MIPI_OUTP(MIPI_DSI_BASE + 0x38, 0x14000000);
	if (ret < 0) {
		pr_err("%s: failed to transmit power_off_set_2 cmds\n", __func__);
		return ret;
	}

	pr_info("%s finished\n", __func__);
	return 0;
}

static void mipi_lgit_lcd_shutdown(void)
{
	int ret = 0;

	if(local_mfd && !local_mfd->panel_power_on) {
		pr_info("%s:panel is already off\n", __func__);
		return;
	}

	MIPI_OUTP(MIPI_DSI_BASE + 0x38, 0x10000000);
	ret = mipi_dsi_cmds_tx(&lgit_tx_buf,
			mipi_lgit_pdata->power_off_set_1,
			mipi_lgit_pdata->power_off_set_size_1);
	MIPI_OUTP(MIPI_DSI_BASE + 0x38, 0x14000000);
	if (ret < 0) {
		pr_err("%s: failed to transmit power_off_set_1 cmds\n", __func__);
	}

	ret = lgit_external_dsv_onoff(0);
	if (ret < 0) {
		pr_err("%s: failed to turn off external dsv\n", __func__);
	}
	mdelay(20);

	MIPI_OUTP(MIPI_DSI_BASE + 0x38, 0x10000000);
	ret = mipi_dsi_cmds_tx(&lgit_tx_buf,
			mipi_lgit_pdata->power_off_set_2,
			mipi_lgit_pdata->power_off_set_size_2);
	MIPI_OUTP(MIPI_DSI_BASE + 0x38, 0x14000000);
	if (ret < 0) {
		pr_err("%s: failed to transmit power_off_set_2 cmds\n", __func__);
	}

	pr_info("%s finished\n", __func__);
}

static int mipi_lgit_backlight_on_status(void)
{
	return (mipi_lgit_pdata->bl_on_status());
}

static void mipi_lgit_set_backlight_board(struct msm_fb_data_type *mfd)
{
	int level;

	level = (int)mfd->bl_level;
	mipi_lgit_pdata->backlight_level(level, 0, 0);
}

struct syscore_ops panel_syscore_ops = {
	.shutdown = mipi_lgit_lcd_shutdown,
};

/******************* Begin sysfs interface *******************/

static unsigned char calc_checksum(int intArr[]) {
	int i = 0;
	unsigned char chksum = 0;

	for (i=1; i<10; i++)
		chksum += intArr[i];

	return chksum;
}

static ssize_t do_kgamma_store(struct device *dev, struct device_attribute *attr,
				const char *buf, size_t count,
				unsigned int offset)
{
	int kgamma[10];
	int i;

	sscanf(buf, "%d %d %d %d %d %d %d %d %d %d",
		&kgamma[0], &kgamma[1], &kgamma[2], &kgamma[3],
		&kgamma[4], &kgamma[5], &kgamma[6], &kgamma[7],
		&kgamma[8], &kgamma[9]);

	if (kgamma[5] > 31 || (kgamma[6] > 31)) {
		pr_info("gamma 0 and gamma 1 values can't be over 31, got %d %d instead!", kgamma[5], kgamma[6]);
		return -EINVAL;
	}

	for (i=1; i<10; i++) {
		if (kgamma[i] > 255) {
			pr_info("char values  can't be over 255, got %d instead!", kgamma[i]);
			return -EINVAL;
		}
	}

	if (calc_checksum(kgamma) == (unsigned char) kgamma[0]) {
		kgamma[0] = 0xd0 + offset;
		for (i=0; i<10; i++) {
			pr_info("kgamma_p [%d] => %d \n", i, kgamma[i]);
			local_power_on_set_1[5+offset].payload[i] = kgamma[i];
		}

		kgamma[0] = 0xd1 + offset;
		for (i=0; i<10; i++) {
			pr_info("kgamma_n [%d] => %d \n", i, kgamma[i]);
			local_power_on_set_1[6+offset].payload[i] = kgamma[i];
		}
		return count;
	}
	return -EINVAL;
}

static ssize_t do_kgamma_show(struct device *dev, struct device_attribute *attr,
				char *buf, unsigned int offset)
{
	int kgamma[10];
	int i;

	for (i=1; i<10; i++)
		kgamma[i] = local_power_on_set_1[5+offset].payload[i];

	kgamma[0] = (int) calc_checksum(kgamma);

	return sprintf(buf, "%d %d %d %d %d %d %d %d %d %d",
		kgamma[0], kgamma[1], kgamma[2], kgamma[3],
		kgamma[4], kgamma[5], kgamma[6], kgamma[7],
		kgamma[8], kgamma[9]);
}

static ssize_t kgamma_r_store(struct device *dev, struct device_attribute *attr,
						const char *buf, size_t count)
{
	return do_kgamma_store(dev,attr,buf,count,0);
}

static ssize_t kgamma_r_show(struct device *dev, struct device_attribute *attr,
								char *buf)
{
	return do_kgamma_show(dev,attr,buf,0);
}

static ssize_t kgamma_g_store(struct device *dev, struct device_attribute *attr,
						const char *buf, size_t count)
{
	return do_kgamma_store(dev,attr,buf,count,2);
}

static ssize_t kgamma_g_show(struct device *dev, struct device_attribute *attr,
								char *buf)
{
	return do_kgamma_show(dev,attr,buf,2);
}

static ssize_t kgamma_b_store(struct device *dev, struct device_attribute *attr,
						const char *buf, size_t count)
{
	return do_kgamma_store(dev,attr,buf,count,4);
}

static ssize_t kgamma_b_show(struct device *dev, struct device_attribute *attr,
								char *buf)
{
	return do_kgamma_show(dev,attr,buf,4);
}

static ssize_t kgamma_apply_store(struct device *dev, struct device_attribute *attr,
						const char *buf, size_t count)
{
	int ret = 0;

	/*
	 * Only attempt to apply if the LCD is active.
	 * If it isn't, the device will panic-reboot
	 */
	if(lcd_isactive) {
		MIPI_OUTP(MIPI_DSI_BASE + 0x38, 0x10000000);
		ret = mipi_dsi_cmds_tx(&lgit_tx_buf,
				local_power_on_set_1,
				mipi_lgit_pdata->power_on_set_size_1);
		MIPI_OUTP(MIPI_DSI_BASE + 0x38, 0x14000000);
		if (ret < 0) {
			pr_err("%s: failed to transmit power_on_set_1 cmds\n", __func__);
			return ret;
		}
	}
	else {
		pr_err("%s: Tried to apply gamma settings when LCD was off\n",__func__);
		//Is ENODEV correct here?  Perhaps it should be something else?
		return -ENODEV;
	}
	return count;
}

static ssize_t kgamma_apply_show(struct device *dev, struct device_attribute *attr,
								char *buf)
{
	return 0;
}

static DEVICE_ATTR(kgamma_r, 0644, kgamma_r_show, kgamma_r_store);
static DEVICE_ATTR(kgamma_g, 0644, kgamma_g_show, kgamma_g_store);
static DEVICE_ATTR(kgamma_b, 0644, kgamma_b_show, kgamma_b_store);
static DEVICE_ATTR(kgamma_apply, 0644, kgamma_apply_show, kgamma_apply_store);

/******************* End sysfs interface *******************/

static int mipi_lgit_lcd_probe(struct platform_device *pdev)
{
	int rc;

	if (pdev->id == 0) {
		mipi_lgit_pdata = pdev->dev.platform_data;
		return 0;
	}

	// Make a copy of platform data
	memcpy((void*)local_power_on_set_1, (void*)mipi_lgit_pdata->power_on_set_1,
		sizeof(local_power_on_set_1));

	pr_info("%s start\n", __func__);

	skip_init = true;
	msm_fb_add_device(pdev);

	register_syscore_ops(&panel_syscore_ops);

	rc = device_create_file(&pdev->dev, &dev_attr_kgamma_r);
	if(rc !=0)
		return -1;

	rc = device_create_file(&pdev->dev, &dev_attr_kgamma_g);
	if(rc !=0)
		return -1;

	rc = device_create_file(&pdev->dev, &dev_attr_kgamma_b);
	if(rc !=0)
		return -1;

	rc = device_create_file(&pdev->dev, &dev_attr_kgamma_apply);
	if(rc !=0)
		return -1;

	return 0;
}

static struct platform_driver this_driver = {
	.probe = mipi_lgit_lcd_probe,
	.driver = {
		.name = "mipi_lgit",
	},
};

static struct msm_fb_panel_data lgit_panel_data = {
	.on = mipi_lgit_lcd_on,
	.off = mipi_lgit_lcd_off,
	.set_backlight = mipi_lgit_set_backlight_board,
	.get_backlight_on_status = mipi_lgit_backlight_on_status,
};

static int ch_used[3];

int mipi_lgit_device_register(struct msm_panel_info *pinfo,
		u32 channel, u32 panel)
{
	struct platform_device *pdev = NULL;
	int ret;

	if ((channel >= 3) || ch_used[channel])
		return -ENODEV;

	ch_used[channel] = TRUE;

	pdev = platform_device_alloc("mipi_lgit", (panel << 8)|channel);
	if (!pdev)
		return -ENOMEM;

	lgit_panel_data.panel_info = *pinfo;

	ret = platform_device_add_data(pdev, &lgit_panel_data,
			sizeof(lgit_panel_data));
	if (ret) {
		pr_err("%s: platform_device_add_data failed!\n", __func__);
		goto err_device_put;
	}

	ret = platform_device_add(pdev);
	if (ret) {
		pr_err("%s: platform_device_register failed!\n", __func__);
		goto err_device_put;
	}
	return 0;

err_device_put:
	platform_device_put(pdev);
	return ret;
}

static int __init mipi_lgit_lcd_init(void)
{
	mipi_dsi_buf_alloc(&lgit_tx_buf, DSI_BUF_SIZE);
	mipi_dsi_buf_alloc(&lgit_rx_buf, DSI_BUF_SIZE);

	return platform_driver_register(&this_driver);
}

module_init(mipi_lgit_lcd_init);
