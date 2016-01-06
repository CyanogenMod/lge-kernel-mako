/* Copyright (c) 2010, Code Aurora Forum. All rights reserved.
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

#ifndef MIPI_JDI_H
#define MIPI_JDI_H

int mipi_JDI_device_register(struct msm_panel_info *pinfo,
					u32 channel, u32 panel);

enum {
	CABC_OFF = 0x00,
	CABC_UI = 0x01,
	CABC_IMAGE = 0x02,
	CABC_VIDEO = 0x03,
	SRE_WEAK = 0x50,
	SRE_MEDIUM = 0x60,
	SRE_STRONG = 0X70,
	CABC_ACO = 0x80,
};

#endif  /* MIPI_JDI_H */
