/*
 * Copyright (C) 2018 MediaTek Inc.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 */

#ifndef _CUST_PMIC_H_
#define _CUST_PMIC_H_

/*#define PMIC_VDVFS_CUST_ENABLE*/

#define LOW_POWER_LIMIT_LEVEL_1 15

#define PT_DLPT_BRINGUP 0

#ifdef VENDOR_EDIT
//qinyonghui@SH, 2019/12/02. disable low battery limit
/* Define for disable low battery protect feature,
 * default no define for enable low battery protect.
 */
#define DISABLE_LOW_BATTERY_PROTECT

/*Define for disable battery OC protect*/
#define DISABLE_BATTERY_OC_PROTECT

/*Define for disable battery 15% protect*/
#define DISABLE_BATTERY_PERCENT_PROTECT

/*Define for DLPT*/
#define DISABLE_DLPT_FEATURE
#endif

#if defined(CONFIG_FPGA_EARLY_PORTING) || PT_DLPT_BRINGUP
/* Define for disable low battery protect feature,
 * default no define for enable low battery protect.
 */
#define DISABLE_LOW_BATTERY_PROTECT

/*Define for disable battery OC protect*/
#define DISABLE_BATTERY_OC_PROTECT

/*Define for disable battery 15% protect*/
#define DISABLE_BATTERY_PERCENT_PROTECT

/*Define for DLPT*/
#define DISABLE_DLPT_FEATURE
#endif /* defined(CONFIG_FPGA_EARLY_PORTING) || PT_DLPT_BRINGUP */

/* if not support GM3, disable DLPT */
#if defined(CONFIG_MTK_DISABLE_GAUGE)
#define DISABLE_DLPT_FEATURE
#endif /* defined(CONFIG_MTK_DISABLE_GAUGE) */

#ifndef VENDOR_EDIT
/* LiYue@BSP.CHG.Basic, 2019/09/18, Modify uvlo from 2.6v to 2.75v */
#define POWER_UVLO_VOLT_LEVEL 2600
#else
#define POWER_UVLO_VOLT_LEVEL 2750
#endif /* VENDOR_EDIT */

#define IMAX_MAX_VALUE 5500

#define POWER_INT0_VOLT 3400
#define POWER_INT1_VOLT 3250
#define POWER_INT2_VOLT 3100

#define POWER_INT0_VOLT_EXT 3550
#define POWER_INT1_VOLT_EXT 3400
#define POWER_INT2_VOLT_EXT 1000

#define POWER_BAT_OC_CURRENT_H    6800
#define POWER_BAT_OC_CURRENT_L    8000

#define DLPT_POWER_OFF_EN
#define POWEROFF_BAT_CURRENT 3000
#define DLPT_POWER_OFF_THD 100

#define DLPT_VOLT_MIN 3100

#define BATTERY_MODULE_INIT

/* ADC Channel Number, not uesed and removed */


extern const char *pmic_auxadc_channel_name[];

/* Legacy pmic auxadc interface */
enum {
	AUXADC_LIST_BATADC,
	AUXADC_LIST_START = AUXADC_LIST_BATADC,
	AUXADC_LIST_VCDT,
	AUXADC_LIST_BATTEMP,
	AUXADC_LIST_CHIP_TEMP,
	AUXADC_LIST_VCORE_TEMP,
	AUXADC_LIST_VPROC_TEMP,
	AUXADC_LIST_VGPU_TEMP,
	AUXADC_LIST_ACCDET,
	AUXADC_LIST_VDCXO,
	AUXADC_LIST_TSX_TEMP,
	AUXADC_LIST_HPOFS_CAL,
	AUXADC_LIST_DCXO_TEMP,
	AUXADC_LIST_VBIF,
	AUXADC_LIST_END = AUXADC_LIST_VBIF,
	AUXADC_LIST_ISENSE,
};


extern void pmic_auxadc_init(void);
extern void pmic_auxadc_dump_regs(char *buf);
extern int pmic_get_auxadc_channel_max(void);
extern int mt_get_auxadc_value(u8 channel);
extern void pmic_auxadc_lock(void);
extern void pmic_auxadc_unlock(void);
#endif /* _CUST_PMIC_H_ */
