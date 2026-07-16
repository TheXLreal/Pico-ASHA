#include <bitset>
#include <pico/assert.h>
#include <pico/time.h>

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
        start_scan();
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
    if (connections_allowed) {
        set_ble_connection_state(comm::BLEConnectionState::Scanning);
        gap_set_scan_params(1, 0x0030, 0x0030, runtime_settings.get_full_set_paired() ? 1 : 0);
        gap_start_scan();
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
    set_ble_connection_state(comm::BLEConnectionState::Connecting);
    gap_stop_scan();
    gap_connect(addr, addr_type);
}

void HearingAid::on_connected(bd_addr_t addr, hci_con_handle_t handle,
                              uint16_t connection_interval, uint16_t peripheral_latency,
                              uint16_t supervision_timeout)
{
    using namespace comm;

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
    audio_stall_trace_ble_disconnect(ha->conn_handle, ha->cid, sequence,
                                     write_index, ha->curr_read_index, busy,
                                     status, reason, busy_duration_us,
                                     ha->trace_last_successful_send_us);
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
    if (connections_allowed) {
        set_ble_connection_state(BLEConnectionState::Recovering);
    }
    start_scan();
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
            if (bt_status != ATT_ERROR_SUCCESS) {
                //LOG_ERROR("%s: Error creating L2CAP cbm connection: %s", ha->get_side_str(), bt_err_str(att_status));
                add_event_to_buffer(ha->conn_id, EventPacket(EventType::L2CAPCon, StatusType::L2CapStatus, bt_status));
                // Try again later
                ha->unset_process_busy();
                ++ha->error_count;
                ha->process_delay_ticks = ha_process_delay_ticks * 3;
            } else {
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
        case L2CAP_EVENT_CAN_SEND_NOW:
            cid = l2cap_event_can_send_now_get_local_cid(packet);
            ha = get_by_cid(cid);
            if (ha == nullptr || ha->audio_tx_state != AudioTxState::WaitingCanSendNow) {
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
            if (ha == nullptr || ha->audio_tx_state != AudioTxState::WaitingPacketSent) {
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
#endif

                // Complete the local phase before any callback-visible work. A
                // stale CAN_SEND_NOW emitted by BTstack after direct recovery is
                // therefore ignored by the case above.
                ha->audio_tx_state = AudioTxState::Idle;
                ha->audio_tx_state_since_us = 0U;
                ha->audio_tx_sequence = AUDIO_STALL_TRACE_INVALID_SEQUENCE;
                ha->audio_tx_local_recovery = false;
                ha->audio_data = nullptr;
#ifdef PICO_ASHA_AUDIO_STALL_TRACE
                audio_stall_trace_packet_sent(ha->conn_handle, cid,
                                              sequence,
                                              asha_audio_get_write_index(),
                                              ha->curr_read_index,
                                              (ha->audio_state & AudioState::AudioBusy) != 0U,
                                              wait_us);
                ha->trace_last_successful_send_us = now_us;
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
        if (ha->process_state != ProcessState::Audio) { continue; }
        // The remote ASHA stream remains active until ACP Stop completes. A
        // selected SDU must therefore be recovered even if host PCM stops while
        // the L2CAP request is stuck.
        bool audio_active = ha->is_streaming();
        if (ha->process_audio_tx_watchdog(w_index, audio_active)) {
            // One SDU already owns the TX path. In particular, never issue a
            // second CAN_SEND_NOW request while the original one is pending.
            continue;
        }
        ha->credits = l2cap_cbm_available_credits(ha->cid);
        switch (ha->audio_state) {
            case AudioState::Ready:
                // After a zero-credits stop, wait up to the cooldown window for
                // credits to fully replenish before restarting; starting at low
                // credit counts immediately re-drains and produces audible
                // cycling. Falls through on timeout so we don't deadlock on aids
                // that never grant back to the ceiling.
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
                        ha->curr_read_index = w_index - 1;
                        ha->first_audio_send = false;
                    }
                    if (ha->curr_read_index < w_index) {
#ifdef PICO_ASHA_AUDIO_STALL_TRACE
                        ha->trace_ring_underrun_reported = false;
#endif
                        if (ha->credits == 0) {
                            short_log(ha->conn_id, "%s", "Zero credits");
                            // Wait long enough for a slow aid to replenish before
                            // attempting to restart; see the Ready branch above.
                            ha->zero_credits_cooldown = 500;
                            if (ha->other && ha->other->is_streaming()) {
                                ha->other->stop_request_from_other = true;
                            }
                            ha->send_acp_stop();
                            break;
                        }
                        // Restart stream if starting to fall behind
                        if (w_index - ha->curr_read_index >= 2) {
                            short_log(ha->conn_id, "%s", "Stream fell behind: restart");
                            if (ha->other && ha->other->is_streaming()) {
                                ha->other->stop_request_from_other = true;
                            }
                            ha->send_acp_stop();
                            break;
                        }

                        enum AshaAudioSide audio_side = ha->rop.side() == Side::Left ? AshaAudioSide::AudioLeft
                                                                                     : AshaAudioSide::AudioRight;
                        uint8_t* encoded = asha_audio_get_encoded_at_index(
                            audio_side, ha->curr_read_index);
                        memcpy(ha->audio_tx_buffer.data(), encoded,
                               ha->audio_tx_buffer.size());
                        ha->audio_data = ha->audio_tx_buffer.data();
                        ha->audio_tx_ring_index = ha->curr_read_index;
                        ha->audio_tx_sdu_since_us = time_us_64();
                        ++(ha->curr_read_index);
                        ha->request_audio_can_send(w_index);
                        enable_process_delay = true;
#ifdef PICO_ASHA_ENC_STATS
                        send_enc_times = true;
#endif
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
    audio_tx_sequence = audio_data[0];
    audio_tx_state = AudioTxState::WaitingCanSendNow;
    audio_tx_state_since_us = now_us;
    audio_tx_local_recovery = false;
    set_audio_busy(audio_tx_sequence, AUDIO_STALL_TRACE_BUSY_CONTEXT_AUDIO_SDU);

#ifdef PICO_ASHA_AUDIO_STALL_TRACE
    trace_pending_sequence = audio_tx_sequence;
    trace_can_send_request_us = now_us;
    // Record the request before the BTstack call because CAN_SEND_NOW may be
    // delivered synchronously from l2cap_request_can_send_now_event().
    audio_stall_trace_can_send_requested(conn_handle, cid, audio_tx_sequence,
                                         write_index, curr_read_index, true);
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

    // The phase transition must precede l2cap_send(): BTstack is allowed to
    // make progress synchronously and emit PACKET_SENT from inside the call.
    audio_tx_state = AudioTxState::WaitingPacketSent;
    audio_tx_state_since_us = now_us;
    audio_tx_local_recovery = local_recovery;
    if ((audio_state & AudioState::AudioBusy) == 0U) {
        set_audio_busy(sequence, AUDIO_STALL_TRACE_BUSY_CONTEXT_AUDIO_SDU);
    }

#ifdef PICO_ASHA_AUDIO_STALL_TRACE
    trace_l2cap_send_us = now_us;
#endif
    uint8_t result = l2cap_send(cid, audio_data, ASHA_SDU_SIZE_BYTES);
#ifdef PICO_ASHA_AUDIO_STALL_TRACE
    audio_stall_trace_l2cap_send(conn_handle, cid, sequence, write_index,
                                 curr_read_index,
                                 (audio_state & AudioState::AudioBusy) != 0U,
                                 ASHA_SDU_SIZE_BYTES, result);
    if (result == ERROR_CODE_SUCCESS) {
        trace_last_sequence = sequence;
    }
#endif
    if (result != ERROR_CODE_SUCCESS) {
        reconnect_after_audio_tx_stall(write_index, 0U, result);
        return false;
    }
    return true;
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
#ifdef PICO_ASHA_AUDIO_STALL_TRACE
                uint32_t dropped_frames = latest_index - audio_tx_ring_index;
                uint8_t old_sequence = audio_tx_sequence;
#endif
                enum AshaAudioSide audio_side = rop.side() == Side::Left
                                                    ? AshaAudioSide::AudioLeft
                                                    : AshaAudioSide::AudioRight;
                uint8_t* latest = asha_audio_get_encoded_at_index(audio_side,
                                                                  latest_index);
                memcpy(audio_tx_buffer.data(), latest, audio_tx_buffer.size());
                audio_data = audio_tx_buffer.data();
                audio_tx_ring_index = latest_index;
                audio_tx_sdu_since_us = now_us;
                audio_tx_sequence = audio_data[0];
                curr_read_index = write_index;
#ifdef PICO_ASHA_AUDIO_STALL_TRACE
                trace_pending_sequence = audio_tx_sequence;
                audio_stall_trace_tx_stale_drop(
                    conn_handle, cid, old_sequence, audio_tx_sequence,
                    write_index, curr_read_index, busy, audio_age_us,
                    dropped_frames);
#endif
            }
#ifdef PICO_ASHA_AUDIO_STALL_TRACE
            audio_stall_trace_tx_watchdog(AUDIO_STALL_TRACE_TX_RECOVERY,
                                          conn_handle, cid, audio_tx_sequence,
                                          write_index, curr_read_index, busy,
                                          stalled_us, ERROR_CODE_SUCCESS);
#endif
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

    // The existing disconnection-complete path resets all ASHA/L2CAP state and
    // calls start_scan(), preserving pairing while creating a fresh CID.
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
    audio_tx_state = AudioTxState::Idle;
    audio_tx_state_since_us = 0U;
    audio_tx_sdu_since_us = 0U;
    audio_tx_ring_index = 0U;
    audio_tx_sequence = AUDIO_STALL_TRACE_INVALID_SEQUENCE;
    audio_tx_local_recovery = false;
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
#ifdef PICO_ASHA_AUDIO_STALL_TRACE
    trace_can_send_request_us = 0U;
    trace_l2cap_send_us = 0U;
    trace_audio_busy_since_us = 0U;
    trace_last_successful_send_us = 0U;
    trace_connection_interval = 0U;
    trace_peripheral_latency = 0U;
    trace_supervision_timeout = 0U;
    trace_pending_sequence = AUDIO_STALL_TRACE_INVALID_SEQUENCE;
    trace_last_sequence = AUDIO_STALL_TRACE_INVALID_SEQUENCE;
    trace_busy_stall_reported = false;
    trace_ring_underrun_reported = false;
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
