/* Temporary J8 bring-up: bounded, read-only firmware CPU counters. */
#ifndef _J8_TZ_SNAPSHOT_H
#define _J8_TZ_SNAPSHOT_H

struct j8_tz_header {
	u32 magic, version, cpu_count, vmid_off, boot_off, reset_off;
	u32 interrupt_off, ring_off, ring_length, wakeup_off;
};

struct j8_tz_snapshot {
	u32 version, cpu_count;
	/* Warmboot, collapse, PSCI entry/exit, reset type/count; no addresses. */
	u32 counters[8][8];
};

static bool j8_tz_read_snapshot(const void __iomem *base, size_t size,
			      u32 qsee_version, struct j8_tz_snapshot *out)
{
	struct j8_tz_header h;
	size_t stride, count_bytes;
	unsigned int cpu;

	memset(out, 0, sizeof(*out));
	if (size < sizeof(h))
		return false;
	memcpy_fromio(&h, base, sizeof(h));
	if (h.magic != 0x747a6461 || !h.cpu_count || h.cpu_count > 8 ||
	    (h.version >> 16) < 2 || (h.version >> 16) > 8)
		return false;
	stride = qsee_version >= 0x800000 ? 40 : 24;
	count_bytes = qsee_version >= 0x800000 ? 24 : 16;
	if (h.boot_off < sizeof(h) || h.boot_off % 4 ||
	    h.reset_off < sizeof(h) || h.reset_off % 4 ||
	    h.boot_off > size || h.reset_off > size ||
	    h.cpu_count > (size - h.boot_off) / stride ||
	    h.cpu_count > (size - h.reset_off) / 8)
		return false;

	out->version = h.version;
	out->cpu_count = h.cpu_count;
	for (cpu = 0; cpu < h.cpu_count; cpu++) {
		memcpy_fromio(out->counters[cpu], base + h.boot_off + cpu * stride,
			      count_bytes);
		memcpy_fromio(out->counters[cpu] + 6,
			      base + h.reset_off + cpu * 8, 8);
	}
	return true;
}
#endif
