#include "audio_stall_trace.h"

#ifdef PICO_ASHA_AUDIO_STALL_TRACE

#include <limits.h>
#include <stdatomic.h>

#include <pico/multicore.h>
#include <pico/time.h>
#include <hardware/structs/watchdog.h>
#include <hardware/watchdog.h>

#include "asha_audio.h"

#define TRACE_RING_SIZE 256u
#define TRACE_RING_MASK (TRACE_RING_SIZE - 1u)
#define TRACE_CONSUMER_SLOTS 2u
#define TRACE_CONNECTION_SLOTS 2u
#define TRACE_BOOT_SESSION_MAGIC 0x41535452u

#ifndef PICO_ASHA_FW_VERS_MAJOR
#define PICO_ASHA_FW_VERS_MAJOR 0u
#define PICO_ASHA_FW_VERS_MINOR 0u
#define PICO_ASHA_FW_VERS_PATCH 0u
#endif

_Static_assert(ATOMIC_INT_LOCK_FREE == 2, "Trace ring indices must be lock-free");

typedef struct TraceRing {
    atomic_uint write_index;
    atomic_uint read_index;
    AudioStallTraceRecord records[TRACE_RING_SIZE];
} TraceRing;

typedef struct TraceConnectionState {
    atomic_uint_least32_t handle;
    atomic_uint_least32_t busy;
    atomic_uint_least32_t busy_since_us;
    atomic_uint_least32_t last_sequence;
    atomic_bool have_sequence;
    atomic_int_least32_t rssi_dbm;
    atomic_uint_least32_t rssi_sample_us;
    atomic_bool have_rssi;
} TraceConnectionState;

typedef struct TraceSduGenerationState {
    atomic_uint_least32_t write_index;
    atomic_uint_least32_t generated_us;
} TraceSduGenerationState;

typedef struct TraceSequenceSkip {
    uint8_t previous_sequence;
    uint32_t skipped_frames;
    uint32_t total_skipped_frames;
} TraceSequenceSkip;

typedef struct TraceCounters {
    atomic_uint_least32_t sdu_generated_count;
    atomic_uint_least32_t l2cap_send_attempt_count;
    atomic_uint_least32_t l2cap_send_success_count;
    atomic_uint_least32_t l2cap_send_error_count;
    atomic_uint_least32_t can_send_wait_last_us;
    atomic_uint_least32_t can_send_wait_max_us;
    atomic_uint_least32_t packet_sent_wait_max_us;
    atomic_uint_least32_t audio_busy_max_duration_us;
    atomic_uint_least32_t ring_fill_current;
    atomic_uint_least32_t ring_fill_min;
    atomic_uint_least32_t ring_fill_max;
    atomic_uint_least32_t ring_underrun_count;
    atomic_uint_least32_t ring_overrun_count;
    atomic_uint_least32_t sequence_generated;
    atomic_uint_least32_t sequence_sent;
    atomic_uint_least32_t sequence_skip_count;
    atomic_uint_least32_t audio_timer_gap_max_us;
    atomic_uint_least32_t audio_timer_late_count;
    atomic_uint_least32_t usb_pcm_underrun_count;
    atomic_uint_least32_t trace_dropped;
} TraceCounters;

typedef struct TraceRuntimeCounters {
    atomic_uint_least32_t hci_write_count;
    atomic_uint_least32_t hci_write_error_count;
    atomic_uint_least32_t hci_write_last_us;
    atomic_uint_least32_t hci_write_max_us;
    atomic_uint_least32_t cyw43_lock_wait_last_us;
    atomic_uint_least32_t cyw43_lock_wait_max_us;
    atomic_uint_least32_t cyw43_lock_hold_last_us;
    atomic_uint_least32_t cyw43_lock_hold_max_us;
    atomic_uint_least32_t btstack_run_loop_gap_last_us;
    atomic_uint_least32_t btstack_run_loop_gap_max_us;
    atomic_uint_least32_t hci_to_packet_sent_last_us;
    atomic_uint_least32_t hci_to_packet_sent_max_us;
    atomic_uint_least32_t rssi_sample_count;
    atomic_uint_least32_t rssi_request_skipped_count;
    atomic_uint_least32_t tx_stale_drop_count;
    atomic_uint_least32_t tx_stale_drop_frames;
    atomic_uint_least32_t core1_run_loop_count;
    atomic_uint_least32_t core1_run_loop_last_us;
    atomic_uint_least32_t process_audio_enter_count;
    atomic_uint_least32_t process_audio_enter_last_us;
    atomic_uint_least32_t hci_transport_progress_count;
    atomic_uint_least32_t hci_transport_progress_last_us;
    atomic_uint_least32_t hci_controller_progress_count;
    atomic_uint_least32_t hci_controller_progress_last_us;
} TraceRuntimeCounters;

static TraceRing trace_rings[2];
static TraceCounters counters;
static TraceRuntimeCounters runtime_counters;
static atomic_uint_least32_t consumer_read_index[TRACE_CONSUMER_SLOTS];
static atomic_bool consumer_active[TRACE_CONSUMER_SLOTS];
static TraceConnectionState connection_state[TRACE_CONNECTION_SLOTS];
static atomic_uint_least32_t last_sdu_generated_us;
static atomic_uint_least32_t last_audio_timer_us;
static atomic_uint_least32_t last_acl_hci_write_end_us;
static uint32_t last_core1_heartbeat_event_us;
static uint32_t last_process_audio_event_us;
static uint32_t last_g722_integrity_event_us[4];
static TraceSduGenerationState sdu_generation[ASHA_G722_RING_SIZE];
static uint32_t boot_session_id;

static uint32_t saturating_us(uint64_t value)
{
    return value > UINT32_MAX ? UINT32_MAX : (uint32_t)value;
}

static uint32_t timestamp_age_us(uint32_t now_us, uint32_t last_us)
{
    if (last_us == 0u) return UINT32_MAX;
    uint32_t age_us = now_us - last_us;
    /* The caller's snapshot timestamp can precede a concurrent core's latest
     * progress store by a few microseconds. Such a future observation has the
     * high bit set after modular subtraction; report a raced zero-age sample
     * instead of a near-UINT32_MAX stall. Ordinary 32-bit timer wrap remains
     * correct because recent elapsed intervals stay below INT32_MAX. */
    return age_us > INT32_MAX ? 0u : age_us;
}

static uint32_t progress_age_us(uint32_t now_us,
                                const atomic_uint_least32_t *last_progress_us)
{
    uint32_t last_us = atomic_load_explicit(last_progress_us,
                                            memory_order_acquire);
    return timestamp_age_us(now_us, last_us);
}

static uint8_t ring_fill(uint32_t write_index, uint32_t read_index)
{
    uint32_t fill = write_index - read_index;
    return fill > UINT8_MAX ? UINT8_MAX : (uint8_t)fill;
}

static void atomic_update_max(atomic_uint_least32_t *target, uint32_t value)
{
    uint_least32_t current = atomic_load_explicit(target, memory_order_relaxed);
    for (uint32_t attempt = 0; attempt < 2u && current < value; ++attempt) {
        if (atomic_compare_exchange_weak_explicit(target, &current, value,
                                                  memory_order_relaxed,
                                                  memory_order_relaxed)) return;
    }
}

static void atomic_update_min(atomic_uint_least32_t *target, uint32_t value)
{
    uint_least32_t current = atomic_load_explicit(target, memory_order_relaxed);
    for (uint32_t attempt = 0; attempt < 2u && current > value; ++attempt) {
        if (atomic_compare_exchange_weak_explicit(target, &current, value,
                                                  memory_order_relaxed,
                                                  memory_order_relaxed)) return;
    }
}

static void update_fill_extrema(uint8_t fill)
{
    atomic_update_min(&counters.ring_fill_min, fill);
    atomic_update_max(&counters.ring_fill_max, fill);
}

static uint32_t slowest_consumer_read_index(uint32_t write_index)
{
    uint32_t read_index = write_index;
    bool found = false;
    for (uint32_t i = 0; i < TRACE_CONSUMER_SLOTS; ++i) {
        if (!atomic_load_explicit(&consumer_active[i], memory_order_acquire)) continue;
        uint32_t candidate = atomic_load_explicit(&consumer_read_index[i], memory_order_relaxed);
        if (!found || (int32_t)(candidate - read_index) < 0) {
            read_index = candidate;
            found = true;
        }
    }
    return read_index;
}

static void push_record(const AudioStallTraceRecord *record)
{
    uint32_t core = get_core_num();
    if (core > 1u) core = 1u;
    TraceRing *ring = &trace_rings[core];
    uint32_t write_index = atomic_load_explicit(&ring->write_index, memory_order_relaxed);
    uint32_t read_index = atomic_load_explicit(&ring->read_index, memory_order_acquire);
    if (write_index - read_index >= TRACE_RING_SIZE) {
        atomic_fetch_add_explicit(&counters.trace_dropped, 1u, memory_order_relaxed);
        return;
    }
    ring->records[write_index & TRACE_RING_MASK] = *record;
    atomic_store_explicit(&ring->write_index, write_index + 1u, memory_order_release);
}

static void emit_record_at(uint64_t timestamp_us, uint8_t event_type, uint16_t handle,
                           uint16_t cid, uint8_t sequence, uint32_t write_index,
                           uint32_t read_index, bool busy, uint32_t duration_us,
                           uint32_t detail0, uint32_t detail1, int32_t result)
{
    uint8_t fill = ring_fill(write_index, read_index);
    /* An event's indices describe that event, and may intentionally be a
     * pre-drop or synthetic pair. Do not overwrite the live snapshot metric. */
    update_fill_extrema(fill);
    AudioStallTraceRecord record = {
        .timestamp_us = timestamp_us,
        .write_index = write_index,
        .read_index = read_index,
        .duration_us = duration_us,
        .detail0 = detail0,
        .detail1 = detail1,
        .result = result,
        .connection_handle = handle,
        .l2cap_cid = cid,
        .event_type = event_type,
        .sequence = sequence,
        .ring_fill = fill,
        .audio_busy = busy ? 1u : 0u,
    };
    push_record(&record);
}

static void emit_record(uint8_t event_type, uint16_t handle, uint16_t cid, uint8_t sequence,
                        uint32_t write_index, uint32_t read_index, bool busy,
                        uint32_t duration_us, uint32_t detail0, uint32_t detail1, int32_t result)
{
    emit_record_at(time_us_64(), event_type, handle, cid, sequence, write_index,
                   read_index, busy, duration_us, detail0, detail1, result);
}

static void emit_send_delay_warning(uint16_t handle, uint16_t cid, uint8_t sequence,
                                    uint32_t write_index, uint32_t read_index, bool busy,
                                    uint8_t delay_kind, uint32_t delay_us)
{
    if (delay_us == UINT32_MAX ||
        delay_us <= AUDIO_STALL_TRACE_SEND_DELAY_WARNING_US) return;
    emit_record(AUDIO_STALL_TRACE_SEND_DELAY_WARNING, handle, cid, sequence,
                write_index, read_index, busy, delay_us,
                AUDIO_STALL_TRACE_SEND_DELAY_WARNING_US, 0u, delay_kind);
}

static uint32_t sdu_to_request_us(uint32_t selected_ring_index, uint64_t now_us)
{
    uint32_t expected_write_index = selected_ring_index + 1u;
    TraceSduGenerationState *state =
        &sdu_generation[selected_ring_index & ASHA_G722_RING_SIZE_MASK];
    uint32_t recorded_write_index = atomic_load_explicit(&state->write_index,
                                                          memory_order_acquire);
    if (recorded_write_index != expected_write_index) return UINT32_MAX;
    uint32_t generated_us = atomic_load_explicit(&state->generated_us,
                                                 memory_order_relaxed);
    uint32_t elapsed = (uint32_t)now_us - generated_us;
    return elapsed > INT32_MAX ? UINT32_MAX : elapsed;
}

static uint32_t pack_address_low(const uint8_t address[6])
{
    if (address == NULL) return 0u;
    return (uint32_t)address[0] |
           ((uint32_t)address[1] << 8u) |
           ((uint32_t)address[2] << 16u) |
           ((uint32_t)address[3] << 24u);
}

static uint32_t pack_address_and_hci_status(const uint8_t address[6],
                                            uint8_t hci_status,
                                            uint8_t hci_reason)
{
    uint32_t address_high = address == NULL
                                ? 0u
                                : (uint32_t)address[4] |
                                      ((uint32_t)address[5] << 8u);
    return address_high | ((uint32_t)hci_status << 16u) |
           ((uint32_t)hci_reason << 24u);
}

static TraceConnectionState *get_connection_state(uint16_t handle, bool create)
{
    TraceConnectionState *empty = NULL;
    for (uint32_t i = 0; i < TRACE_CONNECTION_SLOTS; ++i) {
        uint32_t slot_handle = atomic_load_explicit(&connection_state[i].handle, memory_order_relaxed);
        if (slot_handle == handle) return &connection_state[i];
        if (slot_handle == AUDIO_STALL_TRACE_INVALID_HANDLE && empty == NULL) empty = &connection_state[i];
    }
    if (create && empty != NULL) {
        atomic_store_explicit(&empty->handle, handle, memory_order_relaxed);
        atomic_store_explicit(&empty->have_rssi, false, memory_order_relaxed);
        return empty;
    }
    return NULL;
}

static bool any_audio_busy(void)
{
    for (uint32_t i = 0; i < TRACE_CONNECTION_SLOTS; ++i) {
        if (atomic_load_explicit(&connection_state[i].busy, memory_order_acquire)) return true;
    }
    return false;
}

static TraceSequenceSkip note_sequence_sent(uint16_t handle, uint8_t sequence)
{
    TraceSequenceSkip skip = {
        .previous_sequence = AUDIO_STALL_TRACE_INVALID_SEQUENCE,
        .skipped_frames = 0u,
        .total_skipped_frames = atomic_load_explicit(
            &counters.sequence_skip_count, memory_order_relaxed),
    };
    TraceConnectionState *state = get_connection_state(handle, true);
    if (state != NULL) {
        if (atomic_load_explicit(&state->have_sequence, memory_order_relaxed)) {
            uint8_t last = (uint8_t)atomic_load_explicit(&state->last_sequence, memory_order_relaxed);
            skip.previous_sequence = last;
            uint8_t delta = (uint8_t)(sequence - last);
            if (delta > 1u && delta < 128u) {
                skip.skipped_frames = delta - 1u;
                skip.total_skipped_frames = atomic_fetch_add_explicit(
                                                &counters.sequence_skip_count,
                                                skip.skipped_frames,
                                                memory_order_relaxed) +
                                            skip.skipped_frames;
            }
        }
        atomic_store_explicit(&state->last_sequence, sequence, memory_order_relaxed);
        atomic_store_explicit(&state->have_sequence, true, memory_order_relaxed);
    }
    atomic_store_explicit(&counters.sequence_sent, sequence, memory_order_relaxed);
    return skip;
}

void audio_stall_trace_init(void)
{
    /* Called once before core 1 starts; all other state is zero-initialised BSS. */
    for (uint32_t i = 0; i < 2u; ++i) {
        atomic_store_explicit(&trace_rings[i].write_index, 0u, memory_order_relaxed);
        atomic_store_explicit(&trace_rings[i].read_index, 0u, memory_order_relaxed);
        atomic_store_explicit(&consumer_read_index[i], 0u, memory_order_relaxed);
        atomic_store_explicit(&consumer_active[i], false, memory_order_relaxed);
    }
    for (uint32_t i = 0; i < TRACE_CONNECTION_SLOTS; ++i) {
        atomic_store_explicit(&connection_state[i].handle,
                              AUDIO_STALL_TRACE_INVALID_HANDLE, memory_order_relaxed);
    }
    for (uint32_t i = 0; i < ASHA_G722_RING_SIZE; ++i) {
        atomic_store_explicit(&sdu_generation[i].write_index, UINT32_MAX,
                              memory_order_relaxed);
        atomic_store_explicit(&sdu_generation[i].generated_us, 0u,
                              memory_order_relaxed);
    }
    atomic_store_explicit(&counters.ring_fill_min, UINT32_MAX, memory_order_relaxed);
    atomic_store_explicit(&last_sdu_generated_us, 0u, memory_order_relaxed);
    atomic_store_explicit(&last_audio_timer_us, 0u, memory_order_relaxed);
    atomic_store_explicit(&last_acl_hci_write_end_us, 0u, memory_order_relaxed);
    atomic_store_explicit(&runtime_counters.core1_run_loop_count, 0u,
                          memory_order_relaxed);
    atomic_store_explicit(&runtime_counters.core1_run_loop_last_us, 0u,
                          memory_order_relaxed);
    atomic_store_explicit(&runtime_counters.process_audio_enter_count, 0u,
                          memory_order_relaxed);
    atomic_store_explicit(&runtime_counters.process_audio_enter_last_us, 0u,
                          memory_order_relaxed);
    atomic_store_explicit(&runtime_counters.hci_transport_progress_count, 0u,
                          memory_order_relaxed);
    atomic_store_explicit(&runtime_counters.hci_transport_progress_last_us, 0u,
                          memory_order_relaxed);
    atomic_store_explicit(&runtime_counters.hci_controller_progress_count, 0u,
                          memory_order_relaxed);
    atomic_store_explicit(&runtime_counters.hci_controller_progress_last_us, 0u,
                          memory_order_relaxed);
    last_core1_heartbeat_event_us = 0u;
    last_process_audio_event_us = 0u;
    for (uint32_t i = 0; i < 4u; ++i) {
        last_g722_integrity_event_us[i] = 0u;
    }

    bool watchdog_reboot = watchdog_caused_reboot();
    bool watchdog_enable_reboot = watchdog_enable_caused_reboot();
    uint32_t reset_reason = watchdog_hw->reason;
    if (watchdog_hw->scratch[2] == TRACE_BOOT_SESSION_MAGIC) {
        boot_session_id = watchdog_hw->scratch[3] + 1u;
        if (boot_session_id == 0u) boot_session_id = 1u;
    } else {
        boot_session_id = 1u;
    }
    watchdog_hw->scratch[2] = TRACE_BOOT_SESSION_MAGIC;
    watchdog_hw->scratch[3] = boot_session_id;

    uint32_t firmware_version =
        ((uint32_t)PICO_ASHA_FW_VERS_MAJOR << 24u) |
        ((uint32_t)PICO_ASHA_FW_VERS_MINOR << 16u) |
        ((uint32_t)PICO_ASHA_FW_VERS_PATCH & 0xffffu);
    uint8_t boot_flags = (watchdog_reboot ? 1u : 0u) |
                         (watchdog_enable_reboot ? 2u : 0u);
    emit_record(AUDIO_STALL_TRACE_SYSTEM_BOOT,
                AUDIO_STALL_TRACE_INVALID_HANDLE, 0u, boot_flags,
                0u, 0u, false, 0u, boot_session_id, firmware_version,
                (int32_t)reset_reason);
}

uint64_t audio_stall_trace_now_us(void)
{
    return time_us_64();
}

void audio_stall_trace_set_consumer(uint8_t slot, bool active, uint32_t read_index)
{
    if (slot >= TRACE_CONSUMER_SLOTS) return;
    atomic_store_explicit(&consumer_read_index[slot], read_index, memory_order_relaxed);
    atomic_store_explicit(&consumer_active[slot], active, memory_order_release);
    uint32_t write_index = asha_audio_get_write_index();
    uint8_t fill = ring_fill(write_index,
                             slowest_consumer_read_index(write_index));
    atomic_store_explicit(&counters.ring_fill_current, fill,
                          memory_order_relaxed);
    update_fill_extrema(fill);
}

uint32_t audio_stall_trace_sdu_age_us(uint64_t now_us)
{
    uint32_t generated_us = atomic_load_explicit(&last_sdu_generated_us, memory_order_acquire);
    if (generated_us == 0u) return 0u;
    return timestamp_age_us((uint32_t)now_us, generated_us);
}

void audio_stall_trace_sdu_generated(uint8_t sequence, uint32_t write_index)
{
    uint64_t now_us = time_us_64();
    uint32_t now_us_low = (uint32_t)now_us;
    uint32_t previous_us = atomic_exchange_explicit(&last_sdu_generated_us, now_us_low, memory_order_acq_rel);
    uint32_t interval_us = previous_us == 0u ? 0u : now_us_low - previous_us;
    uint32_t generated_ring_index = write_index - 1u;
    TraceSduGenerationState *generation =
        &sdu_generation[generated_ring_index & ASHA_G722_RING_SIZE_MASK];
    atomic_store_explicit(&generation->generated_us, now_us_low,
                          memory_order_relaxed);
    atomic_store_explicit(&generation->write_index, write_index,
                          memory_order_release);
    uint32_t read_index = slowest_consumer_read_index(write_index);
    uint8_t fill = ring_fill(write_index, read_index);
    atomic_store_explicit(&counters.ring_fill_current, fill,
                          memory_order_relaxed);
    update_fill_extrema(fill);
    atomic_fetch_add_explicit(&counters.sdu_generated_count, 1u, memory_order_relaxed);
    atomic_store_explicit(&counters.sequence_generated, sequence, memory_order_relaxed);
    emit_record(AUDIO_STALL_TRACE_SDU_GENERATED, AUDIO_STALL_TRACE_INVALID_HANDLE, 0u,
                sequence, write_index, read_index, any_audio_busy(), interval_us, 0u, 0u, 0);
    if (fill > ASHA_G722_RING_SIZE) {
        atomic_fetch_add_explicit(&counters.ring_overrun_count, 1u, memory_order_relaxed);
        emit_record(AUDIO_STALL_TRACE_RING_OVERRUN, AUDIO_STALL_TRACE_INVALID_HANDLE, 0u,
                    sequence, write_index, read_index, any_audio_busy(),
                    0u, ASHA_G722_RING_SIZE, 0u, 0);
    }
}

void audio_stall_trace_can_send_requested(uint16_t handle, uint16_t cid, uint8_t sequence,
                                          uint32_t write_index, uint32_t read_index, bool busy,
                                          uint32_t selected_ring_index,
                                          uint32_t previous_send_age_us)
{
    uint64_t now_us = time_us_64();
    uint32_t generation_to_request_us = sdu_to_request_us(selected_ring_index,
                                                          now_us);
    emit_record(AUDIO_STALL_TRACE_CAN_SEND_REQUESTED, handle, cid, sequence,
                write_index, read_index, busy, generation_to_request_us,
                previous_send_age_us, selected_ring_index, 0);
    emit_send_delay_warning(handle, cid, sequence, write_index, read_index,
                            busy, AUDIO_STALL_TRACE_DELAY_SDU_TO_REQUEST,
                            generation_to_request_us);
    emit_send_delay_warning(handle, cid, sequence, write_index, read_index,
                            busy,
                            AUDIO_STALL_TRACE_DELAY_PREVIOUS_SEND_TO_REQUEST,
                            previous_send_age_us);
}

void audio_stall_trace_can_send_now(uint16_t handle, uint16_t cid, uint8_t sequence,
                                    uint32_t write_index, uint32_t read_index, bool busy,
                                    uint32_t wait_us)
{
    atomic_store_explicit(&counters.can_send_wait_last_us, wait_us, memory_order_relaxed);
    atomic_update_max(&counters.can_send_wait_max_us, wait_us);
    emit_record(AUDIO_STALL_TRACE_CAN_SEND_NOW, handle, cid, sequence,
                write_index, read_index, busy, wait_us, 0u, 0u, 0);
    emit_send_delay_warning(handle, cid, sequence, write_index, read_index,
                            busy,
                            AUDIO_STALL_TRACE_DELAY_REQUEST_TO_CAN_SEND_NOW,
                            wait_us);
}

void audio_stall_trace_l2cap_send_begin(uint16_t handle, uint16_t cid,
                                        uint8_t sequence,
                                        uint32_t write_index,
                                        uint32_t read_index, bool busy,
                                        uint32_t can_send_now_to_send_us,
                                        bool local_recovery)
{
    emit_record(AUDIO_STALL_TRACE_L2CAP_SEND_BEGIN, handle, cid, sequence,
                write_index, read_index, busy, can_send_now_to_send_us,
                local_recovery ? 1u : 0u, 0u, 0);
    emit_send_delay_warning(
        handle, cid, sequence, write_index, read_index, busy,
        AUDIO_STALL_TRACE_DELAY_CAN_SEND_NOW_TO_L2CAP_SEND,
        can_send_now_to_send_us);
}

void audio_stall_trace_l2cap_send(uint16_t handle, uint16_t cid, uint8_t sequence,
                                  uint32_t write_index, uint32_t read_index, bool busy,
                                  uint16_t sdu_size, uint32_t call_duration_us,
                                  int32_t result)
{
    atomic_fetch_add_explicit(&counters.l2cap_send_attempt_count, 1u, memory_order_relaxed);
    if (result == 0) {
        atomic_fetch_add_explicit(&counters.l2cap_send_success_count, 1u, memory_order_relaxed);
        TraceSequenceSkip skip = note_sequence_sent(handle, sequence);
        if (skip.skipped_frames != 0u) {
            emit_record(AUDIO_STALL_TRACE_SEQUENCE_SKIP_COUNT_CHANGED,
                        handle, cid, sequence, write_index, read_index, busy,
                        0u, skip.skipped_frames, skip.total_skipped_frames,
                        skip.previous_sequence);
        }
    } else {
        atomic_fetch_add_explicit(&counters.l2cap_send_error_count, 1u, memory_order_relaxed);
    }
    emit_record(AUDIO_STALL_TRACE_L2CAP_SEND, handle, cid, sequence,
                write_index, read_index, busy, call_duration_us, sdu_size, 0u,
                result);
}

void audio_stall_trace_packet_sent(uint16_t handle, uint16_t cid, uint8_t sequence,
                                   uint32_t write_index, uint32_t read_index, bool busy,
                                   uint32_t wait_us)
{
    atomic_update_max(&counters.packet_sent_wait_max_us, wait_us);
    uint32_t hci_end_us = atomic_load_explicit(&last_acl_hci_write_end_us,
                                               memory_order_acquire);
    uint32_t hci_to_callback_us = hci_end_us == 0u
                                      ? 0u
                                      : (uint32_t)time_us_64() - hci_end_us;
    atomic_store_explicit(&runtime_counters.hci_to_packet_sent_last_us,
                          hci_to_callback_us, memory_order_relaxed);
    atomic_update_max(&runtime_counters.hci_to_packet_sent_max_us,
                      hci_to_callback_us);
    emit_record(AUDIO_STALL_TRACE_PACKET_SENT, handle, cid, sequence,
                write_index, read_index, busy, wait_us, hci_to_callback_us, 0u, 0);
}

void audio_stall_trace_busy(uint8_t event_type, uint16_t handle, uint16_t cid, uint8_t sequence,
                            uint32_t write_index, uint32_t read_index, bool busy,
                            uint32_t duration_us, int32_t context)
{
    TraceConnectionState *state = get_connection_state(handle, event_type == AUDIO_STALL_TRACE_BUSY_SET);
    uint64_t now_us = time_us_64();
    if (state != NULL) {
        if (event_type == AUDIO_STALL_TRACE_BUSY_SET) {
            atomic_store_explicit(&state->busy_since_us, (uint32_t)now_us, memory_order_relaxed);
            atomic_store_explicit(&state->busy, 1u, memory_order_release);
        } else if (event_type == AUDIO_STALL_TRACE_BUSY_CLEAR) {
            atomic_store_explicit(&state->busy, 0u, memory_order_release);
            atomic_update_max(&counters.audio_busy_max_duration_us, duration_us);
        }
    }
    emit_record(event_type, handle, cid, sequence, write_index, read_index,
                busy, duration_us, 0u, 0u, context);
}

void audio_stall_trace_ring_underrun(uint16_t handle, uint16_t cid, uint8_t sequence,
                                     uint32_t write_index, uint32_t read_index, bool busy,
                                     uint32_t empty_duration_us)
{
    atomic_fetch_add_explicit(&counters.ring_underrun_count, 1u, memory_order_relaxed);
    emit_record(AUDIO_STALL_TRACE_RING_UNDERRUN, handle, cid, sequence,
                write_index, read_index, busy, empty_duration_us, 0u, 0u, 0);
}

void audio_stall_trace_ble_disconnect(uint16_t handle, uint16_t cid, uint8_t sequence,
                                      uint32_t write_index, uint32_t read_index, bool busy,
                                      uint8_t status, uint8_t reason, uint32_t busy_duration_us,
                                      uint64_t last_successful_send_us)
{
    uint64_t now_us = time_us_64();
    uint32_t last_send_age_us = last_successful_send_us == 0u || now_us <= last_successful_send_us
                                    ? UINT32_MAX
                                    : saturating_us(now_us - last_successful_send_us);
    int32_t result = (int32_t)((uint32_t)status | ((uint32_t)reason << 8u));
    emit_record(AUDIO_STALL_TRACE_BLE_DISCONNECT, handle, cid, sequence,
                write_index, read_index, busy, busy_duration_us, last_send_age_us,
                (uint32_t)last_successful_send_us, result);
    TraceConnectionState *state = get_connection_state(handle, false);
    if (state != NULL) {
        if (atomic_load_explicit(&state->have_rssi, memory_order_acquire)) {
            uint32_t sampled_us = atomic_load_explicit(&state->rssi_sample_us,
                                                       memory_order_relaxed);
            uint32_t age_us = (uint32_t)now_us - sampled_us;
            int32_t rssi = atomic_load_explicit(&state->rssi_dbm,
                                                memory_order_relaxed);
            emit_record(AUDIO_STALL_TRACE_RSSI_CONTEXT, handle, cid, sequence,
                        write_index, read_index, busy, age_us, 0u, 0u, rssi);
        }
        atomic_store_explicit(&state->busy, 0u, memory_order_relaxed);
        atomic_store_explicit(&state->have_sequence, false, memory_order_relaxed);
        atomic_store_explicit(&state->have_rssi, false, memory_order_relaxed);
        atomic_store_explicit(&state->handle, AUDIO_STALL_TRACE_INVALID_HANDLE, memory_order_release);
    }
}

void audio_stall_trace_ble_connect(uint16_t handle, uint16_t cid, uint8_t sequence,
                                   uint32_t write_index, uint32_t read_index, bool busy,
                                   uint16_t interval, uint16_t latency,
                                   uint16_t supervision_timeout)
{
    (void)get_connection_state(handle, true);
    uint32_t packed_params = (uint32_t)latency | ((uint32_t)supervision_timeout << 16u);
    emit_record(AUDIO_STALL_TRACE_BLE_CONNECT, handle, cid, sequence,
                write_index, read_index, busy, 0u, interval, packed_params, 0);
}

void audio_stall_trace_audio_timer_tick(uint64_t now_us)
{
    uint32_t now_us_low = (uint32_t)now_us;
    uint32_t run_loop_count =
        atomic_fetch_add_explicit(&runtime_counters.core1_run_loop_count, 1u,
                                  memory_order_relaxed) + 1u;
    atomic_store_explicit(&runtime_counters.core1_run_loop_last_us, now_us_low,
                          memory_order_release);
    if (last_core1_heartbeat_event_us == 0u ||
        now_us_low - last_core1_heartbeat_event_us >=
            AUDIO_STALL_TRACE_HEARTBEAT_INTERVAL_US) {
        last_core1_heartbeat_event_us = now_us_low;
        uint32_t write_index = asha_audio_get_write_index();
        uint32_t read_index = slowest_consumer_read_index(write_index);
        uint32_t hci_transport_age = progress_age_us(
            now_us_low, &runtime_counters.hci_transport_progress_last_us);
        uint32_t hci_controller_age = progress_age_us(
            now_us_low, &runtime_counters.hci_controller_progress_last_us);
        uint32_t process_audio_count = atomic_load_explicit(
            &runtime_counters.process_audio_enter_count, memory_order_relaxed);
        emit_record_at(now_us, AUDIO_STALL_TRACE_CORE1_RUN_LOOP_HEARTBEAT,
                       AUDIO_STALL_TRACE_INVALID_HANDLE, 0u,
                       AUDIO_STALL_TRACE_INVALID_SEQUENCE, write_index,
                       read_index, any_audio_busy(), hci_transport_age,
                       run_loop_count, hci_controller_age,
                       (int32_t)process_audio_count);
    }
    uint32_t previous_us = atomic_exchange_explicit(&last_audio_timer_us, now_us_low, memory_order_acq_rel);
    if (previous_us == 0u) return;
    uint32_t gap_us = now_us_low - previous_us;
    atomic_update_max(&counters.audio_timer_gap_max_us, gap_us);
    atomic_store_explicit(&runtime_counters.btstack_run_loop_gap_last_us, gap_us,
                          memory_order_relaxed);
    atomic_update_max(&runtime_counters.btstack_run_loop_gap_max_us, gap_us);
    if (gap_us > AUDIO_STALL_TRACE_AUDIO_TIMER_LATE_US) {
        atomic_fetch_add_explicit(&counters.audio_timer_late_count, 1u, memory_order_relaxed);
        uint32_t write_index = asha_audio_get_write_index();
        uint32_t read_index = slowest_consumer_read_index(write_index);
        emit_record(AUDIO_STALL_TRACE_AUDIO_TIMER_LATE, AUDIO_STALL_TRACE_INVALID_HANDLE, 0u,
                    AUDIO_STALL_TRACE_INVALID_SEQUENCE, write_index, read_index,
                    any_audio_busy(),
                    gap_us, 0u, 0u, 0);
    }
}

void audio_stall_trace_process_audio_enter(uint64_t now_us,
                                           uint32_t write_index)
{
    uint32_t now_us_low = (uint32_t)now_us;
    uint32_t enter_count = atomic_fetch_add_explicit(
                               &runtime_counters.process_audio_enter_count, 1u,
                               memory_order_relaxed) + 1u;
    atomic_store_explicit(&runtime_counters.process_audio_enter_last_us,
                          now_us_low, memory_order_release);
    if (last_process_audio_event_us != 0u &&
        now_us_low - last_process_audio_event_us <
            AUDIO_STALL_TRACE_HEARTBEAT_INTERVAL_US) {
        return;
    }
    last_process_audio_event_us = now_us_low;
    uint32_t read_index = slowest_consumer_read_index(write_index);
    uint32_t run_loop_age = progress_age_us(
        now_us_low, &runtime_counters.core1_run_loop_last_us);
    uint32_t hci_transport_age = progress_age_us(
        now_us_low, &runtime_counters.hci_transport_progress_last_us);
    uint32_t hci_controller_age = progress_age_us(
        now_us_low, &runtime_counters.hci_controller_progress_last_us);
    emit_record_at(now_us, AUDIO_STALL_TRACE_PROCESS_AUDIO_ENTER,
                   AUDIO_STALL_TRACE_INVALID_HANDLE, 0u,
                   AUDIO_STALL_TRACE_INVALID_SEQUENCE, write_index,
                   read_index, any_audio_busy(), run_loop_age, enter_count,
                   hci_transport_age, (int32_t)hci_controller_age);
}

void audio_stall_trace_usb_pcm_underrun(uint32_t gap_us, uint32_t write_index)
{
    atomic_fetch_add_explicit(&counters.usb_pcm_underrun_count, 1u, memory_order_relaxed);
    uint32_t read_index = slowest_consumer_read_index(write_index);
    emit_record(AUDIO_STALL_TRACE_USB_PCM_UNDERRUN, AUDIO_STALL_TRACE_INVALID_HANDLE, 0u,
                AUDIO_STALL_TRACE_INVALID_SEQUENCE, write_index, read_index,
                any_audio_busy(),
                gap_us, 0u, 0u, 0);
}

void audio_stall_trace_pcm_discontinuity(
    uint8_t sequence, uint32_t write_index, uint32_t gap_us,
    uint32_t left_jump, uint32_t right_jump, uint16_t sample_count,
    uint8_t channel_flags)
{
    uint32_t read_index = slowest_consumer_read_index(write_index);
    uint32_t packed_result = (uint32_t)sample_count |
                             ((uint32_t)channel_flags << 16u);
    emit_record(AUDIO_STALL_TRACE_PCM_DISCONTINUITY,
                AUDIO_STALL_TRACE_INVALID_HANDLE, 0u, sequence,
                write_index, read_index, any_audio_busy(), gap_us,
                left_jump, right_jump, (int32_t)packed_result);
}

void audio_stall_trace_g722_integrity_error(
    uint16_t handle, uint16_t cid, uint8_t sequence,
    uint32_t write_index, uint32_t selected_ring_index, bool busy,
    uint8_t stage, uint32_t expected_value, uint32_t actual_value,
    uint32_t observed_generation)
{
    uint64_t now_us = time_us_64();
    uint32_t now_us_low = (uint32_t)now_us;
    uint32_t stage_index = stage < 4u ? stage : 0u;
    uint32_t previous_us = last_g722_integrity_event_us[stage_index];
    if (previous_us != 0u &&
        now_us_low - previous_us <
            AUDIO_STALL_TRACE_CONTENT_EVENT_MIN_INTERVAL_US) {
        return;
    }
    last_g722_integrity_event_us[stage_index] = now_us_low;
    emit_record_at(now_us, AUDIO_STALL_TRACE_G722_INTEGRITY_ERROR, handle,
                   cid, sequence, write_index, selected_ring_index, busy,
                   stage, expected_value, actual_value,
                   (int32_t)observed_generation);
}

void audio_stall_trace_user_audio_glitch_marker(void)
{
    uint64_t now_us = time_us_64();
    struct AshaAudioTraceSnapshot audio = {0};
    asha_audio_get_trace_snapshot(&audio);
    uint32_t write_index = asha_audio_get_write_index();
    uint32_t read_index = slowest_consumer_read_index(write_index);
    uint32_t pcm_age_us = timestamp_age_us((uint32_t)now_us,
                                           audio.pcm_block_time_us);
    emit_record_at(now_us, AUDIO_STALL_TRACE_USER_AUDIO_GLITCH_MARKER,
                   AUDIO_STALL_TRACE_INVALID_HANDLE, 0u, audio.sequence,
                   write_index, read_index, any_audio_busy(),
                   pcm_age_us, audio.pcm_block_checksum,
                   audio.sdu_checksum_l, (int32_t)audio.sdu_checksum_r);
}

void audio_stall_trace_rssi(uint16_t handle, uint16_t cid, uint8_t sequence,
                            uint32_t write_index, uint32_t read_index, bool busy, int8_t rssi)
{
    uint64_t now_us = time_us_64();
    TraceConnectionState *state = get_connection_state(handle, true);
    if (state != NULL) {
        atomic_store_explicit(&state->rssi_dbm, rssi, memory_order_relaxed);
        atomic_store_explicit(&state->rssi_sample_us, (uint32_t)now_us,
                              memory_order_relaxed);
        atomic_store_explicit(&state->have_rssi, true, memory_order_release);
    }
    atomic_fetch_add_explicit(&runtime_counters.rssi_sample_count, 1u,
                              memory_order_relaxed);
    emit_record(AUDIO_STALL_TRACE_RSSI_SAMPLE, handle, cid, sequence,
                write_index, read_index, busy, 0u, 0u, 0u, rssi);
}

void audio_stall_trace_rssi_request_skipped(void)
{
    atomic_fetch_add_explicit(&runtime_counters.rssi_request_skipped_count, 1u,
                              memory_order_relaxed);
}

void audio_stall_trace_tx_watchdog(uint8_t event_type, uint16_t handle, uint16_t cid,
                                   uint8_t sequence, uint32_t write_index,
                                   uint32_t read_index, bool busy, uint32_t stalled_us,
                                   int32_t result)
{
    uint32_t rssi_age_us = UINT32_MAX;
    uint32_t packed_rssi = 0u;
    TraceConnectionState *state = get_connection_state(handle, false);
    if (state != NULL &&
        atomic_load_explicit(&state->have_rssi, memory_order_acquire)) {
        uint32_t sampled_us = atomic_load_explicit(&state->rssi_sample_us,
                                                   memory_order_relaxed);
        rssi_age_us = (uint32_t)time_us_64() - sampled_us;
        packed_rssi = 0x100u |
                      (uint8_t)atomic_load_explicit(&state->rssi_dbm,
                                                    memory_order_relaxed);
    }
    emit_record(event_type, handle, cid, sequence, write_index, read_index,
                busy, stalled_us, rssi_age_us, packed_rssi, result);
}

void audio_stall_trace_tx_stale_drop(uint16_t handle, uint16_t cid,
                                     uint8_t old_sequence, uint8_t new_sequence,
                                     uint32_t write_index, uint32_t read_index,
                                     bool busy, uint32_t age_us,
                                     uint32_t dropped_frames)
{
    atomic_fetch_add_explicit(&runtime_counters.tx_stale_drop_count, 1u,
                              memory_order_relaxed);
    atomic_fetch_add_explicit(&runtime_counters.tx_stale_drop_frames,
                              dropped_frames, memory_order_relaxed);
    emit_record(AUDIO_STALL_TRACE_TX_STALE_DROP, handle, cid, old_sequence,
                write_index, read_index, busy, age_us, dropped_frames,
                new_sequence, 0);
}

void audio_stall_trace_stale_frames_dropped(
    uint16_t handle, uint16_t cid, uint8_t old_sequence, uint8_t new_sequence,
    uint32_t write_index, uint32_t read_index, bool busy,
    uint32_t busy_duration_us, uint32_t dropped_frames,
    bool can_send_now_pending, uint32_t last_request_age_us,
    uint32_t last_successful_send_age_us, bool pcm_streaming,
    uint8_t connected_devices, uint16_t available_credits)
{
    uint64_t now_us = time_us_64();
    uint32_t total_sequence_skips = atomic_load_explicit(
        &counters.sequence_skip_count, memory_order_relaxed);
    atomic_fetch_add_explicit(&runtime_counters.tx_stale_drop_count, 1u,
                              memory_order_relaxed);
    atomic_fetch_add_explicit(&runtime_counters.tx_stale_drop_frames,
                              dropped_frames, memory_order_relaxed);

    /* Primary record: pre-drop indices/fill/busy, busy age, both sequences,
     * dropped-frame count, and the sequence-skip total at capture time. */
    emit_record_at(now_us, AUDIO_STALL_TRACE_STALE_FRAMES_DROPPED,
                   handle, cid, old_sequence, write_index, read_index, busy,
                   busy_duration_us, dropped_frames, total_sequence_skips,
                   new_sequence);

    /* Companion record at the identical timestamp carries timing and runtime
     * state without expanding the fixed 40-byte version-1 wire record. */
    uint8_t flags = (can_send_now_pending ? 1u : 0u) |
                    (pcm_streaming ? 2u : 0u);
    uint32_t packed_state = (uint32_t)available_credits |
                            ((uint32_t)connected_devices << 16u) |
                            ((uint32_t)flags << 24u);
    emit_record_at(now_us, AUDIO_STALL_TRACE_STALE_DROP_CONTEXT,
                   handle, cid, new_sequence, write_index, read_index, busy,
                   last_request_age_us, last_successful_send_age_us,
                   packed_state, 0);
}

void audio_stall_trace_send_state_changed(
    uint16_t handle, uint16_t cid, uint8_t sequence,
    uint32_t write_index, uint32_t read_index, bool busy,
    uint8_t old_state, uint8_t new_state, uint8_t reason,
    uint32_t old_state_duration_us)
{
    emit_record(AUDIO_STALL_TRACE_SEND_STATE_CHANGED, handle, cid, sequence,
                write_index, read_index, busy, old_state_duration_us,
                old_state, new_state, reason);
}

void audio_stall_trace_bluetooth_lifecycle(
    uint8_t event_type, uint16_t handle, uint16_t cid,
    const uint8_t address[6], uint8_t hci_status, uint8_t hci_reason,
    uint8_t l2cap_status, uint32_t write_index, uint32_t read_index,
    bool busy)
{
    emit_record(event_type, handle, cid, AUDIO_STALL_TRACE_INVALID_SEQUENCE,
                write_index, read_index, busy, 0u,
                pack_address_low(address),
                pack_address_and_hci_status(address, hci_status, hci_reason),
                l2cap_status);
}

void audio_stall_trace_watchdog_reset_requested(uint8_t reason,
                                                uint32_t delay_ms)
{
    uint32_t write_index = asha_audio_get_write_index();
    uint32_t read_index = slowest_consumer_read_index(write_index);
    emit_record(AUDIO_STALL_TRACE_WATCHDOG_RESET_REQUESTED,
                AUDIO_STALL_TRACE_INVALID_HANDLE, 0u,
                AUDIO_STALL_TRACE_INVALID_SEQUENCE, write_index, read_index,
                any_audio_busy(), delay_ms * 1000u, reason, boot_session_id, 0);
}

void audio_stall_trace_audio_no_progress(
    uint8_t event_type, uint16_t handle, uint16_t cid, uint8_t sequence,
    uint32_t write_index, uint32_t read_index, bool busy,
    uint32_t last_send_age_us, uint32_t newest_sdu_age_us,
    uint8_t tx_state, uint16_t available_credits, int32_t result)
{
    uint32_t packed_state = (uint32_t)available_credits |
                            ((uint32_t)tx_state << 16u);
    emit_record(event_type, handle, cid, sequence, write_index, read_index,
                busy, last_send_age_us, newest_sdu_age_us, packed_state,
                result);
}

void audio_stall_trace_reconnect_transition(
    uint8_t event_type, uint16_t handle, uint16_t cid,
    const uint8_t address[6], uint32_t write_index, uint32_t read_index,
    uint8_t attempt, uint8_t hci_status, uint8_t hci_reason,
    int32_t result)
{
    emit_record(event_type, handle, cid, AUDIO_STALL_TRACE_INVALID_SEQUENCE,
                write_index, read_index, false, attempt,
                pack_address_low(address),
                pack_address_and_hci_status(address, hci_status, hci_reason),
                result);
}

void audio_stall_trace_credits_zero(
    uint8_t event_type, uint16_t handle, uint16_t cid, uint8_t sequence,
    uint32_t write_index, uint32_t read_index, bool busy,
    uint32_t duration_us, uint16_t available_credits)
{
    uint32_t rssi_age_us = UINT32_MAX;
    uint32_t packed_rssi = 0u;
    TraceConnectionState *state = get_connection_state(handle, false);
    if (state != NULL &&
        atomic_load_explicit(&state->have_rssi, memory_order_acquire)) {
        uint32_t sampled_us = atomic_load_explicit(&state->rssi_sample_us,
                                                   memory_order_relaxed);
        rssi_age_us = (uint32_t)time_us_64() - sampled_us;
        packed_rssi = 0x100u |
                      (uint8_t)atomic_load_explicit(&state->rssi_dbm,
                                                    memory_order_relaxed);
    }
    emit_record(event_type, handle, cid, sequence, write_index, read_index,
                busy, duration_us, available_credits, rssi_age_us,
                (int32_t)packed_rssi);
}

void audio_stall_trace_tx_blocked(
    uint16_t handle, uint16_t cid, uint8_t sequence,
    uint32_t write_index, uint32_t read_index, bool busy,
    uint32_t last_send_age_us, uint32_t blocker_mask,
    uint32_t newest_sdu_age_us, uint8_t tx_state,
    uint16_t available_credits, bool can_send_pending)
{
    uint32_t packed_state = (uint32_t)available_credits |
                            ((uint32_t)tx_state << 16u) |
                            (can_send_pending ? (1u << 24u) : 0u);
    emit_record(AUDIO_STALL_TRACE_TX_BLOCKED, handle, cid, sequence,
                write_index, read_index, busy, last_send_age_us,
                blocker_mask, newest_sdu_age_us, (int32_t)packed_state);
}

void audio_stall_trace_tx_wait_age(
    uint8_t event_type, uint16_t handle, uint16_t cid, uint8_t sequence,
    uint32_t write_index, uint32_t read_index, bool busy,
    uint32_t wait_age_us, uint8_t tx_state, uint16_t available_credits)
{
    uint32_t now_us = (uint32_t)time_us_64();
    uint32_t hci_transport_age = progress_age_us(
        now_us, &runtime_counters.hci_transport_progress_last_us);
    uint32_t hci_controller_age = progress_age_us(
        now_us, &runtime_counters.hci_controller_progress_last_us);
    uint32_t packed_state = (uint32_t)available_credits |
                            ((uint32_t)tx_state << 16u);
    emit_record(event_type, handle, cid, sequence, write_index, read_index,
                busy, wait_age_us, hci_transport_age, hci_controller_age,
                (int32_t)packed_state);
}

void audio_stall_trace_hci_write(uint64_t begin_us, uint64_t end_us,
                                 uint8_t packet_type, int32_t result)
{
    uint32_t duration_us = end_us > begin_us
                               ? saturating_us(end_us - begin_us)
                               : 0u;
    atomic_fetch_add_explicit(&runtime_counters.hci_write_count, 1u,
                              memory_order_relaxed);
    atomic_fetch_add_explicit(&runtime_counters.hci_transport_progress_count,
                              1u, memory_order_relaxed);
    atomic_store_explicit(&runtime_counters.hci_transport_progress_last_us,
                          (uint32_t)end_us, memory_order_release);
    if (result != 0) {
        atomic_fetch_add_explicit(&runtime_counters.hci_write_error_count, 1u,
                                  memory_order_relaxed);
    }
    atomic_store_explicit(&runtime_counters.hci_write_last_us, duration_us,
                          memory_order_relaxed);
    atomic_update_max(&runtime_counters.hci_write_max_us, duration_us);
    if (packet_type == 2u) {
        atomic_store_explicit(&last_acl_hci_write_end_us, (uint32_t)end_us,
                              memory_order_release);
    }
    if (duration_us <= AUDIO_STALL_TRACE_HCI_WRITE_SLOW_US) return;

    uint32_t write_index = asha_audio_get_write_index();
    uint32_t read_index = slowest_consumer_read_index(write_index);
    emit_record_at(begin_us, AUDIO_STALL_TRACE_HCI_WRITE_BEGIN,
                   AUDIO_STALL_TRACE_INVALID_HANDLE, 0u,
                   AUDIO_STALL_TRACE_INVALID_SEQUENCE, write_index, read_index,
                   any_audio_busy(), 0u, packet_type, 0u, 0);
    emit_record_at(end_us, AUDIO_STALL_TRACE_HCI_WRITE_END,
                   AUDIO_STALL_TRACE_INVALID_HANDLE, 0u,
                   AUDIO_STALL_TRACE_INVALID_SEQUENCE, write_index, read_index,
                   any_audio_busy(), duration_us, packet_type, 0u, result);
}

void audio_stall_trace_hci_controller_progress(uint64_t now_us)
{
    atomic_fetch_add_explicit(&runtime_counters.hci_controller_progress_count,
                              1u, memory_order_relaxed);
    atomic_store_explicit(&runtime_counters.hci_controller_progress_last_us,
                          (uint32_t)now_us, memory_order_release);
}

void audio_stall_trace_cyw43_lock_acquired(uint64_t begin_us, uint64_t acquired_us)
{
    uint32_t wait_us = acquired_us > begin_us
                           ? saturating_us(acquired_us - begin_us)
                           : 0u;
    atomic_store_explicit(&runtime_counters.cyw43_lock_wait_last_us, wait_us,
                          memory_order_relaxed);
    atomic_update_max(&runtime_counters.cyw43_lock_wait_max_us, wait_us);
    if (wait_us <= AUDIO_STALL_TRACE_CYW43_LOCK_WAIT_SLOW_US) return;

    uint32_t write_index = asha_audio_get_write_index();
    uint32_t read_index = slowest_consumer_read_index(write_index);
    emit_record_at(begin_us, AUDIO_STALL_TRACE_CYW43_LOCK_WAIT_BEGIN,
                   AUDIO_STALL_TRACE_INVALID_HANDLE, 0u,
                   AUDIO_STALL_TRACE_INVALID_SEQUENCE, write_index, read_index,
                   any_audio_busy(), 0u, 0u, 0u, 0);
    emit_record_at(acquired_us, AUDIO_STALL_TRACE_CYW43_LOCK_ACQUIRED,
                   AUDIO_STALL_TRACE_INVALID_HANDLE, 0u,
                   AUDIO_STALL_TRACE_INVALID_SEQUENCE, write_index, read_index,
                   any_audio_busy(), wait_us, 0u, 0u, 0);
}

void audio_stall_trace_cyw43_lock_released(uint64_t acquired_us, uint64_t released_us)
{
    uint32_t held_us = released_us > acquired_us
                           ? saturating_us(released_us - acquired_us)
                           : 0u;
    atomic_store_explicit(&runtime_counters.cyw43_lock_hold_last_us, held_us,
                          memory_order_relaxed);
    atomic_update_max(&runtime_counters.cyw43_lock_hold_max_us, held_us);
    if (held_us <= AUDIO_STALL_TRACE_CYW43_LOCK_HELD_SLOW_US) return;

    uint32_t write_index = asha_audio_get_write_index();
    uint32_t read_index = slowest_consumer_read_index(write_index);
    emit_record_at(released_us, AUDIO_STALL_TRACE_CYW43_LOCK_HELD,
                   AUDIO_STALL_TRACE_INVALID_HANDLE, 0u,
                   AUDIO_STALL_TRACE_INVALID_SEQUENCE, write_index, read_index,
                   any_audio_busy(), held_us, 0u, 0u, 0);
}

bool audio_stall_trace_pop(AudioStallTraceRecord *record)
{
    if (record == NULL) return false;
    for (uint32_t core = 0; core < 2u; ++core) {
        TraceRing *ring = &trace_rings[core];
        uint32_t read_index = atomic_load_explicit(&ring->read_index, memory_order_relaxed);
        uint32_t write_index = atomic_load_explicit(&ring->write_index, memory_order_acquire);
        if (read_index == write_index) continue;
        *record = ring->records[read_index & TRACE_RING_MASK];
        atomic_store_explicit(&ring->read_index, read_index + 1u, memory_order_release);
        return true;
    }
    return false;
}

void audio_stall_trace_snapshot(AudioStallTraceSnapshot *snapshot, uint64_t now_us)
{
    if (snapshot == NULL) return;
    uint32_t current_busy_us = 0u;
    uint32_t now_us_low = (uint32_t)now_us;
    for (uint32_t i = 0; i < TRACE_CONNECTION_SLOTS; ++i) {
        if (!atomic_load_explicit(&connection_state[i].busy, memory_order_acquire)) continue;
        uint32_t since_us = atomic_load_explicit(&connection_state[i].busy_since_us, memory_order_relaxed);
        if (!atomic_load_explicit(&connection_state[i].busy, memory_order_acquire)) continue;
        uint32_t duration_us = now_us_low - since_us;
        /* The snapshot timestamp is captured on core 0 before these values are
         * sampled. Core 1 can set busy in between, making since_us newer than
         * now_us. Treat that observation as a zero-duration/raced sample instead
         * of turning the unsigned subtraction into UINT32_MAX. Durations longer
         * than half the 32-bit timer range are not credible audio-busy intervals. */
        if (duration_us > INT32_MAX) continue;
        if (duration_us > current_busy_us) current_busy_us = duration_us;
        atomic_update_max(&counters.audio_busy_max_duration_us, duration_us);
    }
    /* Recompute current fill from the producer and registered consumers at
     * snapshot time. Previously this field was merely the fill attached to the
     * most recently emitted event, so a later lifecycle record could replace a
     * real backlog with zero. */
    uint32_t live_write_index = asha_audio_get_write_index();
    uint32_t live_read_index = slowest_consumer_read_index(live_write_index);
    uint8_t live_fill = ring_fill(live_write_index, live_read_index);
    atomic_store_explicit(&counters.ring_fill_current, live_fill,
                          memory_order_relaxed);
    update_fill_extrema(live_fill);
    uint32_t fill_min = atomic_load_explicit(&counters.ring_fill_min, memory_order_relaxed);
    *snapshot = (AudioStallTraceSnapshot) {
        .timestamp_us = now_us,
        .sdu_generated_count = atomic_load_explicit(&counters.sdu_generated_count, memory_order_relaxed),
        .l2cap_send_attempt_count = atomic_load_explicit(&counters.l2cap_send_attempt_count, memory_order_relaxed),
        .l2cap_send_success_count = atomic_load_explicit(&counters.l2cap_send_success_count, memory_order_relaxed),
        .l2cap_send_error_count = atomic_load_explicit(&counters.l2cap_send_error_count, memory_order_relaxed),
        .can_send_wait_last_us = atomic_load_explicit(&counters.can_send_wait_last_us, memory_order_relaxed),
        .can_send_wait_max_us = atomic_load_explicit(&counters.can_send_wait_max_us, memory_order_relaxed),
        .packet_sent_wait_max_us = atomic_load_explicit(&counters.packet_sent_wait_max_us, memory_order_relaxed),
        .audio_busy_current_duration_us = current_busy_us,
        .audio_busy_max_duration_us = atomic_load_explicit(&counters.audio_busy_max_duration_us, memory_order_relaxed),
        .ring_fill_current = live_fill,
        .ring_fill_min = fill_min == UINT32_MAX ? 0u : fill_min,
        .ring_fill_max = atomic_load_explicit(&counters.ring_fill_max, memory_order_relaxed),
        .ring_underrun_count = atomic_load_explicit(&counters.ring_underrun_count, memory_order_relaxed),
        .ring_overrun_count = atomic_load_explicit(&counters.ring_overrun_count, memory_order_relaxed),
        .sequence_generated = atomic_load_explicit(&counters.sequence_generated, memory_order_relaxed),
        .sequence_sent = atomic_load_explicit(&counters.sequence_sent, memory_order_relaxed),
        .sequence_skip_count = atomic_load_explicit(&counters.sequence_skip_count, memory_order_relaxed),
        .audio_timer_gap_max_us = atomic_load_explicit(&counters.audio_timer_gap_max_us, memory_order_relaxed),
        .audio_timer_late_count = atomic_load_explicit(&counters.audio_timer_late_count, memory_order_relaxed),
        .usb_pcm_underrun_count = atomic_load_explicit(&counters.usb_pcm_underrun_count, memory_order_relaxed),
        .trace_dropped = atomic_load_explicit(&counters.trace_dropped, memory_order_relaxed),
    };
}

void audio_stall_trace_runtime_snapshot(AudioStallTraceRuntimeSnapshot *snapshot,
                                        uint64_t now_us)
{
    if (snapshot == NULL) return;
    int32_t rssi[TRACE_CONNECTION_SLOTS] = {INT32_MIN, INT32_MIN};
    uint32_t rssi_age[TRACE_CONNECTION_SLOTS] = {UINT32_MAX, UINT32_MAX};
    for (uint32_t i = 0; i < TRACE_CONNECTION_SLOTS; ++i) {
        if (!atomic_load_explicit(&connection_state[i].have_rssi,
                                  memory_order_acquire)) continue;
        rssi[i] = atomic_load_explicit(&connection_state[i].rssi_dbm,
                                       memory_order_relaxed);
        uint32_t sampled_us = atomic_load_explicit(&connection_state[i].rssi_sample_us,
                                                   memory_order_relaxed);
        rssi_age[i] = timestamp_age_us((uint32_t)now_us, sampled_us);
    }
    uint32_t now_us_low = (uint32_t)now_us;
    *snapshot = (AudioStallTraceRuntimeSnapshot) {
        .timestamp_us = now_us,
        .hci_write_count = atomic_load_explicit(&runtime_counters.hci_write_count, memory_order_relaxed),
        .hci_write_error_count = atomic_load_explicit(&runtime_counters.hci_write_error_count, memory_order_relaxed),
        .hci_write_last_us = atomic_load_explicit(&runtime_counters.hci_write_last_us, memory_order_relaxed),
        .hci_write_max_us = atomic_load_explicit(&runtime_counters.hci_write_max_us, memory_order_relaxed),
        .cyw43_lock_wait_last_us = atomic_load_explicit(&runtime_counters.cyw43_lock_wait_last_us, memory_order_relaxed),
        .cyw43_lock_wait_max_us = atomic_load_explicit(&runtime_counters.cyw43_lock_wait_max_us, memory_order_relaxed),
        .cyw43_lock_hold_last_us = atomic_load_explicit(&runtime_counters.cyw43_lock_hold_last_us, memory_order_relaxed),
        .cyw43_lock_hold_max_us = atomic_load_explicit(&runtime_counters.cyw43_lock_hold_max_us, memory_order_relaxed),
        .btstack_run_loop_gap_last_us = atomic_load_explicit(&runtime_counters.btstack_run_loop_gap_last_us, memory_order_relaxed),
        .btstack_run_loop_gap_max_us = atomic_load_explicit(&runtime_counters.btstack_run_loop_gap_max_us, memory_order_relaxed),
        .hci_to_packet_sent_last_us = atomic_load_explicit(&runtime_counters.hci_to_packet_sent_last_us, memory_order_relaxed),
        .hci_to_packet_sent_max_us = atomic_load_explicit(&runtime_counters.hci_to_packet_sent_max_us, memory_order_relaxed),
        .rssi_sample_count = atomic_load_explicit(&runtime_counters.rssi_sample_count, memory_order_relaxed),
        .rssi_request_skipped_count = atomic_load_explicit(&runtime_counters.rssi_request_skipped_count, memory_order_relaxed),
        .rssi_slot0_dbm = rssi[0],
        .rssi_slot1_dbm = rssi[1],
        .rssi_slot0_age_us = rssi_age[0],
        .rssi_slot1_age_us = rssi_age[1],
        .tx_stale_drop_count = atomic_load_explicit(&runtime_counters.tx_stale_drop_count, memory_order_relaxed),
        .tx_stale_drop_frames = atomic_load_explicit(&runtime_counters.tx_stale_drop_frames, memory_order_relaxed),
        .core1_run_loop_count = atomic_load_explicit(&runtime_counters.core1_run_loop_count, memory_order_relaxed),
        .core1_run_loop_age_us = progress_age_us(now_us_low, &runtime_counters.core1_run_loop_last_us),
        .process_audio_enter_count = atomic_load_explicit(&runtime_counters.process_audio_enter_count, memory_order_relaxed),
        .process_audio_enter_age_us = progress_age_us(now_us_low, &runtime_counters.process_audio_enter_last_us),
        .hci_transport_progress_count = atomic_load_explicit(&runtime_counters.hci_transport_progress_count, memory_order_relaxed),
        .hci_transport_progress_age_us = progress_age_us(now_us_low, &runtime_counters.hci_transport_progress_last_us),
        .hci_controller_progress_count = atomic_load_explicit(&runtime_counters.hci_controller_progress_count, memory_order_relaxed),
        .hci_controller_progress_age_us = progress_age_us(now_us_low, &runtime_counters.hci_controller_progress_last_us),
    };
}

#endif
