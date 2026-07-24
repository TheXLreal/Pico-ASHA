#pragma once

#include <array>
#include <cstdint>
#include <cstring>
#include <span>

#include <etl/string.h>

#include "asha_audio.h"
#include "asha_bt.hpp"
#include "asha_comms.hpp"

namespace asha
{

constexpr int ha_process_delay_ticks = 500;

// Android ASHA uses an eight-packet elastic CoC queue. A control-point command
// is allowed three seconds to complete before the link is recovered.
constexpr uint16_t asha_l2cap_queue_depth = ASHA_G722_RING_SIZE;
constexpr uint16_t asha_l2cap_min_mtu = 167u;
constexpr uint16_t asha_l2cap_local_mtu = 512u;
constexpr uint32_t audio_command_timeout_ticks = 3000;

enum class Side  {Left = 0, Right = 1};
enum class Mode  {Mono = 0, Binaural = 1};
enum class Codec {G722_16, G722_24};

struct ROP
{
    static constexpr size_t size = 17;
    uint8_t raw_data[17] = {};

    bool read(const uint8_t* data, size_t data_size) {
        if (data == nullptr || data_size < size) return false;
        memcpy(raw_data, data, size);
        return valid();
    }
    uint8_t version() const { return raw_data[0]; }
    Side side() const { return (raw_data[1] & 0b00000001) ? Side::Right : Side::Left; }
    Mode mode() const { return (raw_data[1] & 0b00000010) ? Mode::Binaural : Mode::Mono; }
    bool csis_supported() const { return raw_data[1] & 0b00000100; }
    uint16_t mfg_id() const { return little_endian_read_16(raw_data, 2); }
    std::span<const uint8_t, 6> unique_id() const { return std::span<const uint8_t, 6>(raw_data + 4, 6); }
    uint64_t hi_sync_id() const {
        uint64_t id = 0;
        for (size_t i = 0; i < 8; ++i) id |= static_cast<uint64_t>(raw_data[2 + i]) << (i * 8);
        return id;
    }
    bool le_coc_supported() const { return raw_data[10] & 0b00000001; }
    uint16_t render_delay() const { return little_endian_read_16(raw_data, 11); }
    uint16_t preparation_delay() const { return little_endian_read_16(raw_data, 13); }
    bool supports_codec(Codec c) const {
        uint16_t codecs = little_endian_read_16(raw_data, 15);
        switch (c) {
            case Codec::G722_16:
                return codecs & 0b0000000000000010;
            case Codec::G722_24:
                return codecs & 0b0000000000000100;
        }
        return false;
    }

    bool same_binaural_set(const ROP& other) const {
        return valid() && other.valid()
            && mode() == Mode::Binaural && other.mode() == Mode::Binaural
            && side() != other.side()
            && hi_sync_id() != 0 && hi_sync_id() == other.hi_sync_id();
    }

    bool valid() const {
        return version() == 0x01 && le_coc_supported() && supports_codec(Codec::G722_16);
    }

    explicit operator bool() const { return valid(); }
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

    uint16_t psm = 0U;
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
    static void on_connected(bd_addr_t addr, hci_con_handle_t handle,
                             uint16_t conn_interval, uint16_t conn_latency);
    static void on_disconnected(hci_con_handle_t handle, uint8_t status, uint8_t reason);
    static void on_data_len_set(hci_con_handle_t handle, uint16_t rx_octets, uint16_t rx_time, uint16_t tx_octets, uint16_t tx_time);
    static void on_connection_update(hci_con_handle_t handle, uint8_t status,
                                     uint16_t conn_interval, uint16_t conn_latency);
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
    uint16_t initial_credits = 0;
    uint16_t remote_mtu = 0;
    int8_t curr_vol = -128;

    uint16_t connection_interval = 0;
    uint16_t connection_latency = 0;
    bool connection_update_pending = false;

    // Array to store current AudioControlPoint command packet
    std::array<uint8_t, 5> acp_cmd_packet = {};
    uint8_t pending_acp_opcode = 0u;

    inline static std::array<HearingAid*, 2> hearing_aids;

    int process_delay_ticks = 0;
    int error_count = 0;

    uint32_t curr_read_index = 0U;
    uint32_t tx_read_index = 0U;
    bool first_audio_send = false;
    uint32_t audio_command_ticks = 0U;
    std::array<uint8_t, ASHA_SDU_SIZE_BYTES> tx_sdu = {};

    bool stop_request_from_other = false;

    std::array<uint8_t, asha_l2cap_local_mtu> recv_buff = {};

    uint16_t conn_id = comm::unset_conn_id;

    inline static uint16_t next_conn_id;

    inline static bool connections_allowed;
    inline static bool audio_streaming_enabled;
    inline static bool auto_pair_enabled;

    /* Member functions */

    static HearingAid* get_by_con_handle(hci_con_handle_t handle);
    static HearingAid* get_by_cid(uint16_t cid);
    static HearingAid* get_by_conn_id(uint16_t conn_id);
    static HearingAid* get_by_cached_addr(bd_addr_t addr);
    static void set_other_side_ptrs();
    void assign_next_conn_id();
    bool is_connected();
    bool is_streaming();
    bool is_audio_busy();
    void set_process_busy();
    void unset_process_busy();
    void set_audio_busy();
    void unset_audio_busy();
    void set_data_length();
    uint8_t request_connection_parameters();
    void send_acp_start();
    void send_acp_stop();
    void send_acp_status(uint8_t status);
    void send_volume(int8_t volume);
    void disconnect();
    void reset();
    const char* get_side_str();

    static void maybe_start_audio_encoder();
    static void stop_audio_encoder_if_idle();
};

} // namespace asha
