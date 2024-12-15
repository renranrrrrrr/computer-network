#include "rtp.h"
#include "util.h"

rtp_packet_t RecvFirstShakehand(int sock, struct sockaddr_in* send_addr) {
    int retry_times = 50;
    int timeout = 100;
    int times = 0;
    int send_addr_size = sizeof(*send_addr);
    while (times < retry_times) {
        if (WaitForMsg(sock, timeout) == 0) {
            // timeout
            times++;
        } else {
            rtp_packet_t buffer = EmptyPacket();
            int pkt_length = recvfrom(sock, &buffer, 1500, 0, (struct sockaddr*)send_addr, (socklen_t*)&send_addr_size);
            if (buffer.rtp.length + sizeof(rtp_header_t) != pkt_length) {
                LOG_DEBUG("Receiver: Length error\n");
                times++;
                continue;
            }
            if (!CheckPacket(buffer)) {
                LOG_DEBUG("Receiver: Checksum error\n");
                times++;
                continue;
            }
            if (buffer.rtp.flags != RTP_SYN) {
                LOG_DEBUG("Receiver: Flags error\n");
                times++;
                continue;
            }
            return buffer;
        }
    }
    LOG_FATAL("Receiver: First shakehand error\n");
    return EmptyPacket();   
}

int RTP_Connect(uint16_t port, struct sockaddr_in* send_addr, int* seq_num) {
    // receiver
    
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) {
        LOG_FATAL("Receiver: Failed to create socket\n");
    }
    
    struct sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        LOG_FATAL("Receiver: Failed to bind socket\n");
    }
    
    rtp_packet_t pkt = RecvFirstShakehand(sock, send_addr);
    LOG_MSG("Receiver: First shakehand success\n");
    
    pkt = NewPacket(pkt.rtp.seq_num + 1, 0, RTP_SYN | RTP_ACK, NULL);
    pkt = SendMsgAck(sock, pkt, pkt.rtp.seq_num, RTP_ACK, *send_addr, 50000, 100);
    LOG_MSG("Receiver: Final shakehand success\n");

    *seq_num = pkt.rtp.seq_num;
    return sock;
}

char confirm[20000];
rtp_packet_t buf[20000];

int DataTrans(int sock, struct sockaddr_in recv_addr, FILE* f, int window_size, int mode, int first_seqnum) {
    memset(confirm, 0, window_size);
    memset(buf, 0, sizeof(rtp_packet_t) * window_size);
    int retry_times = 50;
    int timeout = 100;
    int times = 0;
    int l = first_seqnum, r = first_seqnum + window_size - 1;
    while (times < retry_times) {
        if (WaitForMsg(sock, timeout) == 0) {
            // timeout
            times++;
        } else {
            times = 0;
            rtp_packet_t buffer = EmptyPacket();
            int pkt_length = recvfrom(sock, &buffer, 1500, 0, NULL, NULL);
            if (buffer.rtp.length + sizeof(rtp_header_t) != pkt_length) {
                LOG_DEBUG("Receiver: Length error\n");
                continue;
            }
            if (!CheckPacket(buffer)) {
                LOG_DEBUG("Receiver: Checksum error\n");
                continue;
            }
            if (buffer.rtp.flags != 0 && buffer.rtp.flags != RTP_FIN) {
                LOG_DEBUG("Receiver: Flags error\n");
                continue;
            }
            if (buffer.rtp.seq_num > r) {
                LOG_DEBUG("Receiver: Seqnum error\n");
                continue;
            }
            if (buffer.rtp.flags == RTP_FIN) {
                LOG_MSG("Receiver: DataTrans success\n");
                return buffer.rtp.seq_num;
            }
            if (buffer.rtp.seq_num >= l && buffer.rtp.seq_num <= r) {    
                int idx = buffer.rtp.seq_num % window_size;
                confirm[idx] = 1;
                buf[idx] = buffer;
            }
            while (l <= r && confirm[l % window_size]) {
                fwrite(buf[l % window_size].payload, sizeof(char), buf[l % window_size].rtp.length, f);
                buf[l % window_size] = EmptyPacket();
                confirm[l % window_size] = 0;
                l++;
            }
            r = l + window_size - 1;
            rtp_packet_t pkt = NewPacket(mode == 0 ? l : buffer.rtp.seq_num, 0, RTP_ACK, NULL);
            sendto(sock, &pkt, sizeof(rtp_header_t) + pkt.rtp.length, 0, (struct sockaddr *)&recv_addr, sizeof(recv_addr));
        }
    }
    LOG_FATAL("Receiver: DataTrans timeout\n");
    return 0;
}

int Goodbye(int sock, struct sockaddr_in send_addr, int seq_num) {
    rtp_packet_t pkt = NewPacket(seq_num, 0, RTP_FIN | RTP_ACK, NULL);
    int retry_times = 50;
    int timeout = 200;
    int times = 0;
    while (times < retry_times) {
        sendto(sock, &pkt, sizeof(rtp_header_t) + pkt.rtp.length, 0, (struct sockaddr *)&send_addr, sizeof(send_addr));
        if (WaitForMsg(sock, timeout) == 0) {
            // timeout
            times++;
            LOG_MSG("Receiver: Goodbye success\n");
            return 1;
        } else {
            rtp_packet_t buffer;
            recvfrom(sock, &buffer, 1500, 0, NULL, NULL);
        }
    }
    LOG_FATAL("Receiver: Failed to Goodbye\n");
    return 0;
}

int main(int argc, char **argv) {
    if (argc != 5) {
        LOG_FATAL("Usage: ./receiver [listen port] [file path] [window size] "
                  "[mode]\n");
    }
    struct sockaddr_in send_addr;
    int seq_num;
    int sock = RTP_Connect(atoi(argv[1]), &send_addr, &seq_num);
    char* file_path = argv[2];
    int window_size = atoi(argv[3]);
    int mode = atoi(argv[4]);

    FILE* f = fopen(file_path, "wb");
    if (f == NULL) {
        LOG_FATAL("Receiver: Failed to open file\n");
    }
    seq_num = DataTrans(sock, send_addr, f, window_size, mode, seq_num);

    fclose(f);
    Goodbye(sock, send_addr, seq_num);
    close(sock);

    LOG_DEBUG("Receiver: exiting...\n");
    return 0;
}
