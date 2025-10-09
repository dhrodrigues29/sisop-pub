#include <iostream>
#include <vector>
#include <string>
#include <cstring>
#include <pthread.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <time.h>
#include "packet.h"

struct Client {
    char address[INET_ADDRSTRLEN];
    uint32_t last_req = 0; 
    int balance = INITIAL_BALANCE;
};

std::vector<Client> clients;
uint32_t num_transactions = 0; 
uint32_t total_transferred = 0; 
int total_balance = 0;

pthread_mutex_t data_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t data_cond = PTHREAD_COND_INITIALIZER;
bool new_data = false;

// Função para obter a data e hora atual no formato especificado (YYYY-MM-DD HH:MM:SS).
std::string get_current_time() {
    time_t now = time(0);
    struct tm *ltm = localtime(&now);
    char buffer[20];
    strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", ltm);
    return std::string(buffer);
}

// Função para buscar um cliente na lista pelo endereço IP.
// Retorna um ponteiro para o cliente ou nullptr se não encontrado.
Client* find_client(const char* addr) {
    for (auto& c : clients) {
        if (strcmp(c.address, addr) == 0) {
            return &c;
        }
    }
    return nullptr;
}

// Função para atualizar os dados e exibir logs no formato exigido.
// Sincroniza acesso com mutex e recalcula total_balance.
void update_and_print(const char* src_addr, uint32_t req_id, const char* dest_addr, uint32_t value, bool is_dup, bool success) {
    pthread_mutex_lock(&data_mutex);
    int computed_total_balance = 0;
    for (const auto& c : clients) {
        computed_total_balance += c.balance;
    }
    std::string timestamp = get_current_time();
    std::cout << timestamp << " client " << src_addr;
    if (is_dup) {
        std::cout << " DUP!!";
    }
    std::cout << " id req " << req_id << " dest " << dest_addr << " value " << value << std::endl;
    std::cout << "num transactions " << num_transactions << " total transferred " << total_transferred << " total balance " << computed_total_balance << std::endl;
    new_data = true;
    pthread_cond_broadcast(&data_cond);
    pthread_mutex_unlock(&data_mutex);
}

struct ProcessArg {
    int sock;
    Packet packet;
    struct sockaddr_in client_addr;
};
// Função para processar uma requisição recebida em uma thread separada.
// Atualiza saldo, envia ACK, e registra transação.
void* process_request(void* arg) {
    ProcessArg* parg = static_cast<ProcessArg*>(arg);
    Packet* packet = &parg->packet;
    struct sockaddr_in client_addr = parg->client_addr;
    int sock = parg->sock;

    char src_ip[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &(client_addr.sin_addr), src_ip, INET_ADDRSTRLEN);

    pthread_mutex_lock(&data_mutex);

    Client* src_client = find_client(src_ip);
    if (!src_client) {
        pthread_mutex_unlock(&data_mutex);
        delete parg;
        return nullptr;
    }

    Packet ack;
    ack.type = REQ_ACK;
    ack.ack.seqn = packet->seqn; 
    ack.ack.new_balance = htonl(src_client->balance);

    bool success = false;
    bool is_dup = false;
    char dest_ip[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &packet->req.dest_addr, dest_ip, INET_ADDRSTRLEN);
    uint32_t value = ntohl(packet->req.value);

    uint32_t seqn_host = ntohl(packet->seqn);
    if (seqn_host == src_client->last_req + 1) {
        if (value == 0) {
            success = true;
        } else if (static_cast<uint32_t>(src_client->balance) >= value) {
            Client* dest_client = find_client(dest_ip);
            if (dest_client) {
                src_client->balance -= value;
                dest_client->balance += value;
                num_transactions++;
                total_transferred += value;
                success = true;
            }
        }
        if (success) {
            src_client->last_req = seqn_host;
            ack.ack.new_balance = htonl(src_client->balance);
        }
    } else if (seqn_host <= src_client->last_req) {
        is_dup = true;
        ack.ack.seqn = htonl(src_client->last_req);
        ack.ack.new_balance = htonl(src_client->balance);
    } else {
        ack.ack.seqn = htonl(src_client->last_req);
        ack.ack.new_balance = htonl(src_client->balance);
    }

    update_and_print(src_ip, seqn_host, dest_ip, value, is_dup, success);

    pthread_mutex_unlock(&data_mutex);

    sendto(sock, &ack, sizeof(ack), 0, (struct sockaddr*)&client_addr, sizeof(client_addr));

    delete parg;
    return nullptr;
}

struct ThreadArg {
    int sock;
    int port;
};

// Função para a thread de descoberta, ouvindo pacotes UDP e delegando processamento.
// Mantém o servidor ativo, registrando novos clientes.
void* discovery_thread(void* arg) {
    ThreadArg* t_arg = static_cast<ThreadArg*>(arg);
    int sock = t_arg->sock;
    int port = t_arg->port;

    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port);
    server_addr.sin_addr.s_addr = INADDR_ANY;

    bind(sock, (struct sockaddr*)&server_addr, sizeof(server_addr));

    struct sockaddr_in client_addr;
    socklen_t addr_len = sizeof(client_addr);
    Packet packet;
    char client_ip[INET_ADDRSTRLEN]; // Adicionada a declaração de client_ip

    while (true) {
        recvfrom(sock, &packet, sizeof(packet), 0, (struct sockaddr*)&client_addr, &addr_len);
        inet_ntop(AF_INET, &(client_addr.sin_addr), client_ip, INET_ADDRSTRLEN); // Inicializa client_ip
        std::cout << "Recebido pacote type " << packet.type << " de " << client_ip << std::endl; 

        if (packet.type == DESC) {
            pthread_mutex_lock(&data_mutex);
            Client* c = find_client(client_ip);
            if (!c) {
                Client new_client;
                strcpy(new_client.address, client_ip);
                new_client.last_req = 0;
                new_client.balance = INITIAL_BALANCE;
                clients.push_back(new_client);
                total_balance += INITIAL_BALANCE;
                new_data = true;
                pthread_cond_broadcast(&data_cond);
            }
            pthread_mutex_unlock(&data_mutex);

            packet.type = DESC_ACK;
            sendto(sock, &packet, sizeof(packet), 0, (struct sockaddr*)&client_addr, addr_len);
        } else if (packet.type == REQ) {
            ProcessArg* parg = new ProcessArg;
            parg->sock = sock;
            parg->packet = packet;
            parg->client_addr = client_addr;
            pthread_t tid;
            pthread_create(&tid, NULL, process_request, parg);
            pthread_detach(tid);
        }
    }
    delete t_arg;
    return nullptr;
}
// Função para a thread de interface, aguardando notificações de novas transações.
void* interface_thread(void* arg) {
    while (true) {
        pthread_mutex_lock(&data_mutex);
        while (!new_data) {
            pthread_cond_wait(&data_cond, &data_mutex);
        }
        new_data = false;
        pthread_mutex_unlock(&data_mutex);
    }
    return nullptr;
}
// Configura o socket e inicia descoberta e interface.
int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "Usage: ./servidor <port>" << std::endl;
        return 1;
    }
    int port = atoi(argv[1]);

    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    int opt = 1;
    setsockopt(sock, SOL_SOCKET, SO_BROADCAST, &opt, sizeof(opt));

    std::string timestamp = get_current_time();
    std::cout << timestamp << " num transactions 0 total transferred 0 total balance 0" << std::endl;

    ThreadArg* t_arg = new ThreadArg;
    t_arg->sock = sock;
    t_arg->port = port;

    pthread_t disc_thread;
    pthread_create(&disc_thread, NULL, discovery_thread, t_arg);

    pthread_t int_thread;
    pthread_create(&int_thread, NULL, interface_thread, nullptr);

    pthread_join(disc_thread, nullptr);
    close(sock);
    return 0;
}