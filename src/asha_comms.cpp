#include <pico/time.h>
#include <pico/stdio.h>
#include <pico/stdio_usb.h>

#include <tusb.h>

#include <atomic>
#include <etl/circular_buffer.h>
#include <nanocobs/cobs.h>

#include <btstack.h>

#include "asha_comms.hpp"
#include "asha_vers.h"
#include "audio_stall_trace.h"

namespace asha
{

namespace comm
{
    // BTSnoop header based on https://fte.com/webhelpii/bpa600/Content/Technical_Information/BT_Snoop_File_Format.htm
    struct BTSnoopPacketHeader
    {
        uint32_t orig_len;
        uint32_t incl_len;
        uint32_t pkt_flags;
        uint32_t cuml_drops;
        uint64_t ts_us;

        /** 
         * The header is in network byte order, because of course it is...
         * 
         * Assumes RP2040/RP2350 in little endian mode
         */
        void byte_swap_fields()
        {
            orig_len = __builtin_bswap32(orig_len);
            incl_len = __builtin_bswap32(incl_len);
            pkt_flags = __builtin_bswap32(pkt_flags);
            cuml_drops = __builtin_bswap32(cuml_drops);
            ts_us = __builtin_bswap64(ts_us);
        }
    };

    // Don't empty a large event buffer all at once
    constexpr int send_limit = 2;

    constexpr size_t zero_prefix = 1;
    constexpr size_t cobs_ev_buff_size = zero_prefix + COBS_ENCODE_MAX(sizeof(HeaderPacket) + sizeof(EventPacket));

    static_assert(cobs_ev_buff_size <= COBS_TINYFRAME_SAFE_BUFFER_SIZE);

    constexpr size_t max_hci_packet_len = 180; // Actually 167 for ASHA, but add a few more bytes
    constexpr size_t hci_packet_type = 1;
    constexpr size_t cobs_hci_buff_size = zero_prefix + COBS_ENCODE_MAX(sizeof(HeaderPacket) + sizeof(BTSnoopPacketHeader) + hci_packet_type + max_hci_packet_len);
    constexpr size_t cmd_buff_size = zero_prefix + COBS_ENCODE_MAX(sizeof(HeaderPacket) + sizeof(CmdPacket));
    
    static_assert(cobs_hci_buff_size <= COBS_TINYFRAME_SAFE_BUFFER_SIZE);

    static uint8_t cobs_enc_buff[COBS_TINYFRAME_SAFE_BUFFER_SIZE];

#ifdef PICO_ASHA_AUDIO_STALL_TRACE
    // Protocol and HCI producers run on BTstack core 1. TinyUSB is owned by
    // usb_main() on core 0, so complete COBS frames cross cores through this
    // bounded, non-blocking SPSC queue.
    constexpr uint32_t usb_tx_queue_size = 32;
    constexpr uint32_t usb_tx_queue_mask = usb_tx_queue_size - 1;
    constexpr uint32_t usb_tx_hci_limit = usb_tx_queue_size - 8;
    constexpr uint32_t usb_tx_send_limit = 4;
    static_assert((usb_tx_queue_size & usb_tx_queue_mask) == 0);

    struct USBTxFrame
    {
        uint16_t len = 0;
        std::array<uint8_t, COBS_TINYFRAME_SAFE_BUFFER_SIZE> data = {};
    };

    static std::array<USBTxFrame, usb_tx_queue_size> usb_tx_queue;
    static std::atomic_uint32_t usb_tx_write_index = 0;
    static std::atomic_uint32_t usb_tx_read_index = 0;
    static_assert(std::atomic_uint32_t::is_always_lock_free);

    constexpr size_t trace_records_per_packet = 5;
    constexpr size_t max_trace_decoded_size = sizeof(HeaderPacket) +
                                              sizeof(AudioStallTracePacketHeader) +
                                              trace_records_per_packet * sizeof(AudioStallTraceRecord);
    static_assert(zero_prefix + COBS_ENCODE_MAX(max_trace_decoded_size) <=
                  COBS_TINYFRAME_SAFE_BUFFER_SIZE);
    constexpr size_t max_runtime_snapshot_decoded_size =
        sizeof(HeaderPacket) + sizeof(AudioStallTracePacketHeader) +
        sizeof(AudioStallTraceRuntimeSnapshot);
    static_assert(zero_prefix + COBS_ENCODE_MAX(max_runtime_snapshot_decoded_size) <=
                  COBS_TINYFRAME_SAFE_BUFFER_SIZE);
    static uint8_t trace_cobs_enc_buff[COBS_TINYFRAME_SAFE_BUFFER_SIZE];
    static std::array<AudioStallTraceRecord, trace_records_per_packet> pending_trace_records;
    static uint8_t pending_trace_record_count = 0;
    static uint64_t last_trace_snapshot_us = 0;
#endif

    static etl::circular_buffer<std::array<uint8_t, cobs_ev_buff_size>, 200> event_buff;

    static etl::vector<uint8_t, cmd_buff_size> cmd_buff_enc;

    template<typename T>
    static auto construct_packet(Type header_type, uint16_t conn_id, T const& packet)
    {
        struct {
            HeaderPacket head;
            T pkt;
        } p {
            .head = {
                .type = header_type,
                .len = sizeof(HeaderPacket) + sizeof(T),
                .conn_id = conn_id,
                .ts_ms = to_ms_since_boot(get_absolute_time())
            },
            .pkt = packet
        };

        static_assert(sizeof(p) == sizeof(HeaderPacket) + sizeof(T));
        static_assert((zero_prefix + COBS_ENCODE_MAX(sizeof(p))) <= COBS_TINYFRAME_SAFE_BUFFER_SIZE);

        return p;
    }

#ifdef PICO_ASHA_AUDIO_STALL_TRACE
    static bool enqueue_usb_packet(const uint8_t *data, uint16_t len, bool hci_packet = false)
    {
        if (data == nullptr || len == 0 || len > COBS_TINYFRAME_SAFE_BUFFER_SIZE) return false;
        uint32_t write_index = usb_tx_write_index.load(std::memory_order_relaxed);
        uint32_t read_index = usb_tx_read_index.load(std::memory_order_acquire);
        uint32_t fill = write_index - read_index;
        if (fill >= usb_tx_queue_size || (hci_packet && fill >= usb_tx_hci_limit)) return false;

        auto& frame = usb_tx_queue[write_index & usb_tx_queue_mask];
        frame.len = len;
        memcpy(frame.data.data(), data, len);
        usb_tx_write_index.store(write_index + 1, std::memory_order_release);
        return true;
    }
#endif

    template<typename T>
    static bool construct_and_send_packet(Type header_type, uint16_t conn_id, T const& packet)
    {
        auto pkt = construct_packet(header_type, conn_id, packet);

        size_t enc_len = 0;
        cobs_encode(&pkt, sizeof(pkt), cobs_enc_buff + zero_prefix, sizeof(cobs_enc_buff) - zero_prefix, &enc_len);
#ifdef PICO_ASHA_AUDIO_STALL_TRACE
        return enqueue_usb_packet(cobs_enc_buff, static_cast<uint16_t>(enc_len + zero_prefix));
#else
        stdio_put_string((const char*)cobs_enc_buff, enc_len + zero_prefix, false, false);
        stdio_flush();
        return true;
#endif
    }

    void add_event_to_buffer(uint16_t const conn_id, EventPacket const& event)
    {   
        auto pkt = construct_packet(Type::Event, conn_id, event);
        event_buff.push({0});
        auto& buff = event_buff.back();
        size_t enc_len = 0;
        cobs_encode(&pkt, sizeof(pkt), buff.data() + zero_prefix, buff.size() - zero_prefix, &enc_len);
    }

    void try_send_events()
    {
#ifdef PICO_ASHA_AUDIO_STALL_TRACE
        int send_count = 0;
        while (!event_buff.empty() && send_count < send_limit && stdio_usb_connected()) {
            const auto& buff = event_buff.front();
            if (!enqueue_usb_packet(buff.data(), static_cast<uint16_t>(buff.size()))) break;
            ++send_count;
            event_buff.pop();
        }
#else
        bool flush_req = false;
        int send_count = 0;
        while (!event_buff.empty() && send_count < send_limit && stdio_usb_connected()) {
            const auto& buff = event_buff.front();
            stdio_put_string((const char*)buff.data(), buff.size(), false, false);
            flush_req = true;
            ++send_count;
            event_buff.pop();
        }
        if (flush_req) stdio_flush();
#endif
    }

    void try_send_usb_packets()
    {
#ifdef PICO_ASHA_AUDIO_STALL_TRACE
        if (!tud_cdc_connected()) return;
        for (uint32_t sent = 0; sent < usb_tx_send_limit; ++sent) {
            uint32_t read_index = usb_tx_read_index.load(std::memory_order_relaxed);
            uint32_t write_index = usb_tx_write_index.load(std::memory_order_acquire);
            if (read_index == write_index) return;

            const auto& frame = usb_tx_queue[read_index & usb_tx_queue_mask];
            if (tud_cdc_write_available() < frame.len) return;
            if (tud_cdc_write(frame.data.data(), frame.len) != frame.len) return;
            tud_cdc_write_flush();
            usb_tx_read_index.store(read_index + 1, std::memory_order_release);
        }
#endif
    }

#ifdef PICO_ASHA_AUDIO_STALL_TRACE
    static bool send_audio_trace_payload(uint8_t kind, uint8_t count,
                                         const void *payload, uint8_t payload_size,
                                         uint64_t now_us)
    {
        AudioStallTracePacketHeader trace_header = {
            .magic = AUDIO_STALL_TRACE_MAGIC,
            .version = AUDIO_STALL_TRACE_VERSION,
            .kind = kind,
            .count = count,
            .payload_size = payload_size,
        };
        const size_t decoded_size = sizeof(HeaderPacket) + sizeof(trace_header) + payload_size;
        if (decoded_size > UINT8_MAX) return false;
        HeaderPacket header = {
            .type = Type::AudioTrace,
            .len = static_cast<uint8_t>(decoded_size),
            .conn_id = unset_conn_id,
            .ts_ms = static_cast<uint32_t>(now_us / 1000U),
        };

        trace_cobs_enc_buff[0] = 0;
        cobs_enc_ctx_t enc_ctx = {};
        size_t enc_len = 0;
        if (cobs_encode_inc_begin(trace_cobs_enc_buff + zero_prefix,
                                  sizeof(trace_cobs_enc_buff) - zero_prefix,
                                  &enc_ctx) != COBS_RET_SUCCESS ||
            cobs_encode_inc(&enc_ctx, &header, sizeof(header)) != COBS_RET_SUCCESS ||
            cobs_encode_inc(&enc_ctx, &trace_header, sizeof(trace_header)) != COBS_RET_SUCCESS ||
            cobs_encode_inc(&enc_ctx, payload, payload_size) != COBS_RET_SUCCESS ||
            cobs_encode_inc_end(&enc_ctx, &enc_len) != COBS_RET_SUCCESS) {
            return false;
        }

        uint32_t total = static_cast<uint32_t>(enc_len + zero_prefix);
        return enqueue_usb_packet(trace_cobs_enc_buff,
                                  static_cast<uint16_t>(total));
    }

    void try_send_audio_trace()
    {
        if (!tud_cdc_connected()) return;
        uint64_t now_us = audio_stall_trace_now_us();

        if (last_trace_snapshot_us == 0U ||
            now_us - last_trace_snapshot_us >= AUDIO_STALL_TRACE_SNAPSHOT_INTERVAL_US) {
            AudioStallTraceSnapshot snapshot = {};
            audio_stall_trace_snapshot(&snapshot, now_us);
            if (!send_audio_trace_payload(AUDIO_STALL_TRACE_PAYLOAD_SNAPSHOT, 1U,
                                          &snapshot, sizeof(snapshot), now_us)) {
                return;
            }
            AudioStallTraceRuntimeSnapshot runtime_snapshot = {};
            audio_stall_trace_runtime_snapshot(&runtime_snapshot, now_us);
            if (!send_audio_trace_payload(AUDIO_STALL_TRACE_PAYLOAD_RUNTIME_SNAPSHOT,
                                          1U, &runtime_snapshot,
                                          sizeof(runtime_snapshot), now_us)) {
                return;
            }
            last_trace_snapshot_us = now_us;
        }

        if (pending_trace_record_count == 0U) {
            while (pending_trace_record_count < pending_trace_records.size() &&
                   audio_stall_trace_pop(&pending_trace_records[pending_trace_record_count])) {
                ++pending_trace_record_count;
            }
        }
        if (pending_trace_record_count == 0U) return;

        uint8_t payload_size = static_cast<uint8_t>(pending_trace_record_count *
                                                    sizeof(AudioStallTraceRecord));
        if (send_audio_trace_payload(AUDIO_STALL_TRACE_PAYLOAD_RECORDS,
                                     pending_trace_record_count,
                                     pending_trace_records.data(), payload_size, now_us)) {
            pending_trace_record_count = 0U;
        }
    }
#endif

    void send_intro_packet(int8_t num_connections, uint16_t flags)
    {
        construct_and_send_packet(Type::Intro, unset_conn_id, IntroPacket{.pa_version = {
            .major = PICO_ASHA_FW_VERS_MAJOR,
            .minor = PICO_ASHA_FW_VERS_MINOR,
            .patch = PICO_ASHA_FW_VERS_PATCH
        },
        .num_connected = num_connections,
        .flags = flags});
    }

    void send_usb_info_packet(USBInfo const &usb_info)
    {
        construct_and_send_packet(Type::USBInfo, unset_conn_id, usb_info);
    }

    void send_remote_info_packet(RemoteInfo const& remote_info)
    {
        construct_and_send_packet(Type::RemInfo, unset_conn_id, remote_info);
    }

    void send_advertising_packet(AdvertisingPacket const& ad_packet)
    {
        if (stdio_usb_connected()) {
            construct_and_send_packet(Type::Advert, unset_conn_id, ad_packet);
        }
    }

    bool get_cmd_packet(HeaderPacket& header, CmdPacket& cmd_packet)
    {
        int ch;
        bool got_cmd = false;
        while ((ch = stdio_getchar_timeout_us(0)) != PICO_ERROR_TIMEOUT) {
            uint8_t c = (uint8_t)ch;
            if (c == 0) {
                if (cmd_buff_enc.size() == 0) {
                    continue;
                }
                cmd_buff_enc.push_back(c);
                if (cmd_buff_enc.size() > sizeof(HeaderPacket) + sizeof(CmdPacket)) {
                    auto ret = cobs_decode_tinyframe(cmd_buff_enc.data(), cmd_buff_enc.size());
                    if (ret == COBS_RET_SUCCESS) {
                        memcpy(&header, cmd_buff_enc.data() + 1, sizeof(header));
                        if (header.type == Type::Cmd) {
                            memcpy(&cmd_packet, cmd_buff_enc.data() + 1 + sizeof(header), sizeof(cmd_packet));
                            got_cmd = true;
                        }
                    }
                }
                cmd_buff_enc.clear();
                return got_cmd;
            }
            cmd_buff_enc.push_back(c);
        }
        return got_cmd;
    }

    void send_cmd_resp(uint16_t const conn_id, CmdPacket const& resp)
    {
        construct_and_send_packet(Type::Cmd, conn_id, resp);        
    }

    void send_hci_reset()
    {}

    void send_hci_packet(uint8_t packet_type, uint8_t in, uint8_t *packet, uint16_t len)
    {
        if (packet_type == LOG_MESSAGE_PACKET) return;
        auto abs_time = get_absolute_time();
        uint32_t incl_len = (len > max_hci_packet_len) ? max_hci_packet_len : len;
        BTSnoopPacketHeader snoop_header = {};
        snoop_header.orig_len = sizeof(packet_type) + len;
        snoop_header.incl_len = sizeof(packet_type) + incl_len;
        if (in) {
            snoop_header.pkt_flags |= 1;
        }
        if (packet_type == HCI_COMMAND_DATA_PACKET || packet_type == HCI_EVENT_PACKET) {
            snoop_header.pkt_flags |= 2;
        }
        snoop_header.ts_us = to_us_since_boot(abs_time);
        HeaderPacket header = {
            .type = Type::HCI,
            .len = uint8_t(sizeof(HeaderPacket) + sizeof(BTSnoopPacketHeader) + snoop_header.incl_len),
            .conn_id = unset_conn_id,
            .ts_ms = to_ms_since_boot(abs_time)
        };
        snoop_header.byte_swap_fields();

        cobs_enc_buff[0] = 0;
        cobs_enc_ctx_t enc_ctx = {};
        size_t enc_len = 0;

        cobs_encode_inc_begin(cobs_enc_buff + zero_prefix, sizeof(cobs_enc_buff) - zero_prefix, &enc_ctx);
        cobs_encode_inc(&enc_ctx, &header, sizeof(header));
        cobs_encode_inc(&enc_ctx, &snoop_header, sizeof(snoop_header));
        cobs_encode_inc(&enc_ctx, &packet_type, sizeof(packet_type));
        cobs_encode_inc(&enc_ctx, packet, incl_len);
        cobs_encode_inc_end(&enc_ctx, &enc_len);

        uint32_t total = enc_len + zero_prefix;
#ifdef PICO_ASHA_AUDIO_STALL_TRACE
        // Do not call TinyUSB from this BTstack callback. Queue the complete
        // frame without blocking; usb_main() sends it from core 0. HCI is
        // capped below the queue capacity to reserve room for control events.
        (void)enqueue_usb_packet(cobs_enc_buff, static_cast<uint16_t>(total), true);
#else
        if (tud_cdc_write_available() < total) return;
        tud_cdc_write(cobs_enc_buff, total);
        tud_cdc_write_flush();
#endif
    }

    void send_hci_message([[maybe_unused]] int log_level, 
                          [[maybe_unused]] const char * format, 
                          [[maybe_unused]] va_list argptr)
    {}

} // namespace comm

} // namespace asha
