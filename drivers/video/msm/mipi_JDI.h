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
	CABC_OFF = 0x0,
	CABC_LOW = 0x1,
	CABC_MED = 0x2,
	CABC_HIGH = 0x3,
	CABC_SRE = 0x60,
};

#endif  /* MIPI_JDI_H */
