#include <bitset>
#include <pico/assert.h>
#include <pico/time.h>
#include <hardware/structs/watchdog.h>
#include <hardware/watchdog.h>

#include "hearing_aid.hpp"
#include "asha_uuid.hpp"
#include "bt_status_err.hpp"
#include "usb_common.hpp"

namespace asha
{

/* Data length variables */
static constexpr uint16_t pdu_len = 167u;

static constexpr uint16_t max_tx_time = (pdu_len + 14) * 8;

static constexpr size_t ev_packet_str_size = sizeof comm::EventPacket::data.str;

static constexpr int max_error_count = 10;

static constexpr uint32_t reconnect_reboot_guard_magic = 0x5245434fu;
static constexpr uint32_t reconnect_watchdog_delay_ms = 100u;

static int8_t volume_mute = -128;

namespace ASPStatus
{
    constexpr int8_t unkown_command = -1;
    constexpr int8_t illegal_params = -2;
    constexpr int8_t ok = 0;
}

namespace ACPOpCode
{
    constexpr uint8_t start  = 1;
    constexpr uint8_t stop   = 2;
    constexpr uint8_t status = 3;
}

namespace ACPStatus
{
    constexpr uint8_t other_disconnected = 0;
    constexpr uint8_t other_connected = 1;
    constexpr uint8_t conn_param_updated = 2;
}

static bool gatt_service_valid(gatt_client_service_t* service);

static void delete_paired_devices();
static void delete_paired_device(const bd_addr_t address);

static bool gatt_service_valid(gatt_client_service_t* service)
{
    return service->end_group_handle > 0U;
}

/* Get value from (sub) array of bytes */
template<typename T>
static T get_val(const uint8_t *start)
{
    T val;
    std::memcpy(&val, start, sizeof(val));
    return val;
}

/* Public methods */

HearingAid::HearingAid()
{
    auth_req = SM_AUTHREQ_BONDING | SM_AUTHREQ_SECURE_CONNECTION;
    if (hearing_aids[0] == nullptr) {
        hearing_aids[0] = this;
    } else if (hearing_aids[1] == nullptr) {
        hearing_aids[1] = this;
    } else {
        hard_assert(false);
    }
    next_conn_id = comm::unset_conn_id;
    connections_allowed = true;
    audio_streaming_enabled = true;
    auto_pair_enabled = false;
}

void HearingAid::process()
{
    using enum ProcessState;
    using namespace comm;

    // Reconnect work is deliberately driven from the BTstack run loop, never
    // nested inside an HCI/L2CAP callback.
    process_reconnect();

    for (auto ha : hearing_aids) {
        uint8_t res = ERROR_CODE_SUCCESS;
        if (!ha->is_connected()) { continue; }
        if (ha->process_delay_ticks > 0) {
            --ha->process_delay_ticks;
            continue;
        }
        // Just disconnect the hearing aid if we get too many errors
        if (ha->error_count >= max_error_count) {
            ha->disconnect();
            continue;
        }
        EventType ev_type;
        switch (ha->process_state)
        {
            case DiscoverServices:
                ev_type = EventType::DiscServices;
                //LOG_INFO("%s: Discovering services", ha->get_side_str());
                ha->set_process_busy();
                res = gatt_client_discover_primary_services(&HearingAid::handle_service_discovery, ha->conn_handle);
                break;
            case PairBond:
                ev_type = EventType::PairAndBond;
                //LOG_INFO("%s: Pairing and bonding",  ha->get_side_str())
                ha->set_process_busy();
                sm_request_pairing(ha->conn_handle);
                break;
            case DataLength:
                ev_type = EventType::DLE;
                //LOG_INFO("%s: Setting data length",  ha->get_side_str())
                // If the controller can't take the command yet, yield and retry on
                // the next tick rather than spinning the run loop. Advance to
                // discovery right after sending: the controller only emits
                // HCI_SUBEVENT_LE_DATA_LENGTH_CHANGE when the effective length
                // actually changes, so an aid already at the requested size would
                // never fire it and we'd stall waiting.
                if (!hci_can_send_command_packet_now()) { break; }
                ha->set_data_langth();
                ha->process_state = DiscoverChars;
                break;
            case DiscoverChars: {
                ha->set_process_busy();
                auto i = ha->service_index++; // Yes, the post-increment is on purpose
                auto s = ha->service_arr[i];
                ev_type = ha->service_ev_arr[i];
                res = gatt_client_discover_characteristics_for_service(&HearingAid::handle_char_discovery, ha->conn_handle, s);
                break;
            }
            case ReadChars: {
                ha->set_process_busy();
                auto i = ha->chars_index++; // Yes, the post-increment is on purpose
                auto c = ha->chars_arr[i];
                ev_type = ha->chars_ev_arr[i];
                res = gatt_client_read_value_of_characteristic(&HearingAid::handle_char_read, ha->conn_handle, c);
                break;
            }
            case ConnectL2CAP:
                ev_type = EventType::L2CAPCon;
                //LOG_INFO("%s: Create L2CAP CoC", ha->get_side_str());
                ha->set_process_busy();
                res = l2cap_cbm_create_channel(&HearingAid::handle_l2cap_cbm, 
                                               ha->conn_handle, 
                                               ha->psm, 
                                               ha->recv_buff.data(), 
                                               ha->recv_buff.size(), 
                                               L2CAP_LE_AUTOMATIC_CREDITS, 
                                               LEVEL_2, 
                                               &ha->cid);
                break;
            case EnBattNotification:
                ev_type = EventType::MFIBatteryNotEnable;
                ha->set_process_busy();
                res = gatt_client_write_client_characteristic_configuration(&HearingAid::handle_notification_reg, 
                                                                            ha->conn_handle, 
                                                                            &ha->services.mfi.battery, 
                                                                            GATT_CLIENT_CHARACTERISTICS_CONFIGURATION_NOTIFICATION);
                break;
            case EnASPNotification:
                ev_type = EventType::ASPNotEnable;
                //LOG_INFO("%s: Enable ASP notification", ha->get_side_str());
                ha->set_process_busy();
                res = gatt_client_write_client_characteristic_configuration(&HearingAid::handle_notification_reg, 
                                                                            ha->conn_handle, 
                                                                            &ha->services.asha.asp, 
                                                                            GATT_CLIENT_CHARACTERISTICS_CONFIGURATION_NOTIFICATION);
                break;
            case Finalize:
                ev_type = EventType::StreamReady;
                //LOG_INFO("%s: Ready for audio streaming", ha->get_side_str());
                ha->set_process_busy();
                if (full_set_connected()) {
                    runtime_settings.set_full_set_paired(true);
                    led_mgr.set_led(LEDManager::State::On);
                } else {
                    led_mgr.set_led_pattern(one_connected);
                    start_scan();
                }
                ha->cached = true;
                ha->process_state = Audio;
                reconnect_state = ReconnectState::Idle;
                reconnect_attempt_count = 0U;
                reconnect_next_action_us = 0U;
                reconnect_deadline_us = 0U;
                reconnect_watchdog_requested = false;
                clear_reconnect_reboot_guard();
#ifdef PICO_ASHA_AUDIO_STALL_TRACE
                audio_stall_trace_bluetooth_lifecycle(
                    AUDIO_STALL_TRACE_ASHA_DEVICE_CONNECTED,
                    ha->conn_handle, ha->cid, ha->addr,
                    ERROR_CODE_SUCCESS, AUDIO_STALL_TRACE_STATUS_UNAVAILABLE,
                    ERROR_CODE_SUCCESS, asha_audio_get_write_index(),
                    ha->curr_read_index,
                    (ha->audio_state & AudioState::AudioBusy) != 0U);
#endif
                if (ha->other && ha->other->is_streaming()) {
                    ha->other->stop_request_from_other = true;
                }
                ha->audio_state = AudioState::Ready;
            default:
                break;
        }
        if (res != ERROR_CODE_SUCCESS) {
            add_event_to_buffer(ha->conn_id, EventPacket(ev_type, StatusType::BtstackStatus, res));
            //LOG_ERROR("%s: BTStack error: %s", ha->get_side_str(), bt_err_str(res));
            ha->unset_process_busy();
            ++ha->error_count;
            ha->process_delay_ticks = ha_process_delay_ticks;
        }
    }
}

/* Public static methods */

int HearingAid::num_connected()
{
    int num = 0;
    for (auto ha : hearing_aids) {
        if (ha->is_connected()) {
            ++num;
        }
    }
    return num;
}

std::array<HearingAid*,2> HearingAid::connected_has()
{
    std::array<HearingAid*,2> has = {nullptr, nullptr};
    size_t i = 0;
    for (auto ha : hearing_aids) {
        if (ha->is_connected()) {
            has[i] = ha;
        }
    }
    return has;
}

void HearingAid::on_serial_host_connected()
{
    using namespace comm;
    RemoteInfo info;
    uint16_t intro_flags = 0x00;
    if (connections_allowed) {
        intro_flags |= IntroFlags::conn_allowed;
    }
    if (audio_streaming_enabled) {
        intro_flags |= IntroFlags::streaming_enabled;
    }
    intro_flags |= IntroFlags::firmware_managed_reconnect;
    send_intro_packet((int8_t)num_connected(), intro_flags);
    USBInfo usb_info = {
        .uac_vers = usb_settings.uac_version,
        .min_vol = usb_settings.min_vol,
        .max_vol = usb_settings.max_vol,
        .reserved = 0
    };
    send_usb_info_packet(usb_info);
    set_ble_connection_state(ble_connection_state, true);
    for (auto ha : hearing_aids) {
        if (!ha->is_connected()) {
            continue;
        }
        info.conn_id = ha->conn_id;
        info.hci_handle = ha->conn_handle;
        bd_addr_copy(info.addr, ha->addr);
        info.connected = true;
        info.paired = ha->paired_and_bonded;
        info.psm = ha->psm;
        info.l2cap_id = ha->cid;
        memcpy(info.dev_name, ha->device_name.data(), sizeof(info.dev_name));
        memcpy(info.mfg_name, ha->manufacturer.data(), sizeof(info.mfg_name));
        memcpy(info.model_name, ha->model.data(), sizeof(info.model_name));
        memcpy(info.fw_vers, ha->fw_vers.data(), sizeof(info.fw_vers));
        memcpy(info.sw_vers, ha->sw_vers.data(), sizeof(info.sw_vers));
        if (ha->rop) {
            info.side = (ha->rop.side() == Side::Left) ? CSide::Left : CSide::Right;
            info.mode = (ha->rop.mode() == Mode::Binaural) ? CMode::Binaural : CMode::Mono;
        } else {
            info.side = CSide::Unset;
            info.mode = CMode::Unset;
        }
        info.audio_streaming = ha->is_streaming();
        info.curr_vol = ha->curr_vol;
        info.curr_battery = ha->battery_level;

        send_remote_info_packet(info);
    }
}

bool HearingAid::is_addr_connected(const bd_addr_t addr)
{
    for (auto ha : hearing_aids) {
        if (bd_addr_cmp(addr, ha->addr) == 0 && ha->is_connected()) {
            return true;
        }
    }
    return false;
}

void HearingAid::set_connections_allowed(bool allowed)
{
    desired_connection = allowed;
    manual_shutdown = !allowed;
    if (connections_allowed == allowed) {
        return;
    }
    if (connections_allowed && !allowed) {
        connections_allowed = false;
        gap_stop_scan();
        set_ble_connection_state(comm::BLEConnectionState::Disconnected);
        for (auto ha : hearing_aids) {
            if (ha->is_connected()) {
                ha->disconnect();
            }
        }
    } else if (!connections_allowed && allowed) {
        connections_allowed = true;
        reconnect_attempt_count = 0U;
        reconnect_watchdog_requested = false;
        schedule_reconnect(nullptr, ERROR_CODE_SUCCESS, 0U);
    }
}

void HearingAid::set_audio_streaming_enabled(bool enabled)
{
    audio_streaming_enabled = enabled;
}

void HearingAid::set_auto_pair_enabled(bool enabled)
{
    auto_pair_enabled = enabled;
}

void HearingAid::start_scan()
{
    if (connections_allowed && desired_connection && !manual_shutdown) {
#ifdef PICO_ASHA_AUDIO_STALL_TRACE
        audio_stall_trace_reconnect_transition(
            AUDIO_STALL_TRACE_SCAN_START_REQUESTED, reconnect_handle,
            reconnect_cid, reconnect_address.data(),
            asha_audio_get_write_index(), reconnect_read_index,
            reconnect_attempt_count, ERROR_CODE_SUCCESS, 0U,
            ERROR_CODE_SUCCESS);
#endif
        set_ble_connection_state(comm::BLEConnectionState::Scanning);
        gap_set_scan_params(1, 0x0030, 0x0030, runtime_settings.get_full_set_paired() ? 1 : 0);
        gap_start_scan();
        if (reconnect_state != ReconnectState::Idle) {
            reconnect_state = ReconnectState::Scanning;
            reconnect_deadline_us = time_us_64() + reconnect_attempt_timeout_us;
        }
    } else {
        set_ble_connection_state(comm::BLEConnectionState::Disconnected);
    }
}

void HearingAid::on_ad_report(const AdvertisingReport& report)
{
    if (is_addr_connected(report.address)) { return; }
    gap_stop_scan();
    if (full_set_connected()) { 
        return; 
    }
    if (runtime_settings.get_full_set_paired() || (auto_pair_enabled && report.is_hearing_aid)) {
        connect(report.address, report.address_type);
    } else {
        comm::AdvertisingPacket ad_pkt = {};
        bd_addr_copy(ad_pkt.addr, report.address);
        ad_pkt.addr_type = report.address_type;
        ad_pkt.is_ha = report.is_hearing_aid;
        ad_pkt.rssi = report.rssi;
        size_t name_size = report.name.size() < sizeof(ad_pkt.name) ? report.name.size()
                                                                    : sizeof(ad_pkt.name);
        strncpy(ad_pkt.name, report.name.data(), name_size);
        ad_pkt.name[sizeof(ad_pkt.name)-1] = '\0';
        comm::send_advertising_packet(ad_pkt);
        start_scan();
    }
}

void HearingAid::connect(const bd_addr_t addr, bd_addr_type_t addr_type)
{
#ifdef PICO_ASHA_AUDIO_STALL_TRACE
    audio_stall_trace_reconnect_transition(
        AUDIO_STALL_TRACE_CONNECT_ATTEMPT, reconnect_handle, reconnect_cid,
        addr, asha_audio_get_write_index(), reconnect_read_index,
        reconnect_attempt_count, ERROR_CODE_SUCCESS, 0U,
        ERROR_CODE_SUCCESS);
#endif
    set_ble_connection_state(comm::BLEConnectionState::Connecting);
    gap_stop_scan();
    uint8_t result = gap_connect(addr, addr_type);
    if (result != ERROR_CODE_SUCCESS) {
        schedule_reconnect(nullptr, result, 0U);
        return;
    }
    if (reconnect_state != ReconnectState::Idle) {
        reconnect_state = ReconnectState::Connecting;
        reconnect_deadline_us = time_us_64() + reconnect_attempt_timeout_us;
    }
}

void HearingAid::on_connected(uint8_t status, bd_addr_t addr,
                              hci_con_handle_t handle,
                              uint16_t connection_interval, uint16_t peripheral_latency,
                              uint16_t supervision_timeout)
{
    using namespace comm;

#ifdef PICO_ASHA_AUDIO_STALL_TRACE
    audio_stall_trace_reconnect_transition(
        AUDIO_STALL_TRACE_CONNECT_COMPLETE, handle, 0U, addr,
        asha_audio_get_write_index(), reconnect_read_index,
        reconnect_attempt_count, status, 0U, status);
#endif
    if (status != ERROR_CODE_SUCCESS) {
        schedule_reconnect(nullptr, status, 0U);
        return;
    }
    if (reconnect_state != ReconnectState::Idle) {
        reconnect_state = ReconnectState::Initializing;
        reconnect_deadline_us = time_us_64() + reconnect_attempt_timeout_us;
        reconnect_handle = handle;
        reconnect_cid = 0U;
        bd_addr_copy(reconnect_address.data(), addr);
    }

    auto ha_cached = get_by_cached_addr(addr);
    if (ha_cached) {
        //LOG_INFO("%s: Connected to cached HA", bd_addr_to_str(addr));
        ha_cached->connected = true;
        ha_cached->conn_handle = handle;
#ifdef PICO_ASHA_AUDIO_STALL_TRACE
        ha_cached->trace_connection_interval = connection_interval;
        ha_cached->trace_peripheral_latency = peripheral_latency;
        ha_cached->trace_supervision_timeout = supervision_timeout;
#endif
        set_other_side_ptrs();
        ha_cached->assign_next_conn_id();
        set_ble_connection_state(BLEConnectionState::Connected);

        EventPacket ev_pkt(EventType::RemoteConnected);
        ev_pkt.data.conn_info.hci_handle = handle;
        bd_addr_copy(ev_pkt.data.conn_info.addr, ha_cached->addr);
        add_event_to_buffer(ha_cached->conn_id, ev_pkt);

        ha_cached->process_state = ProcessState::PairBond;
        return;
    }
    for (auto ha : hearing_aids) {
        if (ha->is_connected()) { continue; }
        //LOG_INFO("%s: Connected to HA", bd_addr_to_str(addr))
        ha->connected = true;
        ha->cached = false;
        bd_addr_copy(ha->addr, addr);
        ha->conn_handle = handle;
#ifdef PICO_ASHA_AUDIO_STALL_TRACE
        ha->trace_connection_interval = connection_interval;
        ha->trace_peripheral_latency = peripheral_latency;
        ha->trace_supervision_timeout = supervision_timeout;
#endif
        set_other_side_ptrs();
        ha->assign_next_conn_id();
        set_ble_connection_state(BLEConnectionState::Connected);

        EventPacket ev_pkt(EventType::RemoteConnected);
        ev_pkt.data.conn_info.hci_handle = handle;
        bd_addr_copy(ev_pkt.data.conn_info.addr, ha->addr);
        add_event_to_buffer(ha->conn_id, ev_pkt);

        ha->process_state = ProcessState::DiscoverServices;
        return;
    }
    EventPacket err_pkt(EventType::RemoteConnected, StatusType::PAStatus, PAError::PAMaxConnected);
    add_event_to_buffer(unset_conn_id, err_pkt);
    //LOG_ERROR("%s: Unable to connect - array full", bd_addr_to_str(addr));
    gap_disconnect(handle);
}

void HearingAid::on_connection_parameters_updated(hci_con_handle_t handle,
                                                  uint16_t connection_interval,
                                                  uint16_t peripheral_latency,
                                                  uint16_t supervision_timeout)
{
#ifdef PICO_ASHA_AUDIO_STALL_TRACE
    HearingAid* ha = get_by_con_handle(handle);
    if (ha == nullptr) return;
    ha->trace_connection_interval = connection_interval;
    ha->trace_peripheral_latency = peripheral_latency;
    ha->trace_supervision_timeout = supervision_timeout;
#else
    (void)handle;
    (void)connection_interval;
    (void)peripheral_latency;
    (void)supervision_timeout;
#endif
}

#ifdef PICO_ASHA_AUDIO_STALL_TRACE_RSSI
void HearingAid::sample_rssi()
{
    if (rssi_request_pending) {
        audio_stall_trace_rssi_request_skipped();
        return;
    }
    static uint8_t next_slot = 0U;
    for (uint8_t count = 0U; count < hearing_aids.size(); ++count) {
        HearingAid* ha = hearing_aids[next_slot];
        next_slot = (next_slot + 1U) % hearing_aids.size();
        if (!ha->is_connected()) continue;
        rssi_request_pending = true;
        rssi_request_handle = ha->conn_handle;
        (void)gap_read_rssi(ha->conn_handle);
        break;
    }
}

void HearingAid::on_rssi(hci_con_handle_t handle, int8_t rssi)
{
    if (rssi_request_pending && handle == rssi_request_handle) {
        rssi_request_pending = false;
        rssi_request_handle = HCI_CON_HANDLE_INVALID;
    }
    HearingAid* ha = get_by_con_handle(handle);
    if (ha == nullptr) return;
    audio_stall_trace_rssi(handle, ha->cid, ha->trace_last_sequence,
                           asha_audio_get_write_index(), ha->curr_read_index,
                           (ha->audio_state & AudioState::AudioBusy) != 0U, rssi);
}
#endif

void HearingAid::on_disconnected(hci_con_handle_t handle, uint8_t status, uint8_t reason)
{
    using namespace comm;
#ifdef PICO_ASHA_AUDIO_STALL_TRACE_RSSI
    if (rssi_request_pending && handle == rssi_request_handle) {
        rssi_request_pending = false;
        rssi_request_handle = HCI_CON_HANDLE_INVALID;
    }
#endif
    HearingAid* ha = get_by_con_handle(handle);
    if (ha == nullptr) {
#ifdef PICO_ASHA_AUDIO_STALL_TRACE
        uint32_t write_index = asha_audio_get_write_index();
        audio_stall_trace_bluetooth_lifecycle(
            AUDIO_STALL_TRACE_HCI_DISCONNECTION_COMPLETE, handle, 0U, nullptr,
            status, reason, AUDIO_STALL_TRACE_STATUS_UNAVAILABLE,
            write_index, write_index, false);
#endif
        if (desired_connection && !manual_shutdown) {
            schedule_reconnect(nullptr, status, reason);
        }
        return;
    }
#ifdef PICO_ASHA_AUDIO_STALL_TRACE
    uint64_t now_us = audio_stall_trace_now_us();
    uint32_t write_index = asha_audio_get_write_index();
    bool busy = (ha->audio_state & AudioState::AudioBusy) != 0U;
    uint32_t busy_duration_us = busy && now_us > ha->trace_audio_busy_since_us
                                    ? static_cast<uint32_t>(std::min<uint64_t>(
                                          now_us - ha->trace_audio_busy_since_us, UINT32_MAX))
                                    : 0U;
    uint8_t sequence = ha->trace_last_sequence != AUDIO_STALL_TRACE_INVALID_SEQUENCE
                           ? ha->trace_last_sequence
                           : ha->trace_pending_sequence;
    audio_stall_trace_bluetooth_lifecycle(
        AUDIO_STALL_TRACE_HCI_DISCONNECTION_COMPLETE,
        ha->conn_handle, ha->cid, ha->addr, status, reason,
        AUDIO_STALL_TRACE_STATUS_UNAVAILABLE, write_index,
        ha->curr_read_index, busy);
    audio_stall_trace_ble_disconnect(ha->conn_handle, ha->cid, sequence,
                                     write_index, ha->curr_read_index, busy,
                                     status, reason, busy_duration_us,
                                     ha->last_successful_audio_send_us);
    audio_stall_trace_bluetooth_lifecycle(
        AUDIO_STALL_TRACE_ASHA_DEVICE_DISCONNECTED,
        ha->conn_handle, ha->cid, ha->addr, status, reason,
        AUDIO_STALL_TRACE_STATUS_UNAVAILABLE, write_index,
        ha->curr_read_index, busy);
    audio_stall_trace_set_consumer(ha == hearing_aids[0] ? 0U : 1U, false,
                                   ha->curr_read_index);
    if (busy) ha->unset_audio_busy(AUDIO_STALL_TRACE_BUSY_CONTEXT_RESET);
#endif
    // if (ha->process_state == ProcessState::Disconnect) {
    //     LOG_INFO("%s: Disconnected", ha->get_side_str());
    // } else {
    //     LOG_ERROR("%s: Disconnected with status code: %s and reason: %s", 
    //                ha->get_side_str(), 
    //                bt_err_str(status), bt_err_str(reason));
    // }
    EventPacket ev_pkt(EventType::RemoteDisconnected, StatusType::ATTStatus, status, reason);
    add_event_to_buffer(ha->conn_id, ev_pkt);
    set_ble_connection_state(BLEConnectionState::Disconnected);
    if (ha->other && ha->other->is_streaming()) {
        ha->other->send_acp_status(ACPStatus::other_disconnected);
    }
    bool reconnect_required = desired_connection && !manual_shutdown;
    if (reconnect_required) {
        // Only record intent here. Scan/reset/connect operations are performed
        // later by process_reconnect() in the regular BTstack run loop.
        schedule_reconnect(ha, status, reason);
    }
    ha->process_state = ProcessState::Disconnect;
    ha->connected = false;
    set_other_side_ptrs();
    ha->reset();
    int num_c = num_connected();
    if (num_c == 1) {
        led_mgr.set_led_pattern(one_connected);
    } else {
        led_mgr.set_led_pattern(none_connected);
    }
    if (reconnect_required) {
        set_ble_connection_state(BLEConnectionState::Recovering);
    } else {
        reconnect_state = ReconnectState::Idle;
        set_ble_connection_state(BLEConnectionState::Disconnected);
    }
}

void HearingAid::on_data_len_set(hci_con_handle_t handle, 
    [[maybe_unused]] uint16_t rx_octets, 
    [[maybe_unused]] uint16_t rx_time, 
    [[maybe_unused]] uint16_t tx_octets, 
    [[maybe_unused]] uint16_t tx_time)
{
    HearingAid* ha = get_by_con_handle(handle);

    // DLE changes are handled asynchronously: the DataLength process state issues
    // the set-data-length command and advances to DiscoverChars itself, rather than
    // waiting on this event (which the controller doesn't emit when the effective
    // data length is unchanged). This handler is now informational only.
    (void)ha;
    // LOG_INFO("%s: DL set to: RX Octets: %hu, RX Time: %hu us, TX Octets: %hu, TX Time: %hu us",
    //                     ha->get_side_str(),
    //                     rx_octets, rx_time, tx_octets, tx_time);
}

void HearingAid::delete_pair()
{
    delete_paired_devices();
    runtime_settings.set_full_set_paired(false);
    for (auto ha : hearing_aids) {
        if (ha->is_connected()) {
            ha->disconnect();
        }
    }
}

void HearingAid::delete_pair(uint16_t conn_id)
{
    if (conn_id != comm::unset_conn_id) {
        auto ha = get_by_conn_id(conn_id);
        if (ha) {
            delete_paired_device(ha->addr);
            runtime_settings.set_full_set_paired(false);
            ha->disconnect();
        }
    }
}

void HearingAid::handle_sm(PACKET_HANDLER_PARAMS)
{
    using namespace comm;

    hci_con_handle_t handle = HCI_CON_HANDLE_INVALID;
    HearingAid *ha = nullptr;
    uint8_t att_status = ATT_ERROR_SUCCESS;
    uint8_t reason = 0U;

    uint8_t ev_type = hci_event_packet_get_type(packet);

    switch (ev_type) {
        case SM_EVENT_JUST_WORKS_REQUEST:
            handle = sm_event_just_works_request_get_handle(packet);
            ha = get_by_con_handle(handle);
            short_log(ha->conn_id, "%s", "Just Works Req");
            //LOG_INFO("%s: Just Works requested", ha->get_side_str());
            sm_just_works_confirm(handle);
            break;

        case SM_EVENT_PAIRING_STARTED:
            handle = sm_event_pairing_started_get_handle(packet);
            ha = get_by_con_handle(handle);
            short_log(ha->conn_id, "%s", "Pairing started");
            //LOG_INFO("%s: Pairing started", ha->get_side_str());
            break;

        case SM_EVENT_PAIRING_COMPLETE: {
            handle = sm_event_pairing_complete_get_handle(packet);
            ha = get_by_con_handle(handle);
            att_status = sm_event_pairing_complete_get_status(packet);
            reason = sm_event_pairing_complete_get_reason(packet);

            EventPacket ev_pkt(EventType::PairAndBond, StatusType::SMStatus, att_status, reason);
            switch (att_status) {
                case ERROR_CODE_SUCCESS:
                    //LOG_INFO("%s: Pairing complete", ha->get_side_str());;
                    ha->paired_and_bonded = true;
                    ha->process_state = ProcessState::DataLength;
                    add_event_to_buffer(ha->conn_id, EventPacket(EventType::PairAndBond));
                    break;
                case ERROR_CODE_CONNECTION_TIMEOUT:
                case ERROR_CODE_REMOTE_USER_TERMINATED_CONNECTION:
                    //LOG_ERROR("%s: Pairing failed - timeout", ha->get_side_str());
                    //LOG_ERROR("%s: Pairing failed - remote user terminated connection", ha->get_side_str());
                    add_event_to_buffer(ha->conn_id, ev_pkt);
                    ha->disconnect();
                    break;
                case ERROR_CODE_AUTHENTICATION_FAILURE:
                    if (reason == SM_REASON_AUTHENTHICATION_REQUIREMENTS) {
                        //LOG_ERROR("%s: Auth requirements not met. Attempting downgrade", ha->get_side_str());
                        short_log(ha->conn_id, "%s", "Attempt auth downgrade");
                        auth_req = SM_AUTHREQ_BONDING;
                        sm_set_authentication_requirements(auth_req);
                        sm_request_pairing(handle);
                    } else {
                        //LOG_ERROR("%s: Pairing failed, auth failure with reason: %s", ha->get_side_str(), sm_reason_str(reason));
                        add_event_to_buffer(ha->conn_id, ev_pkt);
                        ha->disconnect();
                    }
                    break;
                default:
                    //LOG_ERROR("%s: Unhandled SM_EVENT_PAIRING_COMPLETE error. Reason: %s", ha->get_side_str(), sm_reason_str(reason))
                    add_event_to_buffer(ha->conn_id, ev_pkt);
                    break;
            }
            break;
        }
        case SM_EVENT_REENCRYPTION_STARTED:
            handle = sm_event_reencryption_started_get_handle(packet);
            ha = get_by_con_handle(handle);
            short_log(ha->conn_id, "%s", "Reencryption started");
            //LOG_INFO("%s: Reencryption started", ha->get_side_str());
            break;

        case SM_EVENT_REENCRYPTION_COMPLETE: {
            handle = sm_event_reencryption_complete_get_handle(packet);
            att_status = sm_event_reencryption_complete_get_status(packet);
            ha = get_by_con_handle(handle);

            EventPacket ev_pkt(EventType::PairAndBond, StatusType::SMStatus, att_status);

            switch (att_status) {
                case ERROR_CODE_SUCCESS:
                    //LOG_INFO("%s: Reencryption succeeded", ha->get_side_str());
                    ha->paired_and_bonded = true;
                    ha->process_state = ProcessState::DataLength;
                    comm::add_event_to_buffer(ha->conn_id, EventPacket(EventType::PairAndBond));
                    break;
                case ERROR_CODE_PIN_OR_KEY_MISSING:
                    //LOG_ERROR("%s: Reencryption failed with ERROR_CODE_PIN_OR_KEY_MISSING", ha->get_side_str());
                    add_event_to_buffer(ha->conn_id, ev_pkt);
                    delete_paired_device(ha->addr);
                    ha->disconnect();
                    break;
                default:
                    //LOG_ERROR("%s: Reencryption error: %s", ha->get_side_str(), bt_err_str(att_status))
                    add_event_to_buffer(ha->conn_id, ev_pkt);
                    ha->disconnect();
                    break;
            }
            break;
        }
        case SM_EVENT_IDENTITY_RESOLVING_STARTED:
        case SM_EVENT_IDENTITY_RESOLVING_FAILED:
        case SM_EVENT_IDENTITY_RESOLVING_SUCCEEDED:
        case SM_EVENT_IDENTITY_CREATED:
            break;
        default:
            //LOG_ERROR("Unhandled SM event: 0x%02x", ev_type);
            short_log(unset_conn_id, "Unhandled SM Ev: 0x%02x", ev_type);
            break;
    }
}

void HearingAid::handle_service_discovery(PACKET_HANDLER_PARAMS)
{
    using namespace comm;
    hci_con_handle_t handle = HCI_CON_HANDLE_INVALID;
    uint8_t att_status = ATT_ERROR_SUCCESS;
    HearingAid *ha = nullptr;
    gatt_client_service_t s = {};
    switch (hci_event_packet_get_type(packet)) {
        case GATT_EVENT_SERVICE_QUERY_RESULT: {
            handle = gatt_event_service_query_result_get_handle(packet);
            ha = get_by_con_handle(handle);

            gatt_event_service_query_result_get_service(packet, &s);
            if (AshaUUID::service == s.uuid128 || AshaUUID::service16 == s.uuid16) {
                //LOG_INFO("%s: Discovered ASHA service", ha->get_side_str());
                short_log(ha->conn_id, "%s", "ASHA service discovered");
                ha->services.asha.service = s;
            } else if (GapUUID::service16 == s.uuid16) {
                //LOG_INFO("%s: Discovered GAP service", ha->get_side_str());
                short_log(ha->conn_id, "%s", "GAP service discovered");
                ha->services.gap.service = s;
            } else if (DisUUID::service16 == s.uuid16) {
                //LOG_INFO("%s: Discovered DIS service", ha->get_side_str());
                short_log(ha->conn_id, "%s", "DIS service discovered");
                ha->services.dis.service = s;
            } else if (MfiUUID::service == s.uuid128) {
                short_log(ha->conn_id, "%s", "MFI service discovered");
                ha->services.mfi.service = s;
            }
            break;
        }
        case GATT_EVENT_QUERY_COMPLETE:
            handle = gatt_event_query_complete_get_handle(packet);
            att_status = gatt_event_query_complete_get_att_status(packet);
            ha = get_by_con_handle(handle);

            if (att_status != ATT_ERROR_SUCCESS) {
                //LOG_ERROR("%s: Error discovering services with status: %s", ha->get_side_str(), att_err_str(att_status));
                add_event_to_buffer(ha->conn_id, EventPacket(EventType::DiscServices, StatusType::ATTStatus, att_status));
                ha->disconnect();
                break;
            } else if (!gatt_service_valid(&ha->services.asha.service)) {
                //LOG_ERROR("%s: ASHA service not discovered", ha->get_side_str());
                add_event_to_buffer(ha->conn_id, EventPacket(EventType::DiscServices, StatusType::PAStatus, PAError::PAASHAServiceNotFound));
                ha->disconnect();
                break;
            }
            ha->process_state = ProcessState::PairBond;
            break;
        default:
            break;
    }
}

void HearingAid::handle_char_discovery(PACKET_HANDLER_PARAMS)
{
    using enum ProcessState;
    using namespace comm;

    hci_con_handle_t handle = HCI_CON_HANDLE_INVALID;
    uint8_t att_status = ATT_ERROR_SUCCESS;
    HearingAid *ha = nullptr;
    gatt_client_characteristic_t c = {};

    switch (hci_event_packet_get_type(packet)) {
        case GATT_EVENT_CHARACTERISTIC_QUERY_RESULT: {
            handle = gatt_event_characteristic_query_result_get_handle(packet);
            ha = get_by_con_handle(handle);
            gatt_event_characteristic_query_result_get_characteristic(packet, &c);

            if (AshaUUID::readOnlyProps == c.uuid128) {
                //LOG_INFO("%s: Discovered ReadOnlyProperties characteristic", ha->get_side_str());
                short_log(ha->conn_id, "%s", "ROP char discovered");
                ha->services.asha.rop = c;
            } else if (AshaUUID::audioControlPoint == c.uuid128) {
                //LOG_INFO("%s: Discovered AudioControlPoint characteristic", ha->get_side_str());
                short_log(ha->conn_id, "%s", "ACP char discovered");
                ha->services.asha.acp = c;
            } else if (AshaUUID::audioStatus == c.uuid128) {
                //LOG_INFO("%s: Discovered AudioStatusPoint characteristic", ha->get_side_str());
                short_log(ha->conn_id, "%s", "ASP char discovered");
                ha->services.asha.asp = c;
            } else if (AshaUUID::volume == c.uuid128) {
                //LOG_INFO("%s: Discovered Volume characteristic", ha->get_side_str());
                short_log(ha->conn_id, "%s", "Volume char discovered");
                ha->services.asha.vol = c;
            } else if (AshaUUID::psm == c.uuid128) {
                //LOG_INFO("%s: Discovered PSM characteristic", ha->get_side_str());
                short_log(ha->conn_id, "%s", "PSM char discovered");
                ha->services.asha.psm = c;
            } else if (GapUUID::deviceName16 == c.uuid16) {
                //LOG_INFO("%s: Discovered Device Name characteristic", ha->get_side_str());
                short_log(ha->conn_id, "%s", "Device name char discovered");
                ha->services.gap.device_name = c;
            } else if (DisUUID::mfgName == c.uuid16) {
                //LOG_INFO("%s: Discovered Manufacturer Name characteristic", ha->get_side_str());
                short_log(ha->conn_id, "%s", "Mfg name char discovered");
                ha->services.dis.manufacture_name = c;
            } else if (DisUUID::modelNum == c.uuid16) {
                //LOG_INFO("%s: Discovered Model Number characteristic", ha->get_side_str());
                short_log(ha->conn_id, "%s", "Model num char discovered");
                ha->services.dis.model_num = c;
            } else if (DisUUID::fwVers == c.uuid16) {
                //LOG_INFO("%s: Discovered FW Version characteristic", ha->get_side_str());
                short_log(ha->conn_id, "%s", "FW vers char discovered");
                ha->services.dis.fw_vers = c;
            } else if (DisUUID::swVers == c.uuid16) {
                //LOG_INFO("%s: Discovered FW Version characteristic", ha->get_side_str());
                short_log(ha->conn_id, "%s", "SW vers char discovered");
                ha->services.dis.sw_vers = c;
            } else if (MfiUUID::battery == c.uuid128) {
                short_log(ha->conn_id, "%s", "MFI battery char discovered");
                ha->services.mfi.battery = c;
            }
            break;
        }
        case GATT_EVENT_QUERY_COMPLETE:
            handle = gatt_event_query_complete_get_handle(packet);
            att_status = gatt_event_query_complete_get_att_status(packet);
            ha = get_by_con_handle(handle);

            if (att_status != ATT_ERROR_SUCCESS) {
                auto ev_type = ha->service_ev_arr[ha->service_index - 1];
                add_event_to_buffer(ha->conn_id, EventPacket(ev_type, StatusType::ATTStatus, att_status));
                if (ev_type == EventType::DiscASHAChar) {
                    ha->disconnect();
                    break;
                }
            }
            if (ha->service_index < ha->service_arr.size()) {
                ha->process_state = DiscoverChars;
            } else {
                ha->process_state = ReadChars;
            }
            break;
    }
}

void HearingAid::handle_char_read(PACKET_HANDLER_PARAMS)
{
    using enum ProcessState;
    using namespace comm;

    hci_con_handle_t handle = HCI_CON_HANDLE_INVALID;
    uint8_t att_status = ATT_ERROR_SUCCESS;
    HearingAid *ha = nullptr;
    uint16_t val_len = 0U;
    uint16_t val_handle = 0U;
    const uint8_t* val = nullptr;

    switch (hci_event_packet_get_type(packet)) {
        case GATT_EVENT_CHARACTERISTIC_VALUE_QUERY_RESULT:
            handle = gatt_event_characteristic_value_query_result_get_handle(packet);
            val_handle = gatt_event_characteristic_value_query_result_get_value_handle(packet);
            val_len = gatt_event_characteristic_value_query_result_get_value_length(packet);
            val = gatt_event_characteristic_value_query_result_get_value(packet);
            ha = get_by_con_handle(handle);

            if (val_handle == ha->services.asha.rop.value_handle) {
                //LOG_INFO("%s: ROP characteristic read", ha->get_side_str());
                ha->rop.read(val);
                ha->side_str = ha->rop.side() == Side::Left ? "Left" : "Right";
            } else if (val_handle == ha->services.asha.psm.value_handle) {
                ha->psm = val[0];
                //LOG_INFO("%s: PSM characteristic read: %d", ha->get_side_str(), ha->psm);
            } else if (val_handle == ha->services.gap.device_name.value_handle) {
                //LOG_INFO("%s: Device name read", ha->get_side_str());
                ha->device_name.clear();
                ha->device_name.append((const char*)val, val_len);
            } else if (val_handle == ha->services.dis.manufacture_name.value_handle) {
                //LOG_INFO("%s: Manufacterer name read", ha->get_side_str());
                ha->manufacturer.clear();
                ha->manufacturer.append((const char*)val, val_len);
            } else if (val_handle == ha->services.dis.model_num.value_handle) {
                //LOG_INFO("%s: Model number read", ha->get_side_str());
                ha->model.clear();
                ha->model.append((const char*)val, val_len);
            } else if (val_handle == ha->services.dis.fw_vers.value_handle) {
                //LOG_INFO("%s: FW version read", ha->get_side_str());
                ha->fw_vers.clear();
                ha->fw_vers.append((const char*)val, val_len);
            } else if (val_handle == ha->services.dis.sw_vers.value_handle) {
                //LOG_INFO("%s: FW version read", ha->get_side_str());
                ha->sw_vers.clear();
                ha->sw_vers.append((const char*)val, val_len);
            } else if (val_handle == ha->services.mfi.battery.value_handle) {
                ha->battery_level = val[0];
            }
            break;

        case GATT_EVENT_QUERY_COMPLETE: {
            handle = gatt_event_query_complete_get_handle(packet);
            att_status = gatt_event_query_complete_get_att_status(packet);
            ha = get_by_con_handle(handle);

            auto ev_type = ha->chars_ev_arr[ha->chars_index - 1];

            if (att_status != ATT_ERROR_SUCCESS) {
                add_event_to_buffer(ha->conn_id, EventPacket(ev_type, StatusType::ATTStatus, att_status));
                if (ev_type == EventType::ROPRead || ev_type == EventType::PSMRead) {
                    ha->disconnect();
                    break;
                }
            }
            EventPacket ev_pkt(ev_type);
            switch (ev_type) {
                case EventType::ROPRead:
                    memcpy(ev_pkt.data.rop, ha->rop.raw_data, sizeof ev_pkt.data.rop);
                    add_event_to_buffer(ha->conn_id, ev_pkt);
                    break;
                case EventType::PSMRead:
                    ev_pkt.data.psm = ha->psm;
                    add_event_to_buffer(ha->conn_id, ev_pkt);
                    break;
                case EventType::DevNameRead:
                    ev_pkt.set_data_str("%s", ha->device_name.c_str());
                    add_event_to_buffer(ha->conn_id, ev_pkt);
                    break;
                case EventType::MfgRead:
                    ev_pkt.set_data_str("%s", ha->manufacturer.c_str());
                    add_event_to_buffer(ha->conn_id, ev_pkt);
                    break;
                case EventType::ModelRead:
                    ev_pkt.set_data_str("%s", ha->model.c_str());
                    add_event_to_buffer(ha->conn_id, ev_pkt);
                    break;
                case EventType::FWRead:
                    ev_pkt.set_data_str("%s", ha->fw_vers.c_str());
                    add_event_to_buffer(ha->conn_id, ev_pkt);
                    break;
                case EventType::SWRead:
                    ev_pkt.set_data_str("%s", ha->sw_vers.c_str());
                    add_event_to_buffer(ha->conn_id, ev_pkt);
                    break;
                case EventType::MfiBatteryRead:
                    ev_pkt.data.battery_level = ha->battery_level;
                    add_event_to_buffer(ha->conn_id, ev_pkt);
                    break;
                default:
                    break;
            }

            if (ha->chars_index < ha->chars_arr.size()) {
                ha->process_state = ReadChars;
            } else {
                ha->process_state = ConnectL2CAP;
            }
            break;
        }
        default:
            break;
    }
}

void HearingAid::handle_acp_write(PACKET_HANDLER_PARAMS)
{
    using namespace comm;
    hci_con_handle_t handle = HCI_CON_HANDLE_INVALID;
    uint8_t att_status = ATT_ERROR_SUCCESS;
    HearingAid *ha = nullptr;

    if (hci_event_packet_get_type(packet) == GATT_EVENT_QUERY_COMPLETE) {
        handle = gatt_event_query_complete_get_handle(packet);
        att_status = gatt_event_query_complete_get_att_status(packet);
        ha = get_by_con_handle(handle);

        EventType ev_type = ha->audio_state == AudioState::Start ? EventType::ACPStart 
                                                                 : EventType::ACPStop;
        if (att_status != ATT_ERROR_SUCCESS) {
            //LOG_ERROR("%s: ACP write failed with status: %s", ha->get_side_str(), att_err_str(att_status));
            add_event_to_buffer(ha->conn_id, EventPacket(ev_type, StatusType::ATTStatus, att_status));
            ha->audio_state = AudioState::Ready;
        } else {
            add_event_to_buffer(ha->conn_id, EventPacket(ev_type));
            if (ha->audio_state == AudioState::Stop) {
                if (ha->other && ha->other->is_streaming()) {
                    ha->other->send_acp_status(ACPStatus::other_disconnected);
                }
            }
        }
    }
}

void HearingAid::handle_l2cap_cbm(PACKET_HANDLER_PARAMS)
{
    using namespace comm;

    hci_con_handle_t handle = HCI_CON_HANDLE_INVALID;
    uint16_t cid = 0U;
    uint8_t bt_status = ERROR_CODE_SUCCESS;
    HearingAid *ha = nullptr;

    switch (hci_event_packet_get_type(packet)) {
        case L2CAP_EVENT_CBM_CHANNEL_OPENED:
            handle = l2cap_event_cbm_channel_opened_get_handle(packet);
            bt_status = l2cap_event_cbm_channel_opened_get_status(packet);
            ha = get_by_con_handle(handle);
#ifdef PICO_ASHA_AUDIO_STALL_TRACE
            audio_stall_trace_bluetooth_lifecycle(
                AUDIO_STALL_TRACE_L2CAP_CHANNEL_OPENED,
                handle, l2cap_event_cbm_channel_opened_get_local_cid(packet),
                ha->addr, AUDIO_STALL_TRACE_STATUS_UNAVAILABLE,
                AUDIO_STALL_TRACE_STATUS_UNAVAILABLE, bt_status,
                asha_audio_get_write_index(), ha->curr_read_index,
                (ha->audio_state & AudioState::AudioBusy) != 0U);
#endif
            if (bt_status != ATT_ERROR_SUCCESS) {
                //LOG_ERROR("%s: Error creating L2CAP cbm connection: %s", ha->get_side_str(), bt_err_str(att_status));
                add_event_to_buffer(ha->conn_id, EventPacket(EventType::L2CAPCon, StatusType::L2CapStatus, bt_status));
                // Try again later
                ha->unset_process_busy();
                ++ha->error_count;
                ha->process_delay_ticks = ha_process_delay_ticks * 3;
            } else {
                ha->l2cap_channel_open = true;
                ha->reset_audio_progress_watchdog(time_us_64(), false);
                //LOG_INFO("%s: L2CAP cbm connection created", ha->get_side_str());
                EventPacket ev_pkt(EventType::L2CAPCon);
                ev_pkt.data.cid = ha->cid;
                add_event_to_buffer(ha->conn_id, ev_pkt);
                ha->process_state = ProcessState::EnASPNotification;
#ifdef PICO_ASHA_AUDIO_STALL_TRACE
                uint32_t write_index = asha_audio_get_write_index();
                audio_stall_trace_ble_connect(ha->conn_handle, ha->cid,
                                              ha->trace_last_sequence,
                                              write_index, ha->curr_read_index,
                                              (ha->audio_state & AudioState::AudioBusy) != 0U,
                                              ha->trace_connection_interval,
                                              ha->trace_peripheral_latency,
                                              ha->trace_supervision_timeout);
#endif
            }
            break;
        case L2CAP_EVENT_CHANNEL_CLOSED:
            cid = l2cap_event_channel_closed_get_local_cid(packet);
            ha = get_by_cid(cid);
            if (ha != nullptr) {
                ha->l2cap_channel_open = false;
                ha->reset_audio_progress_watchdog(time_us_64(), false);
            }
#ifdef PICO_ASHA_AUDIO_STALL_TRACE
            if (ha != nullptr) {
                audio_stall_trace_bluetooth_lifecycle(
                    AUDIO_STALL_TRACE_L2CAP_CHANNEL_CLOSED,
                    ha->conn_handle, cid, ha->addr,
                    AUDIO_STALL_TRACE_STATUS_UNAVAILABLE,
                    AUDIO_STALL_TRACE_STATUS_UNAVAILABLE,
                    AUDIO_STALL_TRACE_STATUS_UNAVAILABLE,
                    asha_audio_get_write_index(), ha->curr_read_index,
                    (ha->audio_state & AudioState::AudioBusy) != 0U);
            } else {
                audio_stall_trace_bluetooth_lifecycle(
                    AUDIO_STALL_TRACE_L2CAP_CHANNEL_CLOSED,
                    HCI_CON_HANDLE_INVALID, cid, nullptr,
                    AUDIO_STALL_TRACE_STATUS_UNAVAILABLE,
                    AUDIO_STALL_TRACE_STATUS_UNAVAILABLE,
                    AUDIO_STALL_TRACE_STATUS_UNAVAILABLE,
                    asha_audio_get_write_index(), 0U, false);
            }
#endif
            break;
        case L2CAP_EVENT_CAN_SEND_NOW:
            cid = l2cap_event_can_send_now_get_local_cid(packet);
            ha = get_by_cid(cid);
            if (ha != nullptr && ha->audio_tx_stale_can_send_callbacks > 0U) {
                --ha->audio_tx_stale_can_send_callbacks;
                break;
            }
            if (ha == nullptr || ha->audio_tx_state != AudioTxState::WaitingCanSendNow ||
                ha->audio_tx_pending_generation == 0U ||
                ha->audio_tx_pending_generation != ha->audio_tx_generation) {
                // A direct watchdog recovery leaves BTstack's original request
                // pending. BTstack clears it before delivering the eventual stale
                // event, which must not trigger a second send.
                break;
            }
            {
                uint32_t write_index = asha_audio_get_write_index();
#ifdef PICO_ASHA_AUDIO_STALL_TRACE
                uint64_t now_us = time_us_64();
                uint32_t wait_us = now_us > ha->audio_tx_state_since_us
                                       ? static_cast<uint32_t>(std::min<uint64_t>(
                                             now_us - ha->audio_tx_state_since_us, UINT32_MAX))
                                       : 0U;
                if (!ha->trace_can_send_age_reported &&
                    wait_us >= AUDIO_STALL_TRACE_CAN_SEND_AGE_US) {
                    ha->trace_can_send_age_reported = true;
                    audio_stall_trace_tx_wait_age(
                        AUDIO_STALL_TRACE_CAN_SEND_REQUEST_AGE,
                        ha->conn_handle, cid, ha->audio_tx_sequence,
                        write_index, ha->curr_read_index,
                        (ha->audio_state & AudioState::AudioBusy) != 0U,
                        wait_us, static_cast<uint8_t>(ha->audio_tx_state),
                        ha->credits);
                }
                ha->trace_can_send_now_us = now_us;
                audio_stall_trace_can_send_now(ha->conn_handle, cid,
                                               ha->audio_tx_sequence,
                                               write_index, ha->curr_read_index,
                                               (ha->audio_state & AudioState::AudioBusy) != 0U,
                                               wait_us);
#endif
                ha->send_pending_audio(false, write_index);
            }
            break;
        case L2CAP_EVENT_PACKET_SENT:
            cid = l2cap_event_packet_sent_get_local_cid(packet);
            ha = get_by_cid(cid);
            if (ha == nullptr || ha->audio_tx_state != AudioTxState::WaitingPacketSent ||
                ha->audio_tx_pending_generation == 0U ||
                ha->audio_tx_pending_generation != ha->audio_tx_generation) {
                // Do not let an event from an older SDU clear AudioBusy or complete
                // the currently pending request.
                break;
            }
            {
#ifdef PICO_ASHA_AUDIO_STALL_TRACE
                uint64_t now_us = time_us_64();
                uint32_t wait_us = now_us > ha->audio_tx_state_since_us
                                       ? static_cast<uint32_t>(std::min<uint64_t>(
                                             now_us - ha->audio_tx_state_since_us, UINT32_MAX))
                                       : 0U;
                uint8_t sequence = ha->audio_tx_sequence;
                if (!ha->trace_packet_sent_age_reported &&
                    wait_us >= AUDIO_STALL_TRACE_PACKET_SENT_AGE_US) {
                    ha->trace_packet_sent_age_reported = true;
                    audio_stall_trace_tx_wait_age(
                        AUDIO_STALL_TRACE_PACKET_SENT_WAIT_AGE,
                        ha->conn_handle, cid, sequence,
                        asha_audio_get_write_index(), ha->curr_read_index,
                        (ha->audio_state & AudioState::AudioBusy) != 0U,
                        wait_us, static_cast<uint8_t>(ha->audio_tx_state),
                        ha->credits);
                }
#endif

                // Complete the local phase before any callback-visible work. A
                // stale CAN_SEND_NOW emitted by BTstack after direct recovery is
                // therefore ignored by the case above.
                ha->audio_tx_state = AudioTxState::Idle;
                ha->audio_tx_state_since_us = 0U;
                ha->audio_tx_sequence = AUDIO_STALL_TRACE_INVALID_SEQUENCE;
                ha->audio_tx_local_recovery = false;
                ha->audio_tx_pending_generation = 0U;
                ha->audio_data = nullptr;
#ifdef PICO_ASHA_AUDIO_STALL_TRACE
                audio_stall_trace_packet_sent(ha->conn_handle, cid,
                                              sequence,
                                              asha_audio_get_write_index(),
                                              ha->curr_read_index,
                                              (ha->audio_state & AudioState::AudioBusy) != 0U,
                                              wait_us);
                ha->trace_last_sequence = sequence;
#endif
                ha->unset_audio_busy(AUDIO_STALL_TRACE_BUSY_CONTEXT_PACKET_SENT);
            }
            break;
        default:
            break;
    }
}

void HearingAid::handle_notification_reg(PACKET_HANDLER_PARAMS)
{
    using namespace comm;

    if (hci_event_packet_get_type(packet) != GATT_EVENT_QUERY_COMPLETE) {
        return;
    }

    hci_con_handle_t handle = HCI_CON_HANDLE_INVALID;
    uint8_t att_status = ATT_ERROR_SUCCESS;
    HearingAid *ha = nullptr;

    handle = gatt_event_query_complete_get_handle(packet);
    att_status = gatt_event_query_complete_get_att_status(packet);
    ha = get_by_con_handle(handle);

    switch (ha->process_state) {
        case EnASPNotification | ProcessBusy:
            if (att_status != ATT_ERROR_SUCCESS) {
                //LOG_ERROR("%s: Failed to enable ASP notifications with status: %s", ha->get_side_str(), att_err_str(att_status));
                // Try again later
                ha->unset_process_busy();
                ++ha->error_count;
                ha->process_delay_ticks = ha_process_delay_ticks;
                add_event_to_buffer(ha->conn_id, EventPacket(EventType::ASPNotEnable, StatusType::ATTStatus, att_status));
            } else {
                //LOG_INFO("%s: ASP notifications enabled", ha->get_side_str());
                ha->process_state = ProcessState::EnBattNotification;
                add_event_to_buffer(ha->conn_id, EventPacket(EventType::ASPNotEnable));
            }
            break;
        case EnBattNotification | ProcessBusy:
            if (att_status != ATT_ERROR_SUCCESS) {
                // Not fatal - just continue
                add_event_to_buffer(ha->conn_id, EventPacket(EventType::MFIBatteryNotEnable, StatusType::ATTStatus, att_status));
            } else {
                add_event_to_buffer(ha->conn_id, EventPacket(EventType::MFIBatteryNotEnable));
            }
            ha->process_state = ProcessState::Finalize;
            break;
    }
    
}

void HearingAid::handle_gatt_notification(PACKET_HANDLER_PARAMS)
{
    using namespace comm;

    hci_con_handle_t handle = HCI_CON_HANDLE_INVALID;
    uint16_t val_handle = 0U;
    int8_t asp_status = 0;
    HearingAid *ha = nullptr;

    if (hci_event_packet_get_type(packet) == GATT_EVENT_NOTIFICATION) {
        handle = gatt_event_notification_get_handle(packet);
        val_handle = gatt_event_notification_get_value_handle(packet);
        ha = get_by_con_handle(handle);
        if (val_handle == ha->services.asha.asp.value_handle) {
            asp_status = (int8_t)gatt_event_notification_get_value(packet)[0];
            EventPacket err_ev_pkt(EventType::ASPError);
            err_ev_pkt.data.asp_not = asp_status;

            switch (asp_status) {
                case ASPStatus::ok:
                    if (ha->audio_state == AudioState::Start) {
                        //LOG_INFO("%s: Audio start OK", ha->get_side_str());
                        add_event_to_buffer(ha->conn_id, EventPacket(EventType::ASPStart));
                        ha->audio_state = AudioState::Streaming;
                        asha_audio_set_encoding_enabled(true);
                        if (ha->other && ha->other->is_streaming()) {
                            ha->other->send_acp_status(ACPStatus::other_connected);
                        }
                        ha->first_audio_send = true;
                        ha->reset_audio_progress_watchdog(time_us_64(), true);
                    } else if (ha->audio_state == AudioState::Stop) {
                        //LOG_INFO("%s: Audio stop OK", ha->get_side_str());
                        add_event_to_buffer(ha->conn_id, EventPacket(EventType::ASPStop));

                        ha->stop_request_from_other = false;
                        ha->audio_state = AudioState::Ready;
                    }
                    break;
                case ASPStatus::unkown_command:
                case ASPStatus::illegal_params:
                    add_event_to_buffer(ha->conn_id, err_ev_pkt);
                    break;
                default:
                    //LOG_ERROR("%s: ASP: Unknown command", ha->get_side_str());
                    //LOG_ERROR("%s: ASP: Illegal parameters", ha->get_side_str());
                    //LOG_ERROR("%s: ASP: Unknown status: %d", ha->get_side_str(), asp_status);
                    add_event_to_buffer(ha->conn_id, err_ev_pkt);
                    break;
            }
        } else if (val_handle == ha->services.mfi.battery.value_handle) {
            ha->battery_level = gatt_event_notification_get_value(packet)[0];
            EventPacket ev_pkt(EventType::MfiBatteryRead);
            ev_pkt.data.battery_level = ha->battery_level;
            add_event_to_buffer(ha->conn_id, ev_pkt);
        }
    }
}

bool HearingAid::process_audio()
{
    using namespace comm;
#ifdef PICO_ASHA_ENC_STATS
    bool send_enc_times = false;
#endif

    uint32_t w_index = asha_audio_get_write_index();
#ifdef PICO_ASHA_AUDIO_STALL_TRACE
    audio_stall_trace_process_audio_enter(audio_stall_trace_now_us(), w_index);
#endif
    int16_t usb_vol_l = asha_audio_get_curr_usb_vol(AshaAudioSide::AudioLeft);
    int16_t usb_vol_r = asha_audio_get_curr_usb_vol(AshaAudioSide::AudioRight);

    // Dividing the USB volume by ASHA_USB_VOL_RES gives a volume that
    // matches the ASHA volume. Volume is clamped to the min/max volume
    // set in runtime settings
    auto min_vol = usb_settings.min_vol;
    auto max_vol = usb_settings.max_vol;
    int8_t vol_l = (usb_vol_l == ASHA_USB_VOL_MUTE) ? volume_mute 
                                                    : (int8_t)(std::clamp(usb_vol_l, min_vol, max_vol) / ASHA_USB_VOL_RES);
    int8_t vol_r = (usb_vol_r == ASHA_USB_VOL_MUTE) ? volume_mute 
                                                    : (int8_t)(std::clamp(usb_vol_r, min_vol, max_vol) / ASHA_USB_VOL_RES);
    
    bool pcm_is_streaming = asha_audio_get_pcm_streaming_enabled();

    asha_audio_set_encode_mono(!(hearing_aids[0]->is_streaming() && hearing_aids[1]->is_streaming()));

    bool enable_process_delay = false;

    for (auto ha : hearing_aids) {
#ifdef PICO_ASHA_AUDIO_STALL_TRACE
        bool consumer_active = ha->process_state == ProcessState::Audio && ha->is_streaming();
        audio_stall_trace_set_consumer(ha == hearing_aids[0] ? 0U : 1U,
                                       consumer_active, ha->curr_read_index);
        if ((ha->audio_state & AudioState::AudioBusy) != 0U) {
            uint64_t now_us = audio_stall_trace_now_us();
            uint32_t busy_duration_us = now_us > ha->trace_audio_busy_since_us
                                            ? static_cast<uint32_t>(std::min<uint64_t>(
                                                  now_us - ha->trace_audio_busy_since_us,
                                                  UINT32_MAX))
                                            : 0U;
            if (busy_duration_us > AUDIO_STALL_TRACE_ANOMALY_US &&
                !ha->trace_busy_stall_reported) {
                ha->trace_busy_stall_reported = true;
                audio_stall_trace_busy(AUDIO_STALL_TRACE_BUSY_STALL,
                                       ha->conn_handle, ha->cid,
                                       ha->trace_pending_sequence, w_index,
                                       ha->curr_read_index, true, busy_duration_us,
                                       AUDIO_STALL_TRACE_BUSY_CONTEXT_AUDIO_SDU);
            }
        }
#endif
        if (ha->process_state != ProcessState::Audio) {
#ifdef PICO_ASHA_AUDIO_STALL_TRACE
            uint64_t now_us = time_us_64();
            ha->trace_audio_tx_blocked_if_needed(w_index, pcm_is_streaming,
                                                 now_us, false);
#endif
            continue;
        }
        uint64_t now_us = time_us_64();
        ha->credits = ha->l2cap_channel_open
                          ? l2cap_cbm_available_credits(ha->cid)
                          : 0U;
#ifdef PICO_ASHA_AUDIO_STALL_TRACE
        if (ha->is_streaming() && ha->l2cap_channel_open) {
            if (ha->credits == 0U && !ha->trace_zero_credits_active) {
                ha->trace_zero_credits_active = true;
                ha->trace_zero_credits_since_us = now_us;
                audio_stall_trace_credits_zero(
                    AUDIO_STALL_TRACE_CREDITS_ZERO_ENTER,
                    ha->conn_handle, ha->cid, ha->trace_last_sequence,
                    w_index, ha->curr_read_index,
                    (ha->audio_state & AudioState::AudioBusy) != 0U,
                    0U, 0U);
            } else if (ha->credits > 0U &&
                       ha->trace_zero_credits_active) {
                uint32_t zero_duration_us =
                    now_us > ha->trace_zero_credits_since_us
                        ? static_cast<uint32_t>(std::min<uint64_t>(
                              now_us - ha->trace_zero_credits_since_us,
                              UINT32_MAX))
                        : 0U;
                audio_stall_trace_credits_zero(
                    AUDIO_STALL_TRACE_CREDITS_ZERO_EXIT,
                    ha->conn_handle, ha->cid, ha->trace_last_sequence,
                    w_index, ha->curr_read_index,
                    (ha->audio_state & AudioState::AudioBusy) != 0U,
                    zero_duration_us, ha->credits);
                ha->trace_zero_credits_active = false;
                ha->trace_zero_credits_since_us = 0U;
            }
        } else {
            ha->trace_zero_credits_active = false;
            ha->trace_zero_credits_since_us = 0U;
        }
#endif
        if (ha->process_audio_no_progress(w_index, pcm_is_streaming, now_us)) {
#ifdef PICO_ASHA_AUDIO_STALL_TRACE
            ha->trace_audio_tx_blocked_if_needed(w_index, pcm_is_streaming,
                                                 now_us, false);
#endif
            continue;
        }
        // The remote ASHA stream remains active until ACP Stop completes. A
        // selected SDU must therefore be recovered even if host PCM stops while
        // the L2CAP request is stuck.
        bool audio_active = ha->is_streaming();
        if (ha->process_audio_tx_watchdog(w_index, audio_active)) {
            // One SDU already owns the TX path. In particular, never issue a
            // second CAN_SEND_NOW request while the original one is pending.
#ifdef PICO_ASHA_AUDIO_STALL_TRACE
            ha->trace_audio_tx_blocked_if_needed(w_index, pcm_is_streaming,
                                                 now_us, false);
#endif
            continue;
        }
        switch (ha->audio_state) {
            case AudioState::Ready:
                // Retained for the established Ready path. The zero-credit
                // flow-control branch no longer starts this cooldown.
                if (ha->zero_credits_cooldown > 0) {
                    --ha->zero_credits_cooldown;
                    if (ha->credits < 8) {
                        break;
                    }
                }
                if ((!ha->other || !ha->other->stop_request_from_other)
                    && audio_streaming_enabled
                    && pcm_is_streaming
                    && ha->credits >= 4) {
                        // Sync curr_vol from the host volume so the start payload
                        // reflects what the host is currently asking for. Without
                        // this, curr_vol's class default of -128 (the ASHA mute
                        // sentinel) gets baked into the first start after boot and
                        // some aids latch onto it, ignoring later volume writes.
                        ha->curr_vol = ha->rop.side() == Side::Left ? vol_l : vol_r;
                        ha->set_audio_busy(AUDIO_STALL_TRACE_INVALID_SEQUENCE,
                                           AUDIO_STALL_TRACE_BUSY_CONTEXT_ACP_START);
                        ha->send_acp_start();
                        ha->ready_stuck_ticks = 0;
                } else if (audio_streaming_enabled && pcm_is_streaming && ha->credits < 4) {
                    // Aid wedged with credits stuck below the start gate. Only a
                    // fresh L2CAP CoC channel resets the credit window, so force
                    // a reconnect once the stuck timeout elapses.
                    if (++ha->ready_stuck_ticks >= ready_stuck_timeout_ticks) {
                        ha->ready_stuck_ticks = 0;
                        short_log(ha->conn_id, "%s", "Ready state stuck, reconnecting");
                        ha->disconnect();
                    }
                } else {
                    ha->ready_stuck_ticks = 0;
                }
                break;
            case AudioState::Streaming:
                if (!audio_streaming_enabled || !pcm_is_streaming) {
                    asha_audio_set_encoding_enabled(false);
                    // LOG_INFO("%s: Stopping audio stream. PCM Streaming: %d, Credits: %d", 
                    //           ha->get_side_str(), (int)pcm_is_streaming, (int)ha->credits);
                    short_log(ha->conn_id, "PCM: %d - Cr: %d", (int)pcm_is_streaming, (int)ha->credits);
                    ha->send_acp_stop();
                } else if (ha->stop_request_from_other) {
                    short_log(ha->conn_id, "%s", "Stop requested from other");
                    ha->send_acp_stop();
                } else {
                    // Send volume update if volume has changed
                    int8_t v = ha->rop.side() == Side::Left ? vol_l : vol_r;
                    if (ha->curr_vol != v) {
                        ha->curr_vol = v;
                        // short_log(ha->conn_id, "USB L:%d R:%d", (int)usb_vol_l, (int)usb_vol_r);
                        ha->send_volume(ha->curr_vol);
                    }
                    if (w_index == 0) {
                        break;
                    }
                    if (ha->first_audio_send) {
                        ha->curr_read_index = w_index - 1U;
                        ha->first_audio_send = false;
                    }
                    if (ha->curr_read_index != w_index) {
#ifdef PICO_ASHA_AUDIO_STALL_TRACE
                        ha->trace_ring_underrun_reported = false;
#endif
                        if (ha->credits == 0) {
                            // Credit exhaustion is flow control, not a stream
                            // configuration change. Keep ACP/L2CAP alive and let
                            // the next run-loop pass schedule as soon as a credit
                            // returns. The independent no-progress watchdog still
                            // handles a genuinely stuck channel.
                            break;
                        }
                        // Level-trigger the scheduling invariant. If an earlier
                        // one-shot notification was lost, every later run-loop
                        // pass can safely re-arm exactly one normal request.
                        if (ha->schedule_audio_if_idle(w_index, now_us, false)) {
                            enable_process_delay = true;
#ifdef PICO_ASHA_ENC_STATS
                            send_enc_times = true;
#endif
                        }
                    }
#ifdef PICO_ASHA_AUDIO_STALL_TRACE
                    else {
                        uint64_t now_us = audio_stall_trace_now_us();
                        uint32_t empty_duration_us = audio_stall_trace_sdu_age_us(now_us);
                        if (empty_duration_us > AUDIO_STALL_TRACE_ANOMALY_US &&
                            !ha->trace_ring_underrun_reported) {
                            ha->trace_ring_underrun_reported = true;
                            audio_stall_trace_ring_underrun(
                                ha->conn_handle, ha->cid, ha->trace_last_sequence,
                                w_index, ha->curr_read_index, false, empty_duration_us);
                        }
                    }
#endif
                }
                break;
            default:
                break;
        }
#ifdef PICO_ASHA_AUDIO_STALL_TRACE
        ha->trace_audio_tx_blocked_if_needed(w_index, pcm_is_streaming,
                                             now_us, true);
#endif
    }
#ifdef PICO_ASHA_ENC_STATS
    if (send_enc_times) {
        auto enc_times = asha_audio_get_encoding_time_at_index(w_index == 0 ? 0 : w_index - 1);
        EventPacket pkt1(EventType::G722EncTimings);
        EventPacket pkt2(EventType::G722EncTimings);
        memcpy(pkt1.data.encode_timings, enc_times, 20);
        memcpy(pkt2.data.encode_timings, enc_times + 10, 20);
        add_event_to_buffer(unset_conn_id, pkt1);
        add_event_to_buffer(unset_conn_id, pkt2);
    }
#endif
    return enable_process_delay;
}

/* Private methods */

HearingAid* HearingAid::get_by_con_handle(hci_con_handle_t handle)
{
    for (auto ha : hearing_aids) {
        if (ha->conn_handle == handle) {
            return ha;
        }
    }
    return nullptr;
}

HearingAid* HearingAid::get_by_cid(uint16_t cid)
{
    for (auto ha : hearing_aids) {
        if (ha->cid == cid) {
            return ha;
        }
    }
    return nullptr;
}

HearingAid* HearingAid::get_by_conn_id(uint16_t conn_id)
{
    for (auto ha : hearing_aids) {
        if (ha->conn_id == conn_id) {
            return ha;
        }
    }
    return nullptr;
}

HearingAid* HearingAid::get_by_cached_addr(bd_addr_t addr)
{
    for (auto ha : hearing_aids) {
        if (ha->cached && bd_addr_cmp(ha->addr, addr) == 0) {
            return ha;
        }
    }
    return nullptr;
}

bool HearingAid::full_set_connected()
{
    bool have_left = false;
    bool have_right = false;
    for (auto ha : hearing_aids) {
        if (ha->is_connected() && ha->rop) {
            if (ha->rop.mode() == Mode::Mono) {
                have_left = true;
                have_right = true;
                break;
            } else if (ha->rop.side() == Side::Left) {
                have_left = true;
            } else if (ha->rop.side() == Side::Right) {
                have_right = true;
            }
        }
    }
    return have_left && have_right;
}

void HearingAid::set_other_side_ptrs()
{
    if (hearing_aids[0]->is_connected() && hearing_aids[1]->is_connected()) {
        hearing_aids[0]->other = hearing_aids[1];
        hearing_aids[1]->other = hearing_aids[0];
    } else {
        hearing_aids[0]->other = nullptr;
        hearing_aids[1]->other = nullptr;
    }
}

comm::BLEConnectionState HearingAid::get_ble_connection_state()
{
    return ble_connection_state;
}

void HearingAid::set_ble_connection_state(comm::BLEConnectionState state,
                                           bool force_event)
{
    if (!force_event && ble_connection_state == state) return;
    ble_connection_state = state;
    comm::EventPacket event(comm::EventType::BLEConnectionState);
    event.data.ble_connection_state = static_cast<uint8_t>(state);
    comm::add_event_to_buffer(comm::unset_conn_id, event);
}

void HearingAid::schedule_reconnect(HearingAid* ha, uint8_t status,
                                    uint8_t reason)
{
    if (!desired_connection || manual_shutdown || !connections_allowed) return;

    if (reconnect_state == ReconnectState::Idle ||
        reconnect_state == ReconnectState::Failed) {
        reconnect_attempt_count = 0U;
    }
    if (ha != nullptr) {
        reconnect_handle = ha->conn_handle;
        reconnect_cid = ha->cid;
        reconnect_read_index = ha->curr_read_index;
        bd_addr_copy(reconnect_address.data(), ha->addr);
    }
    reconnect_state = ReconnectState::Scheduled;
    reconnect_next_action_us = time_us_64() +
        (reconnect_attempt_count == 0U
             ? reconnect_initial_delay_us
             : reconnect_retry_backoff_us * reconnect_attempt_count);
    reconnect_deadline_us = 0U;
    set_ble_connection_state(comm::BLEConnectionState::Recovering);
#ifdef PICO_ASHA_AUDIO_STALL_TRACE
    audio_stall_trace_reconnect_transition(
        AUDIO_STALL_TRACE_RECONNECT_SCHEDULED, reconnect_handle,
        reconnect_cid, reconnect_address.data(),
        asha_audio_get_write_index(), reconnect_read_index,
        reconnect_attempt_count, status, reason, status);
#endif
}

void HearingAid::clear_reconnect_reboot_guard()
{
    if (watchdog_hw->scratch[4] == reconnect_reboot_guard_magic) {
        watchdog_hw->scratch[4] = 0U;
        watchdog_hw->scratch[5] = 0U;
    }
}

void HearingAid::process_reconnect()
{
    if (!desired_connection || manual_shutdown || !connections_allowed) {
        reconnect_state = ReconnectState::Idle;
        return;
    }

    uint64_t now_us = time_us_64();
    if (reconnect_state == ReconnectState::Scheduled) {
        if (now_us < reconnect_next_action_us) return;
        if (reconnect_attempt_count < reconnect_max_attempts) {
            ++reconnect_attempt_count;
            (void)gap_connect_cancel();
            gap_stop_scan();
            start_scan();
            return;
        }
    } else if ((reconnect_state == ReconnectState::Scanning ||
                reconnect_state == ReconnectState::Connecting ||
                reconnect_state == ReconnectState::Initializing) &&
               now_us >= reconnect_deadline_us) {
#ifdef PICO_ASHA_AUDIO_STALL_TRACE
        audio_stall_trace_reconnect_transition(
            AUDIO_STALL_TRACE_RECONNECT_TIMEOUT, reconnect_handle,
            reconnect_cid, reconnect_address.data(),
            asha_audio_get_write_index(), reconnect_read_index,
            reconnect_attempt_count, ERROR_CODE_CONNECTION_TIMEOUT, 0U,
            ERROR_CODE_CONNECTION_TIMEOUT);
#endif
        if (reconnect_state == ReconnectState::Initializing) {
            HearingAid* ha = get_by_con_handle(reconnect_handle);
            if (ha != nullptr && ha->is_connected()) {
                ha->disconnect();
                return;
            }
        } else if (reconnect_state == ReconnectState::Connecting) {
            (void)gap_connect_cancel();
        } else {
            gap_stop_scan();
        }
        if (reconnect_attempt_count < reconnect_max_attempts) {
            reconnect_state = ReconnectState::Scheduled;
            reconnect_next_action_us = now_us +
                reconnect_retry_backoff_us * reconnect_attempt_count;
            reconnect_deadline_us = 0U;
            set_ble_connection_state(comm::BLEConnectionState::Recovering);
            return;
        }
    } else {
        return;
    }

    // A persisted guard in unused watchdog scratch words allows one final
    // reboot, but prevents a missing aid from causing an endless reboot loop.
    uint32_t reboot_count =
        watchdog_hw->scratch[4] == reconnect_reboot_guard_magic
            ? watchdog_hw->scratch[5]
            : 0U;
    reconnect_state = ReconnectState::Failed;
    set_ble_connection_state(comm::BLEConnectionState::Recovering);
    if (reboot_count == 0U && !reconnect_watchdog_requested) {
        reconnect_watchdog_requested = true;
        watchdog_hw->scratch[4] = reconnect_reboot_guard_magic;
        watchdog_hw->scratch[5] = 1U;
#ifdef PICO_ASHA_AUDIO_STALL_TRACE
        audio_stall_trace_watchdog_reset_requested(
            AUDIO_STALL_TRACE_WATCHDOG_RECONNECT_FALLBACK,
            reconnect_watchdog_delay_ms);
#endif
        watchdog_enable(reconnect_watchdog_delay_ms, true);
    }
}

void HearingAid::assign_next_conn_id()
{
    ++next_conn_id;
    conn_id = next_conn_id;
}

bool HearingAid::is_connected()
{
    return connected && conn_handle != HCI_CON_HANDLE_INVALID;
}

bool HearingAid::is_streaming()
{
    return  audio_state == AudioState::Streaming || 
            audio_state == (AudioState::Streaming | AudioState::AudioBusy);
}

void HearingAid::set_process_busy()
{
    process_state |= ProcessState::ProcessBusy;
}

void HearingAid::unset_process_busy()
{
    process_state &= ~ProcessState::ProcessBusy;
}

void HearingAid::set_audio_busy(uint8_t sequence, int32_t context)
{
#ifdef PICO_ASHA_AUDIO_STALL_TRACE
    bool was_busy = (audio_state & AudioState::AudioBusy) != 0U;
#endif
    audio_state |= AudioState::AudioBusy;
#ifdef PICO_ASHA_AUDIO_STALL_TRACE
    if (!was_busy) {
        trace_pending_sequence = sequence;
        trace_audio_busy_since_us = audio_stall_trace_now_us();
        trace_busy_stall_reported = false;
        audio_stall_trace_busy(AUDIO_STALL_TRACE_BUSY_SET, conn_handle, cid,
                               sequence, asha_audio_get_write_index(), curr_read_index,
                               true, 0U, context);
    }
#else
    (void)sequence;
    (void)context;
#endif
}

void HearingAid::unset_audio_busy(int32_t context)
{
#ifdef PICO_ASHA_AUDIO_STALL_TRACE
    bool was_busy = (audio_state & AudioState::AudioBusy) != 0U;
#endif
    audio_state &= ~AudioState::AudioBusy;
#ifdef PICO_ASHA_AUDIO_STALL_TRACE
    if (was_busy) {
        uint64_t now_us = audio_stall_trace_now_us();
        uint32_t duration_us = now_us > trace_audio_busy_since_us
                                   ? static_cast<uint32_t>(std::min<uint64_t>(
                                         now_us - trace_audio_busy_since_us, UINT32_MAX))
                                   : 0U;
        audio_stall_trace_busy(AUDIO_STALL_TRACE_BUSY_CLEAR, conn_handle, cid,
                               trace_pending_sequence, asha_audio_get_write_index(),
                               curr_read_index, false, duration_us, context);
        trace_audio_busy_since_us = 0U;
        trace_busy_stall_reported = false;
    }
#else
    (void)context;
#endif
}

bool HearingAid::request_audio_can_send(uint32_t write_index)
{
    if (audio_tx_state != AudioTxState::Idle || audio_data == nullptr) {
        return false;
    }

    uint64_t now_us = time_us_64();
    ++audio_tx_generation;
    if (audio_tx_generation == 0U) ++audio_tx_generation;
    audio_tx_pending_generation = audio_tx_generation;
    audio_tx_sequence = audio_data[0];
    audio_tx_state = AudioTxState::WaitingCanSendNow;
    audio_tx_state_since_us = now_us;
    audio_tx_local_recovery = false;
    set_audio_busy(audio_tx_sequence, AUDIO_STALL_TRACE_BUSY_CONTEXT_AUDIO_SDU);

#ifdef PICO_ASHA_AUDIO_STALL_TRACE
    trace_can_send_age_reported = false;
    trace_packet_sent_age_reported = false;
    trace_pending_sequence = audio_tx_sequence;
    trace_can_send_request_us = now_us;
    trace_can_send_now_us = 0U;
    uint32_t previous_send_age_us =
        last_successful_audio_send_us == 0U || now_us < last_successful_audio_send_us
            ? UINT32_MAX
            : static_cast<uint32_t>(std::min<uint64_t>(
                  now_us - last_successful_audio_send_us, UINT32_MAX));
    // Record the request before the BTstack call because CAN_SEND_NOW may be
    // delivered synchronously from l2cap_request_can_send_now_event().
    audio_stall_trace_can_send_requested(conn_handle, cid, audio_tx_sequence,
                                         write_index, curr_read_index, true,
                                         audio_tx_ring_index,
                                         previous_send_age_us);
#endif

    uint8_t result = l2cap_request_can_send_now_event(cid);
    if (result != ERROR_CODE_SUCCESS) {
        reconnect_after_audio_tx_stall(write_index, 0U, result);
        return false;
    }
    return true;
}

bool HearingAid::send_pending_audio(bool local_recovery, uint32_t write_index)
{
    if (audio_data == nullptr) {
        reconnect_after_audio_tx_stall(write_index, 0U, ERROR_CODE_COMMAND_DISALLOWED);
        return false;
    }

    uint64_t now_us = time_us_64();
    uint8_t sequence = audio_tx_sequence;

#ifdef PICO_ASHA_AUDIO_STALL_TRACE
    AudioTxState previous_state = audio_tx_state;
    uint32_t previous_state_duration_us =
        now_us > audio_tx_state_since_us
            ? static_cast<uint32_t>(std::min<uint64_t>(
                  now_us - audio_tx_state_since_us, UINT32_MAX))
            : 0U;
    if (local_recovery) {
        audio_stall_trace_send_state_changed(
            conn_handle, cid, sequence, write_index, curr_read_index,
            (audio_state & AudioState::AudioBusy) != 0U,
            static_cast<uint8_t>(previous_state),
            static_cast<uint8_t>(AudioTxState::WaitingPacketSent),
            AUDIO_STALL_TRACE_STATE_LOCAL_RECOVERY,
            previous_state_duration_us);
    }
#endif

    // The phase transition must precede l2cap_send(): BTstack is allowed to
    // make progress synchronously and emit PACKET_SENT from inside the call.
    audio_tx_state = AudioTxState::WaitingPacketSent;
    audio_tx_state_since_us = now_us;
    audio_tx_local_recovery = local_recovery;
    if ((audio_state & AudioState::AudioBusy) == 0U) {
        set_audio_busy(sequence, AUDIO_STALL_TRACE_BUSY_CONTEXT_AUDIO_SDU);
    }

#ifdef PICO_ASHA_AUDIO_STALL_TRACE
    trace_packet_sent_age_reported = false;
    trace_l2cap_send_us = now_us;
    uint32_t can_send_now_to_send_us =
        local_recovery || trace_can_send_now_us == 0U || now_us < trace_can_send_now_us
            ? 0U
            : static_cast<uint32_t>(std::min<uint64_t>(
                  now_us - trace_can_send_now_us, UINT32_MAX));
    audio_stall_trace_l2cap_send_begin(
        conn_handle, cid, sequence, write_index, curr_read_index,
        (audio_state & AudioState::AudioBusy) != 0U,
        can_send_now_to_send_us, local_recovery);
    if (trace_audio_tx_published_write_index != 0U) {
        uint32_t send_checksum = asha_audio_trace_checksum(
            audio_data, ASHA_SDU_SIZE_BYTES);
        if (send_checksum != trace_audio_tx_checksum) {
            audio_stall_trace_g722_integrity_error(
                conn_handle, cid, sequence, write_index,
                audio_tx_ring_index,
                (audio_state & AudioState::AudioBusy) != 0U,
                AUDIO_STALL_TRACE_G722_TX_BUFFER_CHANGED,
                trace_audio_tx_checksum, send_checksum,
                trace_audio_tx_published_write_index);
        }
    }
#endif
    uint8_t result = l2cap_send(cid, audio_data, ASHA_SDU_SIZE_BYTES);
    uint64_t complete_us = time_us_64();
#ifdef PICO_ASHA_AUDIO_STALL_TRACE
    uint32_t call_duration_us = complete_us > now_us
                                    ? static_cast<uint32_t>(std::min<uint64_t>(
                                          complete_us - now_us, UINT32_MAX))
                                    : 0U;
    audio_stall_trace_l2cap_send(conn_handle, cid, sequence, write_index,
                                 curr_read_index,
                                 (audio_state & AudioState::AudioBusy) != 0U,
                                 ASHA_SDU_SIZE_BYTES, call_duration_us, result);
#endif
    if (result != ERROR_CODE_SUCCESS) {
        reconnect_after_audio_tx_stall(write_index, 0U, result);
        return false;
    }
    last_successful_audio_send_us = complete_us;
    ++successful_audio_send_count;
    audio_sdu_count_at_last_success = write_index;
    audio_no_progress_state = AudioNoProgressState::Idle;
    audio_no_progress_started_us = 0U;
    audio_no_progress_deadline_us = 0U;
#ifdef PICO_ASHA_AUDIO_STALL_TRACE
    trace_last_sequence = sequence;
#endif
    return true;
}

void HearingAid::reset_audio_progress_watchdog(uint64_t now_us, bool armed)
{
    audio_sdu_generated_count = asha_audio_get_write_index();
    audio_sdu_count_at_last_success = audio_sdu_generated_count;
    audio_last_sdu_generated_us = 0U;
    audio_progress_baseline_us = now_us;
    audio_progress_grace_deadline_us =
        armed ? now_us + audio_no_progress_startup_grace_us : 0U;
    audio_progress_watchdog_armed = armed;
    audio_progress_success_baseline = successful_audio_send_count;
    audio_no_progress_success_count = successful_audio_send_count;
    audio_no_progress_state = AudioNoProgressState::Idle;
    audio_no_progress_started_us = 0U;
    audio_no_progress_deadline_us = 0U;
}

void HearingAid::clear_local_audio_tx_state()
{
    audio_tx_state = AudioTxState::Idle;
    audio_tx_state_since_us = 0U;
    audio_tx_sdu_since_us = 0U;
    audio_tx_ring_index = 0U;
    audio_tx_sequence = AUDIO_STALL_TRACE_INVALID_SEQUENCE;
    audio_tx_pending_generation = 0U;
    audio_tx_local_recovery = false;
    audio_data = nullptr;
#ifdef PICO_ASHA_AUDIO_STALL_TRACE
    trace_can_send_age_reported = false;
    trace_packet_sent_age_reported = false;
    trace_audio_tx_checksum = 0U;
    trace_audio_tx_published_write_index = 0U;
#endif
    if ((audio_state & AudioState::AudioBusy) != 0U) {
        unset_audio_busy(AUDIO_STALL_TRACE_BUSY_CONTEXT_RESET);
    }
}

bool HearingAid::schedule_audio_if_idle(uint32_t write_index, uint64_t now_us,
                                        bool recovery)
{
    if (audio_tx_state != AudioTxState::Idle ||
        (audio_state & AudioState::AudioBusy) != 0U ||
        write_index == 0U ||
        (!recovery && write_index == curr_read_index)) {
        return false;
    }

    enum AshaAudioSide audio_side = rop.side() == Side::Left
                                        ? AshaAudioSide::AudioLeft
                                        : AshaAudioSide::AudioRight;
    uint32_t original_read_index = curr_read_index;
    uint32_t selected_index = original_read_index;
    uint32_t queued_frames = write_index - curr_read_index;
    // A single missed 20-ms SDU is still fresh and is important for the
    // continuous G.722 predictor state. Send short backlogs in order and catch
    // up on subsequent run-loop passes. Recovery and genuinely old backlogs
    // still collapse to the newest complete SDU.
    if (recovery || queued_frames > audio_short_backlog_max_frames) {
        selected_index = write_index - 1U;
    }
    uint8_t* selected = asha_audio_get_encoded_at_index(audio_side,
                                                        selected_index);

#ifdef PICO_ASHA_AUDIO_STALL_TRACE
    int32_t dropped_delta = static_cast<int32_t>(selected_index - original_read_index);
    uint32_t dropped_frames = dropped_delta > 0
                                  ? static_cast<uint32_t>(dropped_delta)
                                  : 0U;
    if (dropped_frames > 0U) {
        uint8_t* old = asha_audio_get_encoded_at_index(audio_side,
                                                       original_read_index);
        uint32_t last_request_age_us =
            trace_can_send_request_us == 0U || now_us < trace_can_send_request_us
                ? UINT32_MAX
                : static_cast<uint32_t>(std::min<uint64_t>(
                      now_us - trace_can_send_request_us, UINT32_MAX));
        uint32_t last_send_age_us =
            last_successful_audio_send_us == 0U ||
                    now_us < last_successful_audio_send_us
                ? UINT32_MAX
                : static_cast<uint32_t>(std::min<uint64_t>(
                      now_us - last_successful_audio_send_us, UINT32_MAX));
        // Capture the old indices and sequence before advancing the consumer.
        audio_stall_trace_stale_frames_dropped(
            conn_handle, cid, old[0], selected[0], write_index,
            original_read_index, false, 0U, dropped_frames, false,
            last_request_age_us, last_send_age_us,
            asha_audio_get_pcm_streaming_enabled(),
            static_cast<uint8_t>(num_connected()), credits);
    }
#endif

#ifdef PICO_ASHA_AUDIO_STALL_TRACE
    uint32_t published_before_copy = 0U;
    uint32_t checksum_before_copy = 0U;
    asha_audio_get_encoded_trace(audio_side, selected_index,
                                 &published_before_copy,
                                 &checksum_before_copy);
#endif
    memcpy(audio_tx_buffer.data(), selected, audio_tx_buffer.size());
#ifdef PICO_ASHA_AUDIO_STALL_TRACE
    uint32_t published_after_copy = 0U;
    uint32_t checksum_after_copy = 0U;
    asha_audio_get_encoded_trace(audio_side, selected_index,
                                 &published_after_copy,
                                 &checksum_after_copy);
    uint32_t copied_checksum = asha_audio_trace_checksum(
        audio_tx_buffer.data(), audio_tx_buffer.size());
    uint32_t expected_write_index = selected_index + 1U;
    bool generation_changed =
        published_before_copy != expected_write_index ||
        published_after_copy != expected_write_index ||
        published_before_copy != published_after_copy ||
        checksum_before_copy != checksum_after_copy;
    if (generation_changed) {
        audio_stall_trace_g722_integrity_error(
            conn_handle, cid, audio_tx_buffer[0], write_index,
            selected_index, false,
            AUDIO_STALL_TRACE_G722_RING_GENERATION,
            expected_write_index, published_after_copy,
            published_after_copy);
    } else if (copied_checksum != checksum_after_copy) {
        audio_stall_trace_g722_integrity_error(
            conn_handle, cid, audio_tx_buffer[0], write_index,
            selected_index, false,
            AUDIO_STALL_TRACE_G722_RING_CHECKSUM,
            checksum_after_copy, copied_checksum,
            published_after_copy);
    }
    trace_audio_tx_checksum = copied_checksum;
    trace_audio_tx_published_write_index = published_after_copy;
#endif
    audio_data = audio_tx_buffer.data();
    audio_tx_ring_index = selected_index;
    audio_tx_sdu_since_us = now_us;
    curr_read_index = selected_index + 1U;
    return request_audio_can_send(write_index);
}

#ifdef PICO_ASHA_AUDIO_STALL_TRACE
uint32_t HearingAid::trace_audio_tx_blockers(uint32_t write_index,
                                             bool pcm_is_streaming,
                                             uint64_t now_us) const
{
    uint32_t blockers = 0U;
    bool connected_now = connected && conn_handle != HCI_CON_HANDLE_INVALID;
    bool streaming_now = audio_state == AudioState::Streaming ||
                         audio_state ==
                             (AudioState::Streaming | AudioState::AudioBusy);
    if (!desired_connection || manual_shutdown || !connections_allowed) {
        blockers |= AUDIO_STALL_TRACE_BLOCK_CONNECTIONS_DISABLED;
    }
    if (!connected_now) blockers |= AUDIO_STALL_TRACE_BLOCK_NOT_CONNECTED;
    if (process_state != ProcessState::Audio) {
        blockers |= AUDIO_STALL_TRACE_BLOCK_PROCESS_NOT_AUDIO;
    }
    if (!streaming_now) blockers |= AUDIO_STALL_TRACE_BLOCK_NOT_STREAMING;
    if (!audio_streaming_enabled) {
        blockers |= AUDIO_STALL_TRACE_BLOCK_AUDIO_DISABLED;
    }
    if (!pcm_is_streaming) {
        blockers |= AUDIO_STALL_TRACE_BLOCK_PCM_NOT_STREAMING;
    }
    if (!l2cap_channel_open || cid == 0U) {
        blockers |= AUDIO_STALL_TRACE_BLOCK_L2CAP_NOT_READY;
    }
    if (write_index == 0U || write_index == curr_read_index) {
        blockers |= AUDIO_STALL_TRACE_BLOCK_NO_SDU_AVAILABLE;
    } else {
        uint32_t generated_us = asha_audio_get_last_sdu_generated_time_us();
        uint32_t newest_sdu_age_us = generated_us == 0U
                                         ? UINT32_MAX
                                         : (uint32_t)now_us - generated_us;
        if (newest_sdu_age_us > audio_sdu_activity_timeout_us) {
            blockers |= AUDIO_STALL_TRACE_BLOCK_SDU_NOT_FRESH;
        }
    }
    if (credits == 0U) blockers |= AUDIO_STALL_TRACE_BLOCK_NO_CREDITS;
    if (audio_tx_state == AudioTxState::WaitingCanSendNow) {
        blockers |= AUDIO_STALL_TRACE_BLOCK_WAIT_CAN_SEND_NOW |
                    AUDIO_STALL_TRACE_BLOCK_CAN_SEND_PENDING;
    } else if (audio_tx_state == AudioTxState::WaitingPacketSent) {
        blockers |= AUDIO_STALL_TRACE_BLOCK_WAIT_PACKET_SENT;
    }
    if (audio_tx_pending_generation != 0U &&
        audio_tx_state != AudioTxState::WaitingPacketSent) {
        blockers |= AUDIO_STALL_TRACE_BLOCK_CAN_SEND_PENDING;
    }
    if ((audio_state & AudioState::AudioBusy) != 0U) {
        blockers |= AUDIO_STALL_TRACE_BLOCK_AUDIO_BUSY;
    }
    if (audio_data != nullptr && audio_tx_state == AudioTxState::Idle) {
        blockers |= AUDIO_STALL_TRACE_BLOCK_TX_BUFFER_OWNED;
    }
    return blockers;
}

void HearingAid::trace_audio_tx_blocked_if_needed(uint32_t write_index,
                                                   bool pcm_is_streaming,
                                                   uint64_t now_us,
                                                   bool verify_invariant)
{
    uint32_t generated_us = asha_audio_get_last_sdu_generated_time_us();
    uint32_t newest_sdu_age_us = generated_us == 0U
                                     ? UINT32_MAX
                                     : (uint32_t)now_us - generated_us;
    bool fresh_sdu_available = write_index != 0U &&
                               write_index != curr_read_index &&
                               newest_sdu_age_us <= audio_sdu_activity_timeout_us;
    if (!audio_progress_watchdog_armed || !fresh_sdu_available ||
        now_us < audio_progress_grace_deadline_us) {
        trace_last_tx_blocker_mask = 0U;
        trace_last_tx_blocked_report_us = 0U;
        return;
    }

    uint64_t progress_us = last_successful_audio_send_us != 0U
                               ? last_successful_audio_send_us
                               : audio_progress_baseline_us;
    uint32_t last_send_age_us = now_us < progress_us
                                    ? 0U
                                    : static_cast<uint32_t>(
                                          std::min<uint64_t>(
                                              now_us - progress_us,
                                              UINT32_MAX));
    if (last_send_age_us < AUDIO_STALL_TRACE_TX_BLOCKED_US) {
        trace_last_tx_blocker_mask = 0U;
        trace_last_tx_blocked_report_us = 0U;
        return;
    }

    // Pending phases already have their own single-shot age records. TX_BLOCKED
    // is reserved for the case where process_audio() is running but no request
    // owns the TX path.
    if (audio_tx_state != AudioTxState::Idle) return;

    uint32_t blockers = trace_audio_tx_blockers(write_index,
                                                pcm_is_streaming, now_us);
    if (verify_invariant && blockers == 0U) {
        blockers = AUDIO_STALL_TRACE_BLOCK_INVARIANT_NOT_ARMED;
    }
    if (blockers == 0U) return;
    if (blockers == trace_last_tx_blocker_mask &&
        trace_last_tx_blocked_report_us != 0U &&
        now_us - trace_last_tx_blocked_report_us <
            AUDIO_STALL_TRACE_HEARTBEAT_INTERVAL_US) {
        return;
    }

    trace_last_tx_blocker_mask = blockers;
    trace_last_tx_blocked_report_us = now_us;
    enum AshaAudioSide audio_side = rop.side() == Side::Left
                                        ? AshaAudioSide::AudioLeft
                                        : AshaAudioSide::AudioRight;
    uint8_t newest_sequence = asha_audio_get_encoded_at_index(
                                  audio_side, write_index - 1U)[0];
    audio_stall_trace_tx_blocked(
        conn_handle, cid, newest_sequence, write_index, curr_read_index,
        (audio_state & AudioState::AudioBusy) != 0U, last_send_age_us,
        blockers, newest_sdu_age_us, static_cast<uint8_t>(audio_tx_state),
        credits, audio_tx_state == AudioTxState::WaitingCanSendNow);
}
#endif

bool HearingAid::process_audio_no_progress(uint32_t write_index,
                                           bool pcm_is_streaming,
                                           uint64_t now_us)
{
    // Observe the producer's atomic write index on core 1. This is independent
    // of consumer read position, reported ring fill, and the current TX phase.
    if (write_index != audio_sdu_generated_count) {
        audio_sdu_generated_count = write_index;
        audio_last_sdu_generated_us =
            asha_audio_get_last_sdu_generated_time_us();
    }

    bool stream_ready = desired_connection && !manual_shutdown &&
                        connections_allowed && is_connected() &&
                        process_state == ProcessState::Audio &&
                        is_streaming() && audio_streaming_enabled &&
                        pcm_is_streaming && l2cap_channel_open && cid != 0U;
    if (!stream_ready) {
        if (audio_progress_watchdog_armed) {
            reset_audio_progress_watchdog(now_us, false);
        }
        return false;
    }
    if (!audio_progress_watchdog_armed) {
        reset_audio_progress_watchdog(now_us, true);
        return false;
    }

    uint32_t newest_sdu_age_us =
        audio_last_sdu_generated_us == 0U
            ? UINT32_MAX
            : static_cast<uint32_t>(now_us) - audio_last_sdu_generated_us;
    bool current_sdu_generation =
        newest_sdu_age_us <= audio_sdu_activity_timeout_us;

    if (audio_no_progress_state ==
        AudioNoProgressState::WaitingForRearmProgress) {
        if (successful_audio_send_count != audio_no_progress_success_count) {
            audio_no_progress_state = AudioNoProgressState::Idle;
            audio_no_progress_started_us = 0U;
            audio_no_progress_deadline_us = 0U;
            return false;
        }
        if (!current_sdu_generation) {
            audio_no_progress_state = AudioNoProgressState::Idle;
            audio_no_progress_started_us = 0U;
            audio_no_progress_deadline_us = 0U;
            return false;
        }
        if (now_us >= audio_no_progress_deadline_us) {
            uint32_t last_send_age_us =
                last_successful_audio_send_us == 0U ||
                        now_us < last_successful_audio_send_us
                    ? UINT32_MAX
                    : static_cast<uint32_t>(std::min<uint64_t>(
                          now_us - last_successful_audio_send_us, UINT32_MAX));
#ifdef PICO_ASHA_AUDIO_STALL_TRACE
            audio_stall_trace_audio_no_progress(
                AUDIO_STALL_TRACE_AUDIO_NO_PROGRESS_RECONNECT,
                conn_handle, cid, audio_tx_sequence, write_index,
                curr_read_index,
                (audio_state & AudioState::AudioBusy) != 0U,
                last_send_age_us, newest_sdu_age_us,
                static_cast<uint8_t>(audio_tx_state), credits,
                ERROR_CODE_CONNECTION_TIMEOUT);
#endif
            audio_no_progress_state = AudioNoProgressState::Idle;
            reconnect_after_audio_tx_stall(write_index, last_send_age_us,
                                           ERROR_CODE_CONNECTION_TIMEOUT);
            return true;
        }
        return false;
    }

    if (now_us < audio_progress_grace_deadline_us ||
        !current_sdu_generation ||
        audio_sdu_generated_count == audio_sdu_count_at_last_success) {
        return false;
    }
    uint64_t send_progress_us =
        successful_audio_send_count != audio_progress_success_baseline &&
                last_successful_audio_send_us != 0U
                                    ? last_successful_audio_send_us
                                    : audio_progress_baseline_us;
    uint32_t last_send_age_us =
        now_us < send_progress_us
            ? 0U
            : static_cast<uint32_t>(std::min<uint64_t>(
                  now_us - send_progress_us, UINT32_MAX));
    if (last_send_age_us < audio_no_progress_timeout_us) {
        return false;
    }

    enum AshaAudioSide audio_side = rop.side() == Side::Left
                                        ? AshaAudioSide::AudioLeft
                                        : AshaAudioSide::AudioRight;
    uint8_t newest_sequence = asha_audio_get_encoded_at_index(
                                  audio_side, write_index - 1U)[0];
#ifdef PICO_ASHA_AUDIO_STALL_TRACE
    audio_stall_trace_audio_no_progress(
        AUDIO_STALL_TRACE_AUDIO_NO_PROGRESS, conn_handle, cid,
        newest_sequence, write_index, curr_read_index,
        (audio_state & AudioState::AudioBusy) != 0U,
        last_send_age_us, newest_sdu_age_us,
        static_cast<uint8_t>(audio_tx_state), credits, ERROR_CODE_SUCCESS);
#endif

    audio_no_progress_state =
        AudioNoProgressState::WaitingForRearmProgress;
    audio_no_progress_success_count = successful_audio_send_count;
    audio_no_progress_started_us = now_us;
    audio_no_progress_deadline_us =
        now_us + audio_no_progress_rearm_deadline_us;

    // Only IDLE can relinquish and replace the private TX buffer. If BTstack
    // already owns a CAN_SEND/PACKET_SENT phase, preserve it and let the
    // existing phase watchdog make progress; issuing a second request here
    // would overwrite an immutable buffer or create a duplicate callback.
    bool requested = false;
    if (audio_tx_state == AudioTxState::Idle) {
        clear_local_audio_tx_state();
        requested = schedule_audio_if_idle(write_index, now_us, true);
    }
#ifdef PICO_ASHA_AUDIO_STALL_TRACE
    audio_stall_trace_audio_no_progress(
        AUDIO_STALL_TRACE_AUDIO_NO_PROGRESS_REARMED, conn_handle, cid,
        newest_sequence, write_index, curr_read_index,
        (audio_state & AudioState::AudioBusy) != 0U,
        last_send_age_us, newest_sdu_age_us,
        static_cast<uint8_t>(audio_tx_state), credits,
        requested ? ERROR_CODE_SUCCESS : ERROR_CODE_COMMAND_DISALLOWED);
#endif
    // When a phase is already pending, return to the caller so the unchanged
    // WAIT_CAN_SEND / WAIT_PACKET_SENT watchdog can run in this same pass.
    return requested;
}

bool HearingAid::process_audio_tx_watchdog(uint32_t write_index, bool audio_active)
{
    if (audio_tx_state == AudioTxState::Idle) {
        return false;
    }

    uint64_t now_us = time_us_64();
    uint32_t stalled_us = now_us > audio_tx_state_since_us
                              ? static_cast<uint32_t>(std::min<uint64_t>(
                                    now_us - audio_tx_state_since_us, UINT32_MAX))
                              : 0U;
#ifdef PICO_ASHA_AUDIO_STALL_TRACE
    bool busy = (audio_state & AudioState::AudioBusy) != 0U;
    if (audio_tx_state == AudioTxState::WaitingCanSendNow &&
        !trace_can_send_age_reported &&
        stalled_us >= AUDIO_STALL_TRACE_CAN_SEND_AGE_US) {
        trace_can_send_age_reported = true;
        audio_stall_trace_tx_wait_age(
            AUDIO_STALL_TRACE_CAN_SEND_REQUEST_AGE,
            conn_handle, cid, audio_tx_sequence, write_index,
            curr_read_index, busy, stalled_us,
            static_cast<uint8_t>(audio_tx_state), credits);
    } else if (audio_tx_state == AudioTxState::WaitingPacketSent &&
               !trace_packet_sent_age_reported &&
               stalled_us >= AUDIO_STALL_TRACE_PACKET_SENT_AGE_US) {
        trace_packet_sent_age_reported = true;
        audio_stall_trace_tx_wait_age(
            AUDIO_STALL_TRACE_PACKET_SENT_WAIT_AGE,
            conn_handle, cid, audio_tx_sequence, write_index,
            curr_read_index, busy, stalled_us,
            static_cast<uint8_t>(audio_tx_state), credits);
    }
#endif

    if (audio_data == nullptr) {
        reconnect_after_audio_tx_stall(write_index, stalled_us,
                                       ERROR_CODE_COMMAND_DISALLOWED);
        return true;
    }

    // A pending phase owns the TX path even when AudioBusy was incorrectly
    // cleared. This is the invariant that prevents duplicate pending requests.
    if (!audio_active) {
        return true;
    }

    if (audio_tx_state == AudioTxState::WaitingCanSendNow) {
        if (stalled_us < audio_can_send_watchdog_us) {
            return true;
        }
#ifdef PICO_ASHA_AUDIO_STALL_TRACE
        audio_stall_trace_tx_watchdog(AUDIO_STALL_TRACE_TX_WATCHDOG,
                                      conn_handle, cid, audio_tx_sequence,
                                      write_index, curr_read_index, busy,
                                      stalled_us, 0);
#endif

        // Do not call l2cap_request_can_send_now_event() again: BTstack still
        // owns the original request. If the channel can accept an SDU, queue the
        // selected frame directly and ignore the eventual stale callback.
        if (l2cap_can_send_packet_now(cid)) {
            uint32_t audio_age_us = now_us > audio_tx_sdu_since_us
                                        ? static_cast<uint32_t>(std::min<uint64_t>(
                                              now_us - audio_tx_sdu_since_us,
                                              UINT32_MAX))
                                        : 0U;
            if (audio_age_us > audio_tx_max_audio_age_us) {
                // No SDU has been handed to BTstack in this phase, so replacing
                // the private TX copy is safe. Skip the old pending frame and
                // backlog; never replay stale audio after recovery.
                if (write_index <= audio_tx_ring_index + 1U) {
                    reconnect_after_audio_tx_stall(write_index, stalled_us,
                                                   ERROR_CODE_CONNECTION_TIMEOUT);
                    return true;
                }
                uint32_t latest_index = write_index - 1U;
                enum AshaAudioSide audio_side = rop.side() == Side::Left
                                                    ? AshaAudioSide::AudioLeft
                                                    : AshaAudioSide::AudioRight;
                uint8_t* latest = asha_audio_get_encoded_at_index(audio_side,
                                                                  latest_index);
#ifdef PICO_ASHA_AUDIO_STALL_TRACE
                uint32_t dropped_frames = latest_index - audio_tx_ring_index;
                uint8_t old_sequence = audio_tx_sequence;
                uint8_t new_sequence = latest[0];
                uint32_t busy_duration_us =
                    busy && now_us > trace_audio_busy_since_us
                        ? static_cast<uint32_t>(std::min<uint64_t>(
                              now_us - trace_audio_busy_since_us, UINT32_MAX))
                        : 0U;
                uint32_t last_request_age_us =
                    trace_can_send_request_us == 0U || now_us < trace_can_send_request_us
                        ? UINT32_MAX
                        : static_cast<uint32_t>(std::min<uint64_t>(
                              now_us - trace_can_send_request_us, UINT32_MAX));
                uint32_t last_successful_send_age_us =
                    last_successful_audio_send_us == 0U ||
                            now_us < last_successful_audio_send_us
                        ? UINT32_MAX
                        : static_cast<uint32_t>(std::min<uint64_t>(
                              now_us - last_successful_audio_send_us,
                              UINT32_MAX));
                uint16_t available_credits = l2cap_cbm_available_credits(cid);
                audio_stall_trace_stale_frames_dropped(
                    conn_handle, cid, old_sequence, new_sequence,
                    write_index, curr_read_index, busy, busy_duration_us,
                    dropped_frames,
                    audio_tx_state == AudioTxState::WaitingCanSendNow,
                    last_request_age_us, last_successful_send_age_us,
                    asha_audio_get_pcm_streaming_enabled(),
                    static_cast<uint8_t>(num_connected()), available_credits);
#endif
                memcpy(audio_tx_buffer.data(), latest, audio_tx_buffer.size());
                audio_data = audio_tx_buffer.data();
                audio_tx_ring_index = latest_index;
                audio_tx_sdu_since_us = now_us;
                audio_tx_sequence = audio_data[0];
                curr_read_index = write_index;
#ifdef PICO_ASHA_AUDIO_STALL_TRACE
                trace_pending_sequence = audio_tx_sequence;
#endif
            }
#ifdef PICO_ASHA_AUDIO_STALL_TRACE
            audio_stall_trace_tx_watchdog(AUDIO_STALL_TRACE_TX_RECOVERY,
                                          conn_handle, cid, audio_tx_sequence,
                                          write_index, curr_read_index, busy,
                                          stalled_us, ERROR_CODE_SUCCESS);
#endif
            if (audio_tx_stale_can_send_callbacks != UINT8_MAX) {
                ++audio_tx_stale_can_send_callbacks;
            }
            send_pending_audio(true, write_index);
        } else {
            reconnect_after_audio_tx_stall(write_index, stalled_us,
                                           BTSTACK_ACL_BUFFERS_FULL);
        }
        return true;
    }

    if (stalled_us >= audio_packet_sent_watchdog_us) {
#ifdef PICO_ASHA_AUDIO_STALL_TRACE
        audio_stall_trace_tx_watchdog(AUDIO_STALL_TRACE_TX_WATCHDOG,
                                      conn_handle, cid, audio_tx_sequence,
                                      write_index, curr_read_index, busy,
                                      stalled_us, ERROR_CODE_CONNECTION_TIMEOUT);
#endif
        reconnect_after_audio_tx_stall(write_index, stalled_us,
                                       ERROR_CODE_CONNECTION_TIMEOUT);
    }
    return true;
}

void HearingAid::reconnect_after_audio_tx_stall(uint32_t write_index,
                                                uint32_t stalled_us,
                                                int32_t result)
{
#ifdef PICO_ASHA_AUDIO_STALL_TRACE
    audio_stall_trace_tx_watchdog(AUDIO_STALL_TRACE_TX_RECONNECT,
                                  conn_handle, cid, audio_tx_sequence,
                                  write_index, curr_read_index,
                                  (audio_state & AudioState::AudioBusy) != 0U,
                                  stalled_us, result);
#else
    (void)write_index;
    (void)stalled_us;
    (void)result;
#endif

    // Disconnection-complete resets ASHA/L2CAP state and schedules scanning in
    // the regular run loop, preserving pairing while creating a fresh CID.
    disconnect();
}

void HearingAid::set_data_langth()
{
    hci_send_cmd(&hci_le_set_data_length, conn_handle, pdu_len, max_tx_time);
}

void HearingAid::send_acp_start()
{
    using namespace comm;

#ifdef PICO_ASHA_AUDIO_STALL_TRACE
    // The existing assignment to Start clears the transient AudioBusy bit set
    // by the Ready path; make that otherwise implicit transition observable.
    unset_audio_busy(AUDIO_STALL_TRACE_BUSY_CONTEXT_ACP_START);
#endif
    audio_state = AudioState::Start;
    acp_cmd_packet[0] = ACPOpCode::start; // Opcode
    acp_cmd_packet[1] = 1u; // G.722 codec at 16KHz
    acp_cmd_packet[2] = 0u; // Unkown audio type
    acp_cmd_packet[3] = (uint8_t)curr_vol; // Volume
    acp_cmd_packet[4] = (other && other->is_streaming()) ? 1 : 0; // Otherstate

    uint8_t res = gatt_client_write_value_of_characteristic(&HearingAid::handle_acp_write, 
                                                            conn_handle, 
                                                            services.asha.acp.value_handle,
                                                            (uint16_t)acp_cmd_packet.size(),
                                                            acp_cmd_packet.data());
    if (res != ERROR_CODE_SUCCESS) {
        //LOG_ERROR("%s: ACP Write: Start error %s", get_side_str(), bt_err_str(res));
        add_event_to_buffer(conn_id, EventPacket(EventType::ACPStart, StatusType::BtstackStatus, res));
        audio_state = AudioState::Ready;
    }
}

void HearingAid::send_acp_stop()
{
    using namespace comm;

    reset_audio_progress_watchdog(time_us_64(), false);
    audio_state = AudioState::Stop;
    acp_cmd_packet[0] = ACPOpCode::stop;
    uint8_t res = gatt_client_write_value_of_characteristic(&HearingAid::handle_acp_write,
                                                            conn_handle,
                                                            services.asha.acp.value_handle,
                                                            1u,
                                                            acp_cmd_packet.data());
    if (res != ERROR_CODE_SUCCESS) {
        //LOG_ERROR("%s: ACP Write: Stop error %s", get_side_str(), bt_err_str(res));
        add_event_to_buffer(conn_id, EventPacket(EventType::ACPStop, StatusType::BtstackStatus, res));
        disconnect();
    }
}

void HearingAid::send_acp_status(uint8_t status)
{
    using namespace comm;

    acp_cmd_packet[0] = ACPOpCode::status;
    acp_cmd_packet[1] = status;
    auto res = gatt_client_write_value_of_characteristic_without_response(conn_handle,
                                                                          services.asha.acp.value_handle,
                                                                          2u,
                                                                          acp_cmd_packet.data());
    if (res != ERROR_CODE_SUCCESS) {
        //LOG_ERROR("%s: Updating status via ACP failed with %s", get_side_str(), bt_err_str(res));
        add_event_to_buffer(conn_id, EventPacket(EventType::ACPStatus, StatusType::BtstackStatus, res));
    }
}

void HearingAid::send_volume(int8_t volume)
{
    using namespace comm;

    uint8_t res = gatt_client_write_value_of_characteristic_without_response(conn_handle,
                                                                             services.asha.vol.value_handle,
                                                                             sizeof(volume),
                                                                             (uint8_t*)&volume);
    if (res != ERROR_CODE_SUCCESS) {
        //LOG_ERROR("%s: Sending volume failed with %s", get_side_str(), bt_err_str(res));
        add_event_to_buffer(conn_id, EventPacket(EventType::AudioVolume, StatusType::BtstackStatus, res));
    } else {
        //LOG_INFO("%s: Volume changed to %d", get_side_str(), (int)volume);
        EventPacket ev_pkt(EventType::AudioVolume);
        ev_pkt.data.volume = volume;
        add_event_to_buffer(conn_id, ev_pkt);
    }
}

void HearingAid::disconnect()
{
    //LOG_INFO("%s: Disconnect requested", get_side_str());
#ifdef PICO_ASHA_AUDIO_STALL_TRACE
    if (audio_tx_state != AudioTxState::Idle) {
        uint64_t now_us = audio_stall_trace_now_us();
        uint32_t state_duration_us =
            now_us > audio_tx_state_since_us
                ? static_cast<uint32_t>(std::min<uint64_t>(
                      now_us - audio_tx_state_since_us, UINT32_MAX))
                : 0U;
        audio_stall_trace_send_state_changed(
            conn_handle, cid, audio_tx_sequence,
            asha_audio_get_write_index(), curr_read_index,
            (audio_state & AudioState::AudioBusy) != 0U,
            static_cast<uint8_t>(audio_tx_state),
            static_cast<uint8_t>(AudioTxState::Idle),
            AUDIO_STALL_TRACE_STATE_DISCONNECT, state_duration_us);
    }
#endif
    audio_tx_state = AudioTxState::Idle;
    audio_tx_state_since_us = 0U;
    audio_tx_sdu_since_us = 0U;
    audio_tx_ring_index = 0U;
    audio_tx_sequence = AUDIO_STALL_TRACE_INVALID_SEQUENCE;
    audio_tx_local_recovery = false;
    audio_tx_pending_generation = 0U;
    audio_tx_stale_can_send_callbacks = 0U;
    reset_audio_progress_watchdog(time_us_64(), false);
    audio_data = nullptr;
    process_state = ProcessState::Disconnect;
    gap_disconnect(conn_handle);
}

void HearingAid::reset()
{
    conn_id = comm::unset_conn_id;
    conn_handle = HCI_CON_HANDLE_INVALID;
    cid = 0U;
    connected = false;
    process_state = ProcessState::ProcessUnset;
    audio_state = AudioState::AudioUnset;
    other = nullptr;
    psm = 0;
    credits = 0;
    l2cap_channel_open = false;
    paired_and_bonded = false;
    process_delay_ticks = 0;
    error_count = 0;
    service_index = 0;
    chars_index = cached_chars_index;
    audio_data = nullptr;
    audio_tx_state = AudioTxState::Idle;
    audio_tx_state_since_us = 0U;
    audio_tx_sdu_since_us = 0U;
    audio_tx_ring_index = 0U;
    audio_tx_sequence = AUDIO_STALL_TRACE_INVALID_SEQUENCE;
    audio_tx_local_recovery = false;
    audio_tx_pending_generation = 0U;
    audio_tx_stale_can_send_callbacks = 0U;
    audio_sdu_generated_count = asha_audio_get_write_index();
    audio_sdu_count_at_last_success = audio_sdu_generated_count;
    successful_audio_send_count = 0U;
    audio_progress_success_baseline = 0U;
    audio_no_progress_success_count = 0U;
    audio_last_sdu_generated_us = 0U;
    last_successful_audio_send_us = 0U;
    audio_progress_baseline_us = 0U;
    audio_progress_grace_deadline_us = 0U;
    audio_progress_watchdog_armed = false;
    audio_no_progress_state = AudioNoProgressState::Idle;
    audio_no_progress_started_us = 0U;
    audio_no_progress_deadline_us = 0U;
#ifdef PICO_ASHA_AUDIO_STALL_TRACE
    trace_can_send_request_us = 0U;
    trace_can_send_now_us = 0U;
    trace_l2cap_send_us = 0U;
    trace_audio_busy_since_us = 0U;
    trace_connection_interval = 0U;
    trace_peripheral_latency = 0U;
    trace_supervision_timeout = 0U;
    trace_pending_sequence = AUDIO_STALL_TRACE_INVALID_SEQUENCE;
    trace_last_sequence = AUDIO_STALL_TRACE_INVALID_SEQUENCE;
    trace_busy_stall_reported = false;
    trace_ring_underrun_reported = false;
    trace_zero_credits_active = false;
    trace_zero_credits_since_us = 0U;
#endif
    
    if (!cached) {
        memset(addr, 0U, sizeof(bd_addr_t));
        device_name.clear();
        manufacturer.clear();
        model.clear();
        fw_vers.clear();

        rop = {};
        side_str = "Unknown";
        services = {};
        chars_index = 0;
    }
}

const char* HearingAid::get_side_str()
{
    return (rop) ? bd_addr_to_str(addr) : side_str;
}

static void delete_paired_devices()
{
    using namespace comm;
    //LOG_INFO("Removing paired devices");
    EventPacket ev_pkt(EventType::DeletePair);

    int addr_type;
    bd_addr_t addr; 
    sm_key_t irk;
    int max_count = le_device_db_max_count();
    for (int i = 0; i < max_count; ++i) {
        le_device_db_info(i, &addr_type, addr, irk);
        if (addr_type != BD_ADDR_TYPE_UNKNOWN) {
            //LOG_INFO("Removing: %s", bd_addr_to_str(addr));
            le_device_db_remove(i);
            bd_addr_copy(ev_pkt.data.conn_info.addr, addr);
            add_event_to_buffer(unset_conn_id, ev_pkt);
        }
    }
    gap_whitelist_clear();
}

static void delete_paired_device(const bd_addr_t address)
{
    using namespace comm;
    //LOG_INFO("%s: Removing paired device", bd_addr_to_str(address));
    int addr_type;
    bd_addr_t addr; 
    sm_key_t irk;
    int max_count = le_device_db_max_count();
    EventPacket ev_pkt(EventType::DeletePair);
    for (int i = 0; i < max_count; ++i) {
        le_device_db_info(i, &addr_type, addr, irk);
        if (addr_type != BD_ADDR_TYPE_UNKNOWN && bd_addr_cmp(addr, address) == 0) {
            //LOG_INFO("Found. Removing");
            le_device_db_remove(i);
            bd_addr_copy(ev_pkt.data.conn_info.addr, address);
            add_event_to_buffer(unset_conn_id, ev_pkt);
        }
    }
}

} // namespace asha
