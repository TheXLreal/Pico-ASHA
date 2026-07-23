#pragma once

#include <array>
#include <cstdint>
#include <span>

#include <etl/string.h>

#include "asha_audio.h"
#include "audio_stall_trace.h"
#include "asha_bt.hpp"
#include "asha_comms.hpp"

namespace asha
{

constexpr int ha_process_delay_ticks = 500;

// How long the audio loop is allowed to sit in Ready with the host
// actively delivering audio but L2CAP credits stuck below the start
// gate, before forcing a BLE reconnect to recover. Audio loop ticks
// at 1 ms, so this is 3 seconds.
constexpr uint32_t ready_stuck_timeout_ticks = 3000;

// CAN_SEND waits above 25 ms are anomalous. Start recovery at 30 ms, but do
// not transmit audio older than three 20-ms ASHA frames.
constexpr uint32_t audio_can_send_watchdog_us = 30'000;
constexpr uint32_t audio_tx_max_audio_age_us = 60'000;
constexpr uint32_t audio_packet_sent_watchdog_us = 150'000;
// Each ASHA SDU contains 20 ms. Preserve at most 60 ms of fresh ordered audio
// after a short credit drought; larger backlogs still collapse to newest.
constexpr uint32_t audio_short_backlog_max_frames = 3;

// Independent of the current CAN_SEND/PACKET_SENT phase: while fresh SDUs are
// still arriving, a ready stream must make successful L2CAP progress.
constexpr uint32_t audio_no_progress_timeout_us = 100'000;
constexpr uint32_t audio_no_progress_rearm_deadline_us = 150'000;
constexpr uint32_t audio_no_progress_startup_grace_us = 150'000;
constexpr uint32_t audio_sdu_activity_timeout_us = 50'000;

// Firmware-owned reconnects are bounded. One final watchdog reboot is allowed
// per powered session after the normal attempts have all timed out.
constexpr uint32_t reconnect_attempt_timeout_us = 12'000'000;
constexpr uint32_t reconnect_initial_delay_us = 100'000;
constexpr uint32_t reconnect_retry_backoff_us = 500'000;
constexpr uint8_t reconnect_max_attempts = 3;

enum class Side  {Left = 0, Right = 1};
enum class Mode  {Mono = 0, Binaural = 1};
enum class Codec {G722_16, G722_24};

struct ROP
{
    uint8_t raw_data[17] = {};

    void read(const uint8_t* data) { memcpy(raw_data, data, sizeof(raw_data)); }
    Side side() { return (raw_data[1] & 0b00000001) ? Side::Right : Side::Left; }
    Mode mode() { return (raw_data[1] & 0b00000010) ? Mode::Binaural : Mode::Mono; }
    uint16_t mfg_id() { return little_endian_read_16(raw_data, 2); }
    std::span<uint8_t, 6> unique_id() { return std::span<uint8_t, 6>(raw_data + 4, 6); }
    bool le_coc_supported() { return (raw_data[10] & 0b00000001); }
    uint16_t render_delay() { return little_endian_read_16(raw_data, 11); }
    bool supports_codec(Codec c) {
        uint16_t codecs = little_endian_read_16(raw_data, 15);
        switch (c) {
            case Codec::G722_16:
                return codecs & 0b0000000000000010;
            case Codec::G722_24:
                return codecs & 0b0000000000000100;
        }
    }

    explicit operator bool() const { return raw_data[0]; } 
};

struct HearingAid
{
    enum ProcessState : uint32_t {
        ProcessUnset        = 1U <<  0,
        DiscoverServices    = 1U <<  1,
        PairBond            = 1U <<  2,
        DataLength          = 1U <<  3,
        DiscoverChars       = 1U <<  4,
        ReadChars           = 1U <<  5,
        ConnectL2CAP        = 1U <<  6,
        EnBattNotification  = 1U <<  7,
        EnASPNotification   = 1U <<  8,
        Finalize            = 1U <<  9,
        Audio               = 1U << 10,
        Disconnect          = 1U << 29,
        Done                = 1U << 30,
        ProcessBusy         = 1U << 31
    };

    enum AudioState : uint32_t {
        AudioUnset          = 1U <<  0,
        Ready               = 1U <<  1,
        Start               = 1U <<  2,
        Streaming           = 1U <<  3,
        Stop                = 1U <<  4,
        AudioBusy           = 1U << 31
    };

    enum class AudioTxState : uint8_t {
        Idle,
        WaitingCanSendNow,
        WaitingPacketSent,
    };

    enum class AudioNoProgressState : uint8_t {
        Idle,
        WaitingForRearmProgress,
    };

    enum class ReconnectState : uint8_t {
        Idle,
        Scheduled,
        Scanning,
        Connecting,
        Initializing,
        Failed,
    };

    bool connected = false;
    bool cached    = false;

    uint32_t process_state = ProcessState::ProcessUnset;
    uint32_t audio_state   = AudioState::AudioUnset;

    HearingAid* other = nullptr;

    /* BT Remote connection vars */

    bd_addr_t addr = {};
    hci_con_handle_t conn_handle = HCI_CON_HANDLE_INVALID;
    uint16_t cid = 0U;

    /* Common BT vars */

    etl::string<32> device_name = {};
    etl::string<32> manufacturer = {};
    etl::string<32> model = {};
    etl::string<32> fw_vers = {};
    etl::string<32> sw_vers = {};

    /* ASHA vars */

    uint8_t psm = 0U;
    ROP rop = {};

    const char* side_str = "Unknown";

    /* SM vars */

    inline static uint8_t auth_req;
    bool paired_and_bonded = false;

    /* MFI vars */

    uint8_t battery_level = 0;

    /* Member functions */
    
    HearingAid();

    static void process();
    static int num_connected();
    static bool full_set_connected();
    static std::array<HearingAid*,2> connected_has();

    static void on_serial_host_connected();
    static bool is_addr_connected(const bd_addr_t addr);
    static void set_connections_allowed(bool allowed);
    static void set_audio_streaming_enabled(bool enabled);
    static void set_auto_pair_enabled(bool enabled);
    static void start_scan();
    static void on_ad_report(const AdvertisingReport& report);
    static void connect(const bd_addr_t addr, bd_addr_type_t addr_type);
    static void on_connected(uint8_t status, bd_addr_t addr,
                             hci_con_handle_t handle,
                             uint16_t connection_interval, uint16_t peripheral_latency,
                             uint16_t supervision_timeout);
    static void on_disconnected(hci_con_handle_t handle, uint8_t status, uint8_t reason);
    static void on_connection_parameters_updated(hci_con_handle_t handle,
                                                 uint16_t connection_interval,
                                                 uint16_t peripheral_latency,
                                                 uint16_t supervision_timeout);
    static comm::BLEConnectionState get_ble_connection_state();
    static void on_data_len_set(hci_con_handle_t handle, uint16_t rx_octets, uint16_t rx_time, uint16_t tx_octets, uint16_t tx_time);
    static void delete_pair();
    static void delete_pair(uint16_t conn_id);
    static void handle_sm(PACKET_HANDLER_PARAMS);
    static void handle_service_discovery(PACKET_HANDLER_PARAMS);
    static void handle_char_discovery(PACKET_HANDLER_PARAMS);
    static void handle_char_read(PACKET_HANDLER_PARAMS);
    static void handle_acp_write(PACKET_HANDLER_PARAMS);
    static void handle_l2cap_cbm(PACKET_HANDLER_PARAMS);
    static void handle_notification_reg(PACKET_HANDLER_PARAMS);
    static void handle_gatt_notification(PACKET_HANDLER_PARAMS);
    static bool process_audio();
#ifdef PICO_ASHA_AUDIO_STALL_TRACE_RSSI
    static void sample_rssi();
    static void on_rssi(hci_con_handle_t handle, int8_t rssi);
#endif

private:
    /* GATT structures */

    struct {
        struct {
            gatt_client_service_t service = {};
            gatt_client_characteristic_t device_name = {};
        } gap = {};

        struct {
            gatt_client_service_t service = {};
            gatt_client_characteristic_t manufacture_name = {};
            gatt_client_characteristic_t model_num = {};
            gatt_client_characteristic_t fw_vers = {};
            gatt_client_characteristic_t sw_vers = {};
        } dis = {};

        struct {
            gatt_client_service_t service = {};
            gatt_client_characteristic_t rop = {};
            gatt_client_characteristic_t acp = {};
            gatt_client_characteristic_t asp = {};
            gatt_client_characteristic_t vol = {};
            gatt_client_characteristic_t psm = {};  
        } asha = {};

        struct {
            gatt_client_service_t service = {};
            gatt_client_characteristic_t battery = {};
        } mfi = {};
    } services = {};

    std::array<gatt_client_service_t*, 4> service_arr = {
        &services.gap.service,
        &services.dis.service,
        &services.asha.service,
        &services.mfi.service
    };
    std::array<comm::EventType, 4> service_ev_arr = {
        comm::EventType::DiscGAPChar, comm::EventType::DiscDISChar, comm::EventType::DiscASHAChar, comm::EventType::DiscMFIChar,
    };
    size_t service_index = 0;

    std::array<gatt_client_characteristic_t*, 8> chars_arr = {
        &services.gap.device_name,
        &services.dis.manufacture_name,
        &services.dis.model_num,
        &services.dis.fw_vers,
        &services.dis.sw_vers,
        &services.asha.rop,
        &services.asha.psm,
        &services.mfi.battery,
    };
    std::array<comm::EventType, 8> chars_ev_arr = {
        comm::EventType::DevNameRead, comm::EventType::MfgRead, comm::EventType::ModelRead, comm::EventType::FWRead,
        comm::EventType::SWRead,      comm::EventType::ROPRead, comm::EventType::PSMRead,   comm::EventType::MfiBatteryRead,
    };
    size_t chars_index = 0;
    constexpr static size_t cached_chars_index = 6;
    /* L2CAP credit management */

    uint16_t credits = 0;
    uint32_t zero_credits_cooldown = 0;
    uint32_t ready_stuck_ticks = 0;
    int8_t curr_vol = -128;

    // Array to store current AudioControlPoint command packet
    std::array<uint8_t, 5> acp_cmd_packet = {};

    inline static std::array<HearingAid*, 2> hearing_aids;

    int process_delay_ticks = 0;
    int error_count = 0;

    uint32_t curr_read_index = 0U;
    bool first_audio_send = false;
    // BTstack retains the SDU pointer until L2CAP_EVENT_PACKET_SENT. Keep the
    // selected frame out of the eight-slot encoder ring so watchdog recovery
    // cannot race a producer overwrite.
    std::array<uint8_t, ASHA_SDU_SIZE_BYTES> audio_tx_buffer = {};
    uint8_t* audio_data = nullptr;
    AudioTxState audio_tx_state = AudioTxState::Idle;
    uint64_t audio_tx_state_since_us = 0U;
    uint64_t audio_tx_sdu_since_us = 0U;
    uint32_t audio_tx_ring_index = 0U;
    uint8_t audio_tx_sequence = AUDIO_STALL_TRACE_INVALID_SEQUENCE;
    bool audio_tx_local_recovery = false;
    uint32_t audio_tx_generation = 0U;
    uint32_t audio_tx_pending_generation = 0U;
    uint8_t audio_tx_stale_can_send_callbacks = 0U;
    bool l2cap_channel_open = false;

    // Core-1-owned progress observations. The producer's atomic write index is
    // the monotonic SDU generation count; successful sends are counted only
    // after l2cap_send() accepts a complete audio SDU.
    uint32_t audio_sdu_generated_count = 0U;
    uint32_t audio_sdu_count_at_last_success = 0U;
    uint32_t successful_audio_send_count = 0U;
    uint32_t audio_progress_success_baseline = 0U;
    uint32_t audio_no_progress_success_count = 0U;
    uint32_t audio_last_sdu_generated_us = 0U;
    uint64_t last_successful_audio_send_us = 0U;
    uint64_t audio_progress_baseline_us = 0U;
    uint64_t audio_progress_grace_deadline_us = 0U;
    bool audio_progress_watchdog_armed = false;
    AudioNoProgressState audio_no_progress_state = AudioNoProgressState::Idle;
    uint64_t audio_no_progress_started_us = 0U;
    uint64_t audio_no_progress_deadline_us = 0U;

#ifdef PICO_ASHA_AUDIO_STALL_TRACE
    uint64_t trace_can_send_request_us = 0U;
    uint64_t trace_can_send_now_us = 0U;
    uint64_t trace_l2cap_send_us = 0U;
    uint64_t trace_audio_busy_since_us = 0U;
    uint16_t trace_connection_interval = 0U;
    uint16_t trace_peripheral_latency = 0U;
    uint16_t trace_supervision_timeout = 0U;
    uint8_t trace_pending_sequence = AUDIO_STALL_TRACE_INVALID_SEQUENCE;
    uint8_t trace_last_sequence = AUDIO_STALL_TRACE_INVALID_SEQUENCE;
    bool trace_busy_stall_reported = false;
    bool trace_ring_underrun_reported = false;
    bool trace_zero_credits_active = false;
    uint64_t trace_zero_credits_since_us = 0U;
    uint32_t trace_last_tx_blocker_mask = 0U;
    uint64_t trace_last_tx_blocked_report_us = 0U;
    bool trace_can_send_age_reported = false;
    bool trace_packet_sent_age_reported = false;
    uint32_t trace_audio_tx_checksum = 0U;
    uint32_t trace_audio_tx_published_write_index = 0U;
#endif

    bool stop_request_from_other = false;

    std::array<uint8_t, ASHA_SDU_SIZE_BYTES> recv_buff = {};

    uint16_t conn_id = comm::unset_conn_id;

    inline static uint16_t next_conn_id;

    inline static bool connections_allowed;
    inline static bool audio_streaming_enabled;
    inline static bool auto_pair_enabled;
    inline static bool desired_connection = true;
    inline static bool manual_shutdown = false;
    inline static ReconnectState reconnect_state = ReconnectState::Idle;
    inline static uint64_t reconnect_next_action_us = 0U;
    inline static uint64_t reconnect_deadline_us = 0U;
    inline static uint8_t reconnect_attempt_count = 0U;
    inline static bool reconnect_watchdog_requested = false;
    inline static uint16_t reconnect_handle = HCI_CON_HANDLE_INVALID;
    inline static uint16_t reconnect_cid = 0U;
    inline static uint32_t reconnect_read_index = 0U;
    inline static std::array<uint8_t, 6> reconnect_address = {};
    inline static comm::BLEConnectionState ble_connection_state =
        comm::BLEConnectionState::Disconnected;
#ifdef PICO_ASHA_AUDIO_STALL_TRACE_RSSI
    inline static bool rssi_request_pending = false;
    inline static hci_con_handle_t rssi_request_handle = HCI_CON_HANDLE_INVALID;
#endif

    /* Member functions */

    static HearingAid* get_by_con_handle(hci_con_handle_t handle);
    static HearingAid* get_by_cid(uint16_t cid);
    static HearingAid* get_by_conn_id(uint16_t conn_id);
    static HearingAid* get_by_cached_addr(bd_addr_t addr);
    static void set_other_side_ptrs();
    static void set_ble_connection_state(comm::BLEConnectionState state,
                                         bool force_event = false);
    static void schedule_reconnect(HearingAid* ha, uint8_t status,
                                   uint8_t reason);
    static void process_reconnect();
    static void clear_reconnect_reboot_guard();
    void assign_next_conn_id();
    bool is_connected();
    bool is_streaming();
    void set_process_busy();
    void unset_process_busy();
    void set_audio_busy(uint8_t sequence = AUDIO_STALL_TRACE_INVALID_SEQUENCE,
                        int32_t context = 0);
    void unset_audio_busy(int32_t context = 0);
    bool request_audio_can_send(uint32_t write_index);
    bool send_pending_audio(bool local_recovery, uint32_t write_index);
    bool process_audio_tx_watchdog(uint32_t write_index, bool audio_active);
    bool process_audio_no_progress(uint32_t write_index, bool pcm_is_streaming,
                                   uint64_t now_us);
    bool schedule_audio_if_idle(uint32_t write_index, uint64_t now_us,
                                bool recovery);
#ifdef PICO_ASHA_AUDIO_STALL_TRACE
    uint32_t trace_audio_tx_blockers(uint32_t write_index,
                                     bool pcm_is_streaming,
                                     uint64_t now_us) const;
    void trace_audio_tx_blocked_if_needed(uint32_t write_index,
                                          bool pcm_is_streaming,
                                          uint64_t now_us,
                                          bool verify_invariant);
#endif
    void reset_audio_progress_watchdog(uint64_t now_us, bool armed);
    void clear_local_audio_tx_state();
    void reconnect_after_audio_tx_stall(uint32_t write_index, uint32_t stalled_us,
                                        int32_t result);
    void set_data_langth();
    void send_acp_start();
    void send_acp_stop();
    void send_acp_status(uint8_t status);
    void send_volume(int8_t volume);
    void disconnect();
    void reset();
    const char* get_side_str();
};

} // namespace asha
