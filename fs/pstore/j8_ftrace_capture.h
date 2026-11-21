/* Temporary J8 diagnostic: filtered function entries retained across reset. */
#ifndef _J8_FTRACE_CAPTURE_H
#define _J8_FTRACE_CAPTURE_H

#define J8_PSTORE_START (50UL * HZ)
#define J8_PSTORE_STOP (300UL * HZ)

static bool j8_pstore_capture_owned;
static void j8_pstore_capture_start(struct work_struct *work);
static void j8_pstore_capture_stop(struct work_struct *work);
static DECLARE_DELAYED_WORK(j8_pstore_start_work, j8_pstore_capture_start);
static DECLARE_DELAYED_WORK(j8_pstore_stop_work, j8_pstore_capture_stop);

static void j8_pstore_capture_schedule(void)
{
	unsigned long elapsed = jiffies - INITIAL_JIFFIES;

	if (!IS_ENABLED(CONFIG_SEC_J8Y18LTE_PROJECT) ||
	    strcmp(psinfo->name, "ramoops") || elapsed >= J8_PSTORE_STOP)
		return;
	schedule_delayed_work(&j8_pstore_start_work,
		elapsed < J8_PSTORE_START ? J8_PSTORE_START - elapsed : 0);
}

static void j8_pstore_capture_start(struct work_struct *work)
{
	static const char * const patterns[] = {
		"kgsl*", "adreno*", "*iommu*", "*smmu*", "clk_*",
		"*regulator*", "scm_*", "__scm_*", "*qseecom*",
		"*dwc3*", "*qusb*", "mdss*", "msm_cpufreq*",
		"msm_pm*", "*hyp_assign*", "watchdog_kthread",
	};
	unsigned long elapsed = jiffies - INITIAL_JIFFIES;
	unsigned int i;
	int ret;

	if (!IS_ENABLED(CONFIG_SEC_J8Y18LTE_PROJECT) || elapsed >= J8_PSTORE_STOP)
		return;
	mutex_lock(&pstore_ftrace_lock);
	if (pstore_ftrace_enabled)
		goto out;
	/* An empty filter traces everything: never register after a failure. */
	for (i = 0; i < ARRAY_SIZE(patterns); i++) {
		char pattern[32];

		strlcpy(pattern, patterns[i], sizeof(pattern));
		ret = ftrace_set_filter(&pstore_ftrace_ops, pattern,
					strlen(pattern), i == 0);
		if (ret) {
			pr_err("J8_PSTORE_TRACE filter=%s failed=%d; capture disabled\n",
			       patterns[i], ret);
			goto out;
		}
	}
	ret = register_ftrace_function(&pstore_ftrace_ops);
	if (ret) {
		pr_err("J8_PSTORE_TRACE register failed=%d\n", ret);
		goto out;
	}
	pstore_ftrace_enabled = true;
	j8_pstore_capture_owned = true;
	elapsed = jiffies - INITIAL_JIFFIES;
	schedule_delayed_work(&j8_pstore_stop_work,
		elapsed < J8_PSTORE_STOP ? J8_PSTORE_STOP - elapsed : 0);
	pr_err("J8_PSTORE_TRACE enabled patterns=%u stop_at=300s\n",
	       (unsigned int) ARRAY_SIZE(patterns));
out:
	mutex_unlock(&pstore_ftrace_lock);
}

static void j8_pstore_capture_stop(struct work_struct *work)
{
	int ret;

	mutex_lock(&pstore_ftrace_lock);
	if (j8_pstore_capture_owned && pstore_ftrace_enabled) {
		ret = unregister_ftrace_function(&pstore_ftrace_ops);
		if (!ret) {
			pstore_ftrace_enabled = false;
			pr_err("J8_PSTORE_TRACE stopped\n");
		} else {
			pr_err("J8_PSTORE_TRACE stop failed=%d\n", ret);
		}
	}
	j8_pstore_capture_owned = false;
	mutex_unlock(&pstore_ftrace_lock);
}

static void j8_pstore_capture_cancel(void)
{
	cancel_delayed_work_sync(&j8_pstore_start_work);
	cancel_delayed_work_sync(&j8_pstore_stop_work);
}
#endif
