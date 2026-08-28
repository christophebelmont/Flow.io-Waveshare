#include "Core/Hmi/HmiUdpProtocol.h"

#include <string.h>

namespace {

static constexpr size_t kCrcOffset = offsetof(HmiUdpHeader, crc);

static void copyText_(char* out, size_t outLen, const char* in)
{
    if (!out || outLen == 0) return;
    out[0] = '\0';
    if (!in) return;
    size_t i = 0;
    while (i + 1 < outLen && in[i] != '\0') {
        out[i] = in[i];
        ++i;
    }
    out[i] = '\0';
}

static uint16_t packetCrc_(const HmiUdpHeader& header, const uint8_t* payload)
{
    uint16_t crc = hmiUdpCrc16(reinterpret_cast<const uint8_t*>(&header), kCrcOffset);
    if (header.len > 0U && payload) {
        crc = hmiUdpCrc16(payload, header.len, crc);
    }
    return crc;
}

}  // namespace

uint16_t hmiUdpCrc16(const uint8_t* data, size_t len, uint16_t seed)
{
    uint16_t crc = seed;
    if (!data) return crc;
    for (size_t i = 0; i < len; ++i) {
        crc ^= (uint16_t)data[i];
        for (uint8_t bit = 0; bit < 8U; ++bit) {
            if ((crc & 0x0001U) != 0U) crc = (uint16_t)((crc >> 1) ^ 0xA001U);
            else crc = (uint16_t)(crc >> 1);
        }
    }
    return crc;
}

uint32_t hmiUdpTokenCrc(const char* token)
{
    if (!token || token[0] == '\0') return 0U;
    return (uint32_t)hmiUdpCrc16(reinterpret_cast<const uint8_t*>(token), strlen(token));
}

bool hmiUdpBuildPacket(uint8_t* out,
                       size_t outCapacity,
                       size_t& outLen,
                       HmiUdpMsgType type,
                       uint16_t seq,
                       uint16_t ack,
                       uint8_t flags,
                       const void* payload,
                       size_t payloadLen)
{
    outLen = 0;
    if (!out || outCapacity < sizeof(HmiUdpHeader)) return false;
    if (payloadLen > 0xFFFFU) return false;
    if ((size_t)payloadLen + sizeof(HmiUdpHeader) > outCapacity) return false;
    if (payloadLen > 0U && !payload) return false;

    HmiUdpHeader header{};
    header.magic0 = HMI_UDP_MAGIC0;
    header.magic1 = HMI_UDP_MAGIC1;
    header.version = HMI_UDP_VERSION;
    header.type = (uint8_t)type;
    header.seq = seq;
    header.ack = ack;
    header.flags = flags;
    header.len = (uint16_t)payloadLen;
    header.crc = packetCrc_(header, reinterpret_cast<const uint8_t*>(payload));

    memcpy(out, &header, sizeof(header));
    if (payloadLen > 0U) {
        memcpy(out + sizeof(header), payload, payloadLen);
    }
    outLen = sizeof(header) + payloadLen;
    return true;
}

bool hmiUdpValidatePacket(const uint8_t* data, size_t len, const HmiUdpHeader*& header, const uint8_t*& payload)
{
    header = nullptr;
    payload = nullptr;
    if (!data || len < sizeof(HmiUdpHeader) || len > HMI_UDP_MAX_PACKET) return false;

    const HmiUdpHeader* h = reinterpret_cast<const HmiUdpHeader*>(data);
    if (h->magic0 != HMI_UDP_MAGIC0 || h->magic1 != HMI_UDP_MAGIC1) return false;
    if (h->version != HMI_UDP_VERSION) return false;
    if ((size_t)h->len + sizeof(HmiUdpHeader) != len) return false;

    const uint8_t* p = data + sizeof(HmiUdpHeader);
    const uint16_t expected = packetCrc_(*h, p);
    if (expected != h->crc) return false;

    header = h;
    payload = p;
    return true;
}

void hmiUdpEventToPayload(const HmiEvent& event, HmiUdpEventPayload& out)
{
    out = HmiUdpEventPayload{};
    out.type = (uint8_t)event.type;
    out.command = (uint8_t)event.command;
    out.contextRef = event.contextRef;
    out.pageId = event.pageId;
    out.row = event.row;
    out.value = event.value;
    out.direction = event.direction;
    out.sliderValue = event.sliderValue;
    copyText_(out.text, sizeof(out.text), event.text);
}

void hmiUdpPayloadToEvent(const HmiUdpEventPayload& payload, HmiEvent& out)
{
    out = HmiEvent{};
    out.type = (HmiEventType)payload.type;
    out.command = (HmiCommandId)payload.command;
    out.contextRef = payload.contextRef;
    out.pageId = payload.pageId;
    out.row = payload.row;
    out.value = payload.value;
    out.direction = payload.direction;
    out.sliderValue = payload.sliderValue;
    copyText_(out.text, sizeof(out.text), payload.text);
}

void hmiUdpRtcToPayload(const HmiRtcDateTime& rtc, HmiUdpRtcPayload& out)
{
    out.year = rtc.year;
    out.month = rtc.month;
    out.day = rtc.day;
    out.hour = rtc.hour;
    out.minute = rtc.minute;
    out.second = rtc.second;
}

void hmiUdpPayloadToRtc(const HmiUdpRtcPayload& payload, HmiRtcDateTime& out)
{
    out.year = payload.year;
    out.month = payload.month;
    out.day = payload.day;
    out.hour = payload.hour;
    out.minute = payload.minute;
    out.second = payload.second;
}
