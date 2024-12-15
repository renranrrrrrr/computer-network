#include "rtp.h"
#include "util.h"

int SendFinalHandshake(int sock, rtp_packet_t pkt, struct sockaddr_in addr) {
    int retry_times = 50;
    int timeout = 2000;
    int times = 0;
    while (times < retry_times) {
        sendto(sock, &pkt, sizeof(rtp_header_t) + pkt.rtp.length, 0, (struct sockaddr *)&addr, sizeof(addr));
        if (WaitForMsg(sock, timeout) == 0) {
            // timeout -> final handshake success
            times++;
            return 1;
        } else {
            // readable
            rtp_packet_t buffer;
            recvfrom(sock, &buffer, 1500, 0, NULL, NULL);
        }
    }
    return 0;
}

int RTP_Connect(char* ip, uint16_t port, struct sockaddr_in* recv_addr) {
    // sender
    
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) {
        LOG_FATAL("Sender: Failed to create socket\n");
    }
    
    recv_addr->sin_family = AF_INET;
    recv_addr->sin_port = htons(port);
    if (inet_aton(ip, &recv_addr->sin_addr) == 0) {
        LOG_FATAL("Sender: Failed to convert IP address\n");
    }
    
    rtp_packet_t pkt = NewPacket(114514, 0, RTP_SYN, NULL);
    SendMsgAck(sock, pkt, 114515, RTP_SYN | RTP_ACK, *recv_addr, 50000, 100);
    LOG_MSG("Sender: First handshake success\n");

    pkt = NewPacket(114515, 0, RTP_ACK, NULL);
    if (SendFinalHandshake(sock, pkt, *recv_addr)) {
        LOG_MSG("Sender: Final handshake success\n");
    } else {
        LOG_FATAL("Sender: Failed to handshake final time\n");
    }

    return sock;
}

char confirm[20000];
rtp_packet_t buf[20000];

int DataTrans(int sock, struct sockaddr_in recv_addr, FILE* f, int window_size, int mode, int first_seqnum) {
    int timeout = 100;
    memset(confirm, 0, window_size);
    memset(buf, 0, sizeof(rtp_packet_t) * window_size);
    
    int l = first_seqnum, r = first_seqnum - 1;
    while (1) {
        // LOG_DEBUG("Sender: [%d, %d]\n", l, r);
        while (l <= r && confirm[l % window_size]) {
            confirm[l % window_size] = 0;
            buf[l % window_size] = EmptyPacket();
            l += 1;
        }
        while (r < l + window_size - 1) {
            r += 1;
            int idx = r % window_size;
            char payload[PAYLOAD_MAX];
            int length = fread(payload, 1, PAYLOAD_MAX, f);
            if (length == 0) {
                --r;
                break;
            }
            buf[idx] = NewPacket(r, length, 0, payload);
        }
        if (l > r) {
            LOG_MSG("Sender: Data transmission success\n");
            return r + 1; 
        }
        for (int i = l; i <= r; ++i) {
            if (mode == 0 || !confirm[i % window_size]) {
                sendto(sock, &buf[i % window_size], sizeof(rtp_header_t) + buf[i % window_size].rtp.length, 0, (struct sockaddr *)&recv_addr, sizeof(recv_addr));
            }
        }
        int modified = 0;
        while (!modified) {
            if (WaitForMsg(sock, timeout) == 0) {
                // timeout
                break;
            }
            rtp_packet_t buffer;
            int pkt_length = recvfrom(sock, &buffer, 1500, 0, NULL, NULL);
            // if (pkt_length == 0) continue;
            if (buffer.rtp.length + sizeof(rtp_header_t) != pkt_length) {
                LOG_DEBUG("Sender: Length error\n");
                continue;
            }
            if (!CheckPacket(buffer)) {
                LOG_DEBUG("Sender: Checksum error\n");
                continue;
            }
            if (buffer.rtp.flags != RTP_ACK) {
                LOG_DEBUG("Receiver: Flags error\n");
                continue;
            }
            if (mode == 0) {
                if (buffer.rtp.seq_num > l) {
                    l = buffer.rtp.seq_num;
                    modified = 1;
                }
            } else {
                if (buffer.rtp.seq_num < l || buffer.rtp.seq_num > r) {
                    LOG_DEBUG("Sender: Seqnum error\n");
                    continue;
                }
                confirm[buffer.rtp.seq_num % window_size] = 1;
            }
            while (l <= r && confirm[l % window_size]) {
                confirm[l % window_size] = 0;
                buf[l % window_size] = EmptyPacket();
                l += 1;
                modified = 1;
            }
            // }
        }
    }
}

int Goodbye(int sock, struct sockaddr_in recv_addr, int seq_num) {
    rtp_packet_t pkt = NewPacket(seq_num, 0, RTP_FIN, NULL);
    SendMsgAck(sock, pkt, seq_num, RTP_FIN | RTP_ACK, recv_addr, 50000, 100);
    LOG_MSG("Sender: Goodbye success\n");
    return 1;
}

int main(int argc, char **argv) {
    // if (argc != 6) {
    //     LOG_FATAL("Usage: ./sender [receiver ip] [receiver port] [file path] "
    //               "[window size] [mode]\n");
    // }
    struct sockaddr_in recv_addr;
    int sock = RTP_Connect(argv[1], atoi(argv[2]), &recv_addr);
    char* file_path = argv[3];
    int window_size = atoi(argv[4]);
    int mode = atoi(argv[5]);

    FILE* f = fopen(file_path, "rb");
    if (f == NULL) {
        LOG_FATAL("Sender: Failed to open file\n");
    }
    int seq_num = DataTrans(sock, recv_addr, f, window_size, mode, 114515);

    fclose(f);
    Goodbye(sock, recv_addr, seq_num);
    close(sock);

    LOG_DEBUG("Sender: exiting...\n");
    return 0;
}
