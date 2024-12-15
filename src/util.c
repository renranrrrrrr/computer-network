#include "util.h"

static uint32_t crc32_for_byte(uint32_t r) {
    for (int j = 0; j < 8; ++j) r = (r & 1 ? 0 : (uint32_t)0xEDB88320L) ^ r >> 1;
    return r ^ (uint32_t)0xFF000000L;
}

static void crc32(const void* data, size_t n_bytes, uint32_t* crc) {
    static uint32_t table[0x100];
    if (!*table)
        for (size_t i = 0; i < 0x100; ++i) table[i] = crc32_for_byte(i);
    for (size_t i = 0; i < n_bytes; ++i)
        *crc = table[(uint8_t)*crc ^ ((uint8_t*)data)[i]] ^ *crc >> 8;
}

// Computes checksum for `n_bytes` of data
//
// Hint 1: Before computing the checksum, you should set everything up
// and set the "checksum" field to 0. And when checking if a packet
// has the correct check sum, don't forget to set the "checksum" field
// back to 0 before invoking this function.
//
// Hint 2: `len + sizeof(rtp_header_t)` is the real length of a rtp
// data packet.
uint32_t compute_checksum(const void* pkt, size_t n_bytes) {
    uint32_t crc = 0;
    crc32(pkt, n_bytes, &crc);
    return crc;
}

rtp_packet_t EmptyPacket() {
    rtp_packet_t pkt;
    pkt.rtp.seq_num = 0;
    pkt.rtp.length = 0;
    pkt.rtp.checksum = 0;
    pkt.rtp.flags = 0;
    memset(pkt.payload, 0, PAYLOAD_MAX);
    pkt.rtp.checksum = compute_checksum(&pkt, sizeof(rtp_header_t) + pkt.rtp.length);
    return pkt;
}

rtp_packet_t NewPacket(uint32_t seq_num, uint16_t length, uint8_t flags, char* payload) {
    rtp_packet_t pkt;
    pkt.rtp.seq_num = seq_num;
    pkt.rtp.length = length;
    pkt.rtp.checksum = 0;
    pkt.rtp.flags = flags;
    memset(pkt.payload, 0, PAYLOAD_MAX);
    memcpy(pkt.payload, payload, length);
    pkt.rtp.checksum = compute_checksum(&pkt, sizeof(rtp_header_t) + pkt.rtp.length);
    return pkt;
}

int WaitForMsg(int sock, int timeout) {
    // timeout: ms
    // return: 0 if timeout, 1 if readable
    struct timeval tv = {timeout / 1000, (timeout % 1000) * 1000};
    fd_set rset;
    FD_ZERO(&rset);
    FD_SET(sock, &rset);
    int ret = select(sock + 1, &rset, NULL, NULL, &tv);
    if (ret < 0) {
        LOG_FATAL("WaitForMsg: select error\n");
    }
    return ret;
}

int CheckPacket(rtp_packet_t pkt) {
    int length = pkt.rtp.length;
    int checksum = pkt.rtp.checksum;
    pkt.rtp.checksum = 0;
    return checksum == compute_checksum(&pkt, sizeof(rtp_header_t) + length);
}

rtp_packet_t SendMsgAck(int sock, rtp_packet_t pkt, int required_seqnum, int require_flags, struct sockaddr_in addr, int retry_times, int timeout) {
    int times = 0;
    while (times < retry_times) {
        sendto(sock, &pkt, sizeof(rtp_header_t) + pkt.rtp.length, 0, (struct sockaddr *)&addr, sizeof(addr));
        if (WaitForMsg(sock, timeout) == 0) {
            // timeout
            times++;
        } else {
            // readable
            rtp_packet_t buffer = EmptyPacket();
            int pkt_length = recvfrom(sock, &buffer, 1500, 0, NULL, NULL);
            if (buffer.rtp.length + sizeof(rtp_header_t) != pkt_length) {
                LOG_DEBUG("SendMsgAck: Length error\n");
                times++;
                continue;
            }
            if (!CheckPacket(buffer)) {
                LOG_DEBUG("SendMsgAck: Checksum error\n");
                times++;
                continue;
            }
            if (buffer.rtp.flags != require_flags) {
                LOG_DEBUG("SendMsgAck: Flags error\n");
                times++;
                continue;
            }
            if (buffer.rtp.seq_num != required_seqnum) {
                LOG_DEBUG("SendMsgAck: Seqnum error\n");
                times++;
                continue;
            }
            return buffer;
        }
    }
    LOG_FATAL("SendMsgAck: Timeout\n");
    return EmptyPacket();
}