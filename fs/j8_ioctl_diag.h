/* SPDX-License-Identifier: GPL-2.0 */
#ifndef J8_IOCTL_DIAG_H
#define J8_IOCTL_DIAG_H
#include <linux/atomic.h>
#include <linux/jiffies.h>
#include <linux/sched.h>
#include <linux/string.h>
static atomic_t j8_ioctl_sequence = ATOMIC_INIT(0);
static inline unsigned int j8_ioctl_begin(struct file *file, unsigned int cmd,
                                          bool compat)
{
    unsigned int seq;
    unsigned long elapsed = jiffies - INITIAL_JIFFIES;
    const char *node;
    if (!IS_ENABLED(CONFIG_SEC_J8Y18LTE_PROJECT) ||
        elapsed < 35 * HZ || elapsed > 1800 * HZ ||
        strcmp(current->comm, "ImageManager"))
        return 0;
    seq = atomic_inc_return(&j8_ioctl_sequence);
    if (seq > 2000)
        return 0;
    /* Do not log userspace buffers, addresses, or media filenames. */
    node = S_ISCHR(file_inode(file)->i_mode) ?
        (const char *)file->f_path.dentry->d_name.name : "<other>";
    pr_err("J8_IOCTL begin seq=%u cmd=0x%x compat=%u node=%s pid=%d cpu=%u\n",
        seq, cmd, compat, node, current->pid, raw_smp_processor_id());
    return seq;
}
static inline void j8_ioctl_end(unsigned int seq, long ret)
{
    if (seq)
        pr_err("J8_IOCTL end seq=%u ret=%ld pid=%d cpu=%u\n",
            seq, ret, current->pid, raw_smp_processor_id());
}
#endif
