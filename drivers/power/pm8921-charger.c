/* Copyright (c) 2011-2012, Code Aurora Forum. All rights reserved.
 * Copyright (C) 2011 Sony Ericsson Mobile Communications AB.
 * Copyright (C) 2012 Sony Mobile Communications AB.
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
#define pr_fmt(fmt)	"%s: " fmt, __func__

#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/platform_device.h>
#include <linux/errno.h>
#include <linux/mfd/pm8xxx/pm8921-charger.h>
#include <linux/mfd/pm8xxx/pm8921-bms.h>
#include <linux/mfd/pm8xxx/pm8xxx-adc.h>
#include <linux/mfd/pm8xxx/ccadc.h>
#include <linux/mfd/pm8xxx/batt-alarm.h>
#include <linux/mfd/pm8xxx/core.h>
#include <linux/interrupt.h>
#include <linux/delay.h>
#include <linux/bitops.h>
#include <linux/workqueue.h>
#include <linux/debugfs.h>
#include <linux/slab.h>
#include <linux/notifier.h>
#include <linux/kthread.h>
#ifdef CONFIG_SWITCH
#include <linux/switch.h>
#endif /* CONFIG_SWITCH */

#include <mach/msm_xo.h>
#include <mach/msm_hsusb.h>

#define CHG_BUCK_CLOCK_CTRL	0x14

#define PBL_ACCESS1		0x04
#define PBL_ACCESS2		0x05
#define SYS_CONFIG_1		0x06
#define SYS_CONFIG_2		0x07
#define CHG_CNTRL		0x204
#define CHG_IBAT_MAX		0x205
#define CHG_TEST		0x206
#define CHG_BUCK_CTRL_TEST1	0x207
#define CHG_BUCK_CTRL_TEST2	0x208
#define CHG_BUCK_CTRL_TEST3	0x209
#define COMPARATOR_OVERRIDE	0x20A
#define PSI_TXRX_SAMPLE_DATA_0	0x20B
#define PSI_TXRX_SAMPLE_DATA_1	0x20C
#define PSI_TXRX_SAMPLE_DATA_2	0x20D
#define PSI_TXRX_SAMPLE_DATA_3	0x20E
#define PSI_CONFIG_STATUS	0x20F
#define CHG_IBAT_SAFE		0x210
#define CHG_ITRICKLE		0x211
#define CHG_CNTRL_2		0x212
#define CHG_VBAT_DET		0x213
#define CHG_VTRICKLE		0x214
#define CHG_ITERM		0x215
#define CHG_CNTRL_3		0x216
#define CHG_VIN_MIN		0x217
#define CHG_TWDOG		0x218
#define CHG_TTRKL_MAX		0x219
#define CHG_TEMP_THRESH		0x21A
#define CHG_TCHG_MAX		0x21B
#define USB_OVP_CONTROL		0x21C
#define DC_OVP_CONTROL		0x21D
#define USB_OVP_TEST		0x21E
#define DC_OVP_TEST		0x21F
#define CHG_VDD_MAX		0x220
#define CHG_VDD_SAFE		0x221
#define CHG_VBAT_BOOT_THRESH	0x222
#define USB_OVP_TRIM		0x355
#define BUCK_CONTROL_TRIM1	0x356
#define BUCK_CONTROL_TRIM2	0x357
#define BUCK_CONTROL_TRIM3	0x358
#define BUCK_CONTROL_TRIM4	0x359
#define CHG_DEFAULTS_TRIM	0x35A
#define CHG_ITRIM		0x35B
#define CHG_TTRIM		0x35C
#define CHG_COMP_OVR		0x20A
#define IUSB_FINE_RES		0x2B6

/* check EOC every 10 seconds */
#define EOC_CHECK_PERIOD_MS	10000
/* check for USB unplug every 200 msecs */
#define UNPLUG_CHECK_WAIT_PERIOD_MS 200

/* how many times low bat is checked */
#define RETRY_NUM_FOR_FORCE_SHUTDOWN	5

enum chg_fsm_state {
	FSM_STATE_OFF_0 = 0,
	FSM_STATE_BATFETDET_START_12 = 12,
	FSM_STATE_BATFETDET_END_16 = 16,
	FSM_STATE_ON_CHG_HIGHI_1 = 1,
	FSM_STATE_ATC_2A = 2,
	FSM_STATE_ATC_2B = 18,
	FSM_STATE_ON_BAT_3 = 3,
	FSM_STATE_ATC_FAIL_4 = 4 ,
	FSM_STATE_DELAY_5 = 5,
	FSM_STATE_ON_CHG_AND_BAT_6 = 6,
	FSM_STATE_FAST_CHG_7 = 7,
	FSM_STATE_TRKL_CHG_8 = 8,
	FSM_STATE_CHG_FAIL_9 = 9,
	FSM_STATE_EOC_10 = 10,
	FSM_STATE_ON_CHG_VREGOK_11 = 11,
	FSM_STATE_ATC_PAUSE_13 = 13,
	FSM_STATE_FAST_CHG_PAUSE_14 = 14,
	FSM_STATE_TRKL_CHG_PAUSE_15 = 15,
	FSM_STATE_START_BOOT = 20,
	FSM_STATE_FLCB_VREGOK = 21,
	FSM_STATE_FLCB = 22,
};

struct fsm_state_to_batt_status {
	enum chg_fsm_state	fsm_state;
	int			batt_state;
};

static struct fsm_state_to_batt_status map[] = {
	{FSM_STATE_OFF_0, POWER_SUPPLY_STATUS_UNKNOWN},
	{FSM_STATE_BATFETDET_START_12, POWER_SUPPLY_STATUS_UNKNOWN},
	{FSM_STATE_BATFETDET_END_16, POWER_SUPPLY_STATUS_UNKNOWN},
	/*
	 * for CHG_HIGHI_1 report NOT_CHARGING if battery missing,
	 * too hot/cold, charger too hot
	 */
	{FSM_STATE_ON_CHG_HIGHI_1, POWER_SUPPLY_STATUS_FULL},
	{FSM_STATE_ATC_2A, POWER_SUPPLY_STATUS_CHARGING},
	{FSM_STATE_ATC_2B, POWER_SUPPLY_STATUS_CHARGING},
	{FSM_STATE_ON_BAT_3, POWER_SUPPLY_STATUS_DISCHARGING},
	{FSM_STATE_ATC_FAIL_4, POWER_SUPPLY_STATUS_DISCHARGING},
	{FSM_STATE_DELAY_5, POWER_SUPPLY_STATUS_UNKNOWN },
	{FSM_STATE_ON_CHG_AND_BAT_6, POWER_SUPPLY_STATUS_CHARGING},
	{FSM_STATE_FAST_CHG_7, POWER_SUPPLY_STATUS_CHARGING},
	{FSM_STATE_TRKL_CHG_8, POWER_SUPPLY_STATUS_CHARGING},
	{FSM_STATE_CHG_FAIL_9, POWER_SUPPLY_STATUS_DISCHARGING},
	{FSM_STATE_EOC_10, POWER_SUPPLY_STATUS_FULL},
	{FSM_STATE_ON_CHG_VREGOK_11, POWER_SUPPLY_STATUS_NOT_CHARGING},
	{FSM_STATE_ATC_PAUSE_13, POWER_SUPPLY_STATUS_NOT_CHARGING},
	{FSM_STATE_FAST_CHG_PAUSE_14, POWER_SUPPLY_STATUS_NOT_CHARGING},
	{FSM_STATE_TRKL_CHG_PAUSE_15, POWER_SUPPLY_STATUS_NOT_CHARGING},
	{FSM_STATE_START_BOOT, POWER_SUPPLY_STATUS_NOT_CHARGING},
	{FSM_STATE_FLCB_VREGOK, POWER_SUPPLY_STATUS_NOT_CHARGING},
	{FSM_STATE_FLCB, POWER_SUPPLY_STATUS_NOT_CHARGING},
};

enum chg_regulation_loop {
	VDD_LOOP = BIT(3),
	BAT_CURRENT_LOOP = BIT(2),
	INPUT_CURRENT_LOOP = BIT(1),
	INPUT_VOLTAGE_LOOP = BIT(0),
	CHG_ALL_LOOPS = VDD_LOOP | BAT_CURRENT_LOOP
			| INPUT_CURRENT_LOOP | INPUT_VOLTAGE_LOOP,
};

enum pmic_chg_interrupts {
	USBIN_VALID_IRQ = 0,
	USBIN_OV_IRQ,
	BATT_INSERTED_IRQ,
	VBATDET_LOW_IRQ,
	USBIN_UV_IRQ,
	VBAT_OV_IRQ,
	CHGWDOG_IRQ,
	VCP_IRQ,
	ATCDONE_IRQ,
	ATCFAIL_IRQ,
	CHGDONE_IRQ,
	CHGFAIL_IRQ,
	CHGSTATE_IRQ,
	LOOP_CHANGE_IRQ,
	FASTCHG_IRQ,
	TRKLCHG_IRQ,
	BATT_REMOVED_IRQ,
	BATTTEMP_HOT_IRQ,
	CHGHOT_IRQ,
	BATTTEMP_COLD_IRQ,
	CHG_GONE_IRQ,
	BAT_TEMP_OK_IRQ,
	COARSE_DET_LOW_IRQ,
	VDD_LOOP_IRQ,
	VREG_OV_IRQ,
	VBATDET_IRQ,
	BATFET_IRQ,
	PSI_IRQ,
	DCIN_VALID_IRQ,
	DCIN_OV_IRQ,
	DCIN_UV_IRQ,
	PM_CHG_MAX_INTS,
};

enum temp_condition {
	TEMP_COLD,
	TEMP_COOL,
	TEMP_NORMAL,
	TEMP_WARM,
	TEMP_HOT,
	TEMP_MAX_NUM,
};

struct temp_condition_data {
	int	low;
	int	high;
	void	(*temp_func)(void);
};

struct temp_state {
	enum temp_condition	condition;
	struct temp_condition_data data[TEMP_MAX_NUM];
};

struct temp_condition_flg {
	unsigned int	is_bat_cool;
	unsigned int	is_bat_warm;
};

static struct temp_condition_flg temp_condition_flg_data[TEMP_MAX_NUM] = {
	[TEMP_COLD]	= {.is_bat_cool = 0, .is_bat_warm = 0},
	[TEMP_COOL]	= {.is_bat_cool = 1, .is_bat_warm = 0},
	[TEMP_NORMAL]	= {.is_bat_cool = 0, .is_bat_warm = 0},
	[TEMP_WARM]	= {.is_bat_cool = 0, .is_bat_warm = 1},
	[TEMP_HOT]	= {.is_bat_cool = 0, .is_bat_warm = 0},
};

enum charging_dis_bit {
	DIS_BIT_EOC,
	DIS_BIT_THERM_FET_OPEN,
	DIS_BIT_CHG_BATT_INVALID,
	DIS_BIT_THERM_FET_CLOSE,
	DIS_BIT_CHG_TEMP,
	DIS_BIT_USB,
	DIS_BIT_CHG_REVERSE,
	DIS_BIT_CHG_SHUTDOWN,
	DIS_BIT_MAX_NUM,
};

struct charging_enable_reg_val {
	bool chg_en_bit_val;
	bool chg_dis_bit_val;
};

static struct charging_enable_reg_val chg_en_set_data[DIS_BIT_MAX_NUM] = {
	[DIS_BIT_EOC]			= {.chg_en_bit_val = false,
						.chg_dis_bit_val = false},
	[DIS_BIT_THERM_FET_OPEN]	= {.chg_en_bit_val = false,
						.chg_dis_bit_val = false},
	[DIS_BIT_CHG_BATT_INVALID]	= {.chg_en_bit_val = false,
						.chg_dis_bit_val = false},
	[DIS_BIT_THERM_FET_CLOSE]	= {.chg_en_bit_val = false,
						.chg_dis_bit_val = true},
	[DIS_BIT_CHG_TEMP]		= {.chg_en_bit_val = false,
						.chg_dis_bit_val = true},
	[DIS_BIT_USB]			= {.chg_en_bit_val = false,
						.chg_dis_bit_val = true},
	[DIS_BIT_CHG_REVERSE]		= {.chg_en_bit_val = false,
						.chg_dis_bit_val = true},
	[DIS_BIT_CHG_SHUTDOWN]		= {.chg_en_bit_val = false,
						.chg_dis_bit_val = true},
};

enum dis_bit_mask {
	DIS_BIT_EOC_MASK	= BIT(DIS_BIT_EOC),
	DIS_BIT_CHG_BATT_MASK	= BIT(DIS_BIT_CHG_BATT_INVALID),
	DIS_BIT_THERM_MASK	= (BIT(DIS_BIT_THERM_FET_OPEN)
					|BIT(DIS_BIT_THERM_FET_CLOSE)),
	DIS_BIT_CHG_TEMP_MASK	= BIT(DIS_BIT_CHG_TEMP),
	DIS_BIT_USB_MASK	= BIT(DIS_BIT_USB),
	DIS_BIT_CHG_REVERSE_MASK	= BIT(DIS_BIT_CHG_REVERSE),
	DIS_BIT_CHG_SHUT_MASK	= BIT(DIS_BIT_CHG_SHUTDOWN),
};

enum usb_max_cur_src_id {
	SRC_ID_THERMAL,
	SRC_ID_OTG,
	SRC_ID_MAX_NUM,
};

static unsigned int usb_max_cur_ma[SRC_ID_MAX_NUM] = {
	[SRC_ID_THERMAL]	= UINT_MAX,
	[SRC_ID_OTG]		= 0,
};

enum disbled_type {
	DISABLED_OFF,
	DISABLED_ON_FET_OPEN,
	DISABLED_ON_FET_CLOSE,
	DISABLED_MAX_NUM,
};

enum dock_state_type {
	CHG_DOCK_UNDOCK,
	CHG_DOCK_DESK,
};

static unsigned int disabled_bit_val[DISABLED_MAX_NUM] = {
	[DISABLED_OFF]		= false,
	[DISABLED_ON_FET_OPEN]	= BIT(DIS_BIT_THERM_FET_OPEN),
	[DISABLED_ON_FET_CLOSE]	= BIT(DIS_BIT_THERM_FET_CLOSE),
};

static int msm_battery_gauge_alarm_notify(struct notifier_block *nb,
					  unsigned long status, void *unused);

static struct notifier_block alarm_notifier = {
	.notifier_call = msm_battery_gauge_alarm_notify,
};

static int charging_disabled_therm;
static DEFINE_MUTEX(chg_enable_lock);
static DEFINE_MUTEX(update_temp_lock);
static int update_charge_state(unsigned int value, enum dis_bit_mask mask);
static int disable_safety_timer;
module_param(disable_safety_timer, int, 0644);

struct bms_notify {
	int			is_battery_full;
	int			is_charging;
	struct	work_struct	work;
};

/**
 * struct pm8921_chg_chip -device information
 * @dev:			device pointer to access the parent
 * @usb_present:		present status of usb
 * @dc_present:			present status of dc
 * @usb_charger_current:	usb current to charge the battery with used when
 *				the usb path is enabled or charging is resumed
 * @safety_time:		max time for which charging will happen
 * @update_time:		how frequently the userland needs to be updated
 * @max_voltage_mv:		the max volts the batt should be charged up to
 * @min_voltage_mv:		the min battery voltage before turning the FETon
 * @cool_temp_dc:		the cool temp threshold in deciCelcius
 * @warm_temp_dc:		the warm temp threshold in deciCelcius
 * @resume_voltage_delta:	the voltage delta from vdd max at which the
 *				battery should resume charging
 * @term_current:		The charging based term current
 *
 */
struct pm8921_chg_chip {
	struct device			*dev;
	unsigned int			usb_present;
	unsigned int			dc_present;
	unsigned int			usb_charger_current;
	unsigned int			safe_current;
	unsigned int			max_bat_chg_current;
	unsigned int			pmic_chg_irq[PM_CHG_MAX_INTS];
	unsigned int			safety_time;
	unsigned int			ttrkl_time;
	unsigned int			update_time;
	unsigned int			low_bat_update_time;
	unsigned int			default_update_time;
	unsigned int			max_voltage_mv;
	unsigned int			min_voltage_mv;
	unsigned int			num_of_under_voltage;
	int				cold_temp_dc;
	int				cool_temp_dc;
	int				warm_temp_dc;
	int				ibatmax_max_adj_ma;
	int				hot_temp_dc;
	int				hysterisis_temp_dc;
	unsigned int			temp_check_period;
	unsigned int			cool_bat_chg_current;
	unsigned int			warm_bat_chg_current;
	unsigned int			cool_bat_voltage;
	unsigned int			warm_bat_voltage;
	unsigned int			is_bat_cool;
	unsigned int			is_bat_warm;
	unsigned int			resume_voltage_delta;
	unsigned int			resume_soc;
	unsigned int			term_current;
	unsigned int			vbat_channel;
	unsigned int			batt_temp_channel;
	unsigned int			batt_id_channel;
	struct power_supply		usb_psy;
	struct power_supply		dc_psy;
	struct power_supply		*ext_psy;
	struct power_supply		batt_psy;
	enum power_supply_type		usb_type;
	struct dentry			*dent;
	struct temp_state		temp_state;
	struct bms_notify		bms_notify;
	struct timespec			t;
	bool				keep_btm_on_suspend;
	bool				ext_charging;
	bool				ext_charge_done;
	bool				is_temp_control;
	bool				iusb_fine_res;
	bool				dc_unplug_check;
	DECLARE_BITMAP(enabled_irqs, PM_CHG_MAX_INTS);
	struct work_struct		battery_id_valid_work;
	int64_t				batt_id_min;
	int64_t				batt_id_max;
	int				soc;
	int				trkl_voltage;
	int				weak_voltage;
	int				trkl_current;
	int				weak_current;
	int				vin_min;
	unsigned int			*thermal_mitigation;
	int				thermal_levels;
	int				read_fsm_state_param;
	struct delayed_work		update_heartbeat_work;
	struct delayed_work		eoc_work;
	struct delayed_work		unplug_check_work;
	struct delayed_work		vin_collapse_check_work;
	struct delayed_work		delayed_notify_work;
	struct wake_lock		eoc_wake_lock;
	struct wake_lock		low_voltage_wake_lock;
	enum pm8921_chg_cold_thr	cold_thr;
	enum pm8921_chg_hot_thr		hot_thr;
	bool				ibat_calib_enable;
	int				cc_uah;
	int				last_cc_uah; /* used for Iavg calc */
	int				rconn_mohm;
	u8				active_path;
	bool				source_current_disabled;
#ifdef CONFIG_SWITCH
	struct switch_dev		swdev;
	enum dock_state_type		dock_type;
	struct wake_lock		dock_wake_lock;
	spinlock_t			dock_lock;
#endif /* CONFIG_SWITCH */
};

/* user space parameter to limit usb current */
static unsigned int usb_max_current;
static int ibat_target_ma;

/*
 * usb_target_ma is used for wall charger
 * adaptive input current limiting only. Use
 * pm_iusbmax_get() to get current maximum usb current setting.
 */
static int usb_target_ma;
static int charging_disabled;
static int thermal_mitigation;

static struct pm8921_chg_chip *the_chip;

static struct pm8xxx_adc_arb_btm_param btm_config;

static void delayed_notify_worker(struct work_struct *work)
{
	struct delayed_work *dwork = to_delayed_work(work);
	struct pm8921_chg_chip *chip = container_of(dwork,
				struct pm8921_chg_chip, delayed_notify_work);
	pr_debug("Called power_supply_changed\n");
	power_supply_changed(&chip->usb_psy);
	power_supply_changed(&chip->batt_psy);
}

static int pm_chg_masked_write(struct pm8921_chg_chip *chip, u16 addr,
							u8 mask, u8 val)
{
	int rc;
	u8 reg;

	rc = pm8xxx_readb(chip->dev->parent, addr, &reg);
	if (rc) {
		pr_err("pm8xxx_readb failed: addr=%03X, rc=%d\n", addr, rc);
		return rc;
	}
	reg &= ~mask;
	reg |= val & mask;
	rc = pm8xxx_writeb(chip->dev->parent, addr, reg);
	if (rc) {
		pr_err("pm8xxx_writeb failed: addr=%03X, rc=%d\n", addr, rc);
		return rc;
	}
	return 0;
}

static int pm_chg_get_rt_status(struct pm8921_chg_chip *chip, int irq_id)
{
	return pm8xxx_read_irq_stat(chip->dev->parent,
					chip->pmic_chg_irq[irq_id]);
}

/* Treat OverVoltage/UnderVoltage as source missing */
static int is_usb_chg_plugged_in(struct pm8921_chg_chip *chip)
{
	return pm_chg_get_rt_status(chip, USBIN_VALID_IRQ);
}

/* Treat OverVoltage/UnderVoltage as source missing */
static int is_dc_chg_plugged_in(struct pm8921_chg_chip *chip)
{
	return pm_chg_get_rt_status(chip, DCIN_VALID_IRQ);
}

#define CAPTURE_FSM_STATE_CMD	0xC2
#define READ_BANK_7		0x70
#define READ_BANK_4		0x40
static int pm_chg_get_fsm_state(struct pm8921_chg_chip *chip)
{
	u8 temp;
	int err, ret = 0;

	temp = CAPTURE_FSM_STATE_CMD;
	err = pm8xxx_writeb(chip->dev->parent, CHG_TEST, temp);
	if (err) {
		pr_err("Error %d writing %d to addr %d\n", err, temp, CHG_TEST);
		return err;
	}

	temp = READ_BANK_7;
	err = pm8xxx_writeb(chip->dev->parent, CHG_TEST, temp);
	if (err) {
		pr_err("Error %d writing %d to addr %d\n", err, temp, CHG_TEST);
		return err;
	}

	err = pm8xxx_readb(chip->dev->parent, CHG_TEST, &temp);
	if (err) {
		pr_err("pm8xxx_readb fail: addr=%03X, rc=%d\n", CHG_TEST, err);
		return err;
	}
	/* get the lower 4 bits */
	ret = temp & 0xF;

	temp = READ_BANK_4;
	err = pm8xxx_writeb(chip->dev->parent, CHG_TEST, temp);
	if (err) {
		pr_err("Error %d writing %d to addr %d\n", err, temp, CHG_TEST);
		return err;
	}

	err = pm8xxx_readb(chip->dev->parent, CHG_TEST, &temp);
	if (err) {
		pr_err("pm8xxx_readb fail: addr=%03X, rc=%d\n", CHG_TEST, err);
		return err;
	}
	/* get the upper 1 bit */
	ret |= (temp & 0x1) << 4;
	chip->read_fsm_state_param = ret;
	return  ret;
}

#define READ_BANK_6		0x60
static int pm_chg_get_regulation_loop(struct pm8921_chg_chip *chip)
{
	u8 temp;
	int err;

	temp = READ_BANK_6;
	err = pm8xxx_writeb(chip->dev->parent, CHG_TEST, temp);
	if (err) {
		pr_err("Error %d writing %d to addr %d\n", err, temp, CHG_TEST);
		return err;
	}

	err = pm8xxx_readb(chip->dev->parent, CHG_TEST, &temp);
	if (err) {
		pr_err("pm8xxx_readb fail: addr=%03X, rc=%d\n", CHG_TEST, err);
		return err;
	}

	/* return the lower 4 bits */
	return temp & CHG_ALL_LOOPS;
}

#define CHG_USB_SUSPEND_BIT  BIT(2)
static int pm_chg_usb_suspend_enable(struct pm8921_chg_chip *chip, int enable)
{
	return pm_chg_masked_write(chip, CHG_CNTRL_3, CHG_USB_SUSPEND_BIT,
			enable ? CHG_USB_SUSPEND_BIT : 0);
}

#define CHG_EN_BIT	BIT(7)
static int pm_chg_auto_enable(struct pm8921_chg_chip *chip, int enable)
{
	return pm_chg_masked_write(chip, CHG_CNTRL_3, CHG_EN_BIT,
				enable ? CHG_EN_BIT : 0);
}

static void clear_long_time_chg_failed(struct pm8921_chg_chip *chip);
#define CHG_FAILED_CLEAR	BIT(0)
#define ATC_FAILED_CLEAR	BIT(1)
static int pm_chg_failed_clear(struct pm8921_chg_chip *chip, int clear)
{
	int rc;

	rc = pm_chg_masked_write(chip, CHG_CNTRL_3, ATC_FAILED_CLEAR,
				clear ? ATC_FAILED_CLEAR : 0);
	if (disable_safety_timer)
		clear_long_time_chg_failed(chip);

	return rc;
}

#define CHG_CHARGE_DIS_BIT	BIT(1)
static int pm_chg_charge_dis(struct pm8921_chg_chip *chip, int disable)
{
	return pm_chg_masked_write(chip, CHG_CNTRL, CHG_CHARGE_DIS_BIT,
				disable ? CHG_CHARGE_DIS_BIT : 0);
}

static int pm_is_chg_charge_dis(struct pm8921_chg_chip *chip)
{
	u8 temp;

	pm8xxx_readb(chip->dev->parent, CHG_CNTRL, &temp);
	return  temp & CHG_CHARGE_DIS_BIT;
}
#define PM8921_CHG_V_MIN_MV	3240
#define PM8921_CHG_V_STEP_MV	20
#define PM8921_CHG_V_STEP_10MV_OFFSET_BIT	BIT(7)
#define PM8921_CHG_VDDMAX_MAX	4500
#define PM8921_CHG_VDDMAX_MIN	3400
#define PM8921_CHG_V_MASK	0x7F
static int __pm_chg_vddmax_set(struct pm8921_chg_chip *chip, int voltage)
{
	int remainder;
	u8 temp = 0;

	if (voltage < PM8921_CHG_VDDMAX_MIN
			|| voltage > PM8921_CHG_VDDMAX_MAX) {
		pr_err("bad mV=%d asked to set\n", voltage);
		return -EINVAL;
	}

	temp = (voltage - PM8921_CHG_V_MIN_MV) / PM8921_CHG_V_STEP_MV;

	remainder = voltage % 20;
	if (remainder >= 10) {
		temp |= PM8921_CHG_V_STEP_10MV_OFFSET_BIT;
	}

	pr_debug("voltage=%d setting %02x\n", voltage, temp);
	return pm8xxx_writeb(chip->dev->parent, CHG_VDD_MAX, temp);
}

static int pm_chg_vddmax_get(struct pm8921_chg_chip *chip, int *voltage)
{
	u8 temp;
	int rc;

	rc = pm8xxx_readb(chip->dev->parent, CHG_VDD_MAX, &temp);
	if (rc) {
		pr_err("rc = %d while reading vdd max\n", rc);
		*voltage = 0;
		return rc;
	}
	*voltage = (int)(temp & PM8921_CHG_V_MASK) * PM8921_CHG_V_STEP_MV
							+ PM8921_CHG_V_MIN_MV;
	if (temp & PM8921_CHG_V_STEP_10MV_OFFSET_BIT)
		*voltage =  *voltage + 10;
	return 0;
}

static int pm_chg_vddmax_set(struct pm8921_chg_chip *chip, int voltage)
{
	int current_mv, ret, steps, i;
	bool increase;

	ret = 0;

	if (voltage < PM8921_CHG_VDDMAX_MIN
		|| voltage > PM8921_CHG_VDDMAX_MAX) {
		pr_err("bad mV=%d asked to set\n", voltage);
		return -EINVAL;
	}

	ret = pm_chg_vddmax_get(chip, &current_mv);
	if (ret) {
		pr_err("Failed to read vddmax rc=%d\n", ret);
		return -EINVAL;
	}
	if (current_mv == voltage)
		return 0;

	/* Only change in increments when USB is present */
	if (is_usb_chg_plugged_in(chip)) {
		if (current_mv < voltage) {
			steps = (voltage - current_mv) / PM8921_CHG_V_STEP_MV;
			increase = true;
		} else {
			steps = (current_mv - voltage) / PM8921_CHG_V_STEP_MV;
			increase = false;
		}
		for (i = 0; i < steps; i++) {
			if (increase)
				current_mv += PM8921_CHG_V_STEP_MV;
			else
				current_mv -= PM8921_CHG_V_STEP_MV;
			ret |= __pm_chg_vddmax_set(chip, current_mv);
		}
	}
	ret |= __pm_chg_vddmax_set(chip, voltage);
	return ret;
}

#define PM8921_CHG_VDDSAFE_MIN	3400
#define PM8921_CHG_VDDSAFE_MAX	4500
static int pm_chg_vddsafe_set(struct pm8921_chg_chip *chip, int voltage)
{
	u8 temp;

	if (voltage < PM8921_CHG_VDDSAFE_MIN
			|| voltage > PM8921_CHG_VDDSAFE_MAX) {
		pr_err("bad mV=%d asked to set\n", voltage);
		return -EINVAL;
	}
	temp = (voltage - PM8921_CHG_V_MIN_MV) / PM8921_CHG_V_STEP_MV;
	pr_debug("voltage=%d setting %02x\n", voltage, temp);
	return pm_chg_masked_write(chip, CHG_VDD_SAFE, PM8921_CHG_V_MASK, temp);
}

#define PM8921_CHG_VBATDET_MIN	3240
#define PM8921_CHG_VBATDET_MAX	5780
static int pm_chg_vbatdet_set(struct pm8921_chg_chip *chip, int voltage)
{
	u8 temp;

	if (voltage < PM8921_CHG_VBATDET_MIN
			|| voltage > PM8921_CHG_VBATDET_MAX) {
		pr_err("bad mV=%d asked to set\n", voltage);
		return -EINVAL;
	}
	temp = (voltage - PM8921_CHG_V_MIN_MV) / PM8921_CHG_V_STEP_MV;
	pr_debug("voltage=%d setting %02x\n", voltage, temp);
	return pm_chg_masked_write(chip, CHG_VBAT_DET, PM8921_CHG_V_MASK, temp);
}

#define PM8921_CHG_VINMIN_MIN_MV	3800
#define PM8921_CHG_VINMIN_STEP_MV	100
#define PM8921_CHG_VINMIN_USABLE_MAX	6500
#define PM8921_CHG_VINMIN_USABLE_MIN	4300
#define PM8921_CHG_VINMIN_MASK		0x1F
static int pm_chg_vinmin_set(struct pm8921_chg_chip *chip, int voltage)
{
	u8 temp;

	if (voltage < PM8921_CHG_VINMIN_USABLE_MIN
			|| voltage > PM8921_CHG_VINMIN_USABLE_MAX) {
		pr_err("bad mV=%d asked to set\n", voltage);
		return -EINVAL;
	}
	temp = (voltage - PM8921_CHG_VINMIN_MIN_MV) / PM8921_CHG_VINMIN_STEP_MV;
	pr_debug("voltage=%d setting %02x\n", voltage, temp);
	return pm_chg_masked_write(chip, CHG_VIN_MIN, PM8921_CHG_VINMIN_MASK,
									temp);
}

static int pm_chg_vinmin_get(struct pm8921_chg_chip *chip)
{
	u8 temp;
	int rc, voltage_mv;

	rc = pm8xxx_readb(chip->dev->parent, CHG_VIN_MIN, &temp);
	temp &= PM8921_CHG_VINMIN_MASK;

	voltage_mv = PM8921_CHG_VINMIN_MIN_MV +
			(int)temp * PM8921_CHG_VINMIN_STEP_MV;

	return voltage_mv;
}

#define PM8921_CHG_IBATMAX_MIN	325
#define PM8921_CHG_IBATMAX_MAX	3025
#define PM8921_CHG_I_MIN_MA	225
#define PM8921_CHG_I_STEP_MA	50
#define PM8921_CHG_I_MASK	0x3F
static int pm_chg_ibatmax_get(struct pm8921_chg_chip *chip, int *ibat_ma)
{
	u8 temp;
	int rc;

	rc = pm8xxx_readb(chip->dev->parent, CHG_IBAT_MAX, &temp);
	if (rc) {
		pr_err("rc = %d while reading ibat max\n", rc);
		*ibat_ma = 0;
		return rc;
	}
	*ibat_ma = (int)(temp & PM8921_CHG_I_MASK) * PM8921_CHG_I_STEP_MA
							+ PM8921_CHG_I_MIN_MA;
	return 0;
}

static int pm_chg_ibatmax_set(struct pm8921_chg_chip *chip, int chg_current)
{
	u8 temp;

	if (chg_current < PM8921_CHG_IBATMAX_MIN
			|| chg_current > PM8921_CHG_IBATMAX_MAX) {
		pr_err("bad mA=%d asked to set\n", chg_current);
		return -EINVAL;
	}
	temp = (chg_current - PM8921_CHG_I_MIN_MA) / PM8921_CHG_I_STEP_MA;
	return pm_chg_masked_write(chip, CHG_IBAT_MAX, PM8921_CHG_I_MASK, temp);
}

#define PM8921_CHG_IBATSAFE_MIN	225
#define PM8921_CHG_IBATSAFE_MAX	3375
static int pm_chg_ibatsafe_set(struct pm8921_chg_chip *chip, int chg_current)
{
	u8 temp;

	if (chg_current < PM8921_CHG_IBATSAFE_MIN
			|| chg_current > PM8921_CHG_IBATSAFE_MAX) {
		pr_err("bad mA=%d asked to set\n", chg_current);
		return -EINVAL;
	}
	temp = (chg_current - PM8921_CHG_I_MIN_MA) / PM8921_CHG_I_STEP_MA;
	return pm_chg_masked_write(chip, CHG_IBAT_SAFE,
						PM8921_CHG_I_MASK, temp);
}

#define PM8921_CHG_ITERM_MIN_MA		50
#define PM8921_CHG_ITERM_MAX_MA		200
#define PM8921_CHG_ITERM_STEP_MA	10
#define PM8921_CHG_ITERM_MASK		0xF
static int pm_chg_iterm_set(struct pm8921_chg_chip *chip, int chg_current)
{
	u8 temp;

	if (chg_current < PM8921_CHG_ITERM_MIN_MA
			|| chg_current > PM8921_CHG_ITERM_MAX_MA) {
		pr_err("bad mA=%d asked to set\n", chg_current);
		return -EINVAL;
	}

	temp = (chg_current - PM8921_CHG_ITERM_MIN_MA)
				/ PM8921_CHG_ITERM_STEP_MA;
	return pm_chg_masked_write(chip, CHG_ITERM, PM8921_CHG_ITERM_MASK,
					 temp);
}

static int pm_chg_iterm_get(struct pm8921_chg_chip *chip, int *chg_current)
{
	u8 temp;
	int rc;

	rc = pm8xxx_readb(chip->dev->parent, CHG_ITERM, &temp);
	if (rc) {
		pr_err("err=%d reading CHG_ITEM\n", rc);
		*chg_current = 0;
		return rc;
	}
	temp &= PM8921_CHG_ITERM_MASK;
	*chg_current = (int)temp * PM8921_CHG_ITERM_STEP_MA
					+ PM8921_CHG_ITERM_MIN_MA;
	return 0;
}

struct ibatmax_max_adj_entry {
	int ibat_max_ma;
	int max_adj_ma;
};

static struct ibatmax_max_adj_entry ibatmax_adj_table[] = {
	{975, 300},
	{1475, 150},
	{1975, 200},
	{2475, 250},
};

struct usb_ma_limit_entry {
	int	usb_ma;
	u8	value;
};

static struct usb_ma_limit_entry usb_ma_table[] = {
	{100, 0x0},
	{200, 0x1},
	{500, 0x2},
	{600, 0x3},
	{700, 0x4},
	{800, 0x5},
	{850, 0x6},
	{900, 0x8},
	{950, 0x7},
	{1000, 0x9},
	{1100, 0xA},
	{1200, 0xB},
	{1300, 0xC},
	{1400, 0xD},
	{1500, 0xE},
	{1600, 0xF},
};

#define PM8921_CHG_IUSB_MASK 0x1C
#define PM8921_CHG_IUSB_SHIFT 2
#define PM8921_CHG_IUSB_MAX  7
#define PM8921_CHG_IUSB_MIN  0
#define PM8917_IUSB_FINE_RES BIT(0)
static int pm_chg_iusbmax_set(struct pm8921_chg_chip *chip, int reg_val)
{
	u8 temp, fineres;
	int rc;

	fineres = PM8917_IUSB_FINE_RES & usb_ma_table[reg_val].value;
	reg_val = usb_ma_table[reg_val].value >> 1;

	if (reg_val < PM8921_CHG_IUSB_MIN || reg_val > PM8921_CHG_IUSB_MAX) {
		pr_err("bad mA=%d asked to set\n", reg_val);
		return -EINVAL;
	}
	temp = reg_val << PM8921_CHG_IUSB_SHIFT;

	/* IUSB_FINE_RES */
	if (chip->iusb_fine_res) {
		/* Clear IUSB_FINE_RES bit to avoid overshoot */
		rc = pm_chg_masked_write(chip, IUSB_FINE_RES,
			PM8917_IUSB_FINE_RES, 0);

		rc |= pm_chg_masked_write(chip, PBL_ACCESS2,
			PM8921_CHG_IUSB_MASK, temp);

		if (rc) {
			pr_err("Failed to write PBL_ACCESS2 rc=%d\n", rc);
			return rc;
		}

		if (fineres) {
			rc = pm_chg_masked_write(chip, IUSB_FINE_RES,
				PM8917_IUSB_FINE_RES, fineres);
			if (rc)
				pr_err("Failed to write ISUB_FINE_RES rc=%d\n",
					rc);
		}
	} else {
		rc = pm_chg_masked_write(chip, PBL_ACCESS2,
			PM8921_CHG_IUSB_MASK, temp);
		if (rc)
			pr_err("Failed to write PBL_ACCESS2 rc=%d\n", rc);
	}

	return rc;
}

static int pm_chg_iusbmax_get(struct pm8921_chg_chip *chip, int *mA)
{
	u8 temp, fineres;
	int rc, i;

	fineres = 0;
	*mA = 0;
	rc = pm8xxx_readb(chip->dev->parent, PBL_ACCESS2, &temp);
	if (rc) {
		pr_err("err=%d reading PBL_ACCESS2\n", rc);
		return rc;
	}

	if (chip->iusb_fine_res) {
		rc = pm8xxx_readb(chip->dev->parent, IUSB_FINE_RES, &fineres);
		if (rc) {
			pr_err("err=%d reading IUSB_FINE_RES\n", rc);
			return rc;
		}
	}
	temp &= PM8921_CHG_IUSB_MASK;
	temp = temp >> PM8921_CHG_IUSB_SHIFT;

	temp = (temp << 1) | (fineres & PM8917_IUSB_FINE_RES);
	for (i = ARRAY_SIZE(usb_ma_table) - 1; i >= 0; i--) {
		if (usb_ma_table[i].value == temp)
			break;
	}

	*mA = usb_ma_table[i].usb_ma;

	return rc;
}

#define PM8921_CHG_WD_MASK 0x1F
static int pm_chg_disable_wd(struct pm8921_chg_chip *chip)
{
	/* writing 0 to the wd timer disables it */
	return pm_chg_masked_write(chip, CHG_TWDOG, PM8921_CHG_WD_MASK, 0);
}

#define PM8921_CHG_TCHG_MASK	0x7F
#define PM8921_CHG_TCHG_MIN	4
#define PM8921_CHG_TCHG_MAX	512
#define PM8921_CHG_TCHG_STEP	4
static int pm_chg_tchg_max_set(struct pm8921_chg_chip *chip, int minutes)
{
	u8 temp;

	if (minutes < PM8921_CHG_TCHG_MIN || minutes > PM8921_CHG_TCHG_MAX) {
		pr_err("bad max minutes =%d asked to set\n", minutes);
		return -EINVAL;
	}

	temp = (minutes - 1)/PM8921_CHG_TCHG_STEP;
	return pm_chg_masked_write(chip, CHG_TCHG_MAX, PM8921_CHG_TCHG_MASK,
					 temp);
}

#define PM8921_CHG_TTRKL_MASK	0x1F
#define PM8921_CHG_TTRKL_MIN	1
#define PM8921_CHG_TTRKL_MAX	64
static int pm_chg_ttrkl_max_set(struct pm8921_chg_chip *chip, int minutes)
{
	u8 temp;

	if (minutes < PM8921_CHG_TTRKL_MIN || minutes > PM8921_CHG_TTRKL_MAX) {
		pr_err("bad max minutes =%d asked to set\n", minutes);
		return -EINVAL;
	}

	temp = minutes - 1;
	return pm_chg_masked_write(chip, CHG_TTRKL_MAX, PM8921_CHG_TTRKL_MASK,
					 temp);
}

#define PM8921_CHG_VTRKL_MIN_MV		2050
#define PM8921_CHG_VTRKL_MAX_MV		2800
#define PM8921_CHG_VTRKL_STEP_MV	50
#define PM8921_CHG_VTRKL_SHIFT		4
#define PM8921_CHG_VTRKL_MASK		0xF0
static int pm_chg_vtrkl_low_set(struct pm8921_chg_chip *chip, int millivolts)
{
	u8 temp;

	if (millivolts < PM8921_CHG_VTRKL_MIN_MV
			|| millivolts > PM8921_CHG_VTRKL_MAX_MV) {
		pr_err("bad voltage = %dmV asked to set\n", millivolts);
		return -EINVAL;
	}

	temp = (millivolts - PM8921_CHG_VTRKL_MIN_MV)/PM8921_CHG_VTRKL_STEP_MV;
	temp = temp << PM8921_CHG_VTRKL_SHIFT;
	return pm_chg_masked_write(chip, CHG_VTRICKLE, PM8921_CHG_VTRKL_MASK,
					 temp);
}

#define PM8921_CHG_VWEAK_MIN_MV		2100
#define PM8921_CHG_VWEAK_MAX_MV		3600
#define PM8921_CHG_VWEAK_STEP_MV	100
#define PM8921_CHG_VWEAK_MASK		0x0F
static int pm_chg_vweak_set(struct pm8921_chg_chip *chip, int millivolts)
{
	u8 temp;

	if (millivolts < PM8921_CHG_VWEAK_MIN_MV
			|| millivolts > PM8921_CHG_VWEAK_MAX_MV) {
		pr_err("bad voltage = %dmV asked to set\n", millivolts);
		return -EINVAL;
	}

	temp = (millivolts - PM8921_CHG_VWEAK_MIN_MV)/PM8921_CHG_VWEAK_STEP_MV;
	return pm_chg_masked_write(chip, CHG_VTRICKLE, PM8921_CHG_VWEAK_MASK,
					 temp);
}

#define PM8921_CHG_ITRKL_MIN_MA		50
#define PM8921_CHG_ITRKL_MAX_MA		200
#define PM8921_CHG_ITRKL_MASK		0x0F
#define PM8921_CHG_ITRKL_STEP_MA	10
static int pm_chg_itrkl_set(struct pm8921_chg_chip *chip, int milliamps)
{
	u8 temp;

	if (milliamps < PM8921_CHG_ITRKL_MIN_MA
		|| milliamps > PM8921_CHG_ITRKL_MAX_MA) {
		pr_err("bad current = %dmA asked to set\n", milliamps);
		return -EINVAL;
	}

	temp = (milliamps - PM8921_CHG_ITRKL_MIN_MA)/PM8921_CHG_ITRKL_STEP_MA;

	return pm_chg_masked_write(chip, CHG_ITRICKLE, PM8921_CHG_ITRKL_MASK,
					 temp);
}

#define PM8921_CHG_IWEAK_MIN_MA		325
#define PM8921_CHG_IWEAK_MAX_MA		525
#define PM8921_CHG_IWEAK_SHIFT		7
#define PM8921_CHG_IWEAK_MASK		0x80
static int pm_chg_iweak_set(struct pm8921_chg_chip *chip, int milliamps)
{
	u8 temp;

	if (milliamps < PM8921_CHG_IWEAK_MIN_MA
		|| milliamps > PM8921_CHG_IWEAK_MAX_MA) {
		pr_err("bad current = %dmA asked to set\n", milliamps);
		return -EINVAL;
	}

	if (milliamps < PM8921_CHG_IWEAK_MAX_MA)
		temp = 0;
	else
		temp = 1;

	temp = temp << PM8921_CHG_IWEAK_SHIFT;
	return pm_chg_masked_write(chip, CHG_ITRICKLE, PM8921_CHG_IWEAK_MASK,
					 temp);
}

#define PM8921_CHG_BATT_TEMP_THR_COLD	BIT(1)
#define PM8921_CHG_BATT_TEMP_THR_COLD_SHIFT	1
static int pm_chg_batt_cold_temp_config(struct pm8921_chg_chip *chip,
					enum pm8921_chg_cold_thr cold_thr)
{
	u8 temp;

	temp = cold_thr << PM8921_CHG_BATT_TEMP_THR_COLD_SHIFT;
	temp = temp & PM8921_CHG_BATT_TEMP_THR_COLD;
	return pm_chg_masked_write(chip, CHG_CNTRL_2,
					PM8921_CHG_BATT_TEMP_THR_COLD,
					 temp);
}

#define PM8921_CHG_BATT_TEMP_THR_HOT		BIT(0)
#define PM8921_CHG_BATT_TEMP_THR_HOT_SHIFT	0
static int pm_chg_batt_hot_temp_config(struct pm8921_chg_chip *chip,
					enum pm8921_chg_hot_thr hot_thr)
{
	u8 temp;

	temp = hot_thr << PM8921_CHG_BATT_TEMP_THR_HOT_SHIFT;
	temp = temp & PM8921_CHG_BATT_TEMP_THR_HOT;
	return pm_chg_masked_write(chip, CHG_CNTRL_2,
					PM8921_CHG_BATT_TEMP_THR_HOT,
					 temp);
}

static void disable_input_voltage_regulation(struct pm8921_chg_chip *chip)
{
	u8 temp;
	int rc;

	rc = pm8xxx_writeb(chip->dev->parent, CHG_BUCK_CTRL_TEST3, 0x70);
	if (rc) {
		pr_err("Failed to write 0x70 to CTRL_TEST3 rc = %d\n", rc);
		return;
	}
	rc = pm8xxx_readb(chip->dev->parent, CHG_BUCK_CTRL_TEST3, &temp);
	if (rc) {
		pr_err("Failed to read CTRL_TEST3 rc = %d\n", rc);
		return;
	}
	/* set the input voltage disable bit and the write bit */
	temp |= 0x81;
	rc = pm8xxx_writeb(chip->dev->parent, CHG_BUCK_CTRL_TEST3, temp);
	if (rc) {
		pr_err("Failed to write 0x%x to CTRL_TEST3 rc=%d\n", temp, rc);
		return;
	}
}

static void enable_input_voltage_regulation(struct pm8921_chg_chip *chip)
{
	u8 temp;
	int rc;

	rc = pm8xxx_writeb(chip->dev->parent, CHG_BUCK_CTRL_TEST3, 0x70);
	if (rc) {
		pr_err("Failed to write 0x70 to CTRL_TEST3 rc = %d\n", rc);
		return;
	}
	rc = pm8xxx_readb(chip->dev->parent, CHG_BUCK_CTRL_TEST3, &temp);
	if (rc) {
		pr_err("Failed to read CTRL_TEST3 rc = %d\n", rc);
		return;
	}
	/* unset the input voltage disable bit */
	temp &= 0xFE;
	/* set the write bit */
	temp |= 0x80;
	rc = pm8xxx_writeb(chip->dev->parent, CHG_BUCK_CTRL_TEST3, temp);
	if (rc) {
		pr_err("Failed to write 0x%x to CTRL_TEST3 rc=%d\n", temp, rc);
		return;
	}
}

static int64_t read_battery_id(struct pm8921_chg_chip *chip)
{
	int rc;
	struct pm8xxx_adc_chan_result result;

	rc = pm8xxx_adc_read(chip->batt_id_channel, &result);
	if (rc) {
		pr_err("error reading batt id channel = %d, rc = %d\n",
					chip->vbat_channel, rc);
		return rc;
	}
	pr_debug("batt_id phy = %lld meas = 0x%llx\n", result.physical,
						result.measurement);
	return result.physical;
}

static int is_battery_valid(struct pm8921_chg_chip *chip)
{
	int64_t rc;

	if (chip->batt_id_min == 0 && chip->batt_id_max == 0)
		return 1;

	rc = read_battery_id(chip);
	if (rc < 0) {
		pr_err("error reading batt id channel = %d, rc = %lld\n",
					chip->vbat_channel, rc);
		/* assume battery id is valid when adc error happens */
		return 1;
	}

	if (rc < chip->batt_id_min || rc > chip->batt_id_max) {
		pr_err("batt_id phy =%lld is not valid\n", rc);
		return 0;
	}
	return 1;
}

static void check_battery_valid(struct pm8921_chg_chip *chip)
{
	if (is_battery_valid(chip) == 0) {
		pr_err("batt_id not valid, disbling charging\n");
		update_charge_state(BIT(DIS_BIT_CHG_BATT_INVALID),
			DIS_BIT_CHG_BATT_MASK);
	} else {
		update_charge_state(false, DIS_BIT_CHG_BATT_MASK);
	}
}

static void battery_id_valid(struct work_struct *work)
{
	struct pm8921_chg_chip *chip = container_of(work,
				struct pm8921_chg_chip, battery_id_valid_work);

	check_battery_valid(chip);
}

static void pm8921_chg_enable_irq(struct pm8921_chg_chip *chip, int interrupt)
{
	if (!__test_and_set_bit(interrupt, chip->enabled_irqs)) {
		dev_dbg(chip->dev, "%d\n", chip->pmic_chg_irq[interrupt]);
		enable_irq(chip->pmic_chg_irq[interrupt]);
	}
}

static void pm8921_chg_disable_irq(struct pm8921_chg_chip *chip, int interrupt)
{
	if (__test_and_clear_bit(interrupt, chip->enabled_irqs)) {
		dev_dbg(chip->dev, "%d\n", chip->pmic_chg_irq[interrupt]);
		disable_irq_nosync(chip->pmic_chg_irq[interrupt]);
	}
}

static int pm8921_chg_is_enabled(struct pm8921_chg_chip *chip, int interrupt)
{
	return test_bit(interrupt, chip->enabled_irqs);
}

static bool is_ext_charging(struct pm8921_chg_chip *chip)
{
	union power_supply_propval ret = {0,};

	if (!chip->ext_psy)
		return false;
	if (chip->ext_psy->get_property(chip->ext_psy,
					POWER_SUPPLY_PROP_CHARGE_TYPE, &ret))
		return false;
	if (ret.intval > POWER_SUPPLY_CHARGE_TYPE_NONE)
		return ret.intval;

	return false;
}

static bool is_ext_trickle_charging(struct pm8921_chg_chip *chip)
{
	union power_supply_propval ret = {0,};

	if (!chip->ext_psy)
		return false;
	if (chip->ext_psy->get_property(chip->ext_psy,
					POWER_SUPPLY_PROP_CHARGE_TYPE, &ret))
		return false;
	if (ret.intval == POWER_SUPPLY_CHARGE_TYPE_TRICKLE)
		return true;

	return false;
}

static int is_battery_charging(int fsm_state)
{
	if (is_ext_charging(the_chip))
		return 1;

	switch (fsm_state) {
	case FSM_STATE_ATC_2A:
	case FSM_STATE_ATC_2B:
	case FSM_STATE_ON_CHG_AND_BAT_6:
	case FSM_STATE_FAST_CHG_7:
	case FSM_STATE_TRKL_CHG_8:
		return 1;
	}
	return 0;
}

static void bms_notify(struct work_struct *work)
{
	struct bms_notify *n = container_of(work, struct bms_notify, work);

	if (n->is_charging) {
		pm8921_bms_charging_began();
	} else {
		pm8921_bms_charging_end(n->is_battery_full);
		n->is_battery_full = 0;
	}
}

static void bms_notify_check(struct pm8921_chg_chip *chip)
{
	int fsm_state, new_is_charging;

	fsm_state = pm_chg_get_fsm_state(chip);
	new_is_charging = is_battery_charging(fsm_state);

	if (chip->bms_notify.is_charging ^ new_is_charging) {
		chip->bms_notify.is_charging = new_is_charging;
		schedule_work(&(chip->bms_notify.work));
	}
}

static enum power_supply_property pm_power_props_usb[] = {
	POWER_SUPPLY_PROP_PRESENT,
	POWER_SUPPLY_PROP_ONLINE,
	POWER_SUPPLY_PROP_CURRENT_MAX,
};

static enum power_supply_property pm_power_props_mains[] = {
	POWER_SUPPLY_PROP_PRESENT,
	POWER_SUPPLY_PROP_ONLINE,
};

static char *pm_power_supplied_to[] = {
	"battery",
};

#define USB_WALL_THRESHOLD_MA	500
static int pm_power_get_property_mains(struct power_supply *psy,
				  enum power_supply_property psp,
				  union power_supply_propval *val)
{
	/* Check if called before init */
	if (!the_chip)
		return -EINVAL;

	switch (psp) {
	case POWER_SUPPLY_PROP_PRESENT:
	case POWER_SUPPLY_PROP_ONLINE:
		val->intval = 0;
		if (charging_disabled & DIS_BIT_USB_MASK)
			return 0;

		/* check external charger first before the dc path */
		if (is_ext_charging(the_chip)) {
			val->intval = 1;
			return 0;
		}

		if (the_chip->dc_present) {
			val->intval = 1;
			return 0;
		}

		if (the_chip->usb_type == POWER_SUPPLY_TYPE_USB_DCP) {
			val->intval = is_usb_chg_plugged_in(the_chip);
			return 0;
		}
		break;
	default:
		return -EINVAL;
	}
	return 0;
}

static int pm_power_get_property_usb(struct power_supply *psy,
				  enum power_supply_property psp,
				  union power_supply_propval *val)
{
	int current_max;

	/* Check if called before init */
	if (!the_chip)
		return -EINVAL;

	switch (psp) {
	case POWER_SUPPLY_PROP_CURRENT_MAX:
		if (pm_is_chg_charge_dis(the_chip)) {
			val->intval = 0;
		} else {
			pm_chg_iusbmax_get(the_chip, &current_max);
			val->intval = current_max;
		}
		break;
	case POWER_SUPPLY_PROP_PRESENT:
		val->intval = is_usb_chg_plugged_in(the_chip);
		break;
	case POWER_SUPPLY_PROP_ONLINE:
		val->intval = 0;
		if (charging_disabled & DIS_BIT_USB_MASK)
			return 0;

		/* USB charging */
		if (the_chip->usb_type == POWER_SUPPLY_TYPE_USB ||
			the_chip->usb_type == POWER_SUPPLY_TYPE_USB_CDP ||
			the_chip->usb_type == POWER_SUPPLY_TYPE_USB_ACA) {
			val->intval = is_usb_chg_plugged_in(the_chip);
			return 0;
		}
		break;
	default:
		return -EINVAL;
	}
	return 0;
}

static enum power_supply_property msm_batt_power_props[] = {
	POWER_SUPPLY_PROP_STATUS,
	POWER_SUPPLY_PROP_CHARGE_TYPE,
	POWER_SUPPLY_PROP_HEALTH,
	POWER_SUPPLY_PROP_PRESENT,
	POWER_SUPPLY_PROP_TECHNOLOGY,
	POWER_SUPPLY_PROP_VOLTAGE_MAX_DESIGN,
	POWER_SUPPLY_PROP_VOLTAGE_MIN_DESIGN,
	POWER_SUPPLY_PROP_VOLTAGE_NOW,
	POWER_SUPPLY_PROP_VOLTAGE_AVG,
	POWER_SUPPLY_PROP_CAPACITY,
	POWER_SUPPLY_PROP_CURRENT_NOW,
	POWER_SUPPLY_PROP_CURRENT_AVG,
	POWER_SUPPLY_PROP_CHARGE_FULL_DESIGN,
	POWER_SUPPLY_PROP_CHARGE_FULL,
	POWER_SUPPLY_PROP_TEMP,
};

static int get_prop_battery_uvolts(struct pm8921_chg_chip *chip)
{
	int rc;
	struct pm8xxx_adc_chan_result result;

	rc = pm8xxx_adc_read(chip->vbat_channel, &result);
	if (rc) {
		pr_err("error reading adc channel = %d, rc = %d\n",
					chip->vbat_channel, rc);
		return rc;
	}
	pr_debug("mvolts phy = %lld meas = 0x%llx\n", result.physical,
						result.measurement);
	return (int)result.physical;
}

static int voltage_based_capacity(struct pm8921_chg_chip *chip)
{
	int current_voltage_uv = get_prop_battery_uvolts(chip);
	int current_voltage_mv = current_voltage_uv / 1000;
	unsigned int low_voltage = chip->min_voltage_mv;
	unsigned int high_voltage = chip->max_voltage_mv;

	if (current_voltage_mv < 0)
		return -EIO;
	else if (current_voltage_mv <= low_voltage)
		return 0;
	else if (current_voltage_mv >= high_voltage)
		return 100;
	else
		return (current_voltage_mv - low_voltage) * 100
		    / (high_voltage - low_voltage);
}

static int enable_lowbatt_detect;
module_param(enable_lowbatt_detect, int, 0644);
static int get_prop_batt_current(struct pm8921_chg_chip *chip, int *curr);
static int get_prop_batt_capacity(struct pm8921_chg_chip *chip)
{
	int vbat_meas_uv;
	int percent_soc = pm8921_bms_get_percent_charge();
	int rc;
	int ibat;

	if (percent_soc == -ENXIO)
		percent_soc = voltage_based_capacity(chip);

	vbat_meas_uv = get_prop_battery_uvolts(chip);
	pr_debug("percent_soc = %d, vbat_meas_uv = %d\n",
			percent_soc, vbat_meas_uv);
	rc = get_prop_batt_current(chip, &ibat);
	if (vbat_meas_uv < 0 || rc) {
		percent_soc = -EIO;
	} else if (vbat_meas_uv < chip->min_voltage_mv * 1000 &&
		enable_lowbatt_detect && ibat > 0) {
		chip->num_of_under_voltage++;
		pr_err("vbat_meas_uv = %d, %d times, ibat = %d\n",
			vbat_meas_uv, chip->num_of_under_voltage, ibat);
		if (chip->num_of_under_voltage >= RETRY_NUM_FOR_FORCE_SHUTDOWN)
			percent_soc = 0;
	} else {
		chip->num_of_under_voltage = 0;
	}

	if (percent_soc == 0) {
		if (chip->num_of_under_voltage >=
			RETRY_NUM_FOR_FORCE_SHUTDOWN) {
			pr_err("shutdown since battery is 0\n");
			update_charge_state(BIT(DIS_BIT_CHG_SHUTDOWN),
				DIS_BIT_CHG_SHUT_MASK);
		} else {
			percent_soc = 1;
		}
	}

	if (percent_soc <= 10)
		pr_warn("low battery charge = %d%%\n", percent_soc);

	return percent_soc;
}

static int get_prop_batt_current(struct pm8921_chg_chip *chip, int *curr)
{
	int result_ua, rc;

	rc = pm8921_bms_get_battery_current(&result_ua);
	if (rc == -ENXIO)
		rc = pm8xxx_ccadc_get_battery_current(&result_ua);

	if (rc)
		pr_err("unable to get batt current rc = %d\n", rc);
	else
		*curr = result_ua;
	return rc;
}

static int get_prop_batt_init_fcc(struct pm8921_chg_chip *chip)
{
	int rc;

	rc = pm8921_bms_get_init_fcc();
	if (rc < 0)
		pr_err("unable to get batt init fcc rc = %d\n", rc);
	return rc;
}

static int get_prop_batt_fcc(struct pm8921_chg_chip *chip)
{
	int rc;

	rc = pm8921_bms_get_fcc();
	if (rc < 0)
		pr_err("unable to get batt fcc rc = %d\n", rc);
	return rc;
}

static int get_prop_batt_health(struct pm8921_chg_chip *chip)
{
	if (chip->temp_state.condition <= TEMP_COLD)
		return POWER_SUPPLY_HEALTH_COLD;
	else if (chip->temp_state.condition >= TEMP_HOT)
		return POWER_SUPPLY_HEALTH_OVERHEAT;

	return POWER_SUPPLY_HEALTH_GOOD;
}

static int get_prop_batt_present(struct pm8921_chg_chip *chip)
{
	return pm_chg_get_rt_status(chip, BATT_INSERTED_IRQ);
}

static int get_prop_charge_type(struct pm8921_chg_chip *chip)
{
	int temp;

	if (!get_prop_batt_present(chip))
		return POWER_SUPPLY_CHARGE_TYPE_NONE;

	if (is_ext_trickle_charging(chip))
		return POWER_SUPPLY_CHARGE_TYPE_TRICKLE;

	if (is_ext_charging(chip))
		return POWER_SUPPLY_CHARGE_TYPE_FAST;

	temp = pm_chg_get_rt_status(chip, TRKLCHG_IRQ);
	if (temp)
		return POWER_SUPPLY_CHARGE_TYPE_TRICKLE;

	temp = pm_chg_get_rt_status(chip, FASTCHG_IRQ);
	if (temp)
		return POWER_SUPPLY_CHARGE_TYPE_FAST;

	return POWER_SUPPLY_CHARGE_TYPE_NONE;
}

static int get_prop_batt_status(struct pm8921_chg_chip *chip)
{
	int batt_state = POWER_SUPPLY_STATUS_DISCHARGING;
	int fsm_state = pm_chg_get_fsm_state(chip);
	int i;

	if (chip->ext_psy) {
		if (chip->ext_charge_done)
			return POWER_SUPPLY_STATUS_FULL;
		if (chip->ext_charging)
			return POWER_SUPPLY_STATUS_CHARGING;
	}

	for (i = 0; i < ARRAY_SIZE(map); i++)
		if (map[i].fsm_state == fsm_state)
			batt_state = map[i].batt_state;

	if (fsm_state == FSM_STATE_ON_CHG_HIGHI_1) {
		if (!pm_chg_get_rt_status(chip, BAT_TEMP_OK_IRQ)
			|| pm_chg_get_rt_status(chip, CHGHOT_IRQ)
			|| chip->soc < 100)
			batt_state = POWER_SUPPLY_STATUS_NOT_CHARGING;
	}
	return batt_state;
}

#define MAX_TOLERABLE_BATT_TEMP_DDC	680
static int get_prop_batt_temp(struct pm8921_chg_chip *chip, int *temp)
{
	int rc;
	struct pm8xxx_adc_chan_result result;

	rc = pm8xxx_adc_read(chip->batt_temp_channel, &result);
	if (rc) {
		pr_err("error reading adc channel = %d, rc = %d\n",
					chip->vbat_channel, rc);
		return rc;
	}
	pr_debug("batt_temp phy = %lld meas = 0x%llx\n", result.physical,
						result.measurement);
	if (result.physical > MAX_TOLERABLE_BATT_TEMP_DDC)
		pr_err("BATT_TEMP= %d > 68degC, device will be shutdown\n",
							(int) result.physical);

	*temp = (int)result.physical;
	return rc;
}

static int pm_batt_power_get_property(struct power_supply *psy,
				       enum power_supply_property psp,
				       union power_supply_propval *val)
{
	struct pm8921_chg_chip *chip = container_of(psy, struct pm8921_chg_chip,
								batt_psy);
	int value;
	int rc = 0;

	switch (psp) {
	case POWER_SUPPLY_PROP_STATUS:
		val->intval = get_prop_batt_status(chip);
		break;
	case POWER_SUPPLY_PROP_CHARGE_TYPE:
		val->intval = get_prop_charge_type(chip);
		break;
	case POWER_SUPPLY_PROP_HEALTH:
		val->intval = get_prop_batt_health(chip);
		break;
	case POWER_SUPPLY_PROP_PRESENT:
		val->intval = get_prop_batt_present(chip);
		break;
	case POWER_SUPPLY_PROP_TECHNOLOGY:
		val->intval = POWER_SUPPLY_TECHNOLOGY_LION;
		break;
	case POWER_SUPPLY_PROP_VOLTAGE_MAX_DESIGN:
		val->intval = chip->max_voltage_mv * 1000;
		break;
	case POWER_SUPPLY_PROP_VOLTAGE_MIN_DESIGN:
		val->intval = chip->min_voltage_mv * 1000;
		break;
	case POWER_SUPPLY_PROP_VOLTAGE_NOW:
	case POWER_SUPPLY_PROP_VOLTAGE_AVG:
		rc = get_prop_battery_uvolts(chip);
		if (rc >= 0) {
			val->intval = rc;
			rc = 0;
		}
		break;
	case POWER_SUPPLY_PROP_CAPACITY:
		val->intval = chip->soc;
		break;
	case POWER_SUPPLY_PROP_CURRENT_NOW:
	case POWER_SUPPLY_PROP_CURRENT_AVG:
		rc = get_prop_batt_current(chip, &value);
		if (!rc)
			val->intval = value * -1;
		break;
	case POWER_SUPPLY_PROP_CHARGE_FULL_DESIGN:
		rc = get_prop_batt_init_fcc(chip);
		if (rc >= 0) {
			val->intval = rc * 1000;
			rc = 0;
		}
		break;
	case POWER_SUPPLY_PROP_CHARGE_FULL:
		rc = get_prop_batt_fcc(chip);
		if (rc >= 0) {
			val->intval = rc;
			rc = 0;
		}
		break;
	case POWER_SUPPLY_PROP_TEMP:
		rc = get_prop_batt_temp(chip, &value);
		if (!rc)
			val->intval = value;
		break;
	default:
		rc = -EINVAL;
		break;
	}

	return rc;
}

static void (*notify_vbus_state_func_ptr)(int);
static int usb_chg_current;
static DEFINE_SPINLOCK(vbus_lock);
static DEFINE_SPINLOCK(update_lock);

int pm8921_charger_register_vbus_sn(void (*callback)(int))
{
	pr_debug("%p\n", callback);
	notify_vbus_state_func_ptr = callback;
	return 0;
}
EXPORT_SYMBOL_GPL(pm8921_charger_register_vbus_sn);

/* this is passed to the hsusb via platform_data msm_otg_pdata */
void pm8921_charger_unregister_vbus_sn(void (*callback)(int))
{
	pr_debug("%p\n", callback);
	notify_vbus_state_func_ptr = NULL;
}
EXPORT_SYMBOL_GPL(pm8921_charger_unregister_vbus_sn);

static void notify_usb_of_the_plugin_event(int plugin)
{
	plugin = !!plugin;
	if (notify_vbus_state_func_ptr) {
		pr_debug("notifying plugin\n");
		(*notify_vbus_state_func_ptr) (plugin);
	} else {
		pr_debug("unable to notify plugin\n");
	}
}

/* assumes vbus_lock is held */
static void __pm8921_charger_vbus_draw(unsigned int mA)
{
	int i, rc;
	if (!the_chip) {
		pr_err("called before init\n");
		return;
	}

	if (mA > 0 && mA <= 2) {
		usb_chg_current = 0;
		rc = pm_chg_iusbmax_set(the_chip, 0);
		if (rc) {
			pr_err("unable to set iusb to %d rc = %d\n", 0, rc);
		}
		rc = pm_chg_usb_suspend_enable(the_chip, 1);
		if (rc)
			pr_err("fail to set suspend bit rc=%d\n", rc);
	} else {
		if (!the_chip->source_current_disabled) {
			rc = pm_chg_usb_suspend_enable(the_chip, 0);
			if (rc)
				pr_err("fail to reset suspend bit rc=%d\n", rc);
		} else {
			pr_debug("Boosting vbus now, "
						"do not reset suspend bit\n");
		}
		for (i = ARRAY_SIZE(usb_ma_table) - 1; i >= 0; i--) {
			if (usb_ma_table[i].usb_ma <= mA)
				break;
		}

		/* Check if IUSB_FINE_RES is available */
		if ((usb_ma_table[i].value & PM8917_IUSB_FINE_RES)
				&& !the_chip->iusb_fine_res)
			i--;
		if (i < 0)
			i = 0;
		rc = pm_chg_iusbmax_set(the_chip, i);
		if (rc) {
			pr_err("unable to set iusb to %d rc = %d\n", i, rc);
		}
	}
}

/* USB calls these to tell us how much max usb current the system can draw */
void pm8921_charger_vbus_draw_local(enum usb_max_cur_src_id id, unsigned int mA)
{
	unsigned long flags;

	if (id >= SRC_ID_MAX_NUM)
		return;

	pr_debug("Enter charge=%d\n", mA);

	spin_lock_irqsave(&vbus_lock, flags);
	usb_max_cur_ma[id] = mA;
	if (usb_max_cur_ma[SRC_ID_THERMAL] < usb_max_cur_ma[SRC_ID_OTG])
		mA = usb_max_cur_ma[SRC_ID_THERMAL];
	else
		mA = usb_max_cur_ma[SRC_ID_OTG];

	if (usb_max_cur_ma[SRC_ID_OTG] != USB_WALL_THRESHOLD_MA)
		usb_target_ma = mA;

	if (the_chip) {
		if (mA > USB_WALL_THRESHOLD_MA)
			__pm8921_charger_vbus_draw(USB_WALL_THRESHOLD_MA);
		else
			__pm8921_charger_vbus_draw(mA);
	} else {
		/*
		 * called before pmic initialized,
		 * save this value and use it at probe
		 */
		if (mA > USB_WALL_THRESHOLD_MA)
			usb_chg_current = USB_WALL_THRESHOLD_MA;
		else
			usb_chg_current = mA;
	}
	spin_unlock_irqrestore(&vbus_lock, flags);
}

void pm8921_charger_vbus_draw(unsigned int mA)
{
	pm8921_charger_vbus_draw_local(SRC_ID_OTG, mA);
}
EXPORT_SYMBOL_GPL(pm8921_charger_vbus_draw);

int pm8921_is_usb_chg_plugged_in(void)
{
	if (!the_chip) {
		pr_err("called before init\n");
		return -EINVAL;
	}
	return is_usb_chg_plugged_in(the_chip);
}
EXPORT_SYMBOL(pm8921_is_usb_chg_plugged_in);

int pm8921_is_dc_chg_plugged_in(void)
{
	if (!the_chip) {
		pr_err("called before init\n");
		return -EINVAL;
	}
	return is_dc_chg_plugged_in(the_chip);
}
EXPORT_SYMBOL(pm8921_is_dc_chg_plugged_in);

int pm8921_is_battery_present(void)
{
	if (!the_chip) {
		pr_err("called before init\n");
		return -EINVAL;
	}
	return get_prop_batt_present(the_chip);
}
EXPORT_SYMBOL(pm8921_is_battery_present);

/*
 * Disabling the charge current limit causes current
 * current limits to have no monitoring. An adequate charger
 * capable of supplying high current while sustaining VIN_MIN
 * is required if the limiting is disabled.
 */
int pm8921_disable_input_current_limit(bool disable)
{
	if (!the_chip) {
		pr_err("called before init\n");
		return -EINVAL;
	}
	if (disable) {
		pr_warn("Disabling input current limit!\n");

		return pm8xxx_writeb(the_chip->dev->parent,
			 CHG_BUCK_CTRL_TEST3, 0xF2);
	}
	return 0;
}
EXPORT_SYMBOL(pm8921_disable_input_current_limit);

int pm8921_set_max_battery_charge_current(int ma)
{
	if (!the_chip) {
		pr_err("called before init\n");
		return -EINVAL;
	}
	return pm_chg_ibatmax_set(the_chip, ma);
}
EXPORT_SYMBOL(pm8921_set_max_battery_charge_current);

int pm8921_disable_source_current(bool disable)
{
	if (!the_chip) {
		pr_err("called before init\n");
		return -EINVAL;
	}
	if (disable)
		pr_warn("current drawn from chg=0, battery provides current\n");
	the_chip->source_current_disabled = disable;
	pm_chg_usb_suspend_enable(the_chip, disable);
	return update_charge_state(disable << DIS_BIT_USB, DIS_BIT_USB_MASK);
}
EXPORT_SYMBOL(pm8921_disable_source_current);

int pm8921_regulate_input_voltage(int voltage)
{
	int rc;

	if (!the_chip) {
		pr_err("called before init\n");
		return -EINVAL;
	}
	rc = pm_chg_vinmin_set(the_chip, voltage);

	if (rc == 0)
		the_chip->vin_min = voltage;

	return rc;
}

#define USB_OV_THRESHOLD_MASK  0x60
#define USB_OV_THRESHOLD_SHIFT  5
int pm8921_usb_ovp_set_threshold(enum pm8921_usb_ov_threshold ov)
{
	u8 temp;

	if (!the_chip) {
		pr_err("called before init\n");
		return -EINVAL;
	}

	if (ov > PM_USB_OV_7V) {
		pr_err("limiting to over voltage threshold to 7volts\n");
		ov = PM_USB_OV_7V;
	}

	temp = USB_OV_THRESHOLD_MASK & (ov << USB_OV_THRESHOLD_SHIFT);

	return pm_chg_masked_write(the_chip, USB_OVP_CONTROL,
				USB_OV_THRESHOLD_MASK, temp);
}
EXPORT_SYMBOL(pm8921_usb_ovp_set_threshold);

#define USB_DEBOUNCE_TIME_MASK	0x06
#define USB_DEBOUNCE_TIME_SHIFT 1
int pm8921_usb_ovp_set_hystersis(enum pm8921_usb_debounce_time ms)
{
	u8 temp;

	if (!the_chip) {
		pr_err("called before init\n");
		return -EINVAL;
	}

	if (ms > PM_USB_DEBOUNCE_80P5MS) {
		pr_err("limiting debounce to 80.5ms\n");
		ms = PM_USB_DEBOUNCE_80P5MS;
	}

	temp = USB_DEBOUNCE_TIME_MASK & (ms << USB_DEBOUNCE_TIME_SHIFT);

	return pm_chg_masked_write(the_chip, USB_OVP_CONTROL,
				USB_DEBOUNCE_TIME_MASK, temp);
}
EXPORT_SYMBOL(pm8921_usb_ovp_set_hystersis);

#define USB_OVP_DISABLE_MASK	0x80
int pm8921_usb_ovp_disable(int disable)
{
	u8 temp = 0;

	if (!the_chip) {
		pr_err("called before init\n");
		return -EINVAL;
	}

	if (disable)
		temp = USB_OVP_DISABLE_MASK;

	return pm_chg_masked_write(the_chip, USB_OVP_CONTROL,
				USB_OVP_DISABLE_MASK, temp);
}

bool pm8921_is_battery_charging(int *source)
{
	int fsm_state, is_charging, dc_present, usb_present;

	if (!the_chip) {
		pr_err("called before init\n");
		return -EINVAL;
	}
	fsm_state = pm_chg_get_fsm_state(the_chip);
	is_charging = is_battery_charging(fsm_state);
	if (is_charging == 0) {
		*source = PM8921_CHG_SRC_NONE;
		return is_charging;
	}

	if (source == NULL)
		return is_charging;

	/* the battery is charging, the source is requested, find it */
	dc_present = is_dc_chg_plugged_in(the_chip);
	usb_present = is_usb_chg_plugged_in(the_chip);

	if (dc_present && !usb_present)
		*source = PM8921_CHG_SRC_DC;

	if (usb_present && !dc_present)
		*source = PM8921_CHG_SRC_USB;

	if (usb_present && dc_present)
		/*
		 * The system always chooses dc for charging since it has
		 * higher priority.
		 */
		*source = PM8921_CHG_SRC_DC;

	return is_charging;
}
EXPORT_SYMBOL(pm8921_is_battery_charging);

int pm8921_set_usb_power_supply_type(enum power_supply_type type)
{
	if (!the_chip) {
		pr_err("called before init\n");
		return -EINVAL;
	}

	if (type < POWER_SUPPLY_TYPE_USB && type > POWER_SUPPLY_TYPE_BATTERY)
		return -EINVAL;

	the_chip->usb_type = type;
	power_supply_changed(&the_chip->usb_psy);
	power_supply_changed(&the_chip->dc_psy);
	return 0;
}
EXPORT_SYMBOL_GPL(pm8921_set_usb_power_supply_type);

static void clear_long_time_chg_failed(struct pm8921_chg_chip *chip)
{
	int rc;

	rc = pm_chg_masked_write(chip, CHG_CNTRL_3,
			CHG_FAILED_CLEAR, CHG_FAILED_CLEAR);
	if (rc)
		pr_err("Failed to set CHG_FAILED_CLEAR rc=%d\n", rc);
}

static ulong limiting_msec = 500;
module_param(limiting_msec, ulong, 0644);

static bool check_if_update_necessary(struct pm8921_chg_chip *chip)
{
	bool rc;
	unsigned long flags;

	spin_lock_irqsave(&update_lock, flags);
	rc = !!schedule_delayed_work(&chip->delayed_notify_work,
					msecs_to_jiffies(limiting_msec));
	pr_debug("%s delayed_notify_worker (rc = %d)\n",
		rc ? "Start" : "Not start", rc);
	spin_unlock_irqrestore(&update_lock, flags);
	return rc;
}

static void handle_usb_insertion_removal(struct pm8921_chg_chip *chip)
{
	int usb_present;

	pm_chg_failed_clear(chip, 1);
	usb_present = is_usb_chg_plugged_in(chip);

	if (chip->usb_present ^ usb_present) {
		if (usb_present)
			chip->usb_type = POWER_SUPPLY_TYPE_BATTERY;
		notify_usb_of_the_plugin_event(usb_present);
		chip->usb_present = usb_present;
		if (check_if_update_necessary(chip)) {
			power_supply_changed(&chip->usb_psy);
			power_supply_changed(&chip->batt_psy);
		}
		pm8921_bms_calibrate_hkadc();
	}
	if (usb_present) {
		schedule_delayed_work(&chip->unplug_check_work,
			round_jiffies_relative(msecs_to_jiffies
				(UNPLUG_CHECK_WAIT_PERIOD_MS)));
		pm8921_chg_enable_irq(chip, CHG_GONE_IRQ);
	} else {
		clear_long_time_chg_failed(chip);

		/* USB unplugged reset target current */
		usb_target_ma = 0;
		pm8921_chg_disable_irq(chip, CHG_GONE_IRQ);
	}
	enable_input_voltage_regulation(chip);
	bms_notify_check(chip);
}

static void handle_stop_ext_chg(struct pm8921_chg_chip *chip)
{
	if (!chip->ext_psy) {
		pr_debug("external charger not registered.\n");
		return;
	}

	if (!chip->ext_charging) {
		pr_debug("already not charging.\n");
		return;
	}

	power_supply_set_charge_type(chip->ext_psy,
					POWER_SUPPLY_CHARGE_TYPE_NONE);
	pm8921_disable_source_current(false); /* release BATFET */
	power_supply_changed(&chip->dc_psy);
	chip->ext_charging = false;
	chip->ext_charge_done = false;
	bms_notify_check(chip);
	/* Update battery charging LEDs and user space battery info */
	power_supply_changed(&chip->batt_psy);
}

static void handle_start_ext_chg(struct pm8921_chg_chip *chip)
{
	int dc_present;
	int batt_present;
	int batt_temp_ok;
	int vbat_ov;
	unsigned long delay =
		round_jiffies_relative(msecs_to_jiffies(EOC_CHECK_PERIOD_MS));

	if (!chip->ext_psy) {
		pr_debug("external charger not registered.\n");
		return;
	}

	if (chip->ext_charging) {
		pr_debug("already charging.\n");
		return;
	}

	dc_present = is_dc_chg_plugged_in(the_chip);
	batt_present = pm_chg_get_rt_status(chip, BATT_INSERTED_IRQ);
	batt_temp_ok = pm_chg_get_rt_status(chip, BAT_TEMP_OK_IRQ);

	if (!dc_present) {
		pr_warn("%s. dc not present.\n", __func__);
		return;
	}
	if (!batt_present) {
		pr_warn("%s. battery not present.\n", __func__);
		return;
	}
	if (!batt_temp_ok) {
		pr_warn("%s. battery temperature not ok.\n", __func__);
		return;
	}
	pm8921_disable_source_current(true); /* Force BATFET=ON */
	vbat_ov = pm_chg_get_rt_status(chip, VBAT_OV_IRQ);
	if (vbat_ov) {
		pr_warn("%s. battery over voltage.\n", __func__);
		return;
	}

	schedule_delayed_work(&chip->unplug_check_work,
	round_jiffies_relative(msecs_to_jiffies
		(UNPLUG_CHECK_WAIT_PERIOD_MS)));
	pm8921_chg_enable_irq(chip, CHG_GONE_IRQ);

	power_supply_set_online(chip->ext_psy, dc_present);
	power_supply_set_charge_type(chip->ext_psy,
					POWER_SUPPLY_CHARGE_TYPE_FAST);
	power_supply_changed(&chip->dc_psy);
	chip->ext_charging = true;
	chip->ext_charge_done = false;
	bms_notify_check(chip);
	/* Start BMS */
	schedule_delayed_work(&chip->eoc_work, delay);
	wake_lock(&chip->eoc_wake_lock);
	/* Update battery charging LEDs and user space battery info */
	power_supply_changed(&chip->batt_psy);
}

static void turn_off_ovp_fet(struct pm8921_chg_chip *chip, u16 ovptestreg)
{
	u8 temp;
	int rc;

	rc = pm8xxx_writeb(chip->dev->parent, ovptestreg, 0x30);
	if (rc) {
		pr_err("Failed to write 0x30 to OVP_TEST rc = %d\n", rc);
		return;
	}
	rc = pm8xxx_readb(chip->dev->parent, ovptestreg, &temp);
	if (rc) {
		pr_err("Failed to read from OVP_TEST rc = %d\n", rc);
		return;
	}
	/* set ovp fet disable bit and the write bit */
	temp |= 0x81;
	rc = pm8xxx_writeb(chip->dev->parent, ovptestreg, temp);
	if (rc) {
		pr_err("Failed to write 0x%x OVP_TEST rc=%d\n", temp, rc);
		return;
	}
}

static void turn_on_ovp_fet(struct pm8921_chg_chip *chip, u16 ovptestreg)
{
	u8 temp;
	int rc;

	rc = pm8xxx_writeb(chip->dev->parent, ovptestreg, 0x30);
	if (rc) {
		pr_err("Failed to write 0x30 to OVP_TEST rc = %d\n", rc);
		return;
	}
	rc = pm8xxx_readb(chip->dev->parent, ovptestreg, &temp);
	if (rc) {
		pr_err("Failed to read from OVP_TEST rc = %d\n", rc);
		return;
	}
	/* unset ovp fet disable bit and set the write bit */
	temp &= 0xFE;
	temp |= 0x80;
	rc = pm8xxx_writeb(chip->dev->parent, ovptestreg, temp);
	if (rc) {
		pr_err("Failed to write 0x%x to OVP_TEST rc = %d\n",
								temp, rc);
		return;
	}
}

static int param_open_ovp_counter = 10;
module_param(param_open_ovp_counter, int, 0644);

#define USB_ACTIVE_BIT BIT(5)
#define DC_ACTIVE_BIT BIT(6)
static int is_active_chg_plugged_in(struct pm8921_chg_chip *chip,
						u8 active_chg_mask)
{
	if (active_chg_mask & USB_ACTIVE_BIT)
		return pm_chg_get_rt_status(chip, USBIN_VALID_IRQ);
	else if (active_chg_mask & DC_ACTIVE_BIT)
		return pm_chg_get_rt_status(chip, DCIN_VALID_IRQ);
	else
		return 0;
}

#define WRITE_BANK_4		0xC0
#define OVP_DEBOUNCE_TIME 0x06
static void unplug_ovp_fet_open(struct pm8921_chg_chip *chip)
{
	int chg_gone, active_chg_plugged_in;
	int count = 0;
	u8 active_mask = 0;
	u16 ovpreg, ovptestreg;

	if (is_usb_chg_plugged_in(chip) &&
		(chip->active_path & USB_ACTIVE_BIT)) {
		ovpreg = USB_OVP_CONTROL;
		ovptestreg = USB_OVP_TEST;
		active_mask = USB_ACTIVE_BIT;
	} else if (is_dc_chg_plugged_in(chip) &&
		(chip->active_path & DC_ACTIVE_BIT)) {
		ovpreg = DC_OVP_CONTROL;
		ovptestreg = DC_OVP_TEST;
		active_mask = DC_ACTIVE_BIT;
	} else {
		return;
	}

	while (count++ < param_open_ovp_counter) {
		pm_chg_masked_write(chip, ovpreg, OVP_DEBOUNCE_TIME, 0x0);
		usleep(10);
		active_chg_plugged_in
			= is_active_chg_plugged_in(chip, active_mask);
		chg_gone = pm_chg_get_rt_status(chip, CHG_GONE_IRQ);
		pr_debug("OVP FET count = %d chg_gone=%d, active_valid = %d\n",
					count, chg_gone, active_chg_plugged_in);

		/* note usb_chg_plugged_in=0 => chg_gone=1 */
		if (chg_gone == 1 && active_chg_plugged_in == 1) {
			pr_debug("since chg_gone = 1 dis ovp_fet for 20msec\n");
			turn_off_ovp_fet(chip, ovptestreg);

			msleep(20);

			turn_on_ovp_fet(chip, ovptestreg);
		} else {
			break;
		}
	}
	pm_chg_masked_write(chip, ovpreg, OVP_DEBOUNCE_TIME, 0x2);
	pr_debug("Exit count=%d chg_gone=%d, active_valid=%d\n",
		count, chg_gone, active_chg_plugged_in);
	return;
}

static int find_usb_ma_value(int value)
{
	int i;

	for (i = ARRAY_SIZE(usb_ma_table) - 1; i >= 0; i--) {
		if (value >= usb_ma_table[i].usb_ma)
			break;
	}

	return i;
}

static void decrease_usb_ma_value(int *value)
{
	int i;

	if (value) {
		i = find_usb_ma_value(*value);
		if (i > 0)
			i--;
		while (!the_chip->iusb_fine_res && i > 0
			&& (usb_ma_table[i].value & PM8917_IUSB_FINE_RES))
			i--;
		*value = usb_ma_table[i].usb_ma;
	}
}

static void increase_usb_ma_value(int *value)
{
	int i;

	if (value) {
		i = find_usb_ma_value(*value);

		if (i < (ARRAY_SIZE(usb_ma_table) - 1))
			i++;
		/* Get next correct entry if IUSB_FINE_RES is not available */
		while (!the_chip->iusb_fine_res
			&& (usb_ma_table[i].value & PM8917_IUSB_FINE_RES)
			&& i < (ARRAY_SIZE(usb_ma_table) - 1))
			i++;

		*value = usb_ma_table[i].usb_ma;
	}
}

static void vin_collapse_check_worker(struct work_struct *work)
{
	struct delayed_work *dwork = to_delayed_work(work);
	struct pm8921_chg_chip *chip = container_of(dwork,
			struct pm8921_chg_chip, vin_collapse_check_work);

	/* AICL only for wall-chargers */
	if (is_usb_chg_plugged_in(chip) &&
		usb_target_ma > USB_WALL_THRESHOLD_MA) {
		/* decrease usb_target_ma */
		decrease_usb_ma_value(&usb_target_ma);
		/* reset here, increase in unplug_check_worker */
		__pm8921_charger_vbus_draw(USB_WALL_THRESHOLD_MA);
		pr_debug("usb_now=%d, usb_target = %d\n",
				USB_WALL_THRESHOLD_MA, usb_target_ma);
	} else {
		handle_usb_insertion_removal(chip);
	}
}

#define VIN_MIN_COLLAPSE_CHECK_MS	50
static irqreturn_t usbin_valid_irq_handler(int irq, void *data)
{
	if (usb_target_ma)
		schedule_delayed_work(&the_chip->vin_collapse_check_work,
				      round_jiffies_relative(msecs_to_jiffies
						(VIN_MIN_COLLAPSE_CHECK_MS)));
	else
	    handle_usb_insertion_removal(data);
	return IRQ_HANDLED;
}

static irqreturn_t usbin_ov_irq_handler(int irq, void *data)
{
	pr_err("USB OverVoltage\n");
	return IRQ_HANDLED;
}

static irqreturn_t batt_inserted_irq_handler(int irq, void *data)
{
	struct pm8921_chg_chip *chip = data;
	int status;

	status = pm_chg_get_rt_status(chip, BATT_INSERTED_IRQ);
	schedule_work(&chip->battery_id_valid_work);
	handle_start_ext_chg(chip);
	pr_debug("battery present=%d", status);
	power_supply_changed(&chip->batt_psy);
	return IRQ_HANDLED;
}

/*
 * this interrupt used to restart charging a battery.
 *
 * Note: When DC-inserted the VBAT can't go low.
 * VPH_PWR is provided by the ext-charger.
 * After End-Of-Charging from DC, charging can be resumed only
 * if DC is removed and then inserted after the battery was in use.
 * Therefore the handle_start_ext_chg() is not called.
 */
static irqreturn_t vbatdet_low_irq_handler(int irq, void *data)
{
	struct pm8921_chg_chip *chip = data;

	power_supply_changed(&chip->batt_psy);
	power_supply_changed(&chip->usb_psy);
	power_supply_changed(&chip->dc_psy);

	return IRQ_HANDLED;
}

static irqreturn_t usbin_uv_irq_handler(int irq, void *data)
{
	pr_err("USB UnderVoltage\n");
	return IRQ_HANDLED;
}

static irqreturn_t vbat_ov_irq_handler(int irq, void *data)
{
	pr_debug("fsm_state=%d\n", pm_chg_get_fsm_state(data));
	return IRQ_HANDLED;
}

static irqreturn_t chgwdog_irq_handler(int irq, void *data)
{
	pr_debug("fsm_state=%d\n", pm_chg_get_fsm_state(data));
	return IRQ_HANDLED;
}

static irqreturn_t vcp_irq_handler(int irq, void *data)
{
	pr_debug("fsm_state=%d\n", pm_chg_get_fsm_state(data));
	return IRQ_HANDLED;
}

static irqreturn_t atcdone_irq_handler(int irq, void *data)
{
	pr_debug("fsm_state=%d\n", pm_chg_get_fsm_state(data));
	return IRQ_HANDLED;
}

static irqreturn_t atcfail_irq_handler(int irq, void *data)
{
	pr_debug("fsm_state=%d\n", pm_chg_get_fsm_state(data));
	return IRQ_HANDLED;
}

static irqreturn_t chgdone_irq_handler(int irq, void *data)
{
	struct pm8921_chg_chip *chip = data;

	pr_debug("state_changed_to=%d\n", pm_chg_get_fsm_state(data));

	handle_stop_ext_chg(chip);

	power_supply_changed(&chip->batt_psy);
	power_supply_changed(&chip->usb_psy);
	power_supply_changed(&chip->dc_psy);

	bms_notify_check(chip);

	return IRQ_HANDLED;
}

static irqreturn_t chgfail_irq_handler(int irq, void *data)
{
	struct pm8921_chg_chip *chip = data;
	int ret;

	ret = pm_chg_failed_clear(chip, 1);
	if (ret)
		pr_err("Failed to write CHG_FAILED_CLEAR bit\n");

	pr_err("batt_present = %d, batt_temp_ok = %d, state_changed_to=%d\n",
			get_prop_batt_present(chip),
			pm_chg_get_rt_status(chip, BAT_TEMP_OK_IRQ),
			pm_chg_get_fsm_state(data));

	power_supply_changed(&chip->batt_psy);
	power_supply_changed(&chip->usb_psy);
	power_supply_changed(&chip->dc_psy);
	return IRQ_HANDLED;
}

static irqreturn_t chgstate_irq_handler(int irq, void *data)
{
	struct pm8921_chg_chip *chip = data;

	pr_debug("state_changed_to=%d\n", pm_chg_get_fsm_state(data));
	power_supply_changed(&chip->batt_psy);
	power_supply_changed(&chip->usb_psy);
	power_supply_changed(&chip->dc_psy);

	bms_notify_check(chip);

	return IRQ_HANDLED;
}

static int param_vin_disable_counter = 1;
module_param(param_vin_disable_counter, int, 0644);

static void attempt_reverse_boost_fix(struct pm8921_chg_chip *chip,
							int count, int usb_ma)
{
	if (usb_ma)
		__pm8921_charger_vbus_draw(500);
	pr_debug("count = %d iusb=500mA\n", count);
	update_charge_state(BIT(DIS_BIT_CHG_REVERSE), DIS_BIT_CHG_REVERSE_MASK);
	pr_debug("count = %d enable CHG_DIS bit\n", count);

	msleep(20);

	pr_debug("count = %d end sleep 20ms chg_gone=%d, usb_valid = %d\n",
				count,
				pm_chg_get_rt_status(chip, CHG_GONE_IRQ),
				is_usb_chg_plugged_in(chip));
	pr_debug("count = %d restoring input regulation and usb_ma = %d\n",
		 count, usb_ma);
	update_charge_state(false, DIS_BIT_CHG_REVERSE_MASK);
	if (usb_ma)
		__pm8921_charger_vbus_draw(usb_ma);
}

static void calculate_iavg_ua(struct pm8921_chg_chip *chip, int cc_uah,
					int *iavg_ua, int *delta_time_us)
{
	int delta_cc_uah;
	struct timespec now;

	delta_cc_uah = cc_uah - chip->last_cc_uah;
	/* since a wake lock is held while charging it is safe to
	 * use clock_monotonic to obtain the time difference */
	do_posix_clock_monotonic_gettime(&now);

	if (chip->t.tv_sec != 0) {
		*delta_time_us = (now.tv_sec - chip->t.tv_sec) * USEC_PER_SEC
			+ (now.tv_nsec - chip->t.tv_nsec) / 1000;
	} else {
		*delta_time_us = 0;
	}

	if (*delta_time_us != 0)
		*iavg_ua = div_s64((s64) delta_cc_uah * 3600 * 1000000,
				*delta_time_us);
	else
		*iavg_ua = 0;

	pr_debug("t.tv_sec = %d, now.tv_sec = %d delta_us = %d iavg_ua = %d\n",
		(int)chip->t.tv_sec, (int)now.tv_sec,
			*delta_time_us, (int)*iavg_ua);
	chip->t = now;
	chip->last_cc_uah = cc_uah;
}

#define IBAT_ACTIVE_MASK 0x64
#define IBAT_MEAS_DELTA_UA 100000
static int ibat_calib_offset_ua = 30000;
module_param(ibat_calib_offset_ua, int, 0644);
static void pm_chg_ibat_calibrate(struct pm8921_chg_chip *chip)
{
	u8 reg_loop, fast_chg;
	int rc = 0, cc_ua = 0, ibat_max_ua = 0, delta_time_us = 0;
	int iavg_ua = 0, ibat_inst_ua = 0;

	fast_chg = pm_chg_get_rt_status(chip, FASTCHG_IRQ);
	if (!fast_chg) {
		pr_err("not fast charging\n");
		return;
	}

	reg_loop = pm_chg_get_regulation_loop(chip);
	if (reg_loop & IBAT_ACTIVE_MASK) {
		rc = pm8921_bms_cc_uah(&cc_ua);
		if (rc) {
			pr_err("Failed to get cc current rc=%d\n", rc);
			return;
		}

		reg_loop = pm_chg_get_regulation_loop(chip);
		if (!(reg_loop & IBAT_ACTIVE_MASK)) {
			pr_debug("IBAT not active, returning\n");
			return;
		}

		calculate_iavg_ua(chip, cc_ua, &iavg_ua, &delta_time_us);
		if (iavg_ua == 0) {
			pr_debug("iavg_ua is zero skipping...\n");
			return;
		} else {
			iavg_ua *= -1;
		}

		rc = get_prop_batt_current(chip, &ibat_inst_ua);
		if(rc) {
			pr_err("couldnt read batt current rc = %d\n", rc);
			return;
		}
		ibat_inst_ua  *= -1;
		if (abs(ibat_inst_ua) - abs(iavg_ua) > IBAT_MEAS_DELTA_UA) {
			pr_debug("too much discrepancy ibat_ua %d iavg_ua %d\n",
					ibat_inst_ua, iavg_ua);
			chip->t.tv_sec = 0;
			chip->last_cc_uah = 0;
			return;
		}
		rc = pm_chg_ibatmax_get(chip, &ibat_max_ua);
		if (rc) {
			pr_err("Failed to get ibatmax rc=%d\n", rc);
			return;
		}
		/* convert to uA */
		ibat_max_ua *= 1000;

		if (iavg_ua < ((ibat_target_ma * 1000) - ibat_calib_offset_ua))
			ibat_max_ua += 50000;
		else if (iavg_ua >= ((ibat_target_ma * 1000)
					+ ibat_calib_offset_ua))
			ibat_max_ua -= 50000;

		pr_debug("target %d iavg %d imax: %d max_cur %d max_adj %d\n",
				ibat_target_ma, iavg_ua, ibat_max_ua,
				chip->max_bat_chg_current,
				chip->ibatmax_max_adj_ma);
		if ((ibat_max_ua) <=
			((chip->max_bat_chg_current +
					chip->ibatmax_max_adj_ma) * 1000)
			&& ((ibat_max_ua) >=
			((chip->max_bat_chg_current -
					chip->ibatmax_max_adj_ma)*1000))) {
			rc = pm_chg_ibatmax_set(chip, ibat_max_ua / 1000);
			if (rc) {
				pr_err("Failed to set ibatmax rc=%d\n", rc);
				return;
			}
		} else {
			pr_debug("ibatmax is within spec - not adjusting\n");
		}
	} else {
		chip->t.tv_sec = 0;
		chip->last_cc_uah = 0;
	}
	return;
}

#define VIN_ACTIVE_BIT BIT(0)
#define UNPLUG_WRKARND_RESTORE_WAIT_PERIOD_US	200
#define VIN_MIN_INCREASE_MV	100
static void unplug_check_worker(struct work_struct *work)
{
	struct delayed_work *dwork = to_delayed_work(work);
	struct pm8921_chg_chip *chip = container_of(dwork,
				struct pm8921_chg_chip, unplug_check_work);
	u8 reg_loop, active_path;
	int rc, ibat, active_chg_plugged_in, usb_ma = 0;
	int chg_gone = 0;

	reg_loop = 0;
	rc = pm8xxx_readb(chip->dev->parent, PBL_ACCESS1, &active_path);
	if (rc) {
		pr_err("Failed to read PBL_ACCESS1 rc=%d\n", rc);
		return;
	}
	chip->active_path = active_path;

	active_chg_plugged_in = is_active_chg_plugged_in(chip, active_path);
	pr_debug("active_path = 0x%x, active_chg_plugged_in = %d\n",
			active_path, active_chg_plugged_in);
	if (active_path & USB_ACTIVE_BIT) {
		pr_debug("USB charger active\n");

		pm_chg_iusbmax_get(chip, &usb_ma);
		if (usb_ma == 500 && !usb_target_ma) {
			pr_debug("Stopping Unplug Check Worker USB == 500mA\n");
			disable_input_voltage_regulation(chip);
			return;
		}

		if (usb_ma <= 100) {
			pr_debug(
				"Unenumerated or suspended usb_ma = %d skip\n",
				usb_ma);
			goto check_again_later;
		}

		if (pm_is_chg_charge_dis(chip)) {
			pr_debug("Charging disabled usb_ma=%d, usb_target = %d "
				"skipping\n", usb_ma, usb_target_ma);
			goto check_again_later;
		}
	} else if (active_path & DC_ACTIVE_BIT) {
		pr_debug("DC charger active\n");
		/* Some board designs are not prone to reverse boost on DC
		 * charging path */
		if (!chip->dc_unplug_check)
			return;
	} else {
		/* No charger active */
		if (!(is_usb_chg_plugged_in(chip)
				&& !(is_dc_chg_plugged_in(chip)))) {
			pr_debug("Stopping Unplug Check - chargers are removed"
				"reg_loop = %d, fsm = %d\n",
				pm_chg_get_regulation_loop(chip),
				pm_chg_get_fsm_state(chip)
				);
			return;
		} else {
			goto check_again_later;
		}
	}

	if (active_path & USB_ACTIVE_BIT) {
		reg_loop = pm_chg_get_regulation_loop(chip);
		pr_debug("reg_loop=0x%x usb_ma = %d\n", reg_loop, usb_ma);
		if ((reg_loop & VIN_ACTIVE_BIT) &&
			(usb_ma > USB_WALL_THRESHOLD_MA)) {
			decrease_usb_ma_value(&usb_ma);
			usb_target_ma = usb_ma;
			/* end AICL here */
			__pm8921_charger_vbus_draw(usb_ma);
			pr_debug("usb_now=%d, usb_target = %d\n",
				usb_ma, usb_target_ma);
		}
	}

	reg_loop = pm_chg_get_regulation_loop(chip);
	pr_debug("reg_loop=0x%x usb_ma = %d\n", reg_loop, usb_ma);

	rc = get_prop_batt_current(chip, &ibat);
	if (reg_loop & VIN_ACTIVE_BIT) {
		pr_debug("ibat = %d fsm = %d reg_loop = 0x%x\n",
				ibat, pm_chg_get_fsm_state(chip), reg_loop);
		if (!rc && ibat > 0) {
			int count = 0;

			while (count++ < param_vin_disable_counter
					&& active_chg_plugged_in == 1) {
				if (active_path & USB_ACTIVE_BIT)
					attempt_reverse_boost_fix(chip,
								count, usb_ma);
				else
					attempt_reverse_boost_fix(chip,
								count, 0);
				/* after reverse boost fix check if the active
				 * charger was detected as removed */
				active_chg_plugged_in
					= is_active_chg_plugged_in(chip,
						active_path);
				pr_debug("active_chg_plugged_in = %d\n",
						active_chg_plugged_in);
			}
		}
	}

	active_chg_plugged_in = is_active_chg_plugged_in(chip, active_path);
	pr_debug("active_path = 0x%x, active_chg = %d\n",
			active_path, active_chg_plugged_in);
	chg_gone = pm_chg_get_rt_status(chip, CHG_GONE_IRQ);

	/* battery needs to be supplying current for this check to be valid
	 * in the case of a DC charger that is not connected via the SMBC
	 * buck */
	if (chg_gone == 1  && active_chg_plugged_in == 1) {
		pr_debug("chg_gone=%d, active_chg_plugged_in = %d\n",
					chg_gone, active_chg_plugged_in);
		unplug_ovp_fet_open(chip);
	}

	if (!(reg_loop & VIN_ACTIVE_BIT) && (active_path & USB_ACTIVE_BIT)) {
		/* only increase iusb_max if vin loop not active */
		if (usb_ma < usb_target_ma) {
			increase_usb_ma_value(&usb_ma);
			__pm8921_charger_vbus_draw(usb_ma);
			pr_debug("usb_now=%d, usb_target = %d\n",
					usb_ma, usb_target_ma);
		} else {
			usb_target_ma = usb_ma;
		}
	}

	if (chip->ibat_calib_enable)
		pm_chg_ibat_calibrate(chip);

check_again_later:
	/* schedule to check again later */
	schedule_delayed_work(&chip->unplug_check_work,
		      round_jiffies_relative(msecs_to_jiffies
				(UNPLUG_CHECK_WAIT_PERIOD_MS)));
}

static irqreturn_t loop_change_irq_handler(int irq, void *data)
{
	struct pm8921_chg_chip *chip = data;

	pr_debug("fsm_state=%d reg_loop=0x%x\n",
		pm_chg_get_fsm_state(data),
		pm_chg_get_regulation_loop(data));
	schedule_work(&chip->unplug_check_work.work);
	return IRQ_HANDLED;
}

static int find_ibat_max_adj_ma(int ibat_target_ma)
{
	int i = 0;

	for (i = ARRAY_SIZE(ibatmax_adj_table) - 1; i >= 0; i--) {
		if (ibat_target_ma <= ibatmax_adj_table[i].ibat_max_ma)
			break;
	}

	return ibatmax_adj_table[i].max_adj_ma;
}


static irqreturn_t fastchg_irq_handler(int irq, void *data)
{
	struct pm8921_chg_chip *chip = data;
	int high_transition = 0, ibat_max_ma = 0;

	pm_chg_ibatmax_get(chip, &ibat_max_ma);
	pr_info("ibatmax=%d\n", ibat_max_ma);
	high_transition = pm_chg_get_rt_status(chip, FASTCHG_IRQ);

	if (high_transition && !delayed_work_pending(&chip->eoc_work)) {
		wake_lock(&chip->eoc_wake_lock);
		schedule_delayed_work(&chip->eoc_work,
				      round_jiffies_relative(msecs_to_jiffies
						     (EOC_CHECK_PERIOD_MS)));
	} else {
		ibat_target_ma = ibat_max_ma;
		chip->ibatmax_max_adj_ma = find_ibat_max_adj_ma(ibat_target_ma);

		pr_debug("setting max_adj=%d ibat_target=%d\n",
				chip->ibatmax_max_adj_ma, ibat_target_ma);
	}
	power_supply_changed(&chip->batt_psy);
	bms_notify_check(chip);
	return IRQ_HANDLED;
}

static irqreturn_t trklchg_irq_handler(int irq, void *data)
{
	struct pm8921_chg_chip *chip = data;

	power_supply_changed(&chip->batt_psy);
	return IRQ_HANDLED;
}

static irqreturn_t batt_removed_irq_handler(int irq, void *data)
{
	struct pm8921_chg_chip *chip = data;
	int status;

	status = pm_chg_get_rt_status(chip, BATT_REMOVED_IRQ);
	pr_debug("battery present=%d state=%d", !status,
					 pm_chg_get_fsm_state(data));
	handle_stop_ext_chg(chip);
	power_supply_changed(&chip->batt_psy);
	return IRQ_HANDLED;
}

static irqreturn_t batttemp_hot_irq_handler(int irq, void *data)
{
	struct pm8921_chg_chip *chip = data;

	handle_stop_ext_chg(chip);
	power_supply_changed(&chip->batt_psy);
	return IRQ_HANDLED;
}

static irqreturn_t chghot_irq_handler(int irq, void *data)
{
	struct pm8921_chg_chip *chip = data;

	pr_debug("Chg hot fsm_state=%d\n", pm_chg_get_fsm_state(data));
	power_supply_changed(&chip->batt_psy);
	power_supply_changed(&chip->usb_psy);
	handle_stop_ext_chg(chip);
	return IRQ_HANDLED;
}

static irqreturn_t batttemp_cold_irq_handler(int irq, void *data)
{
	struct pm8921_chg_chip *chip = data;

	pr_debug("Batt cold fsm_state=%d\n", pm_chg_get_fsm_state(data));
	handle_stop_ext_chg(chip);

	power_supply_changed(&chip->batt_psy);
	power_supply_changed(&chip->usb_psy);
	return IRQ_HANDLED;
}

static irqreturn_t chg_gone_irq_handler(int irq, void *data)
{
	struct pm8921_chg_chip *chip = data;
	int chg_gone, usb_chg_plugged_in;

	usb_chg_plugged_in = is_usb_chg_plugged_in(chip);
	chg_gone = pm_chg_get_rt_status(chip, CHG_GONE_IRQ);

	pr_debug("chg_gone=%d, usb_valid = %d\n", chg_gone, usb_chg_plugged_in);
	pr_debug("Chg gone fsm_state=%d\n", pm_chg_get_fsm_state(data));

	if (check_if_update_necessary(chip)) {
		power_supply_changed(&chip->batt_psy);
		power_supply_changed(&chip->usb_psy);
	}
	return IRQ_HANDLED;
}
/*
 *
 * bat_temp_ok_irq_handler - is edge triggered, hence it will
 * fire for two cases:
 *
 * If the interrupt line switches to high temperature is okay
 * and thus charging begins.
 * If bat_temp_ok is low this means the temperature is now
 * too hot or cold, so charging is stopped.
 *
 */
static irqreturn_t bat_temp_ok_irq_handler(int irq, void *data)
{
	int bat_temp_ok;
	struct pm8921_chg_chip *chip = data;

	bat_temp_ok = pm_chg_get_rt_status(chip, BAT_TEMP_OK_IRQ);

	pr_debug("batt_temp_ok = %d fsm_state%d\n",
			 bat_temp_ok, pm_chg_get_fsm_state(data));

	if (bat_temp_ok)
		handle_start_ext_chg(chip);
	else
		handle_stop_ext_chg(chip);

	power_supply_changed(&chip->batt_psy);
	power_supply_changed(&chip->usb_psy);
	bms_notify_check(chip);
	return IRQ_HANDLED;
}

static irqreturn_t coarse_det_low_irq_handler(int irq, void *data)
{
	pr_debug("fsm_state=%d\n", pm_chg_get_fsm_state(data));
	return IRQ_HANDLED;
}

static irqreturn_t vdd_loop_irq_handler(int irq, void *data)
{
	pr_debug("fsm_state=%d\n", pm_chg_get_fsm_state(data));
	return IRQ_HANDLED;
}

static irqreturn_t vreg_ov_irq_handler(int irq, void *data)
{
	pr_debug("fsm_state=%d\n", pm_chg_get_fsm_state(data));
	return IRQ_HANDLED;
}

static irqreturn_t vbatdet_irq_handler(int irq, void *data)
{
	pr_debug("fsm_state=%d\n", pm_chg_get_fsm_state(data));
	return IRQ_HANDLED;
}

static irqreturn_t batfet_irq_handler(int irq, void *data)
{
	struct pm8921_chg_chip *chip = data;

	pr_debug("vreg ov\n");
	power_supply_changed(&chip->batt_psy);
	return IRQ_HANDLED;
}

static void create_dock_event(struct pm8921_chg_chip *chip,
		enum dock_state_type type)
{
#ifdef CONFIG_SWITCH
	unsigned long flags;

	if (chip->dock_type != type) {
		spin_lock_irqsave(&chip->dock_lock, flags);
		switch_set_state(&chip->swdev, type);
		chip->dock_type = type;
		wake_lock_timeout(&chip->dock_wake_lock, HZ / 2);
		pr_debug("dock_type = %d\n", chip->dock_type);
		spin_unlock_irqrestore(&chip->dock_lock, flags);
	}
#endif /* CONFIG_SWITCH */
}

static irqreturn_t dcin_valid_irq_handler(int irq, void *data)
{
	struct pm8921_chg_chip *chip = data;
	int dc_present;

	dc_present = pm_chg_get_rt_status(chip, DCIN_VALID_IRQ);
	if (chip->ext_psy)
		power_supply_set_online(chip->ext_psy, dc_present);
	else
		schedule_delayed_work(&chip->unplug_check_work,
				round_jiffies_relative(msecs_to_jiffies
				(UNPLUG_CHECK_WAIT_PERIOD_MS)));
	chip->dc_present = dc_present;
	if (dc_present) {
		handle_start_ext_chg(chip);
		create_dock_event(chip, CHG_DOCK_DESK);
	} else {
		handle_stop_ext_chg(chip);
		clear_long_time_chg_failed(chip);
		create_dock_event(chip, CHG_DOCK_UNDOCK);
	}
	if (!chip->ext_psy) {
		power_supply_changed(&chip->dc_psy);
		power_supply_changed(&chip->batt_psy);
	}
	return IRQ_HANDLED;
}

static irqreturn_t dcin_ov_irq_handler(int irq, void *data)
{
	struct pm8921_chg_chip *chip = data;

	handle_stop_ext_chg(chip);
	create_dock_event(chip, CHG_DOCK_UNDOCK);
	return IRQ_HANDLED;
}

static irqreturn_t dcin_uv_irq_handler(int irq, void *data)
{
	struct pm8921_chg_chip *chip = data;

	handle_stop_ext_chg(chip);
	create_dock_event(chip, CHG_DOCK_UNDOCK);

	return IRQ_HANDLED;
}

static int update_charge_state(unsigned int value, enum dis_bit_mask mask)
{
	int rc, bitno;
	bool en_bit_val = true;
	bool dis_bit_val = false;

	if (!the_chip)
		return -EINVAL;

	mutex_lock(&chg_enable_lock);
	charging_disabled = (charging_disabled & ~mask) | (value & mask);

	for (bitno = DIS_BIT_MAX_NUM-1; bitno >= 0; bitno--) {
		if (charging_disabled & BIT(bitno)) {
			en_bit_val = chg_en_set_data[bitno].chg_en_bit_val;
			dis_bit_val = chg_en_set_data[bitno].chg_dis_bit_val;
			break;
		}
	}
	rc = pm_chg_auto_enable(the_chip, en_bit_val);
	if (rc) {
		pr_err("Failed pm_chg_auto_enable rc=%d\n", rc);
	} else {
		rc = pm_chg_charge_dis(the_chip, dis_bit_val);
		if (rc)
			pr_err("Failed pm_chg_charge_dis rc=%d\n", rc);
	}
	mutex_unlock(&chg_enable_lock);
	return rc;
}

static void check_soc_and_resume(struct pm8921_chg_chip *chip)
{
	if (chip->resume_soc >= chip->soc)
		update_charge_state(false, DIS_BIT_EOC_MASK);
}

static void temp_cold_func(void)
{
	update_charge_state(BIT(DIS_BIT_CHG_TEMP), DIS_BIT_CHG_TEMP_MASK);
}

static void set_appropriate_battery_current(struct pm8921_chg_chip *chip);
static void temp_cool_func(void)
{
	set_appropriate_battery_current(the_chip);
	pm_chg_vddmax_set(the_chip, the_chip->cool_bat_voltage);
	update_charge_state(false, DIS_BIT_CHG_TEMP_MASK);
}

static void temp_normal_func(void)
{
	set_appropriate_battery_current(the_chip);
	pm_chg_vddmax_set(the_chip, the_chip->max_voltage_mv);
	update_charge_state(false, DIS_BIT_CHG_TEMP_MASK);
}

static void temp_warm_func(void)
{
	set_appropriate_battery_current(the_chip);
	pm_chg_vddmax_set(the_chip, the_chip->warm_bat_voltage);
	update_charge_state(false, DIS_BIT_CHG_TEMP_MASK);
}

static void temp_hot_func(void)
{
	update_charge_state(BIT(DIS_BIT_CHG_TEMP), DIS_BIT_CHG_TEMP_MASK);
}

static bool is_temp_condition(struct pm8921_chg_chip *chip, int temp)
{
	enum temp_condition now_condition = chip->temp_state.condition;

	if (temp <= chip->temp_state.data[now_condition].low)
		chip->temp_state.condition--;
	else if (temp >= chip->temp_state.data[now_condition].high)
		chip->temp_state.condition++;
	else
		return true;

	chip->is_bat_cool =
	temp_condition_flg_data[chip->temp_state.condition].is_bat_cool;
	chip->is_bat_warm =
	temp_condition_flg_data[chip->temp_state.condition].is_bat_warm;

	return false;
}

static void update_temp_state(struct pm8921_chg_chip *chip)
{
	bool is_inside;
	enum temp_condition old_condition = chip->temp_state.condition;
	int temp;

	mutex_lock(&update_temp_lock);
	if (get_prop_batt_temp(chip, &temp)) {
		mutex_unlock(&update_temp_lock);
		return;
	}

	do {
		is_inside = is_temp_condition(chip, temp);
	} while (!is_inside);

	pr_debug("temp = %d, condition = %d\n", temp,
					chip->temp_state.condition);

	if (chip->temp_state.condition != old_condition) {
		chip->temp_state.data[chip->temp_state.condition].temp_func();
		pr_info("temp = %d, new condition = %d\n",
					temp, chip->temp_state.condition);
	}
	mutex_unlock(&update_temp_lock);
}

static void configure_temp_control(struct pm8921_chg_chip *chip)
{
	chip->temp_state.data[TEMP_COLD].low = INT_MIN;
	chip->temp_state.data[TEMP_COLD].high =
		chip->cold_temp_dc + chip->hysterisis_temp_dc;
	chip->temp_state.data[TEMP_COLD].temp_func = temp_cold_func;

	chip->temp_state.data[TEMP_COOL].low = chip->cold_temp_dc;
	chip->temp_state.data[TEMP_COOL].high =
				chip->cool_temp_dc + chip->hysterisis_temp_dc;
	chip->temp_state.data[TEMP_COOL].temp_func = temp_cool_func;

	chip->temp_state.data[TEMP_NORMAL].low = chip->cool_temp_dc;
	chip->temp_state.data[TEMP_NORMAL].high = chip->warm_temp_dc;
	chip->temp_state.data[TEMP_NORMAL].temp_func = temp_normal_func;

	chip->temp_state.data[TEMP_WARM].low =
				chip->warm_temp_dc - chip->hysterisis_temp_dc;
	chip->temp_state.data[TEMP_WARM].high = chip->hot_temp_dc;
	chip->temp_state.data[TEMP_WARM].temp_func = temp_warm_func;

	chip->temp_state.data[TEMP_HOT].low =
				chip->hot_temp_dc - chip->hysterisis_temp_dc;
	chip->temp_state.data[TEMP_HOT].high = INT_MAX;
	chip->temp_state.data[TEMP_HOT].temp_func = temp_hot_func;

	chip->temp_state.condition = TEMP_NORMAL;
	chip->is_temp_control = true;
	update_temp_state(chip);
}

static int __pm_batt_external_power_changed_work(struct device *dev, void *data)
{
	struct power_supply *psy = &the_chip->batt_psy;
	struct power_supply *epsy = dev_get_drvdata(dev);
	int i, dcin_irq;

	/* Only search for external supply if none is registered */
	if (!the_chip->ext_psy) {
		dcin_irq = the_chip->pmic_chg_irq[DCIN_VALID_IRQ];
		for (i = 0; i < epsy->num_supplicants; i++) {
			if (!strncmp(epsy->supplied_to[i], psy->name, 7)) {
				if (!strncmp(epsy->name, "dc", 2)) {
					the_chip->ext_psy = epsy;
					dcin_valid_irq_handler(dcin_irq,
							the_chip);
				}
			}
		}
	}
	return 0;
}

static void pm_batt_external_power_changed(struct power_supply *psy)
{
	/* Only look for an external supply if it hasn't been registered */
	if (!the_chip->ext_psy)
		class_for_each_device(power_supply_class, NULL, psy,
					 __pm_batt_external_power_changed_work);
}

static void
set_heartbeat_update_interval(struct pm8921_chg_chip *chip, int time_ms)
{
	if (chip->update_time != time_ms && time_ms > 0) {
		(void) cancel_delayed_work_sync(&chip->update_heartbeat_work);
		/*
		 * here we have everything stopped, and we
		 * can reschedule with a new given time
		 */
		pr_info("set new heartbeat interval: %dms\n", time_ms);

		chip->update_time = time_ms;
		schedule_delayed_work(&chip->update_heartbeat_work,
		      round_jiffies_relative(msecs_to_jiffies
					     (chip->update_time)));
	}
}

/**
 * update_heartbeat - internal function to update userspace
 *		per update_time minutes
 *
 */
static void update_heartbeat(struct work_struct *work)
{
	int rc;
	struct delayed_work *dwork = to_delayed_work(work);
	struct pm8921_chg_chip *chip = container_of(dwork,
				struct pm8921_chg_chip, update_heartbeat_work);

	if (chip->is_temp_control)
		update_temp_state(chip);
	rc = get_prop_batt_capacity(chip);
	if (rc >= 0)
		chip->soc = rc;
	check_soc_and_resume(chip);

	pm_chg_failed_clear(chip, 1);
	power_supply_changed(&chip->batt_psy);
	schedule_delayed_work(&chip->update_heartbeat_work,
			      round_jiffies_relative(msecs_to_jiffies
						     (chip->update_time)));
}
#define VDD_LOOP_ACTIVE_BIT	BIT(3)
#define VDD_MAX_INCREASE_MV	400
static int vdd_max_increase_mv = VDD_MAX_INCREASE_MV;
module_param(vdd_max_increase_mv, int, 0644);

static int ichg_threshold_ua = -400000;
module_param(ichg_threshold_ua, int, 0644);

#define PM8921_CHG_VDDMAX_RES_MV	10
static void adjust_vdd_max_for_fastchg(struct pm8921_chg_chip *chip)
{
	int ichg_meas_ua, vbat_uv;
	int ichg_meas_ma;
	int adj_vdd_max_mv, programmed_vdd_max;
	int vbat_batt_terminal_uv;
	int vbat_batt_terminal_mv;
	int reg_loop;
	int delta_mv = 0;

	if (chip->rconn_mohm == 0) {
		pr_debug("Exiting as rconn_mohm is 0\n");
		return;
	}
	/* adjust vdd_max only in normal temperature zone */
	if (chip->is_bat_cool || chip->is_bat_warm) {
		pr_debug("Exiting is_bat_cool = %d is_batt_warm = %d\n",
				chip->is_bat_cool, chip->is_bat_warm);
		return;
	}

	reg_loop = pm_chg_get_regulation_loop(chip);
	if (!(reg_loop & VDD_LOOP_ACTIVE_BIT)) {
		pr_debug("Exiting Vdd loop is not active reg loop=0x%x\n",
			reg_loop);
		return;
	}

	pm8921_bms_get_simultaneous_battery_voltage_and_current(&ichg_meas_ua,
								&vbat_uv);
	if (ichg_meas_ua >= 0) {
		pr_debug("Exiting ichg_meas_ua = %d > 0\n", ichg_meas_ua);
		return;
	}
	if (ichg_meas_ua <= ichg_threshold_ua) {
		pr_debug("Exiting ichg_meas_ua = %d < ichg_threshold_ua = %d\n",
					ichg_meas_ua, ichg_threshold_ua);
		return;
	}
	ichg_meas_ma = ichg_meas_ua / 1000;

	/* rconn_mohm is in milliOhms */
	vbat_batt_terminal_uv = vbat_uv + ichg_meas_ma * the_chip->rconn_mohm;
	vbat_batt_terminal_mv = vbat_batt_terminal_uv/1000;
	pm_chg_vddmax_get(the_chip, &programmed_vdd_max);

	delta_mv =  chip->max_voltage_mv - vbat_batt_terminal_mv;

	adj_vdd_max_mv = programmed_vdd_max + delta_mv;
	pr_debug("vdd_max needs to be changed by %d mv from %d to %d\n",
			delta_mv,
			programmed_vdd_max,
			adj_vdd_max_mv);

	if (adj_vdd_max_mv < chip->max_voltage_mv) {
		pr_debug("adj vdd_max lower than default max voltage\n");
		return;
	}

	adj_vdd_max_mv = DIV_ROUND_UP(adj_vdd_max_mv, PM8921_CHG_VDDMAX_RES_MV)
					* PM8921_CHG_VDDMAX_RES_MV;

	if (adj_vdd_max_mv > (chip->max_voltage_mv + vdd_max_increase_mv))
		adj_vdd_max_mv = chip->max_voltage_mv + vdd_max_increase_mv;
	pr_debug("adjusting vdd_max_mv to %d to make "
		"vbat_batt_termial_uv = %d to %d\n",
		adj_vdd_max_mv, vbat_batt_terminal_uv, chip->max_voltage_mv);
	pm_chg_vddmax_set(chip, adj_vdd_max_mv);
}

enum {
	CHG_IN_PROGRESS,
	CHG_NOT_IN_PROGRESS,
	CHG_FINISHED,
};

#define VBAT_TOLERANCE_MV	70
#define CHG_DISABLE_MSLEEP	100
static int is_charging_finished(struct pm8921_chg_chip *chip)
{
	int vbat_meas_uv, vbat_meas_mv, vbat_programmed, vbatdet_low;
	int ichg_meas_ma, iterm_programmed;
	int regulation_loop, fast_chg, vcp;
	int rc;
	static int last_vbat_programmed = -EINVAL;

	if (!is_ext_charging(chip)) {
		/* return if the battery is not being fastcharged */
		fast_chg = pm_chg_get_rt_status(chip, FASTCHG_IRQ);
		pr_debug("fast_chg = %d\n", fast_chg);
		if (fast_chg == 0)
			return CHG_NOT_IN_PROGRESS;

		vcp = pm_chg_get_rt_status(chip, VCP_IRQ);
		pr_debug("vcp = %d\n", vcp);
		if (vcp == 1)
			return CHG_IN_PROGRESS;

		vbatdet_low = pm_chg_get_rt_status(chip, VBATDET_LOW_IRQ);
		pr_debug("vbatdet_low = %d\n", vbatdet_low);
		if (vbatdet_low == 1)
			return CHG_IN_PROGRESS;

		/* reset count if battery is hot/cold */
		rc = pm_chg_get_rt_status(chip, BAT_TEMP_OK_IRQ);
		pr_debug("batt_temp_ok = %d\n", rc);
		if (rc == 0)
			return CHG_IN_PROGRESS;

		/* reset count if battery is warm */
		if (chip->temp_state.condition == TEMP_WARM) {
			pr_info("battery is warm\n");
			return CHG_IN_PROGRESS;
		}

		/* reset count if battery voltage is less than vddmax */
		vbat_meas_uv = get_prop_battery_uvolts(chip);
		if (vbat_meas_uv < 0)
			return CHG_IN_PROGRESS;
		vbat_meas_mv = vbat_meas_uv / 1000;

		rc = pm_chg_vddmax_get(chip, &vbat_programmed);
		if (rc) {
			pr_err("couldnt read vddmax rc = %d\n", rc);
			return CHG_IN_PROGRESS;
		}
		pr_debug("vddmax = %d vbat_meas_mv=%d\n",
			 vbat_programmed, vbat_meas_mv);

		if (last_vbat_programmed == -EINVAL)
			last_vbat_programmed = vbat_programmed;
		if (last_vbat_programmed !=  vbat_programmed) {
			/* vddmax changed, reset and check again */
			pr_debug("vddmax = %d last_vdd_max=%d\n",
				 vbat_programmed, last_vbat_programmed);
			last_vbat_programmed = vbat_programmed;
			return CHG_IN_PROGRESS;
		}

		if (vbat_meas_mv <= the_chip->max_voltage_mv -
				the_chip->resume_voltage_delta) {
			pr_debug("Voltage is lower than %d mV\n",
				the_chip->max_voltage_mv -
				the_chip->resume_voltage_delta);
			return CHG_IN_PROGRESS;
		}

		regulation_loop = pm_chg_get_regulation_loop(chip);
		if (regulation_loop < 0) {
			pr_err("couldnt read the regulation loop err=%d\n",
				regulation_loop);
			return CHG_IN_PROGRESS;
		}
		pr_debug("regulation_loop=%d\n", regulation_loop);

		if (regulation_loop != 0 && regulation_loop != VDD_LOOP)
			return CHG_IN_PROGRESS;
	} /* !is_ext_charging */

	/* reset count if battery chg current is more than iterm */
	rc = pm_chg_iterm_get(chip, &iterm_programmed);
	if (rc) {
		pr_err("couldnt read iterm rc = %d\n", rc);
		return CHG_IN_PROGRESS;
	}

	rc = get_prop_batt_current(chip, &ichg_meas_ma);
	if (rc) {
		pr_err("couldnt read batt current rc = %d\n", rc);
		return CHG_IN_PROGRESS;
	}
	ichg_meas_ma /= 1000;
	pr_debug("iterm_programmed = %d ichg_meas_ma=%d\n",
				iterm_programmed, ichg_meas_ma);
	/*
	 * ichg_meas_ma < 0 means battery is drawing current
	 * ichg_meas_ma > 0 means battery is providing current
	 */
	if (ichg_meas_ma > 0)
		return CHG_IN_PROGRESS;

	if (ichg_meas_ma * -1 > iterm_programmed)
		return CHG_IN_PROGRESS;

	return CHG_FINISHED;
}

/**
 * eoc_worker - internal function to check if battery EOC
 *		has happened
 *
 * If all conditions favouring, if the charge current is
 * less than the term current for three consecutive times
 * an EOC has happened.
 * The wakelock is released if there is no need to reshedule
 * - this happens when the battery is removed or EOC has
 * happened
 */
#define CONSECUTIVE_COUNT	3
static void eoc_worker(struct work_struct *work)
{
	struct delayed_work *dwork = to_delayed_work(work);
	struct pm8921_chg_chip *chip = container_of(dwork,
				struct pm8921_chg_chip, eoc_work);
	static int count;
	int end;

	pm_chg_failed_clear(chip, 1);
	end = is_charging_finished(chip);

	if (end == CHG_NOT_IN_PROGRESS) {
		count = 0;
		wake_unlock(&chip->eoc_wake_lock);
		return;
	}

	if (end == CHG_FINISHED) {
		count++;
	} else {
		count = 0;
	}

	if (count == CONSECUTIVE_COUNT) {
		count = 0;
		pr_info("End of Charging\n");

		update_charge_state(BIT(DIS_BIT_EOC), DIS_BIT_EOC_MASK);

		if (is_ext_charging(chip))
			chip->ext_charge_done = true;

		chip->bms_notify.is_battery_full = 1;
		/* declare end of charging by invoking chgdone interrupt */
		chgdone_irq_handler(chip->pmic_chg_irq[CHGDONE_IRQ], chip);
		wake_unlock(&chip->eoc_wake_lock);
	} else {
		adjust_vdd_max_for_fastchg(chip);
		pr_debug("EOC count = %d\n", count);
		schedule_delayed_work(&chip->eoc_work,
			      round_jiffies_relative(msecs_to_jiffies
						     (EOC_CHECK_PERIOD_MS)));
	}
}

static void btm_configure_work(struct work_struct *work)
{
	int rc;

	rc = pm8xxx_adc_btm_configure(&btm_config);
	if (rc)
		pr_err("failed to configure btm rc=%d", rc);
}

DECLARE_WORK(btm_config_work, btm_configure_work);

static void set_appropriate_battery_current(struct pm8921_chg_chip *chip)
{
	unsigned int chg_current = chip->max_bat_chg_current;

	if (chip->is_bat_cool)
		chg_current = min(chg_current, chip->cool_bat_chg_current);

	if (chip->is_bat_warm)
		chg_current = min(chg_current, chip->warm_bat_chg_current);

	if (thermal_mitigation != 0 && chip->thermal_mitigation)
		chg_current = min(chg_current,
				chip->thermal_mitigation[thermal_mitigation]);

	pm_chg_ibatmax_set(the_chip, chg_current);
}

#define TEMP_HYSTERISIS_DEGC 2
static void battery_cool(bool enter)
{
	pr_debug("enter = %d\n", enter);
	if (enter == the_chip->is_bat_cool)
		return;
	the_chip->is_bat_cool = enter;
	if (enter) {
		btm_config.low_thr_temp =
			the_chip->cool_temp_dc + TEMP_HYSTERISIS_DEGC;
		set_appropriate_battery_current(the_chip);
		pm_chg_vddmax_set(the_chip, the_chip->cool_bat_voltage);
		pm_chg_vbatdet_set(the_chip,
			the_chip->cool_bat_voltage
			- the_chip->resume_voltage_delta);
	} else {
		btm_config.low_thr_temp = the_chip->cool_temp_dc;
		set_appropriate_battery_current(the_chip);
		pm_chg_vddmax_set(the_chip, the_chip->max_voltage_mv);
		pm_chg_vbatdet_set(the_chip,
			the_chip->max_voltage_mv
			- the_chip->resume_voltage_delta);
	}
	schedule_work(&btm_config_work);
}

static void battery_warm(bool enter)
{
	pr_debug("enter = %d\n", enter);
	if (enter == the_chip->is_bat_warm)
		return;
	the_chip->is_bat_warm = enter;
	if (enter) {
		btm_config.high_thr_temp =
			the_chip->warm_temp_dc - TEMP_HYSTERISIS_DEGC;
		set_appropriate_battery_current(the_chip);
		pm_chg_vddmax_set(the_chip, the_chip->warm_bat_voltage);
		pm_chg_vbatdet_set(the_chip,
			the_chip->warm_bat_voltage
			- the_chip->resume_voltage_delta);
	} else {
		btm_config.high_thr_temp = the_chip->warm_temp_dc;
		set_appropriate_battery_current(the_chip);
		pm_chg_vddmax_set(the_chip, the_chip->max_voltage_mv);
		pm_chg_vbatdet_set(the_chip,
			the_chip->max_voltage_mv
			- the_chip->resume_voltage_delta);
	}
	schedule_work(&btm_config_work);
}

static int configure_btm(struct pm8921_chg_chip *chip)
{
	int rc;

	if (chip->warm_temp_dc != INT_MIN)
		btm_config.btm_warm_fn = battery_warm;
	else
		btm_config.btm_warm_fn = NULL;

	if (chip->cool_temp_dc != INT_MIN)
		btm_config.btm_cool_fn = battery_cool;
	else
		btm_config.btm_cool_fn = NULL;

	btm_config.low_thr_temp = chip->cool_temp_dc;
	btm_config.high_thr_temp = chip->warm_temp_dc;
	btm_config.interval = chip->temp_check_period;
	rc = pm8xxx_adc_btm_configure(&btm_config);
	if (rc)
		pr_err("failed to configure btm rc = %d\n", rc);
	rc = pm8xxx_adc_btm_start();
	if (rc)
		pr_err("failed to start btm rc = %d\n", rc);

	return rc;
}

/**
 * set_disable_status_param -
 *
 * Internal function to disable battery charging and also disable drawing
 * any current from the source. The device is forced to run on a battery
 * after this.
 */
static int set_disable_status_param(const char *val, struct kernel_param *kp)
{
	int ret;

	ret = param_set_int(val, kp);
	if (ret) {
		pr_err("error setting value %d\n", ret);
		return ret;
	}
	pr_info("factory set disable param to %d\n", charging_disabled_therm);
	if (charging_disabled_therm < DISABLED_OFF
		|| charging_disabled_therm >= DISABLED_MAX_NUM) {
		pr_err("invalid value is set [%d]\n", charging_disabled_therm);
		return -EINVAL;
	}
	update_charge_state(disabled_bit_val[charging_disabled_therm],
			DIS_BIT_THERM_MASK);
	return 0;
}
module_param_call(disabled, set_disable_status_param, param_get_uint,
					&charging_disabled_therm, 0644);

static int rconn_mohm;
static int set_rconn_mohm(const char *val, struct kernel_param *kp)
{
	int ret;
	struct pm8921_chg_chip *chip = the_chip;

	ret = param_set_int(val, kp);
	if (ret) {
		pr_err("error setting value %d\n", ret);
		return ret;
	}
	if (chip)
		chip->rconn_mohm = rconn_mohm;
	return 0;
}
module_param_call(rconn_mohm, set_rconn_mohm, param_get_uint,
					&rconn_mohm, 0644);
/**
 * set_thermal_mitigation_level -
 *
 * Internal function to control battery charging current to reduce
 * temperature
 */
static int set_therm_mitigation_level(const char *val, struct kernel_param *kp)
{
	int ret;
	struct pm8921_chg_chip *chip = the_chip;

	ret = param_set_int(val, kp);
	if (ret) {
		pr_err("error setting value %d\n", ret);
		return ret;
	}

	if (!chip) {
		pr_err("called before init\n");
		return -EINVAL;
	}

	if (!chip->thermal_mitigation) {
		pr_err("no thermal mitigation\n");
		return -EINVAL;
	}

	if (thermal_mitigation < 0
		|| thermal_mitigation >= chip->thermal_levels) {
		pr_err("out of bound level selected\n");
		return -EINVAL;
	}

	set_appropriate_battery_current(chip);
	return ret;
}
module_param_call(thermal_mitigation, set_therm_mitigation_level,
					param_get_uint,
					&thermal_mitigation, 0644);

static int set_usb_max_current(const char *val, struct kernel_param *kp)
{
	int ret;
	struct pm8921_chg_chip *chip = the_chip;

	ret = param_set_int(val, kp);
	if (ret) {
		pr_err("error setting value %d\n", ret);
		return ret;
	}
	if (chip) {
		pr_warn("setting current max to %d\n", usb_max_current);
		pm8921_charger_vbus_draw_local(SRC_ID_THERMAL, usb_max_current);
		return 0;
	}
	return -EINVAL;
}
module_param_call(usb_max_current, set_usb_max_current,
	param_get_uint, &usb_max_current, 0644);

static void free_irqs(struct pm8921_chg_chip *chip)
{
	int i;

	for (i = 0; i < PM_CHG_MAX_INTS; i++)
		if (chip->pmic_chg_irq[i]) {
			free_irq(chip->pmic_chg_irq[i], chip);
			chip->pmic_chg_irq[i] = 0;
		}
}

/* determines the initial present states */
static void __devinit determine_initial_state(struct pm8921_chg_chip *chip)
{
	unsigned long flags;
	int fsm_state;

	chip->dc_present = !!is_dc_chg_plugged_in(chip);
	chip->usb_present = !!is_usb_chg_plugged_in(chip);

	notify_usb_of_the_plugin_event(chip->usb_present);
	if (chip->usb_present) {
		schedule_delayed_work(&chip->unplug_check_work,
			round_jiffies_relative(msecs_to_jiffies
				(UNPLUG_CHECK_WAIT_PERIOD_MS)));
		pm8921_chg_enable_irq(chip, CHG_GONE_IRQ);
	}

	pm8921_chg_enable_irq(chip, DCIN_VALID_IRQ);
	pm8921_chg_enable_irq(chip, USBIN_VALID_IRQ);
	pm8921_chg_enable_irq(chip, BATT_REMOVED_IRQ);
	pm8921_chg_enable_irq(chip, DCIN_OV_IRQ);
	pm8921_chg_enable_irq(chip, DCIN_UV_IRQ);
	pm8921_chg_enable_irq(chip, CHGFAIL_IRQ);
	pm8921_chg_enable_irq(chip, FASTCHG_IRQ);
	pm8921_chg_enable_irq(chip, VBATDET_LOW_IRQ);

	spin_lock_irqsave(&vbus_lock, flags);
	if (usb_chg_current) {
		/* reissue a vbus draw call */
		__pm8921_charger_vbus_draw(usb_chg_current);
		fastchg_irq_handler(chip->pmic_chg_irq[FASTCHG_IRQ], chip);
	} else if (chip->dc_present) {
		dcin_valid_irq_handler(chip->pmic_chg_irq[DCIN_VALID_IRQ],
					chip);
		fastchg_irq_handler(chip->pmic_chg_irq[FASTCHG_IRQ], chip);
	}
	spin_unlock_irqrestore(&vbus_lock, flags);

	fsm_state = pm_chg_get_fsm_state(chip);
	if (is_battery_charging(fsm_state)) {
		chip->bms_notify.is_charging = 1;
		pm8921_bms_charging_began();
	}

	check_battery_valid(chip);

	pr_debug("usb = %d, dc = %d  batt = %d state=%d\n",
			chip->usb_present,
			chip->dc_present,
			get_prop_batt_present(chip),
			fsm_state);
}

struct pm_chg_irq_init_data {
	unsigned int	irq_id;
	char		*name;
	unsigned long	flags;
	irqreturn_t	(*handler)(int, void *);
};

#define CHG_IRQ(_id, _flags, _handler) \
{ \
	.irq_id		= _id, \
	.name		= #_id, \
	.flags		= _flags, \
	.handler	= _handler, \
}
struct pm_chg_irq_init_data chg_irq_data[] = {
	CHG_IRQ(USBIN_VALID_IRQ, IRQF_TRIGGER_RISING | IRQF_TRIGGER_FALLING,
						usbin_valid_irq_handler),
	CHG_IRQ(USBIN_OV_IRQ, IRQF_TRIGGER_RISING, usbin_ov_irq_handler),
	CHG_IRQ(BATT_INSERTED_IRQ, IRQF_TRIGGER_RISING | IRQF_TRIGGER_FALLING,
						batt_inserted_irq_handler),
	CHG_IRQ(VBATDET_LOW_IRQ, IRQF_TRIGGER_RISING | IRQF_TRIGGER_FALLING,
						vbatdet_low_irq_handler),
	CHG_IRQ(USBIN_UV_IRQ, IRQF_TRIGGER_RISING | IRQF_TRIGGER_FALLING,
							usbin_uv_irq_handler),
	CHG_IRQ(VBAT_OV_IRQ, IRQF_TRIGGER_RISING, vbat_ov_irq_handler),
	CHG_IRQ(CHGWDOG_IRQ, IRQF_TRIGGER_RISING, chgwdog_irq_handler),
	CHG_IRQ(VCP_IRQ, IRQF_TRIGGER_RISING, vcp_irq_handler),
	CHG_IRQ(ATCDONE_IRQ, IRQF_TRIGGER_RISING, atcdone_irq_handler),
	CHG_IRQ(ATCFAIL_IRQ, IRQF_TRIGGER_RISING, atcfail_irq_handler),
	CHG_IRQ(CHGDONE_IRQ, IRQF_TRIGGER_RISING, chgdone_irq_handler),
	CHG_IRQ(CHGFAIL_IRQ, IRQF_TRIGGER_RISING, chgfail_irq_handler),
	CHG_IRQ(CHGSTATE_IRQ, IRQF_TRIGGER_RISING, chgstate_irq_handler),
	CHG_IRQ(LOOP_CHANGE_IRQ, IRQF_TRIGGER_RISING, loop_change_irq_handler),
	CHG_IRQ(FASTCHG_IRQ, IRQF_TRIGGER_RISING | IRQF_TRIGGER_FALLING,
						fastchg_irq_handler),
	CHG_IRQ(TRKLCHG_IRQ, IRQF_TRIGGER_RISING, trklchg_irq_handler),
	CHG_IRQ(BATT_REMOVED_IRQ, IRQF_TRIGGER_RISING,
						batt_removed_irq_handler),
	CHG_IRQ(BATTTEMP_HOT_IRQ, IRQF_TRIGGER_RISING,
						batttemp_hot_irq_handler),
	CHG_IRQ(CHGHOT_IRQ, IRQF_TRIGGER_RISING, chghot_irq_handler),
	CHG_IRQ(BATTTEMP_COLD_IRQ, IRQF_TRIGGER_RISING,
						batttemp_cold_irq_handler),
	CHG_IRQ(CHG_GONE_IRQ, IRQF_TRIGGER_RISING | IRQF_TRIGGER_FALLING,
						chg_gone_irq_handler),
	CHG_IRQ(BAT_TEMP_OK_IRQ, IRQF_TRIGGER_RISING | IRQF_TRIGGER_FALLING,
						bat_temp_ok_irq_handler),
	CHG_IRQ(COARSE_DET_LOW_IRQ, IRQF_TRIGGER_RISING,
						coarse_det_low_irq_handler),
	CHG_IRQ(VDD_LOOP_IRQ, IRQF_TRIGGER_RISING, vdd_loop_irq_handler),
	CHG_IRQ(VREG_OV_IRQ, IRQF_TRIGGER_RISING, vreg_ov_irq_handler),
	CHG_IRQ(VBATDET_IRQ, IRQF_TRIGGER_RISING, vbatdet_irq_handler),
	CHG_IRQ(BATFET_IRQ, IRQF_TRIGGER_RISING | IRQF_TRIGGER_FALLING,
						batfet_irq_handler),
	CHG_IRQ(DCIN_VALID_IRQ, IRQF_TRIGGER_RISING | IRQF_TRIGGER_FALLING,
						dcin_valid_irq_handler),
	CHG_IRQ(DCIN_OV_IRQ, IRQF_TRIGGER_RISING | IRQF_TRIGGER_FALLING,
						dcin_ov_irq_handler),
	CHG_IRQ(DCIN_UV_IRQ, IRQF_TRIGGER_RISING, dcin_uv_irq_handler),
};

static int __devinit request_irqs(struct pm8921_chg_chip *chip,
					struct platform_device *pdev)
{
	struct resource *res;
	int ret, i;

	ret = 0;
	bitmap_fill(chip->enabled_irqs, PM_CHG_MAX_INTS);

	for (i = 0; i < ARRAY_SIZE(chg_irq_data); i++) {
		res = platform_get_resource_byname(pdev, IORESOURCE_IRQ,
				chg_irq_data[i].name);
		if (res == NULL) {
			pr_err("couldn't find %s\n", chg_irq_data[i].name);
			goto err_out;
		}
		chip->pmic_chg_irq[chg_irq_data[i].irq_id] = res->start;
		ret = request_irq(res->start, chg_irq_data[i].handler,
			chg_irq_data[i].flags,
			chg_irq_data[i].name, chip);
		if (ret < 0) {
			pr_err("couldn't request %d (%s) %d\n", res->start,
					chg_irq_data[i].name, ret);
			chip->pmic_chg_irq[chg_irq_data[i].irq_id] = 0;
			goto err_out;
		}
		pm8921_chg_disable_irq(chip, chg_irq_data[i].irq_id);
	}
	return 0;

err_out:
	free_irqs(chip);
	return -EINVAL;
}

static void pm8921_chg_force_19p2mhz_clk(struct pm8921_chg_chip *chip)
{
	int err;
	u8 temp;

	temp  = 0xD1;
	err = pm8xxx_writeb(chip->dev->parent, CHG_TEST, temp);
	if (err) {
		pr_err("Error %d writing %d to addr %d\n", err, temp, CHG_TEST);
		return;
	}

	temp  = 0xD3;
	err = pm8xxx_writeb(chip->dev->parent, CHG_TEST, temp);
	if (err) {
		pr_err("Error %d writing %d to addr %d\n", err, temp, CHG_TEST);
		return;
	}

	temp  = 0xD1;
	err = pm8xxx_writeb(chip->dev->parent, CHG_TEST, temp);
	if (err) {
		pr_err("Error %d writing %d to addr %d\n", err, temp, CHG_TEST);
		return;
	}

	temp  = 0xD5;
	err = pm8xxx_writeb(chip->dev->parent, CHG_TEST, temp);
	if (err) {
		pr_err("Error %d writing %d to addr %d\n", err, temp, CHG_TEST);
		return;
	}

	udelay(183);

	temp  = 0xD1;
	err = pm8xxx_writeb(chip->dev->parent, CHG_TEST, temp);
	if (err) {
		pr_err("Error %d writing %d to addr %d\n", err, temp, CHG_TEST);
		return;
	}

	temp  = 0xD0;
	err = pm8xxx_writeb(chip->dev->parent, CHG_TEST, temp);
	if (err) {
		pr_err("Error %d writing %d to addr %d\n", err, temp, CHG_TEST);
		return;
	}
	udelay(32);

	temp  = 0xD1;
	err = pm8xxx_writeb(chip->dev->parent, CHG_TEST, temp);
	if (err) {
		pr_err("Error %d writing %d to addr %d\n", err, temp, CHG_TEST);
		return;
	}

	temp  = 0xD3;
	err = pm8xxx_writeb(chip->dev->parent, CHG_TEST, temp);
	if (err) {
		pr_err("Error %d writing %d to addr %d\n", err, temp, CHG_TEST);
		return;
	}
}

static void pm8921_chg_set_hw_clk_switching(struct pm8921_chg_chip *chip)
{
	int err;
	u8 temp;

	temp  = 0xD1;
	err = pm8xxx_writeb(chip->dev->parent, CHG_TEST, temp);
	if (err) {
		pr_err("Error %d writing %d to addr %d\n", err, temp, CHG_TEST);
		return;
	}

	temp  = 0xD0;
	err = pm8xxx_writeb(chip->dev->parent, CHG_TEST, temp);
	if (err) {
		pr_err("Error %d writing %d to addr %d\n", err, temp, CHG_TEST);
		return;
	}
}

#define VREF_BATT_THERM_FORCE_ON	BIT(7)
static void detect_battery_removal(struct pm8921_chg_chip *chip)
{
	u8 temp;

	pm8xxx_readb(chip->dev->parent, CHG_CNTRL, &temp);
	pr_debug("upon restart CHG_CNTRL = 0x%x\n",  temp);

	if (!(temp & VREF_BATT_THERM_FORCE_ON))
		/*
		 * batt therm force on bit is battery backed and is default 0
		 * The charger sets this bit at init time. If this bit is found
		 * 0 that means the battery was removed. Tell the bms about it
		 */
		pm8921_bms_invalidate_shutdown_soc();
}

#define ENUM_TIMER_STOP_BIT	BIT(1)
#define BOOT_DONE_BIT		BIT(6)
#define CHG_BATFET_ON_BIT	BIT(3)
#define CHG_VCP_EN		BIT(0)
#define CHG_BAT_TEMP_DIS_BIT	BIT(2)
#define SAFE_CURRENT_MA		2000
#define RATIO_TERMINAL_CURRENT_TO_FCC	20
static int __devinit pm8921_chg_hw_init(struct pm8921_chg_chip *chip)
{
	int rc;
	int vdd_safe;

	/* forcing 19p2mhz before accessing any charger registers */
	pm8921_chg_force_19p2mhz_clk(chip);

	clear_long_time_chg_failed(chip);

	detect_battery_removal(chip);

	rc = pm_chg_masked_write(chip, SYS_CONFIG_2,
					BOOT_DONE_BIT, BOOT_DONE_BIT);
	if (rc) {
		pr_err("Failed to set BOOT_DONE_BIT rc=%d\n", rc);
		return rc;
	}

	vdd_safe = chip->max_voltage_mv + VDD_MAX_INCREASE_MV;

	if (vdd_safe > PM8921_CHG_VDDSAFE_MAX)
		vdd_safe = PM8921_CHG_VDDSAFE_MAX;

	rc = pm_chg_vddsafe_set(chip, vdd_safe);

	if (rc) {
		pr_err("Failed to set safe voltage to %d rc=%d\n",
						chip->max_voltage_mv, rc);
		return rc;
	}
	rc = pm_chg_vbatdet_set(chip,
				chip->max_voltage_mv
				- chip->resume_voltage_delta);
	if (rc) {
		pr_err("Failed to set vbatdet comprator voltage to %d rc=%d\n",
			chip->max_voltage_mv - chip->resume_voltage_delta, rc);
		return rc;
	}

	rc = pm_chg_vddmax_set(chip, chip->max_voltage_mv);
	if (rc) {
		pr_err("Failed to set max voltage to %d rc=%d\n",
						chip->max_voltage_mv, rc);
		return rc;
	}
	rc = pm_chg_ibatsafe_set(chip, chip->safe_current ?
			chip->safe_current : SAFE_CURRENT_MA);
	if (rc) {
		pr_err("Failed to set max voltage to %d rc=%d\n",
				chip->safe_current ?
				chip->safe_current : SAFE_CURRENT_MA, rc);
		return rc;
	}

	rc = pm_chg_ibatmax_set(chip, chip->max_bat_chg_current);
	if (rc) {
		pr_err("Failed to set max current to 400 rc=%d\n", rc);
		return rc;
	}

	rc = get_prop_batt_init_fcc(chip);
	if (rc >= 0)
		chip->term_current = rc / RATIO_TERMINAL_CURRENT_TO_FCC;

	chip->ibatmax_max_adj_ma = find_ibat_max_adj_ma(
				chip->max_bat_chg_current);

	rc = pm_chg_iterm_set(chip, chip->term_current);
	if (rc) {
		pr_err("Failed to set term current to %d rc=%d\n",
						chip->term_current, rc);
		return rc;
	}

	/* Disable the ENUM TIMER */
	rc = pm_chg_masked_write(chip, PBL_ACCESS2, ENUM_TIMER_STOP_BIT,
			ENUM_TIMER_STOP_BIT);
	if (rc) {
		pr_err("Failed to set enum timer stop rc=%d\n", rc);
		return rc;
	}

	/* init with the lowest USB current */
	rc = pm_chg_iusbmax_set(chip, 0);
	if (rc) {
		pr_err("Failed to set usb max to %d rc=%d\n", 0, rc);
		return rc;
	}

	if (chip->safety_time != 0) {
		rc = pm_chg_tchg_max_set(chip, chip->safety_time);
		if (rc) {
			pr_err("Failed to set max time to %d minutes rc=%d\n",
							chip->safety_time, rc);
			return rc;
		}
	}

	if (chip->ttrkl_time != 0) {
		rc = pm_chg_ttrkl_max_set(chip, chip->ttrkl_time);
		if (rc) {
			pr_err("Failed to set trkl time to %d minutes rc=%d\n",
							chip->ttrkl_time, rc);
			return rc;
		}
	}

	if (chip->vin_min != 0) {
		rc = pm_chg_vinmin_set(chip, chip->vin_min);
		if (rc) {
			pr_err("Failed to set vin min to %d mV rc=%d\n",
							chip->vin_min, rc);
			return rc;
		}
	} else {
		chip->vin_min = pm_chg_vinmin_get(chip);
	}

	rc = pm_chg_disable_wd(chip);
	if (rc) {
		pr_err("Failed to disable wd rc=%d\n", rc);
		return rc;
	}

	rc = pm_chg_masked_write(chip, CHG_CNTRL_2,
				CHG_BAT_TEMP_DIS_BIT, 0);
	if (rc) {
		pr_err("Failed to enable temp control chg rc=%d\n", rc);
		return rc;
	}
	/* switch to a 3.2Mhz for the buck */
	rc = pm8xxx_writeb(chip->dev->parent, CHG_BUCK_CLOCK_CTRL, 0x15);
	if (rc) {
		pr_err("Failed to switch buck clk rc=%d\n", rc);
		return rc;
	}

	if (chip->trkl_voltage != 0) {
		rc = pm_chg_vtrkl_low_set(chip, chip->trkl_voltage);
		if (rc) {
			pr_err("Failed to set trkl voltage to %dmv  rc=%d\n",
							chip->trkl_voltage, rc);
			return rc;
		}
	}

	if (chip->weak_voltage != 0) {
		rc = pm_chg_vweak_set(chip, chip->weak_voltage);
		if (rc) {
			pr_err("Failed to set weak voltage to %dmv  rc=%d\n",
							chip->weak_voltage, rc);
			return rc;
		}
	}

	if (chip->trkl_current != 0) {
		rc = pm_chg_itrkl_set(chip, chip->trkl_current);
		if (rc) {
			pr_err("Failed to set trkl current to %dmA  rc=%d\n",
							chip->trkl_voltage, rc);
			return rc;
		}
	}

	if (chip->weak_current != 0) {
		rc = pm_chg_iweak_set(chip, chip->weak_current);
		if (rc) {
			pr_err("Failed to set weak current to %dmA  rc=%d\n",
							chip->weak_current, rc);
			return rc;
		}
	}

	rc = pm_chg_batt_cold_temp_config(chip, chip->cold_thr);
	if (rc) {
		pr_err("Failed to set cold config %d  rc=%d\n",
						chip->cold_thr, rc);
	}

	rc = pm_chg_batt_hot_temp_config(chip, chip->hot_thr);
	if (rc) {
		pr_err("Failed to set hot config %d  rc=%d\n",
						chip->hot_thr, rc);
	}

	/* Workarounds for die 1.1 and 1.0 */
	if (pm8xxx_get_revision(chip->dev->parent) < PM8XXX_REVISION_8921_2p0) {
		pm8xxx_writeb(chip->dev->parent, CHG_BUCK_CTRL_TEST2, 0xF1);
		pm8xxx_writeb(chip->dev->parent, CHG_BUCK_CTRL_TEST3, 0xCE);
		pm8xxx_writeb(chip->dev->parent, CHG_BUCK_CTRL_TEST3, 0xD8);

		/* software workaround for correct battery_id detection */
		pm8xxx_writeb(chip->dev->parent, PSI_TXRX_SAMPLE_DATA_0, 0xFF);
		pm8xxx_writeb(chip->dev->parent, PSI_TXRX_SAMPLE_DATA_1, 0xFF);
		pm8xxx_writeb(chip->dev->parent, PSI_TXRX_SAMPLE_DATA_2, 0xFF);
		pm8xxx_writeb(chip->dev->parent, PSI_TXRX_SAMPLE_DATA_3, 0xFF);
		pm8xxx_writeb(chip->dev->parent, PSI_CONFIG_STATUS, 0x0D);
		udelay(100);
		pm8xxx_writeb(chip->dev->parent, PSI_CONFIG_STATUS, 0x0C);
	}

	/* Enable isub_fine resolution AICL for PM8917 */
	if (pm8xxx_get_version(chip->dev->parent) == PM8XXX_VERSION_8917)
		chip->iusb_fine_res = true;

	pm8xxx_writeb(chip->dev->parent, CHG_BUCK_CTRL_TEST3, 0xD9);

	/* Disable EOC FSM processing */
	pm8xxx_writeb(chip->dev->parent, CHG_BUCK_CTRL_TEST3, 0x91);

	rc = pm_chg_masked_write(chip, CHG_CNTRL, VREF_BATT_THERM_FORCE_ON,
						VREF_BATT_THERM_FORCE_ON);
	if (rc)
		pr_err("Failed to Force Vref therm rc=%d\n", rc);

	rc = pm_chg_charge_dis(chip, charging_disabled);
	if (rc) {
		pr_err("Failed to disable CHG_CHARGE_DIS bit rc=%d\n", rc);
		return rc;
	}

	rc = pm_chg_auto_enable(chip, !charging_disabled);
	if (rc) {
		pr_err("Failed to enable charging rc=%d\n", rc);
		return rc;
	}

	return 0;
}

static int msm_battery_gauge_alarm_notify(struct notifier_block *nb,
		unsigned long status, void *unused)
{
	switch (status) {
	case 0:
		pr_info("spurious interrupt\n");
		break;
	case PM8XXX_BATT_ALARM_STATUS_BELOW_LOWER:
		pr_info("trip of low threshold\n");
		if (!wake_lock_active(&the_chip->low_voltage_wake_lock))
			wake_lock(&the_chip->low_voltage_wake_lock);

		set_heartbeat_update_interval(the_chip,
			the_chip->low_bat_update_time);
		break;
	case PM8XXX_BATT_ALARM_STATUS_ABOVE_UPPER:
		pr_info("trip of high threshold\n");
		if (wake_lock_active(&the_chip->low_voltage_wake_lock))
			wake_unlock(&the_chip->low_voltage_wake_lock);

		set_heartbeat_update_interval(the_chip,
			the_chip->default_update_time);
		break;
	default:
		pr_err("error received\n");
	}
	return 0;
}

#define BATT_ALARM_HYSTERISIS_MV	100
#define PWM_UPDATE_RATE_ENABLED		1
#define PWM_CLOCK_SCALER		7
#define PWM_CLOCK_DIVIDER		4
static int __devinit pm8921_set_batt_alarm(struct pm8921_chg_chip *chip)
{
	int rc;
	rc = pm8xxx_batt_alarm_disable(PM8XXX_BATT_ALARM_LOWER_COMPARATOR);
	if (rc) {
		pr_err("unable to set batt alarm state\n");
		goto err_out;
	}
	rc = pm8xxx_batt_alarm_disable(PM8XXX_BATT_ALARM_UPPER_COMPARATOR);
	if (rc) {
		pr_err("unable to set batt alarm state\n");
		goto err_out;
	}
	rc = pm8xxx_batt_alarm_threshold_set(
			PM8XXX_BATT_ALARM_LOWER_COMPARATOR,
			chip->min_voltage_mv);
	if (rc) {
		pr_err("unable to set batt alarm threshold\n");
		goto err_out;
	}
	rc = pm8xxx_batt_alarm_threshold_set(
			PM8XXX_BATT_ALARM_UPPER_COMPARATOR,
			chip->min_voltage_mv + BATT_ALARM_HYSTERISIS_MV);
	if (rc) {
		pr_err("unable to set batt alarm threshold\n");
		goto err_out;
	}
	rc = pm8xxx_batt_alarm_hold_time_set(
			PM8XXX_BATT_ALARM_HOLD_TIME_16_MS);
	if (rc) {
		pr_err("unable to set batt alarm hold time\n");
		goto err_out;
	}

	/* PWM enabled at 2Hz */
	rc = pm8xxx_batt_alarm_pwm_rate_set(
			PWM_UPDATE_RATE_ENABLED,
			PWM_CLOCK_SCALER,
			PWM_CLOCK_DIVIDER);
	if (rc) {
		pr_err("unable to set batt alarm pwm rate\n");
		goto err_out;
	}

	rc = pm8xxx_batt_alarm_register_notifier(&alarm_notifier);
	if (rc) {
		pr_err("unable to register alarm notifier\n");
		goto err_out;
	}
	rc = pm8xxx_batt_alarm_enable(PM8XXX_BATT_ALARM_LOWER_COMPARATOR);
	if (rc) {
		pr_err("unable to enable batt alarm\n");
		goto err_out;
	}
	rc = pm8xxx_batt_alarm_enable(PM8XXX_BATT_ALARM_UPPER_COMPARATOR);
	if (rc) {
		pr_err("unable to set batt alarm state\n");
		goto err_out;
	}

err_out:
	return rc;
}

static int get_rt_status(void *data, u64 * val)
{
	int i = (int)data;
	int ret;

	/* global irq number is passed in via data */
	ret = pm_chg_get_rt_status(the_chip, i);
	*val = ret;
	return 0;
}
DEFINE_SIMPLE_ATTRIBUTE(rt_fops, get_rt_status, NULL, "%llu\n");

static int get_fsm_status(void *data, u64 * val)
{
	u8 temp;

	temp = pm_chg_get_fsm_state(the_chip);
	*val = temp;
	return 0;
}
DEFINE_SIMPLE_ATTRIBUTE(fsm_fops, get_fsm_status, NULL, "%llu\n");

static int get_reg_loop(void *data, u64 * val)
{
	u8 temp;

	if (!the_chip) {
		pr_err("%s called before init\n", __func__);
		return -EINVAL;
	}
	temp = pm_chg_get_regulation_loop(the_chip);
	*val = temp;
	return 0;
}
DEFINE_SIMPLE_ATTRIBUTE(reg_loop_fops, get_reg_loop, NULL, "0x%02llx\n");

static int get_temp_condition(void *data, u64 * val)
{
	u8 temp;

	temp = the_chip->temp_state.condition;
	*val = temp;
	return 0;
}
DEFINE_SIMPLE_ATTRIBUTE(temp_condition, get_temp_condition, NULL, "%llu\n");

static int get_reg(void *data, u64 * val)
{
	int addr = (int)data;
	int ret;
	u8 temp;

	ret = pm8xxx_readb(the_chip->dev->parent, addr, &temp);
	if (ret) {
		pr_err("pm8xxx_readb to %x value =%d errored = %d\n",
			addr, temp, ret);
		return -EAGAIN;
	}
	*val = temp;
	return 0;
}

static int set_reg(void *data, u64 val)
{
	int addr = (int)data;
	int ret;
	u8 temp;

	temp = (u8) val;
	ret = pm8xxx_writeb(the_chip->dev->parent, addr, temp);
	if (ret) {
		pr_err("pm8xxx_writeb to %x value =%d errored = %d\n",
			addr, temp, ret);
		return -EAGAIN;
	}
	return 0;
}
DEFINE_SIMPLE_ATTRIBUTE(reg_fops, get_reg, set_reg, "0x%02llx\n");

enum {
	BAT_WARM_ZONE,
	BAT_COOL_ZONE,
};
static int get_warm_cool(void *data, u64 * val)
{
	if (!the_chip) {
		pr_err("%s called before init\n", __func__);
		return -EINVAL;
	}
	if ((int)data == BAT_WARM_ZONE)
		*val = the_chip->is_bat_warm;
	if ((int)data == BAT_COOL_ZONE)
		*val = the_chip->is_bat_cool;
	return 0;
}
DEFINE_SIMPLE_ATTRIBUTE(warm_cool_fops, get_warm_cool, NULL, "0x%lld\n");

static void create_debugfs_entries(struct pm8921_chg_chip *chip)
{
	int i;

	chip->dent = debugfs_create_dir("pm8921_chg", NULL);

	if (IS_ERR(chip->dent)) {
		pr_err("pmic charger couldnt create debugfs dir\n");
		return;
	}

	debugfs_create_file("CHG_CNTRL", 0644, chip->dent,
			    (void *)CHG_CNTRL, &reg_fops);
	debugfs_create_file("CHG_CNTRL_2", 0644, chip->dent,
			    (void *)CHG_CNTRL_2, &reg_fops);
	debugfs_create_file("CHG_CNTRL_3", 0644, chip->dent,
			    (void *)CHG_CNTRL_3, &reg_fops);
	debugfs_create_file("PBL_ACCESS1", 0644, chip->dent,
			    (void *)PBL_ACCESS1, &reg_fops);
	debugfs_create_file("PBL_ACCESS2", 0644, chip->dent,
			    (void *)PBL_ACCESS2, &reg_fops);
	debugfs_create_file("SYS_CONFIG_1", 0644, chip->dent,
			    (void *)SYS_CONFIG_1, &reg_fops);
	debugfs_create_file("SYS_CONFIG_2", 0644, chip->dent,
			    (void *)SYS_CONFIG_2, &reg_fops);
	debugfs_create_file("CHG_VDD_MAX", 0644, chip->dent,
			    (void *)CHG_VDD_MAX, &reg_fops);
	debugfs_create_file("CHG_VDD_SAFE", 0644, chip->dent,
			    (void *)CHG_VDD_SAFE, &reg_fops);
	debugfs_create_file("CHG_VBAT_DET", 0644, chip->dent,
			    (void *)CHG_VBAT_DET, &reg_fops);
	debugfs_create_file("CHG_IBAT_MAX", 0644, chip->dent,
			    (void *)CHG_IBAT_MAX, &reg_fops);
	debugfs_create_file("CHG_IBAT_SAFE", 0644, chip->dent,
			    (void *)CHG_IBAT_SAFE, &reg_fops);
	debugfs_create_file("CHG_VIN_MIN", 0644, chip->dent,
			    (void *)CHG_VIN_MIN, &reg_fops);
	debugfs_create_file("CHG_VTRICKLE", 0644, chip->dent,
			    (void *)CHG_VTRICKLE, &reg_fops);
	debugfs_create_file("CHG_ITRICKLE", 0644, chip->dent,
			    (void *)CHG_ITRICKLE, &reg_fops);
	debugfs_create_file("CHG_ITERM", 0644, chip->dent,
			    (void *)CHG_ITERM, &reg_fops);
	debugfs_create_file("CHG_TCHG_MAX", 0644, chip->dent,
			    (void *)CHG_TCHG_MAX, &reg_fops);
	debugfs_create_file("CHG_TWDOG", 0644, chip->dent,
			    (void *)CHG_TWDOG, &reg_fops);
	debugfs_create_file("CHG_TEMP_THRESH", 0644, chip->dent,
			    (void *)CHG_TEMP_THRESH, &reg_fops);
	debugfs_create_file("CHG_COMP_OVR", 0644, chip->dent,
			    (void *)CHG_COMP_OVR, &reg_fops);
	debugfs_create_file("CHG_BUCK_CTRL_TEST1", 0644, chip->dent,
			    (void *)CHG_BUCK_CTRL_TEST1, &reg_fops);
	debugfs_create_file("CHG_BUCK_CTRL_TEST2", 0644, chip->dent,
			    (void *)CHG_BUCK_CTRL_TEST2, &reg_fops);
	debugfs_create_file("CHG_BUCK_CTRL_TEST3", 0644, chip->dent,
			    (void *)CHG_BUCK_CTRL_TEST3, &reg_fops);
	debugfs_create_file("CHG_TEST", 0644, chip->dent,
			    (void *)CHG_TEST, &reg_fops);

	debugfs_create_file("FSM_STATE", 0644, chip->dent, NULL,
			    &fsm_fops);

	debugfs_create_file("REGULATION_LOOP_CONTROL", 0644, chip->dent, NULL,
			    &reg_loop_fops);

	debugfs_create_file("BAT_WARM_ZONE", 0644, chip->dent,
				(void *)BAT_WARM_ZONE, &warm_cool_fops);
	debugfs_create_file("BAT_COOL_ZONE", 0644, chip->dent,
				(void *)BAT_COOL_ZONE, &warm_cool_fops);
	debugfs_create_file("TEMP_CONDITION", 0644, chip->dent, NULL,
			    &temp_condition);

	for (i = 0; i < ARRAY_SIZE(chg_irq_data); i++) {
		if (chip->pmic_chg_irq[chg_irq_data[i].irq_id])
			debugfs_create_file(chg_irq_data[i].name, 0444,
				chip->dent,
				(void *)chg_irq_data[i].irq_id,
				&rt_fops);
	}
}

static ssize_t pm8921_chg_temp_condition(struct device *dev,
					struct device_attribute *attr,
					char *buf)
{
	struct pm8921_chg_chip *chip = dev_get_drvdata(dev);

	return scnprintf(buf, PAGE_SIZE, "%d\n",
		(int)chip->temp_state.condition);
}

static ssize_t pm8921_chg_min_voltage_mv_show(struct device *dev,
					struct device_attribute *attr,
					char *buf)
{
	ssize_t ret;
	struct pm8921_chg_chip *chip = dev_get_drvdata(dev);

	ret = snprintf(buf, PAGE_SIZE, "%d\n", chip->min_voltage_mv);
	return ret;
}

static ssize_t pm8921_chg_min_voltage_mv_store(struct device *dev,
					struct device_attribute *attr,
					const char *buf, size_t count)
{
	int rc;
	unsigned long arg;
	struct pm8921_chg_chip *chip = dev_get_drvdata(dev);

	rc = strict_strtoul(buf, 10, &arg);
	if (!rc)
		chip->min_voltage_mv = arg;

	return strnlen(buf, PAGE_SIZE);
}

static ssize_t pm8921_chg_read_fsm_state(struct device *dev,
					struct device_attribute *attr,
					char *buf)
{
	struct pm8921_chg_chip *chip = dev_get_drvdata(dev);

	return scnprintf(buf, PAGE_SIZE, "%d\n", chip->read_fsm_state_param);
}

static ssize_t pm8921_chg_read_chg_cntrl(struct device *dev,
					struct device_attribute *attr,
					char *buf)
{
	ssize_t ret;
	int rc;
	u8 temp;

	rc = pm8xxx_readb(the_chip->dev->parent, CHG_CNTRL, &temp);
	if (rc) {
		pr_err("pm8xxx_readb failed: CHG_CNTRL, rc=%d\n", rc);
		return -EAGAIN;
	}

	ret = snprintf(buf, PAGE_SIZE, "%d\n", temp);
	return ret;
}

static ssize_t pm8921_chg_read_chg_cntrl3(struct device *dev,
					struct device_attribute *attr,
					char *buf)
{
	ssize_t ret;
	int rc;
	u8 temp;

	rc = pm8xxx_readb(the_chip->dev->parent, CHG_CNTRL_3, &temp);
	if (rc) {
		pr_err("pm8xxx_readb failed: CHG_CNTRL_3, rc=%d\n", rc);
		return -EAGAIN;
	}

	ret = snprintf(buf, PAGE_SIZE, "%d\n", temp);
	return ret;
}

static ssize_t pm8921_chg_read_bat_fet(struct device *dev,
					struct device_attribute *attr,
					char *buf)
{
	ssize_t ret;
	int temp;

	temp = pm_chg_get_rt_status(the_chip, BATFET_IRQ);
	ret = snprintf(buf, PAGE_SIZE, "%d\n", temp);
	return ret;
}

static ssize_t pm8921_chg_read_vdd_max(struct device *dev,
					struct device_attribute *attr,
					char *buf)
{
	ssize_t ret;
	int rc, voltage_mv;

	rc = pm_chg_vddmax_get(the_chip, &voltage_mv);
	if (rc) {
		pr_err("couldn't read vddmax rc = %d\n", rc);
		return -EAGAIN;
	}
	ret = snprintf(buf, PAGE_SIZE, "%d\n", voltage_mv);
	return ret;
}

static ssize_t pm8921_chg_read_ibat_max(struct device *dev,
					struct device_attribute *attr,
					char *buf)
{
	ssize_t ret;
	u8 temp;
	int rc, current_ma;

	rc = pm8xxx_readb(the_chip->dev->parent, CHG_IBAT_MAX, &temp);
	if (rc) {
		pr_err("rc = %d while reading ibat max\n", rc);
		return -EAGAIN;
	}
	temp &= PM8921_CHG_I_MASK;
	current_ma = PM8921_CHG_I_MIN_MA + (int)temp * PM8921_CHG_I_STEP_MA;
	ret = snprintf(buf, PAGE_SIZE, "%d\n", current_ma);
	return ret;
}

static ssize_t pm8921_chg_read_iusb_max(struct device *dev,
					struct device_attribute *attr,
					char *buf)
{
	ssize_t ret;
	int rc, current_ma;

	rc = pm_chg_iusbmax_get(the_chip, &current_ma);
	if (rc) {
		pr_err("couldn't read iusbmax rc = %d\n", rc);
		return -EAGAIN;
	}
	ret = snprintf(buf, PAGE_SIZE, "%d\n", current_ma);
	return ret;
}

static struct device_attribute pm8921_chg_attrs[] = {
	__ATTR(temp_condition, 0444, pm8921_chg_temp_condition, NULL),
	__ATTR(min_voltage_mv, 0644, pm8921_chg_min_voltage_mv_show,
			pm8921_chg_min_voltage_mv_store),
	__ATTR(read_fsm_state, 0444, pm8921_chg_read_fsm_state, NULL),
	__ATTR(read_chg_cntrl, 0444, pm8921_chg_read_chg_cntrl, NULL),
	__ATTR(read_chg_cntrl3, 0444, pm8921_chg_read_chg_cntrl3, NULL),
	__ATTR(read_bat_fet, 0444, pm8921_chg_read_bat_fet, NULL),
	__ATTR(read_vdd_max, 0444, pm8921_chg_read_vdd_max, NULL),
	__ATTR(read_ibat_max, 0444, pm8921_chg_read_ibat_max, NULL),
	__ATTR(read_iusb_max, 0444, pm8921_chg_read_iusb_max, NULL),
};

static int create_sysfs_entries(struct pm8921_chg_chip *chip)
{
	int i, rc;

	for (i = 0; i < ARRAY_SIZE(pm8921_chg_attrs); i++) {
		rc = device_create_file(chip->dev, &pm8921_chg_attrs[i]);
		if (rc < 0)
			goto revert;
	}

	return 0;

revert:
	for (; i >= 0; i--)
		device_remove_file(chip->dev, &pm8921_chg_attrs[i]);

	return rc;
}

static void remove_sysfs_entries(struct pm8921_chg_chip *chip)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(pm8921_chg_attrs); i++)
		device_remove_file(chip->dev, &pm8921_chg_attrs[i]);
}

static int pm8921_charger_suspend_noirq(struct device *dev)
{
	int rc;
	struct pm8921_chg_chip *chip = dev_get_drvdata(dev);

	rc = pm_chg_masked_write(chip, CHG_CNTRL, VREF_BATT_THERM_FORCE_ON, 0);
	if (rc)
		pr_err("Failed to Force Vref therm off rc=%d\n", rc);
	pm8921_chg_set_hw_clk_switching(chip);
	return 0;
}

static int pm8921_charger_resume_noirq(struct device *dev)
{
	int rc;
	struct pm8921_chg_chip *chip = dev_get_drvdata(dev);

	pm8921_chg_force_19p2mhz_clk(chip);

	rc = pm_chg_masked_write(chip, CHG_CNTRL, VREF_BATT_THERM_FORCE_ON,
						VREF_BATT_THERM_FORCE_ON);
	if (rc)
		pr_err("Failed to Force Vref therm on rc=%d\n", rc);
	return 0;
}

static int pm8921_update_thread(void *__chip)
{
	struct pm8921_chg_chip *chip = (struct pm8921_chg_chip *)__chip;
	int rc;

	if (chip->is_temp_control)
		update_temp_state(chip);

	rc = get_prop_batt_capacity(chip);
	pr_info("soc = %d\n", rc);
	if (rc >= 0) {
		chip->soc = rc;
		power_supply_changed(&chip->batt_psy);
	}
	return 0;
}

static void pm8921_update_at_resume(struct pm8921_chg_chip *chip)
{
	struct task_struct *th;

	th = kthread_create(pm8921_update_thread, chip, "pm8921_update_thread");
	if (IS_ERR(th)) {
		pr_err("failed to create kthread\n");
		goto err_out;
	}
	wake_up_process(th);
err_out:
	return;
}

static int pm8921_charger_resume(struct device *dev)
{
	int rc;
	struct pm8921_chg_chip *chip = dev_get_drvdata(dev);

	if (!(chip->cool_temp_dc == INT_MIN && chip->warm_temp_dc == INT_MIN)
		&& !(chip->is_temp_control)
		&& !(chip->keep_btm_on_suspend)) {
		rc = pm8xxx_adc_btm_configure(&btm_config);
		if (rc)
			pr_err("couldn't reconfigure btm rc=%d\n", rc);

		rc = pm8xxx_adc_btm_start();
		if (rc)
			pr_err("couldn't restart btm rc=%d\n", rc);
	}
	if (pm8921_chg_is_enabled(chip, LOOP_CHANGE_IRQ)) {
		disable_irq_wake(chip->pmic_chg_irq[LOOP_CHANGE_IRQ]);
		pm8921_chg_disable_irq(chip, LOOP_CHANGE_IRQ);
	}

	/* update soc and power supply at resume */
	pm8921_update_at_resume(chip);

	return 0;
}

static int pm8921_charger_suspend(struct device *dev)
{
	int rc;
	struct pm8921_chg_chip *chip = dev_get_drvdata(dev);

	if (!(chip->cool_temp_dc == INT_MIN && chip->warm_temp_dc == INT_MIN)
		&& !(chip->is_temp_control)
		&& !(chip->keep_btm_on_suspend)) {
		rc = pm8xxx_adc_btm_end();
		if (rc)
			pr_err("Failed to disable BTM on suspend rc=%d\n", rc);
	}

	if (is_usb_chg_plugged_in(chip)) {
		pm8921_chg_enable_irq(chip, LOOP_CHANGE_IRQ);
		enable_irq_wake(chip->pmic_chg_irq[LOOP_CHANGE_IRQ]);
	}

	return 0;
}
static int __devinit pm8921_charger_probe(struct platform_device *pdev)
{
	int rc = 0;
	struct pm8921_chg_chip *chip;
	const struct pm8921_charger_platform_data *pdata
				= pdev->dev.platform_data;

	if (!pdata) {
		pr_err("missing platform data\n");
		return -EINVAL;
	}

	chip = kzalloc(sizeof(struct pm8921_chg_chip),
					GFP_KERNEL);
	if (!chip) {
		pr_err("Cannot allocate pm_chg_chip\n");
		return -ENOMEM;
	}

	chip->dev = &pdev->dev;
	chip->safety_time = pdata->safety_time;
	chip->ttrkl_time = pdata->ttrkl_time;
	chip->default_update_time = pdata->update_time;
	chip->low_bat_update_time = pdata->update_time_at_low_bat;
	chip->max_voltage_mv = pdata->max_voltage;
	chip->min_voltage_mv = pdata->min_voltage;
	chip->resume_voltage_delta = pdata->resume_voltage_delta;
	chip->resume_soc = pdata->resume_soc;
	chip->term_current = pdata->term_current;
	chip->vbat_channel = pdata->charger_cdata.vbat_channel;
	chip->batt_temp_channel = pdata->charger_cdata.batt_temp_channel;
	chip->batt_id_channel = pdata->charger_cdata.batt_id_channel;
	chip->batt_id_min = pdata->batt_id_min;
	chip->batt_id_max = pdata->batt_id_max;
	chip->ibat_calib_enable = pdata->ibat_calib_enable;
	if (pdata->cool_temp != INT_MIN)
		chip->cool_temp_dc = pdata->cool_temp * 10;
	else
		chip->cool_temp_dc = INT_MIN;

	if (pdata->warm_temp != INT_MIN)
		chip->warm_temp_dc = pdata->warm_temp * 10;
	else
		chip->warm_temp_dc = INT_MIN;

	chip->cold_temp_dc = pdata->cold_temp * 10;
	chip->hot_temp_dc = pdata->hot_temp * 10;
	chip->hysterisis_temp_dc = pdata->hysterisis_temp * 10;
	chip->temp_check_period = pdata->temp_check_period;
	chip->safe_current = pdata->safe_current;
	chip->dc_unplug_check = pdata->dc_unplug_check;
	chip->max_bat_chg_current = pdata->max_bat_chg_current;
	chip->cool_bat_chg_current = pdata->cool_bat_chg_current;
	chip->warm_bat_chg_current = pdata->warm_bat_chg_current;
	chip->cool_bat_voltage = pdata->cool_bat_voltage;
	chip->warm_bat_voltage = pdata->warm_bat_voltage;
	chip->keep_btm_on_suspend = pdata->keep_btm_on_suspend;
	chip->trkl_voltage = pdata->trkl_voltage;
	chip->weak_voltage = pdata->weak_voltage;
	chip->trkl_current = pdata->trkl_current;
	chip->weak_current = pdata->weak_current;
	chip->vin_min = pdata->vin_min;
	chip->thermal_mitigation = pdata->thermal_mitigation;
	chip->thermal_levels = pdata->thermal_levels;

	chip->cold_thr = pdata->cold_thr;
	chip->hot_thr = pdata->hot_thr;
	chip->rconn_mohm = pdata->rconn_mohm;

	rc = pm8921_chg_hw_init(chip);
	if (rc) {
		pr_err("couldn't init hardware rc=%d\n", rc);
		goto free_chip;
	}

	rc = get_prop_batt_capacity(chip);
	if (rc >= 0)
		chip->soc = rc;

	chip->usb_psy.name = "usb",
	chip->usb_psy.type = POWER_SUPPLY_TYPE_USB,
	chip->usb_psy.supplied_to = pm_power_supplied_to,
	chip->usb_psy.num_supplicants = ARRAY_SIZE(pm_power_supplied_to),
	chip->usb_psy.properties = pm_power_props_usb,
	chip->usb_psy.num_properties = ARRAY_SIZE(pm_power_props_usb),
	chip->usb_psy.get_property = pm_power_get_property_usb,

	chip->dc_psy.name = "pm8921-dc",
	chip->dc_psy.type = POWER_SUPPLY_TYPE_MAINS,
	chip->dc_psy.supplied_to = pm_power_supplied_to,
	chip->dc_psy.num_supplicants = ARRAY_SIZE(pm_power_supplied_to),
	chip->dc_psy.properties = pm_power_props_mains,
	chip->dc_psy.num_properties = ARRAY_SIZE(pm_power_props_mains),
	chip->dc_psy.get_property = pm_power_get_property_mains,

	chip->batt_psy.name = "battery",
	chip->batt_psy.type = POWER_SUPPLY_TYPE_BATTERY,
	chip->batt_psy.properties = msm_batt_power_props,
	chip->batt_psy.num_properties = ARRAY_SIZE(msm_batt_power_props),
	chip->batt_psy.get_property = pm_batt_power_get_property,
	chip->batt_psy.external_power_changed = pm_batt_external_power_changed,
	rc = power_supply_register(chip->dev, &chip->usb_psy);
	if (rc < 0) {
		pr_err("power_supply_register usb failed rc = %d\n", rc);
		goto free_chip;
	}

	rc = power_supply_register(chip->dev, &chip->dc_psy);
	if (rc < 0) {
		pr_err("power_supply_register usb failed rc = %d\n", rc);
		goto unregister_usb;
	}

	rc = power_supply_register(chip->dev, &chip->batt_psy);
	if (rc < 0) {
		pr_err("power_supply_register batt failed rc = %d\n", rc);
		goto unregister_dc;
	}

#ifdef CONFIG_SWITCH
	chip->swdev.name = "dock";
	rc = switch_dev_register(&chip->swdev);
	if (rc < 0) {
		pr_err("switch_dev_register failed rc = %d\n", rc);
		goto unregister_batt;
	}
#endif /* CONFIG_SWITCH */

	platform_set_drvdata(pdev, chip);
	the_chip = chip;

	wake_lock_init(&chip->eoc_wake_lock, WAKE_LOCK_SUSPEND, "pm8921_eoc");
	wake_lock_init(&chip->low_voltage_wake_lock,
			WAKE_LOCK_SUSPEND, "pm8921_low_voltage");
#ifdef CONFIG_SWITCH
	wake_lock_init(&chip->dock_wake_lock, WAKE_LOCK_SUSPEND, "pm8921_dock");
	spin_lock_init(&chip->dock_lock);
#endif /* CONFIG_SWITCH */
	INIT_DELAYED_WORK(&chip->eoc_work, eoc_worker);
	INIT_DELAYED_WORK(&chip->vin_collapse_check_work,
						vin_collapse_check_worker);
	INIT_DELAYED_WORK(&chip->unplug_check_work, unplug_check_worker);
	INIT_DELAYED_WORK(&chip->delayed_notify_work, delayed_notify_worker);
	INIT_DELAYED_WORK(&chip->update_heartbeat_work, update_heartbeat);

	INIT_WORK(&chip->bms_notify.work, bms_notify);
	INIT_WORK(&chip->battery_id_valid_work, battery_id_valid);

	rc = request_irqs(chip, pdev);
	if (rc) {
		pr_err("couldn't register interrupts rc=%d\n", rc);
#ifdef CONFIG_SWITCH
		goto unregister_swdev;
#else
		goto unregister_batt;
#endif /* CONFIG_SWITCH */
	}

	enable_irq_wake(chip->pmic_chg_irq[USBIN_VALID_IRQ]);
	enable_irq_wake(chip->pmic_chg_irq[USBIN_OV_IRQ]);
	enable_irq_wake(chip->pmic_chg_irq[USBIN_UV_IRQ]);
	enable_irq_wake(chip->pmic_chg_irq[VBATDET_LOW_IRQ]);
	enable_irq_wake(chip->pmic_chg_irq[FASTCHG_IRQ]);
	enable_irq_wake(chip->pmic_chg_irq[DCIN_VALID_IRQ]);
	enable_irq_wake(chip->pmic_chg_irq[DCIN_OV_IRQ]);
	enable_irq_wake(chip->pmic_chg_irq[DCIN_UV_IRQ]);
	/*
	 * if both the cool_temp_dc and warm_temp_dc are invalid device doesnt
	 * care for jeita compliance
	 */
	if ((chip->cold_temp_dc == 0 && chip->hot_temp_dc == 0)
		&& !(chip->cool_temp_dc == INT_MIN
			&& chip->warm_temp_dc == INT_MIN)) {
		rc = configure_btm(chip);
		if (rc) {
			pr_err("couldn't register with btm rc=%d\n", rc);
			goto free_irq;
		}
	}

	create_debugfs_entries(chip);
	rc = create_sysfs_entries(chip);
	if (rc < 0)
		pr_err("sysfs create failed rc=%d\n", rc);

	/* determine what state the charger is in */
	determine_initial_state(chip);

	if (!(chip->cold_temp_dc == 0 && chip->hot_temp_dc == 0))
		configure_temp_control(chip);

	/*
	 * set default heartbeat update interval
	 * schedule the work with specified time
	 */
	if (chip->default_update_time)
		set_heartbeat_update_interval(chip, chip->default_update_time);

	rc = pm8921_set_batt_alarm(chip);
	if (rc) {
		pr_err("couldn't set battery alarm rc=%d\n", rc);
		goto free_irq;
	}
	return 0;

free_irq:
	free_irqs(chip);
#ifdef CONFIG_SWITCH
unregister_swdev:
	switch_dev_unregister(&chip->swdev);
#endif /* CONFIG_SWITCH */
unregister_batt:
	power_supply_unregister(&chip->batt_psy);
unregister_dc:
	power_supply_unregister(&chip->dc_psy);
unregister_usb:
	power_supply_unregister(&chip->usb_psy);
free_chip:
	kfree(chip);
	return rc;
}

static int __devexit pm8921_charger_remove(struct platform_device *pdev)
{
	struct pm8921_chg_chip *chip = platform_get_drvdata(pdev);
	int rc, rc_notify;

	remove_sysfs_entries(chip);
	free_irqs(chip);
#ifdef CONFIG_SWITCH
	switch_dev_unregister(&chip->swdev);
	wake_lock_destroy(&chip->dock_wake_lock);
#endif /* CONFIG_SWITCH */
	platform_set_drvdata(pdev, NULL);
	the_chip = NULL;
	kfree(chip);

	rc = pm8xxx_batt_alarm_disable(
			PM8XXX_BATT_ALARM_LOWER_COMPARATOR);
	if (rc)
		pr_err("%s: unable to set batt alarm Lower state. %d\n",
				__func__, rc);
	else {
		rc = pm8xxx_batt_alarm_disable(
			PM8XXX_BATT_ALARM_UPPER_COMPARATOR);
		if (rc)
			pr_err("%s: unable to set batt alarm Upper state. %d\n",
					__func__, rc);
	}

	rc_notify = pm8xxx_batt_alarm_unregister_notifier(&alarm_notifier);
	if (rc_notify)
		pr_err("%s: unable to register alarm notifier. rc is %d\n",
			__func__, rc_notify);

	return rc ? rc : rc_notify;
}
static const struct dev_pm_ops pm8921_pm_ops = {
	.suspend	= pm8921_charger_suspend,
	.suspend_noirq  = pm8921_charger_suspend_noirq,
	.resume_noirq   = pm8921_charger_resume_noirq,
	.resume		= pm8921_charger_resume,
};
static struct platform_driver pm8921_charger_driver = {
	.probe		= pm8921_charger_probe,
	.remove		= __devexit_p(pm8921_charger_remove),
	.driver		= {
			.name	= PM8921_CHARGER_DEV_NAME,
			.owner	= THIS_MODULE,
			.pm	= &pm8921_pm_ops,
	},
};

static int __init pm8921_charger_init(void)
{
	return platform_driver_register(&pm8921_charger_driver);
}

static void __exit pm8921_charger_exit(void)
{
	platform_driver_unregister(&pm8921_charger_driver);
}

late_initcall(pm8921_charger_init);
module_exit(pm8921_charger_exit);

MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("PMIC8921 charger/battery driver");
MODULE_VERSION("1.0");
MODULE_ALIAS("platform:" PM8921_CHARGER_DEV_NAME);
