#include "ts_demux.h"

#include <cstring>

#include <esp_log.h>

static const char* TAG = "TsDemux";

namespace {
constexpr size_t kTsPacket = 188;
}

void TsDemux::Reset() {
    pmt_pid_ = 0xFFFF;
    audio_pid_ = 0xFFFF;
    pmt_parsed_ = false;
}

void TsDemux::ParsePat(const uint8_t* p, int len) {
    // PAT payload: pointer_field(1) + table_id(1) ...
    if (len < 8) return;
    int ptr = p[0];
    if (1 + ptr + 8 > len) return;
    const uint8_t* t = p + 1 + ptr;
    if (t[0] != 0x00) return;                       // table_id: PAT = 0
    int section_len = ((t[1] & 0x0F) << 8) | t[2];
    if (section_len < 9) return;
    // 跳过 8 字节头，之后每 4 字节一个 program（program_number 2 + pid 2）
    // 边界必须是 3(表头) + section_len - 4(CRC)，写成 1+ 会少算 2 字节导致整个循环不执行
    for (int i = 8; i + 4 <= 3 + section_len - 4; i += 4) {
        uint16_t prog = (uint16_t)((t[i] << 8) | t[i + 1]);
        uint16_t pid = (uint16_t)(((t[i + 2] & 0x1F) << 8) | t[i + 3]);
        if (prog != 0) {                            // 0 是 NIT，跳过
            pmt_pid_ = pid;
            break;
        }
    }
}

void TsDemux::ParsePmt(const uint8_t* p, int len) {
    if (len < 12) return;
    int ptr = p[0];
    if (1 + ptr + 12 > len) return;
    const uint8_t* t = p + 1 + ptr;
    if (t[0] != 0x02) return;                       // table_id: PMT = 2
    int section_len = ((t[1] & 0x0F) << 8) | t[2];
    int prog_info_len = ((t[10] & 0x0F) << 8) | t[11];
    int pos = 12 + prog_info_len;
    int end = 3 + section_len - 4;                  // 去掉 CRC
    if (end > len) end = len;

    while (pos + 5 <= end) {
        uint8_t stream_type = t[pos];
        uint16_t pid = (uint16_t)(((t[pos + 1] & 0x1F) << 8) | t[pos + 2]);
        int es_len = ((t[pos + 3] & 0x0F) << 8) | t[pos + 4];
        // 0x0F = AAC(ADTS), 0x03/0x04 = MPEG 音频
        if (stream_type == 0x0F || stream_type == 0x03 || stream_type == 0x04) {
            audio_pid_ = pid;
            pmt_parsed_ = true;
            ESP_LOGI(TAG, "找到音频流: PID=0x%04x stream_type=0x%02x", pid, stream_type);
            return;
        }
        pos += 5 + es_len;
    }
}

void TsDemux::Feed(const uint8_t* data, size_t len, FrameSink sink, void* ctx) {
    size_t off = 0;
    while (off + kTsPacket <= len) {
        const uint8_t* p = data + off;
        if (p[0] != 0x47) {          // 丢失同步，往后找一个包
            off++;
            continue;
        }
        bool pusi = (p[1] & 0x40) != 0;
        uint16_t pid = (uint16_t)(((p[1] & 0x1F) << 8) | p[2]);
        int afc = (p[3] >> 4) & 0x03;
        int hdr = 4;
        if (afc & 0x02) {            // 有 adaptation field
            if (hdr >= (int)kTsPacket) {
                off += kTsPacket;
                continue;
            }
            hdr += 1 + p[4];
        }
        if (!(afc & 0x01) || hdr >= (int)kTsPacket) {   // 无 payload
            off += kTsPacket;
            continue;
        }
        const uint8_t* payload = p + hdr;
        int plen = (int)kTsPacket - hdr;

        if (pid == 0) {
            if (pusi) ParsePat(payload, plen);
        } else if (pid == pmt_pid_ && !pmt_parsed_) {
            if (pusi) ParsePmt(payload, plen);
        } else if (pid == audio_pid_) {
            if (pusi) {
                // PES 头：00 00 01 xx | len(2) | flags(2) | hdr_len(1) | 可选头
                if (plen >= 9 && payload[0] == 0 && payload[1] == 0 && payload[2] == 1) {
                    int hdr_len = 9 + payload[8];
                    if (hdr_len < plen) {
                        payload += hdr_len;
                        plen -= hdr_len;
                    } else {
                        plen = 0;
                    }
                }
            }
            if (plen > 0 && sink != nullptr) {
                sink(ctx, payload, (size_t)plen);
            }
        }
        off += kTsPacket;
    }
}
