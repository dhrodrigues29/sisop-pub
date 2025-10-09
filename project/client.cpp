// client.cpp
#include <iostream>
#include <string>
#include <cstring>
#include <pthread.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <time.h>
#include <sys/select.h>
#include "packet.h"

std::string server_ip;
uint32_t seq = 1;

std::string get_current_time() {
    time_t now = time(0);
    struct tm *ltm = localtime(&now);
    char buffer[20];
    strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", ltm);
    return std::string(buffer);
}
// Função para exibir a confirmação de uma requisição processada.
// Imprime o ACK recebido no formato exigido
void print_ack(const Packet& ack, const char* dest_ip, uint32_t value) {
    std::string timestamp = get_current_time();
    std::cout << timestamp << " server " << server_ip << " id req " << ntohl(ack.ack.seqn) << " dest " << dest_ip << " value " << value << " new balance " << ntohl(ack.ack.new_balance) << std::endl;
}
//Envia descoberta, recebe IP do servidor, e gerencia requisições.
int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "Usage: ./cliente <port>" << std::endl;
        return 1;
    }
    int port = atoi(argv[1]);

    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    int opt = 1;
    setsockopt(sock, SOL_SOCKET, SO_BROADCAST, &opt, sizeof(opt));

    struct sockaddr_in broadcast_addr;
    memset(&broadcast_addr, 0, sizeof(broadcast_addr));
    broadcast_addr.sin_family = AF_INET;
    broadcast_addr.sin_port = htons(port);
    broadcast_addr.sin_addr.s_addr = inet_addr("255.255.255.255");

    // Discovery
    Packet disc;
    disc.type = DESC;
    disc.seqn = 0;

    sendto(sock, &disc, sizeof(disc), 0, (struct sockaddr*)&broadcast_addr, sizeof(broadcast_addr));

    struct sockaddr_in recv_addr;
    socklen_t addr_len = sizeof(recv_addr);
    Packet ack;
    recvfrom(sock, &ack, sizeof(ack), 0, (struct sockaddr*)&recv_addr, &addr_len);

    if (ack.type == DESC_ACK) {
        char serv_ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &(recv_addr.sin_addr), serv_ip, INET_ADDRSTRLEN);
        server_ip = serv_ip;
        std::string timestamp = get_current_time();
        std::cout << timestamp << " server addr " << server_ip << std::endl;
    } else {
        std::cerr << "Discovery failed" << std::endl;
        close(sock);
        return 1;
    }

    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port);
    inet_pton(AF_INET, server_ip.c_str(), &server_addr.sin_addr);

   //process inputs
    std::string line;
    while (std::getline(std::cin, line)) {
        if (line.empty()) continue;

        char dest_ip_str[INET_ADDRSTRLEN];
        uint32_t value;
        if (sscanf(line.c_str(), "%s %u", dest_ip_str, &value) != 2) continue;

        uint32_t dest_ip;
        inet_pton(AF_INET, dest_ip_str, &dest_ip);

        Packet req;
        req.type = REQ;
        req.seqn = htonl(seq);
        req.req.dest_addr = dest_ip; 
        req.req.value = htonl(value);

        bool acknowledged = false;
        while (!acknowledged) {
            sendto(sock, &req, sizeof(req), 0, (struct sockaddr*)&server_addr, sizeof(server_addr));

            fd_set fds;
            FD_ZERO(&fds);
            FD_SET(sock, &fds);
            struct timeval tv;
            tv.tv_sec = 0;
            tv.tv_usec = TIMEOUT_MS * 1000;

            int ready = select(sock + 1, &fds, NULL, NULL, &tv);
            if (ready > 0) {
                recvfrom(sock, &ack, sizeof(ack), 0, NULL, NULL);
                if (ack.type == REQ_ACK && ntohl(ack.ack.seqn) == seq) {
                    print_ack(ack, dest_ip_str, value);
                    acknowledged = true;
                    seq++;
                }
            }
           
        }
    }

    close(sock);
    return 0;
}