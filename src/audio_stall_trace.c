#include "audio_stall_trace.h"

#ifdef PICO_ASHA_AUDIO_STALL_TRACE

#include <limits.h>
#include <stdatomic.h>

#include <pico/multicore.h>
#include <pico/time.h>

#include "asha_audio.h"

#define TRACE_RING_SIZE 256u
#define TRACE_RING_MASK (TRACE_RING_SIZE - 1u)
#define TRACE_CONSUMER_SLOTS 2u
#define TRACE_CONNECTION_SLOTS 2u

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

static uint32_t saturating_us(uint64_t value)
{
    return value > UINT32_MAX ? UINT32_MAX : (uint32_t)value;
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

static void update_fill_stats(uint8_t fill)
{
    atomic_store_explicit(&counters.ring_fill_current, fill, memory_order_relaxed);
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
    update_fill_stats(fill);
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

static void note_sequence_sent(uint16_t handle, uint8_t sequence)
{
    TraceConnectionState *state = get_connection_state(handle, true);
    if (state != NULL) {
        if (atomic_load_explicit(&state->have_sequence, memory_order_relaxed)) {
            uint8_t last = (uint8_t)atomic_load_explicit(&state->last_sequence, memory_order_relaxed);
            uint8_t delta = (uint8_t)(sequence - last);
            if (delta > 1u && delta < 128u) {
                atomic_fetch_add_explicit(&counters.sequence_skip_count, delta - 1u, memory_order_relaxed);
            }
        }
        atomic_store_explicit(&state->last_sequence, sequence, memory_order_relaxed);
        atomic_store_explicit(&state->have_sequence, true, memory_order_relaxed);
    }
    atomic_store_explicit(&counters.sequence_sent, sequence, memory_order_relaxed);
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
    atomic_store_explicit(&counters.ring_fill_min, UINT32_MAX, memory_order_relaxed);
    atomic_store_explicit(&last_sdu_generated_us, 0u, memory_order_relaxed);
    atomic_store_explicit(&last_audio_timer_us, 0u, memory_order_relaxed);
    atomic_store_explicit(&last_acl_hci_write_end_us, 0u, memory_order_relaxed);
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
}

uint32_t audio_stall_trace_sdu_age_us(uint64_t now_us)
{
    uint32_t generated_us = atomic_load_explicit(&last_sdu_generated_us, memory_order_acquire);
    if (generated_us == 0u) return 0u;
    return (uint32_t)now_us - generated_us;
}

void audio_stall_trace_sdu_generated(uint8_t sequence, uint32_t write_index)
{
    uint64_t now_us = time_us_64();
    uint32_t now_us_low = (uint32_t)now_us;
    uint32_t previous_us = atomic_exchange_explicit(&last_sdu_generated_us, now_us_low, memory_order_acq_rel);
    uint32_t interval_us = previous_us == 0u ? 0u : now_us_low - previous_us;
    uint32_t read_index = slowest_consumer_read_index(write_index);
    uint8_t fill = ring_fill(write_index, read_index);
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
                                          uint32_t write_index, uint32_t read_index, bool busy)
{
    emit_record(AUDIO_STALL_TRACE_CAN_SEND_REQUESTED, handle, cid, sequence,
                write_index, read_index, busy, 0u, 0u, 0u, 0);
}

void audio_stall_trace_can_send_now(uint16_t handle, uint16_t cid, uint8_t sequence,
                                    uint32_t write_index, uint32_t read_index, bool busy,
                                    uint32_t wait_us)
{
    atomic_store_explicit(&counters.can_send_wait_last_us, wait_us, memory_order_relaxed);
    atomic_update_max(&counters.can_send_wait_max_us, wait_us);
    emit_record(AUDIO_STALL_TRACE_CAN_SEND_NOW, handle, cid, sequence,
                write_index, read_index, busy, wait_us, 0u, 0u, 0);
}

void audio_stall_trace_l2cap_send(uint16_t handle, uint16_t cid, uint8_t sequence,
                                  uint32_t write_index, uint32_t read_index, bool busy,
                                  uint16_t sdu_size, int32_t result)
{
    atomic_fetch_add_explicit(&counters.l2cap_send_attempt_count, 1u, memory_order_relaxed);
    if (result == 0) {
        atomic_fetch_add_explicit(&counters.l2cap_send_success_count, 1u, memory_order_relaxed);
        note_sequence_sent(handle, sequence);
    } else {
        atomic_fetch_add_explicit(&counters.l2cap_send_error_count, 1u, memory_order_relaxed);
    }
    emit_record(AUDIO_STALL_TRACE_L2CAP_SEND, handle, cid, sequence,
                write_index, read_index, busy, 0u, sdu_size, 0u, result);
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

void audio_stall_trace_usb_pcm_underrun(uint32_t gap_us, uint32_t write_index)
{
    atomic_fetch_add_explicit(&counters.usb_pcm_underrun_count, 1u, memory_order_relaxed);
    uint32_t read_index = slowest_consumer_read_index(write_index);
    emit_record(AUDIO_STALL_TRACE_USB_PCM_UNDERRUN, AUDIO_STALL_TRACE_INVALID_HANDLE, 0u,
                AUDIO_STALL_TRACE_INVALID_SEQUENCE, write_index, read_index,
                any_audio_busy(),
                gap_us, 0u, 0u, 0);
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

void audio_stall_trace_hci_write(uint64_t begin_us, uint64_t end_us,
                                 uint8_t packet_type, int32_t result)
{
    uint32_t duration_us = end_us > begin_us
                               ? saturating_us(end_us - begin_us)
                               : 0u;
    atomic_fetch_add_explicit(&runtime_counters.hci_write_count, 1u,
                              memory_order_relaxed);
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
    for (uint32_t i = 0; i < TRACE_CONNECTION_SLOTS; ++i) {
        if (!atomic_load_explicit(&connection_state[i].busy, memory_order_acquire)) continue;
        uint32_t since_us = atomic_load_explicit(&connection_state[i].busy_since_us, memory_order_relaxed);
        uint32_t duration_us = (uint32_t)now_us - since_us;
        if (duration_us > current_busy_us) current_busy_us = duration_us;
        atomic_update_max(&counters.audio_busy_max_duration_us, duration_us);
    }
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
        .ring_fill_current = atomic_load_explicit(&counters.ring_fill_current, memory_order_relaxed),
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
        rssi_age[i] = (uint32_t)now_us - sampled_us;
    }
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
    };
}

#endif
