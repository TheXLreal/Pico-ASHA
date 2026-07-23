#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define AUDIO_STALL_TRACE_MAGIC 0x52545341u /* "ASTR" in little endian */
#define AUDIO_STALL_TRACE_VERSION 1u
#define AUDIO_STALL_TRACE_INVALID_SEQUENCE 0xffu
#define AUDIO_STALL_TRACE_INVALID_HANDLE 0xffffu
#define AUDIO_STALL_TRACE_SNAPSHOT_INTERVAL_US 250000u
#define AUDIO_STALL_TRACE_ANOMALY_US 25000u
#define AUDIO_STALL_TRACE_SEND_DELAY_WARNING_US 50000u
#define AUDIO_STALL_TRACE_STATUS_UNAVAILABLE 0xffu
/* The Pico BTstack adapter adds one millisecond to the requested 1-ms timer;
 * observed healthy cadence is about 2.16 ms. Only gaps above 5 ms are late. */
#define AUDIO_STALL_TRACE_AUDIO_TIMER_LATE_US 5000u
#define AUDIO_STALL_TRACE_HCI_WRITE_SLOW_US 5000u
#define AUDIO_STALL_TRACE_CYW43_LOCK_WAIT_SLOW_US 1000u
#define AUDIO_STALL_TRACE_CYW43_LOCK_HELD_SLOW_US 5000u
#define AUDIO_STALL_TRACE_HEARTBEAT_INTERVAL_US 1000000u
#define AUDIO_STALL_TRACE_TX_BLOCKED_US 50000u
#define AUDIO_STALL_TRACE_CAN_SEND_AGE_US 25000u
#define AUDIO_STALL_TRACE_PACKET_SENT_AGE_US 50000u
#define AUDIO_STALL_TRACE_PCM_DISCONTINUITY_THRESHOLD 24576u
#define AUDIO_STALL_TRACE_CONTENT_EVENT_MIN_INTERVAL_US 100000u

enum AudioStallTraceEvent {
    AUDIO_STALL_TRACE_SDU_GENERATED = 1,
    AUDIO_STALL_TRACE_CAN_SEND_REQUESTED,
    AUDIO_STALL_TRACE_CAN_SEND_NOW,
    AUDIO_STALL_TRACE_L2CAP_SEND,
    AUDIO_STALL_TRACE_PACKET_SENT,
    AUDIO_STALL_TRACE_BUSY_SET,
    AUDIO_STALL_TRACE_BUSY_CLEAR,
    AUDIO_STALL_TRACE_RING_UNDERRUN,
    AUDIO_STALL_TRACE_RING_OVERRUN,
    AUDIO_STALL_TRACE_BLE_DISCONNECT,
    AUDIO_STALL_TRACE_BLE_CONNECT,
    AUDIO_STALL_TRACE_BUSY_STALL,
    AUDIO_STALL_TRACE_AUDIO_TIMER_LATE,
    AUDIO_STALL_TRACE_USB_PCM_UNDERRUN,
    AUDIO_STALL_TRACE_RSSI_SAMPLE,
    AUDIO_STALL_TRACE_TX_WATCHDOG,
    AUDIO_STALL_TRACE_TX_RECOVERY,
    AUDIO_STALL_TRACE_TX_RECONNECT,
    AUDIO_STALL_TRACE_TX_STALE_DROP,
    AUDIO_STALL_TRACE_HCI_WRITE_BEGIN,
    AUDIO_STALL_TRACE_HCI_WRITE_END,
    AUDIO_STALL_TRACE_CYW43_LOCK_WAIT_BEGIN,
    AUDIO_STALL_TRACE_CYW43_LOCK_ACQUIRED,
    AUDIO_STALL_TRACE_CYW43_LOCK_HELD,
    AUDIO_STALL_TRACE_RSSI_CONTEXT,
    AUDIO_STALL_TRACE_L2CAP_SEND_BEGIN,
    AUDIO_STALL_TRACE_SEND_STATE_CHANGED,
    AUDIO_STALL_TRACE_SEND_DELAY_WARNING,
    AUDIO_STALL_TRACE_STALE_FRAMES_DROPPED,
    AUDIO_STALL_TRACE_STALE_DROP_CONTEXT,
    AUDIO_STALL_TRACE_SEQUENCE_SKIP_COUNT_CHANGED,
    AUDIO_STALL_TRACE_HCI_CONNECTION_OPENED,
    AUDIO_STALL_TRACE_HCI_DISCONNECTION_COMPLETE,
    AUDIO_STALL_TRACE_L2CAP_CHANNEL_OPENED,
    AUDIO_STALL_TRACE_L2CAP_CHANNEL_CLOSED,
    AUDIO_STALL_TRACE_ASHA_DEVICE_CONNECTED,
    AUDIO_STALL_TRACE_ASHA_DEVICE_DISCONNECTED,
    AUDIO_STALL_TRACE_SYSTEM_BOOT,
    AUDIO_STALL_TRACE_WATCHDOG_RESET_REQUESTED,
    AUDIO_STALL_TRACE_AUDIO_NO_PROGRESS,
    AUDIO_STALL_TRACE_AUDIO_NO_PROGRESS_REARMED,
    AUDIO_STALL_TRACE_AUDIO_NO_PROGRESS_RECONNECT,
    AUDIO_STALL_TRACE_RECONNECT_SCHEDULED,
    AUDIO_STALL_TRACE_SCAN_START_REQUESTED,
    AUDIO_STALL_TRACE_CONNECT_ATTEMPT,
    AUDIO_STALL_TRACE_CONNECT_COMPLETE,
    AUDIO_STALL_TRACE_RECONNECT_TIMEOUT,
    AUDIO_STALL_TRACE_CREDITS_ZERO_ENTER,
    AUDIO_STALL_TRACE_CREDITS_ZERO_EXIT,
    AUDIO_STALL_TRACE_CORE1_RUN_LOOP_HEARTBEAT,
    AUDIO_STALL_TRACE_PROCESS_AUDIO_ENTER,
    AUDIO_STALL_TRACE_TX_BLOCKED,
    AUDIO_STALL_TRACE_CAN_SEND_REQUEST_AGE,
    AUDIO_STALL_TRACE_PACKET_SENT_WAIT_AGE,
    AUDIO_STALL_TRACE_PCM_DISCONTINUITY,
    AUDIO_STALL_TRACE_G722_INTEGRITY_ERROR,
    AUDIO_STALL_TRACE_USER_AUDIO_GLITCH_MARKER,
};

/* Descriptive aliases for the original version-1 event IDs. */
#define AUDIO_STALL_TRACE_SEND_REQUESTED AUDIO_STALL_TRACE_CAN_SEND_REQUESTED
#define AUDIO_STALL_TRACE_CAN_SEND_NOW_RECEIVED AUDIO_STALL_TRACE_CAN_SEND_NOW
#define AUDIO_STALL_TRACE_L2CAP_SEND_COMPLETE AUDIO_STALL_TRACE_L2CAP_SEND

enum AudioStallTraceSendDelay {
    AUDIO_STALL_TRACE_DELAY_SDU_TO_REQUEST = 1,
    AUDIO_STALL_TRACE_DELAY_REQUEST_TO_CAN_SEND_NOW,
    AUDIO_STALL_TRACE_DELAY_CAN_SEND_NOW_TO_L2CAP_SEND,
    AUDIO_STALL_TRACE_DELAY_PREVIOUS_SEND_TO_REQUEST,
};

enum AudioStallTraceSendStateReason {
    AUDIO_STALL_TRACE_STATE_LOCAL_RECOVERY = 1,
    AUDIO_STALL_TRACE_STATE_DISCONNECT,
    AUDIO_STALL_TRACE_STATE_RESET,
};

enum AudioStallTraceWatchdogReason {
    AUDIO_STALL_TRACE_WATCHDOG_HCI_DUMP_SETTING = 1,
    AUDIO_STALL_TRACE_WATCHDOG_RESTART_COMMAND,
    AUDIO_STALL_TRACE_WATCHDOG_USB_SETTING,
    AUDIO_STALL_TRACE_WATCHDOG_RECONNECT_FALLBACK,
};

enum AudioStallTracePayloadKind {
    AUDIO_STALL_TRACE_PAYLOAD_RECORDS = 1,
    AUDIO_STALL_TRACE_PAYLOAD_SNAPSHOT,
    AUDIO_STALL_TRACE_PAYLOAD_RUNTIME_SNAPSHOT,
};

enum AudioStallTraceBusyContext {
    AUDIO_STALL_TRACE_BUSY_CONTEXT_ACP_START = 1,
    AUDIO_STALL_TRACE_BUSY_CONTEXT_AUDIO_SDU,
    AUDIO_STALL_TRACE_BUSY_CONTEXT_PACKET_SENT,
    AUDIO_STALL_TRACE_BUSY_CONTEXT_RESET,
};

enum AudioStallTraceTxBlocker {
    AUDIO_STALL_TRACE_BLOCK_NOT_CONNECTED = 1u << 0,
    AUDIO_STALL_TRACE_BLOCK_NOT_STREAMING = 1u << 1,
    AUDIO_STALL_TRACE_BLOCK_L2CAP_NOT_READY = 1u << 2,
    AUDIO_STALL_TRACE_BLOCK_NO_SDU_AVAILABLE = 1u << 3,
    AUDIO_STALL_TRACE_BLOCK_SDU_NOT_FRESH = 1u << 4,
    AUDIO_STALL_TRACE_BLOCK_WAIT_CAN_SEND_NOW = 1u << 5,
    AUDIO_STALL_TRACE_BLOCK_WAIT_PACKET_SENT = 1u << 6,
    AUDIO_STALL_TRACE_BLOCK_CAN_SEND_PENDING = 1u << 7,
    AUDIO_STALL_TRACE_BLOCK_AUDIO_BUSY = 1u << 8,
    AUDIO_STALL_TRACE_BLOCK_NO_CREDITS = 1u << 9,
    AUDIO_STALL_TRACE_BLOCK_PCM_NOT_STREAMING = 1u << 10,
    AUDIO_STALL_TRACE_BLOCK_AUDIO_DISABLED = 1u << 11,
    AUDIO_STALL_TRACE_BLOCK_PROCESS_NOT_AUDIO = 1u << 12,
    AUDIO_STALL_TRACE_BLOCK_CONNECTIONS_DISABLED = 1u << 13,
    AUDIO_STALL_TRACE_BLOCK_TX_BUFFER_OWNED = 1u << 14,
    AUDIO_STALL_TRACE_BLOCK_INVARIANT_NOT_ARMED = 1u << 15,
};

enum AudioStallTraceG722IntegrityStage {
    AUDIO_STALL_TRACE_G722_RING_GENERATION = 1,
    AUDIO_STALL_TRACE_G722_RING_CHECKSUM,
    AUDIO_STALL_TRACE_G722_TX_BUFFER_CHANGED,
};

#if defined(_MSC_VER)
#define AUDIO_STALL_TRACE_WIRE_STRUCT __declspec(align(4))
#pragma pack(push, 1)
#else
#define AUDIO_STALL_TRACE_WIRE_STRUCT __attribute__((packed, aligned(4)))
#endif

/*
 * Fixed wire record. All fields are little endian. Event-specific meanings:
 * - duration_us: generation interval, CAN_SEND wait, send-to-packet wait, or
 *   audio_busy duration.
 * - detail0/detail1: SDU size; connect interval/latency/timeout; or the age
 *   and low 32 bits of the last successful send at disconnect.
 * - result: BTstack result/status, packed disconnect status/reason, busy
 *   context, or signed RSSI.
 */
typedef struct AUDIO_STALL_TRACE_WIRE_STRUCT AudioStallTraceRecord {
    uint64_t timestamp_us;
    uint32_t write_index;
    uint32_t read_index;
    uint32_t duration_us;
    uint32_t detail0;
    uint32_t detail1;
    int32_t result;
    uint16_t connection_handle;
    uint16_t l2cap_cid;
    uint8_t event_type;
    uint8_t sequence;
    uint8_t ring_fill;
    uint8_t audio_busy;
} AudioStallTraceRecord;

typedef struct AUDIO_STALL_TRACE_WIRE_STRUCT AudioStallTracePacketHeader {
    uint32_t magic;
    uint8_t version;
    uint8_t kind;
    uint8_t count;
    uint8_t payload_size;
} AudioStallTracePacketHeader;

typedef struct AUDIO_STALL_TRACE_WIRE_STRUCT AudioStallTraceSnapshot {
    uint64_t timestamp_us;
    uint32_t sdu_generated_count;
    uint32_t l2cap_send_attempt_count;
    uint32_t l2cap_send_success_count;
    uint32_t l2cap_send_error_count;
    uint32_t can_send_wait_last_us;
    uint32_t can_send_wait_max_us;
    uint32_t packet_sent_wait_max_us;
    uint32_t audio_busy_current_duration_us;
    uint32_t audio_busy_max_duration_us;
    uint32_t ring_fill_current;
    uint32_t ring_fill_min;
    uint32_t ring_fill_max;
    uint32_t ring_underrun_count;
    uint32_t ring_overrun_count;
    uint32_t sequence_generated;
    uint32_t sequence_sent;
    uint32_t sequence_skip_count;
    uint32_t audio_timer_gap_max_us;
    uint32_t audio_timer_late_count;
    uint32_t usb_pcm_underrun_count;
    uint32_t trace_dropped;
} AudioStallTraceSnapshot;

/* Optional version-1 extension. Existing record and snapshot layouts stay
 * unchanged; older parsers safely ignore this payload kind. */
typedef struct AUDIO_STALL_TRACE_WIRE_STRUCT AudioStallTraceRuntimeSnapshot {
    uint64_t timestamp_us;
    uint32_t hci_write_count;
    uint32_t hci_write_error_count;
    uint32_t hci_write_last_us;
    uint32_t hci_write_max_us;
    uint32_t cyw43_lock_wait_last_us;
    uint32_t cyw43_lock_wait_max_us;
    uint32_t cyw43_lock_hold_last_us;
    uint32_t cyw43_lock_hold_max_us;
    uint32_t btstack_run_loop_gap_last_us;
    uint32_t btstack_run_loop_gap_max_us;
    uint32_t hci_to_packet_sent_last_us;
    uint32_t hci_to_packet_sent_max_us;
    uint32_t rssi_sample_count;
    uint32_t rssi_request_skipped_count;
    int32_t rssi_slot0_dbm;
    int32_t rssi_slot1_dbm;
    uint32_t rssi_slot0_age_us;
    uint32_t rssi_slot1_age_us;
    uint32_t tx_stale_drop_count;
    uint32_t tx_stale_drop_frames;
    uint32_t core1_run_loop_count;
    uint32_t core1_run_loop_age_us;
    uint32_t process_audio_enter_count;
    uint32_t process_audio_enter_age_us;
    uint32_t hci_transport_progress_count;
    uint32_t hci_transport_progress_age_us;
    uint32_t hci_controller_progress_count;
    uint32_t hci_controller_progress_age_us;
} AudioStallTraceRuntimeSnapshot;

#if defined(_MSC_VER)
#pragma pack(pop)
#endif
#undef AUDIO_STALL_TRACE_WIRE_STRUCT

#if defined(__cplusplus)
static_assert(sizeof(AudioStallTraceRecord) == 40);
static_assert(sizeof(AudioStallTracePacketHeader) == 8);
static_assert(sizeof(AudioStallTraceSnapshot) == 92);
static_assert(sizeof(AudioStallTraceRuntimeSnapshot) == 120);
#else
_Static_assert(sizeof(AudioStallTraceRecord) == 40, "Unexpected trace record size");
_Static_assert(sizeof(AudioStallTracePacketHeader) == 8, "Unexpected trace header size");
_Static_assert(sizeof(AudioStallTraceSnapshot) == 92, "Unexpected trace snapshot size");
_Static_assert(sizeof(AudioStallTraceRuntimeSnapshot) == 120,
               "Unexpected runtime snapshot size");
#endif

#ifdef PICO_ASHA_AUDIO_STALL_TRACE

void audio_stall_trace_init(void);
uint64_t audio_stall_trace_now_us(void);
void audio_stall_trace_set_consumer(uint8_t slot, bool active, uint32_t read_index);
uint32_t audio_stall_trace_sdu_age_us(uint64_t now_us);

void audio_stall_trace_sdu_generated(uint8_t sequence, uint32_t write_index);
void audio_stall_trace_can_send_requested(uint16_t handle, uint16_t cid, uint8_t sequence,
                                          uint32_t write_index, uint32_t read_index, bool busy,
                                          uint32_t selected_ring_index,
                                          uint32_t previous_send_age_us);
void audio_stall_trace_can_send_now(uint16_t handle, uint16_t cid, uint8_t sequence,
                                    uint32_t write_index, uint32_t read_index, bool busy,
                                    uint32_t wait_us);
void audio_stall_trace_l2cap_send_begin(uint16_t handle, uint16_t cid, uint8_t sequence,
                                        uint32_t write_index, uint32_t read_index, bool busy,
                                        uint32_t can_send_now_to_send_us,
                                        bool local_recovery);
void audio_stall_trace_l2cap_send(uint16_t handle, uint16_t cid, uint8_t sequence,
                                  uint32_t write_index, uint32_t read_index, bool busy,
                                  uint16_t sdu_size, uint32_t call_duration_us,
                                  int32_t result);
void audio_stall_trace_packet_sent(uint16_t handle, uint16_t cid, uint8_t sequence,
                                   uint32_t write_index, uint32_t read_index, bool busy,
                                   uint32_t wait_us);
void audio_stall_trace_busy(uint8_t event_type, uint16_t handle, uint16_t cid, uint8_t sequence,
                            uint32_t write_index, uint32_t read_index, bool busy,
                            uint32_t duration_us, int32_t context);
void audio_stall_trace_ring_underrun(uint16_t handle, uint16_t cid, uint8_t sequence,
                                     uint32_t write_index, uint32_t read_index, bool busy,
                                     uint32_t empty_duration_us);
void audio_stall_trace_ble_disconnect(uint16_t handle, uint16_t cid, uint8_t sequence,
                                      uint32_t write_index, uint32_t read_index, bool busy,
                                      uint8_t status, uint8_t reason, uint32_t busy_duration_us,
                                      uint64_t last_successful_send_us);
void audio_stall_trace_ble_connect(uint16_t handle, uint16_t cid, uint8_t sequence,
                                   uint32_t write_index, uint32_t read_index, bool busy,
                                   uint16_t interval, uint16_t latency,
                                   uint16_t supervision_timeout);
void audio_stall_trace_audio_timer_tick(uint64_t now_us);
void audio_stall_trace_process_audio_enter(uint64_t now_us,
                                           uint32_t write_index);
void audio_stall_trace_usb_pcm_underrun(uint32_t gap_us, uint32_t write_index);
void audio_stall_trace_pcm_discontinuity(
    uint8_t sequence, uint32_t write_index, uint32_t gap_us,
    uint32_t left_jump, uint32_t right_jump, uint16_t sample_count,
    uint8_t channel_flags);
void audio_stall_trace_g722_integrity_error(
    uint16_t handle, uint16_t cid, uint8_t sequence,
    uint32_t write_index, uint32_t selected_ring_index, bool busy,
    uint8_t stage, uint32_t expected_value, uint32_t actual_value,
    uint32_t observed_generation);
void audio_stall_trace_user_audio_glitch_marker(void);
void audio_stall_trace_rssi(uint16_t handle, uint16_t cid, uint8_t sequence,
                            uint32_t write_index, uint32_t read_index, bool busy, int8_t rssi);
void audio_stall_trace_rssi_request_skipped(void);
void audio_stall_trace_tx_watchdog(uint8_t event_type, uint16_t handle, uint16_t cid,
                                   uint8_t sequence, uint32_t write_index,
                                   uint32_t read_index, bool busy, uint32_t stalled_us,
                                   int32_t result);
void audio_stall_trace_tx_stale_drop(uint16_t handle, uint16_t cid,
                                     uint8_t old_sequence, uint8_t new_sequence,
                                     uint32_t write_index, uint32_t read_index,
                                     bool busy, uint32_t age_us,
                                     uint32_t dropped_frames);
void audio_stall_trace_stale_frames_dropped(
    uint16_t handle, uint16_t cid, uint8_t old_sequence, uint8_t new_sequence,
    uint32_t write_index, uint32_t read_index, bool busy,
    uint32_t busy_duration_us, uint32_t dropped_frames,
    bool can_send_now_pending, uint32_t last_request_age_us,
    uint32_t last_successful_send_age_us, bool pcm_streaming,
    uint8_t connected_devices, uint16_t available_credits);
void audio_stall_trace_send_state_changed(
    uint16_t handle, uint16_t cid, uint8_t sequence,
    uint32_t write_index, uint32_t read_index, bool busy,
    uint8_t old_state, uint8_t new_state, uint8_t reason,
    uint32_t old_state_duration_us);
void audio_stall_trace_bluetooth_lifecycle(
    uint8_t event_type, uint16_t handle, uint16_t cid,
    const uint8_t address[6], uint8_t hci_status, uint8_t hci_reason,
    uint8_t l2cap_status, uint32_t write_index, uint32_t read_index,
    bool busy);
void audio_stall_trace_watchdog_reset_requested(uint8_t reason,
                                                uint32_t delay_ms);
void audio_stall_trace_audio_no_progress(
    uint8_t event_type, uint16_t handle, uint16_t cid, uint8_t sequence,
    uint32_t write_index, uint32_t read_index, bool busy,
    uint32_t last_send_age_us, uint32_t newest_sdu_age_us,
    uint8_t tx_state, uint16_t available_credits, int32_t result);
void audio_stall_trace_reconnect_transition(
    uint8_t event_type, uint16_t handle, uint16_t cid,
    const uint8_t address[6], uint32_t write_index, uint32_t read_index,
    uint8_t attempt, uint8_t hci_status, uint8_t hci_reason,
    int32_t result);
void audio_stall_trace_credits_zero(
    uint8_t event_type, uint16_t handle, uint16_t cid, uint8_t sequence,
    uint32_t write_index, uint32_t read_index, bool busy,
    uint32_t duration_us, uint16_t available_credits);
void audio_stall_trace_tx_blocked(
    uint16_t handle, uint16_t cid, uint8_t sequence,
    uint32_t write_index, uint32_t read_index, bool busy,
    uint32_t last_send_age_us, uint32_t blocker_mask,
    uint32_t newest_sdu_age_us, uint8_t tx_state,
    uint16_t available_credits, bool can_send_pending);
void audio_stall_trace_tx_wait_age(
    uint8_t event_type, uint16_t handle, uint16_t cid, uint8_t sequence,
    uint32_t write_index, uint32_t read_index, bool busy,
    uint32_t wait_age_us, uint8_t tx_state, uint16_t available_credits);
void audio_stall_trace_hci_write(uint64_t begin_us, uint64_t end_us,
                                 uint8_t packet_type, int32_t result);
void audio_stall_trace_hci_controller_progress(uint64_t now_us);
void audio_stall_trace_cyw43_lock_acquired(uint64_t begin_us, uint64_t acquired_us);
void audio_stall_trace_cyw43_lock_released(uint64_t acquired_us, uint64_t released_us);

bool audio_stall_trace_pop(AudioStallTraceRecord *record);
void audio_stall_trace_snapshot(AudioStallTraceSnapshot *snapshot, uint64_t now_us);
void audio_stall_trace_runtime_snapshot(AudioStallTraceRuntimeSnapshot *snapshot,
                                        uint64_t now_us);

#else

static inline void audio_stall_trace_init(void) {}

#endif

#ifdef __cplusplus
}
#endif
