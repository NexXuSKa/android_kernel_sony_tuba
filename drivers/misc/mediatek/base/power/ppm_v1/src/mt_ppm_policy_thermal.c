
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <asm/uaccess.h>
#include <asm/topology.h>

#include "mt_ppm_internal.h"
#include "mt_cpufreq.h"


static void ppm_thermal_update_limit_cb(enum ppm_power_state new_state);
static void ppm_thermal_status_change_cb(bool enable);
static void ppm_thermal_mode_change_cb(enum ppm_mode mode);

/* other members will init by ppm_main */
static struct ppm_policy_data thermal_policy = {
	.name			= __stringify(PPM_POLICY_THERMAL),
	.lock			= __MUTEX_INITIALIZER(thermal_policy.lock),
	.policy			= PPM_POLICY_THERMAL,
	.priority		= PPM_POLICY_PRIO_POWER_BUDGET_BASE,
	.get_power_state_cb	= NULL,	/* decide in ppm main via min power budget */
	.update_limit_cb	= ppm_thermal_update_limit_cb,
	.status_change_cb	= ppm_thermal_status_change_cb,
	.mode_change_cb		= ppm_thermal_mode_change_cb,
};

void mt_ppm_cpu_thermal_protect(unsigned int limited_power)
{
	FUNC_ENTER(FUNC_LV_POLICY);

	ppm_info("Get budget from thermal => limited_power = %d\n", limited_power);

	ppm_lock(&thermal_policy.lock);

	if (!thermal_policy.is_enabled) {
		ppm_warn("@%s: thermal policy is not enabled!\n", __func__);
		ppm_unlock(&thermal_policy.lock);
		goto end;
	}

	thermal_policy.req.power_budget = limited_power;
	thermal_policy.is_activated = (limited_power) ? true : false;
	ppm_unlock(&thermal_policy.lock);
	ppm_task_wakeup();

end:
	FUNC_EXIT(FUNC_LV_POLICY);
}

unsigned int mt_ppm_thermal_get_min_power(void)
{
	struct ppm_power_tbl_data power_table = ppm_get_power_table();
	unsigned int size = power_table.nr_power_tbl;

	return power_table.power_tbl[size - 1].power_idx;
}

unsigned int mt_ppm_thermal_get_max_power(void)
{
	return ppm_get_power_table().power_tbl[0].power_idx;
}

unsigned int mt_ppm_thermal_get_cur_power(void)
{
	struct ppm_cluster_status *cluster_status;
	struct cpumask cluster_cpu, online_cpu;
	int i;
#if 0 /* PPM_DLPT_ENHANCEMENT */
	unsigned int power;
#else
	int power;
#endif

	cluster_status = kcalloc(ppm_main_info.cluster_num, sizeof(*cluster_status), GFP_KERNEL);
	if (!cluster_status)
		return mt_ppm_thermal_get_max_power();

	for_each_ppm_clusters(i) {
		arch_get_cluster_cpus(&cluster_cpu, i);
		cpumask_and(&online_cpu, &cluster_cpu, cpu_online_mask);

		cluster_status[i].core_num = cpumask_weight(&online_cpu);
		cluster_status[i].volt = 0;	/* don't care */
		if (!cluster_status[i].core_num)
			cluster_status[i].freq_idx = -1;
		else
			cluster_status[i].freq_idx = ppm_main_freq_to_idx(i,
					mt_cpufreq_get_cur_phy_freq(i), CPUFREQ_RELATION_L);

		ppm_ver("[%d] core = %d, freq_idx = %d\n",
			i, cluster_status[i].core_num, cluster_status[i].freq_idx);
	}

#if 0 /* PPM_DLPT_ENHANCEMENT */
	power = ppm_calc_total_power(cluster_status, ppm_main_info.cluster_num, 100);
	kfree(cluster_status);
	return (power == 0) ? mt_ppm_thermal_get_max_power() : power;
#else
	power = ppm_find_pwr_idx(cluster_status);
	kfree(cluster_status);
	return (power == -1) ? mt_ppm_thermal_get_max_power() : (unsigned int)power;
#endif
}

static void ppm_thermal_update_limit_cb(enum ppm_power_state new_state)
{
	FUNC_ENTER(FUNC_LV_POLICY);

	ppm_ver("@%s: thermal policy update limit for new state = %s\n",
		__func__, ppm_get_power_state_name(new_state));

	ppm_hica_set_default_limit_by_state(new_state, &thermal_policy);

	/* update limit according to power budget */
	ppm_main_update_req_by_pwr(new_state, &thermal_policy.req);

	FUNC_EXIT(FUNC_LV_POLICY);
}

static void ppm_thermal_status_change_cb(bool enable)
{
	FUNC_ENTER(FUNC_LV_POLICY);

	ppm_ver("@%s: thermal policy status changed to %d\n", __func__, enable);

	FUNC_EXIT(FUNC_LV_POLICY);
}

static void ppm_thermal_mode_change_cb(enum ppm_mode mode)
{
	FUNC_ENTER(FUNC_LV_POLICY);

	ppm_ver("@%s: ppm mode changed to %d\n", __func__, mode);

	FUNC_EXIT(FUNC_LV_POLICY);
}

static int ppm_thermal_limit_proc_show(struct seq_file *m, void *v)
{
	seq_printf(m, "limited power = %d\n", thermal_policy.req.power_budget);
	seq_printf(m, "PPM thermal activate = %d\n", thermal_policy.is_activated);

	return 0;
}

static ssize_t ppm_thermal_limit_proc_write(struct file *file, const char __user *buffer,
					size_t count, loff_t *pos)
{
	unsigned int limited_power;

	char *buf = ppm_copy_from_user_for_proc(buffer, count);

	if (!buf)
		return -EINVAL;

	if (!kstrtouint(buf, 10, &limited_power))
		mt_ppm_cpu_thermal_protect(limited_power);
	else
		ppm_err("@%s: Invalid input!\n", __func__);

	free_page((unsigned long)buf);
	return count;
}

static int ppm_thermal_cur_power_proc_show(struct seq_file *m, void *v)
{
	seq_printf(m, "current power = %d\n", mt_ppm_thermal_get_cur_power());
	seq_printf(m, "min power = %d\n", mt_ppm_thermal_get_min_power());
	seq_printf(m, "max power = %d\n", mt_ppm_thermal_get_max_power());

	return 0;
}

PROC_FOPS_RW(thermal_limit);
PROC_FOPS_RO(thermal_cur_power);

static int __init ppm_thermal_policy_init(void)
{
	int i, ret = 0;

	struct pentry {
		const char *name;
		const struct file_operations *fops;
	};

	const struct pentry entries[] = {
		PROC_ENTRY(thermal_limit),
		PROC_ENTRY(thermal_cur_power),
	};

	FUNC_ENTER(FUNC_LV_POLICY);

	/* create procfs */
	for (i = 0; i < ARRAY_SIZE(entries); i++) {
		if (!proc_create(entries[i].name, S_IRUGO | S_IWUSR | S_IWGRP, policy_dir, entries[i].fops)) {
			ppm_err("%s(), create /proc/ppm/policy/%s failed\n", __func__, entries[i].name);
			ret = -EINVAL;
			goto out;
		}
	}

	if (ppm_main_register_policy(&thermal_policy)) {
		ppm_err("@%s: thermal policy register failed\n", __func__);
		ret = -EINVAL;
		goto out;
	}

	ppm_info("@%s: register %s done!\n", __func__, thermal_policy.name);

out:
	FUNC_EXIT(FUNC_LV_POLICY);

	return ret;
}

static void __exit ppm_thermal_policy_exit(void)
{
	FUNC_ENTER(FUNC_LV_POLICY);

	ppm_main_unregister_policy(&thermal_policy);

	FUNC_EXIT(FUNC_LV_POLICY);
}

module_init(ppm_thermal_policy_init);
module_exit(ppm_thermal_policy_exit);

