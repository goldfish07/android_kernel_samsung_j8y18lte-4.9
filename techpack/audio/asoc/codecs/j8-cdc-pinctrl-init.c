// SPDX-License-Identifier: GPL-2.0
/* The PMIC codec needs pinctrl without the unrelated external WCD core. */
#include <linux/init.h>
#include <linux/module.h>
#include "msm-cdc-pinctrl.h"

static int __init j8_cdc_pinctrl_init(void)
{
	return msm_cdc_pinctrl_drv_init();
}
module_init(j8_cdc_pinctrl_init);

static void __exit j8_cdc_pinctrl_exit(void)
{
	msm_cdc_pinctrl_drv_exit();
}
module_exit(j8_cdc_pinctrl_exit);

MODULE_LICENSE("GPL v2");
