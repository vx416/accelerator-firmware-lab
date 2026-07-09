#include "virtual_device_internal.h"

#include <linux/string.h>

void kvdev_trace_locked(struct afl_kernel_vdev *vdev, afl_ioctl_u32_t event, afl_ioctl_u32_t opcode, afl_ioctl_u32_t status, afl_ioctl_u32_t error)
{
    struct afl_ioctl_trace_entry *entry = &vdev->trace_entries[vdev->trace_head];

    entry->cycle = vdev->cycle_counter;
    entry->event = event;
    entry->opcode = opcode;
    entry->status = status;
    entry->error_code = error;

    vdev->trace_head = (vdev->trace_head + 1u) % AFL_IOCTL_TRACE_ENTRY_COUNT;
    if (vdev->trace_count < AFL_IOCTL_TRACE_ENTRY_COUNT)
        vdev->trace_count++;
    else
        vdev->trace_dropped_count++;
    kvdev_refresh_counter_registers(vdev);
}

void afl_kvdev_get_trace(struct afl_kernel_vdev *vdev, struct afl_ioctl_trace *trace)
{
    afl_ioctl_u32_t first;
    afl_ioctl_u32_t i;

    memset(trace, 0, sizeof(*trace));

    mutex_lock(&vdev->lock);
    trace->count = vdev->trace_count;
    trace->dropped_count = vdev->trace_dropped_count;
    first = (vdev->trace_head + AFL_IOCTL_TRACE_ENTRY_COUNT - vdev->trace_count) % AFL_IOCTL_TRACE_ENTRY_COUNT;
    for (i = 0; i < vdev->trace_count; ++i)
        trace->entries[i] = vdev->trace_entries[(first + i) % AFL_IOCTL_TRACE_ENTRY_COUNT];
    mutex_unlock(&vdev->lock);
}
