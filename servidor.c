// servidor.c
// Compilar: gcc -O2 -std=c11 servidor.c -o servidor -lpthread

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>

#define BUF_SIZE 512

// --- Configuráveis ---
#define INITIAL_BALANCE 100  // saldo inicial atribuído a cada cliente
// -----------------------

#define TYPE_DESC      1
#define TYPE_DESC_ACK  2
#define TYPE_REQ       3
#define TYPE_REQ_ACK   4

typedef enum {
    ACK_OK = 0,
    ACK_FAILED_INSUF_FUNDS = 1,
    ACK_FAILED_DEST_NOT_REG = 2
} ack_status_t;

typedef struct client_entry {
    uint32_t ip; // host byte order
    uint32_t last_req;
    uint32_t balance;
    struct client_entry *next;
} client_entry_t;

// Globals for server state
static client_entry_t *clients_head = NULL;
static pthread_rwlock_t table_lock = PTHREAD_RWLOCK_INITIALIZER;
static pthread_mutex_t queue_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t queue_cond = PTHREAD_COND_INITIALIZER;

static uint64_t num_transactions = 0;
static uint64_t total_transferred = 0;
static int64_t total_balance = 0;

// Message queue for interface reader
typedef struct msg_node {
    char *msg;
    struct msg_node *next;
} msg_node_t;
static msg_node_t *msg_head = NULL, *msg_tail = NULL;

// Utility: current timestamp
static void timestamp_now(char *out, size_t n) {
    time_t t = time(NULL);
    struct tm tm;
    localtime_r(&t, &tm);
    strftime(out, n, "%Y-%m-%d %H:%M:%S", &tm);
}

// Enqueue message for interface reader
static void enqueue_message(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    char buf[1024];
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    msg_node_t *node = malloc(sizeof(msg_node_t));
    node->msg = strdup(buf);
    node->next = NULL;

    pthread_mutex_lock(&queue_mutex);
    if (msg_tail) {
        msg_tail->next = node;
        msg_tail = node;
    } else {
        msg_head = msg_tail = node;
    }
    pthread_cond_signal(&queue_cond);
    pthread_mutex_unlock(&queue_mutex);
}

// Interface reader thread
static void *interface_reader_thread(void *arg) {
    (void)arg;
    for (;;) {
        pthread_mutex_lock(&queue_mutex);
        while (msg_head == NULL) {
            pthread_cond_wait(&queue_cond, &queue_mutex);
        }
        msg_node_t *cur = msg_head;
        msg_head = msg_tail = NULL;
        pthread_mutex_unlock(&queue_mutex);

        pthread_rwlock_rdlock(&table_lock);
        while (cur) {
            printf("%s\n", cur->msg);
            free(cur->msg);
            msg_node_t *tmp = cur;
            cur = cur->next;
            free(tmp);
        }
        fflush(stdout);
        pthread_rwlock_unlock(&table_lock);
    }
    return NULL;
}

// Find client by IP (host order)
static client_entry_t *find_client(uint32_t ip) {
    client_entry_t *cur = clients_head;
    while (cur) {
        if (cur->ip == ip) return cur;
        cur = cur->next;
    }
    return NULL;
}

// Handle discovery: add new client
static void handle_discovery(int sockfd, struct sockaddr_in *peer, socklen_t peerlen) {
    uint32_t ip = ntohl(peer->sin_addr.s_addr);
    int added = 0;

    pthread_rwlock_wrlock(&table_lock);
    client_entry_t *c = find_client(ip);
    if (!c) {
        c = malloc(sizeof(client_entry_t));
        c->ip = ip;
        c->last_req = 0;
        c->balance = INITIAL_BALANCE;
        c->next = clients_head;
        clients_head = c;
        total_balance += INITIAL_BALANCE;
        added = 1;
    }

    char sbuf[8];
    uint16_t t = htons(TYPE_DESC_ACK);
    memcpy(sbuf, &t, 2);

    char tstamp[32], ipstr[INET_ADDRSTRLEN];
    timestamp_now(tstamp, sizeof(tstamp));
    inet_ntop(AF_INET, &(peer->sin_addr), ipstr, sizeof(ipstr));

    if (added) {
        enqueue_message("%s client %s joined last_req 0 balance %u num transactions %llu total transferred %llu total balance %lld",
                        tstamp, ipstr, (unsigned)c->balance,
                        (unsigned long long)num_transactions, (unsigned long long)total_transferred, (long long)total_balance);
    } else {
        enqueue_message("%s client %s discovery (already registered) last_req %u balance %u",
                        tstamp, ipstr, (unsigned)c->last_req, (unsigned)c->balance);
    }

    pthread_rwlock_unlock(&table_lock);
    sendto(sockfd, sbuf, 2, 0, (struct sockaddr*)peer, peerlen);
}

// Send REQ_ACK
static void send_req_ack(int sockfd, struct sockaddr_in *peer, socklen_t peerlen,
                         uint32_t seqn, uint32_t balance, ack_status_t status) {
    char buf[11];
    uint16_t type_n = htons(TYPE_REQ_ACK);
    uint32_t seqn_n = htonl(seqn);
    uint32_t bal_n = htonl(balance);

    memcpy(buf, &type_n, 2);
    memcpy(buf + 2, &seqn_n, 4);
    memcpy(buf + 6, &bal_n, 4);
    buf[10] = (uint8_t)status;

    if (sendto(sockfd, buf, sizeof(buf), 0, (struct sockaddr*)peer, peerlen) < 0) {
        perror("sendto req_ack");
    }
}

// Processing thread arguments
typedef struct {
    char buf[BUF_SIZE];
    ssize_t len;
    struct sockaddr_in peer;
    socklen_t peerlen;
    int sockfd;
} proc_arg_t;

// Process TYPE_REQ packets
static void *process_request_thread(void *arg) {
    proc_arg_t *pa = (proc_arg_t*)arg;

    uint16_t type;
    memcpy(&type, pa->buf, 2);
    type = ntohs(type);
    if (type != TYPE_REQ) {
        free(pa);
        return NULL;
    }

    uint32_t seqn = ntohl(*(uint32_t*)(pa->buf + 2));
    uint32_t dest_ip = ntohl(*(uint32_t*)(pa->buf + 6));
    uint32_t value   = ntohl(*(uint32_t*)(pa->buf + 10));

    uint32_t origin_ip = ntohl(pa->peer.sin_addr.s_addr);
    char origin_str[INET_ADDRSTRLEN], dest_str[INET_ADDRSTRLEN];
    struct in_addr in;
    in.s_addr = htonl(origin_ip);
    inet_ntop(AF_INET, &in, origin_str, sizeof(origin_str));
    in.s_addr = htonl(dest_ip);
    inet_ntop(AF_INET, &in, dest_str, sizeof(dest_str));

    pthread_rwlock_wrlock(&table_lock);
    client_entry_t *origin = find_client(origin_ip);
    if (!origin) {
        send_req_ack(pa->sockfd, &pa->peer, pa->peerlen, 0, 0, ACK_FAILED_DEST_NOT_REG);
        char tstamp[32];
        timestamp_now(tstamp, sizeof(tstamp));
        enqueue_message("%s client %s UNREGISTERED sent id req %u dest %s value %u",
                        tstamp, origin_str, seqn, dest_str, value);
        pthread_rwlock_unlock(&table_lock);
        free(pa);
        return NULL;
    }

    uint32_t expected = origin->last_req + 1;
    if (seqn == expected) {
        client_entry_t *dest = find_client(dest_ip);
        if (!dest) {
            origin->last_req = seqn;
            send_req_ack(pa->sockfd, &pa->peer, pa->peerlen, seqn, origin->balance, ACK_FAILED_DEST_NOT_REG);
            char tstamp[32];
            timestamp_now(tstamp, sizeof(tstamp));
            enqueue_message("%s client %s id req %u dest %s value %u DEST_NOT_REGISTERED\nnum transactions %llu total transferred %llu total balance %lld",
                            tstamp, origin_str, seqn, dest_str, value,
                            (unsigned long long)num_transactions,
                            (unsigned long long)total_transferred,
                            (long long)total_balance);
            pthread_rwlock_unlock(&table_lock);
            free(pa);
            return NULL;
        }
        if (value == 0) {
            origin->last_req = seqn;
            send_req_ack(pa->sockfd, &pa->peer, pa->peerlen, seqn, origin->balance, ACK_OK);
            char tstamp[32];
            timestamp_now(tstamp, sizeof(tstamp));
            enqueue_message("%s client %s id req %u dest %s value %u (balance check)\nnum transactions %llu total transferred %llu total balance %lld",
                            tstamp, origin_str, seqn, dest_str, value,
                            (unsigned long long)num_transactions,
                            (unsigned long long)total_transferred,
                            (long long)total_balance);
            pthread_rwlock_unlock(&table_lock);
            free(pa);
            return NULL;
        }
        if (origin->balance < value) {
            origin->last_req = seqn;
            send_req_ack(pa->sockfd, &pa->peer, pa->peerlen, seqn, origin->balance, ACK_FAILED_INSUF_FUNDS);
            char tstamp[32];
            timestamp_now(tstamp, sizeof(tstamp));
            enqueue_message("%s client %s id req %u dest %s value %u FAILED: Saldo Insuficiente \nnum transactions %llu \ntotal transferred %llu total balance %lld",
                            tstamp, origin_str, seqn, dest_str, value,
                            (unsigned long long)num_transactions,
                            (unsigned long long)total_transferred,
                            (long long)total_balance);
            pthread_rwlock_unlock(&table_lock);
            free(pa);
            return NULL;
        }

        // Perform transfer
        origin->balance -= value;
        dest->balance += value;
        origin->last_req = seqn;
        num_transactions++;
        total_transferred += value;

        send_req_ack(pa->sockfd, &pa->peer, pa->peerlen, seqn, origin->balance, ACK_OK);
        char tstamp[32];
        timestamp_now(tstamp, sizeof(tstamp));
        enqueue_message("%s client %s id req %u dest %s value %u\nnum transactions %llu\ntotal transferred %llu total balance %lld",
                        tstamp, origin_str, seqn, dest_str, value,
                        (unsigned long long)num_transactions,
                        (unsigned long long)total_transferred,
                        (long long)total_balance);
        pthread_rwlock_unlock(&table_lock);
        free(pa);
        return NULL;
    } else if (seqn <= origin->last_req) {
        send_req_ack(pa->sockfd, &pa->peer, pa->peerlen, origin->last_req, origin->balance, ACK_OK);
        char tstamp[32];
        timestamp_now(tstamp, sizeof(tstamp));
        enqueue_message("%s client %s DUP!! id req %u dest %s value %u\nnum transactions %llu total transferred %llu total balance %lld",
                        tstamp, origin_str, seqn, dest_str, value,
                        (unsigned long long)num_transactions,
                        (unsigned long long)total_transferred,
                        (long long)total_balance);
        pthread_rwlock_unlock(&table_lock);
        free(pa);
        return NULL;
    } else {
        send_req_ack(pa->sockfd, &pa->peer, pa->peerlen, origin->last_req, origin->balance, ACK_OK);
        char tstamp[32];
        timestamp_now(tstamp, sizeof(tstamp));
        enqueue_message("%s client %s OUT_OF_ORDER id req %u (expected %u) dest %s value %u\nnum transactions %llu total transferred %llu total balance %lld",
                        tstamp, origin_str, seqn, expected, dest_str, value,
                        (unsigned long long)num_transactions,
                        (unsigned long long)total_transferred,
                        (long long)total_balance);
        pthread_rwlock_unlock(&table_lock);
        free(pa);
        return NULL;
    }
}

int main(int argc, char *argv[]) {
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <udp_port>\n", argv[0]);
        exit(EXIT_FAILURE);
    }
    int port = atoi(argv[1]);

    int sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd < 0) { perror("socket"); exit(EXIT_FAILURE); }

    struct sockaddr_in servaddr;
    memset(&servaddr, 0, sizeof(servaddr));
    servaddr.sin_family = AF_INET;
    servaddr.sin_addr.s_addr = INADDR_ANY;
    servaddr.sin_port = htons(port);

    if (bind(sockfd, (struct sockaddr*)&servaddr, sizeof(servaddr)) < 0) {
        perror("bind");
        close(sockfd);
        exit(EXIT_FAILURE);
    }

    pthread_t reader_tid;
    pthread_create(&reader_tid, NULL, interface_reader_thread, NULL);
    pthread_detach(reader_tid);

    char tstamp[32];
    timestamp_now(tstamp, sizeof(tstamp));
    printf("%s num transactions %llu total transferred %llu total balance %lld\n",
           tstamp, (unsigned long long)num_transactions,
           (unsigned long long)total_transferred, (long long)total_balance);
    fflush(stdout);

    while (1) {
        struct sockaddr_in peer;
        socklen_t peerlen = sizeof(peer);
        char buf[BUF_SIZE];
        ssize_t len = recvfrom(sockfd, buf, sizeof(buf), 0, (struct sockaddr*)&peer, &peerlen);
        if (len < 0) {
            if (errno == EINTR) continue;
            perror("recvfrom");
            continue;
        }
        if (len < 2) continue;

        uint16_t type = ntohs(*(uint16_t*)buf);

        if (type == TYPE_DESC) {
            handle_discovery(sockfd, &peer, peerlen);
        } else if (type == TYPE_REQ) {
            proc_arg_t *pa = malloc(sizeof(proc_arg_t));
            if (!pa) continue;
            memcpy(pa->buf, buf, len);
            pa->len = len;
            pa->peer = peer;
            pa->peerlen = peerlen;
            pa->sockfd = sockfd;
            pthread_t th;
            pthread_create(&th, NULL, process_request_thread, pa);
            pthread_detach(th);
        }
    }

    close(sockfd);
    return 0;
}
