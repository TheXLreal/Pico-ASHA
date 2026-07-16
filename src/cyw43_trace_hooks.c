#include "audio_stall_trace.h"

#ifdef PICO_ASHA_AUDIO_STALL_TRACE

#include <stdatomic.h>
#include <stdint.h>

#include <pico/multicore.h>
#include <pico/time.h>

extern int __real_cyw43_bluetooth_hci_write(uint8_t *buf, size_t len);
extern void __real_cyw43_thread_enter(void);
extern void __real_cyw43_thread_exit(void);

static atomic_uint lock_depth[2];
static uint64_t outer_lock_acquired_us[2];

int __wrap_cyw43_bluetooth_hci_write(uint8_t *buf, size_t len)
{
    uint64_t begin_us = time_us_64();
    int result = __real_cyw43_bluetooth_hci_write(buf, len);
    uint64_t end_us = time_us_64();
    uint8_t packet_type = len >= 4u ? buf[3] : 0u;
    audio_stall_trace_hci_write(begin_us, end_us, packet_type, result);
    return result;
}

void __wrap_cyw43_thread_enter(void)
{
    uint64_t begin_us = time_us_64();
    __real_cyw43_thread_enter();
    uint64_t acquired_us = time_us_64();
    uint32_t core = get_core_num() & 1u;
    if (atomic_fetch_add_explicit(&lock_depth[core], 1u,
                                  memory_order_relaxed) == 0u) {
        outer_lock_acquired_us[core] = acquired_us;
    }
    audio_stall_trace_cyw43_lock_acquired(begin_us, acquired_us);
}

void __wrap_cyw43_thread_exit(void)
{
    uint32_t core = get_core_num() & 1u;
    uint32_t depth = atomic_load_explicit(&lock_depth[core], memory_order_relaxed);
    bool outer = depth == 1u;
    uint64_t acquired_us = outer_lock_acquired_us[core];
    if (depth > 0u) {
        atomic_fetch_sub_explicit(&lock_depth[core], 1u, memory_order_relaxed);
    }
    __real_cyw43_thread_exit();
    if (outer) {
        audio_stall_trace_cyw43_lock_released(acquired_us, time_us_64());
    }
}

#endif
