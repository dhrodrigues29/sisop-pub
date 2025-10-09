// servidor.c (VERSÃO OTIMIZADA COM LOCK POR CLIENTE)
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

#include <stdint.h>
#include <stdarg.h>

#define BUF_SIZE 512

// --- Configuráveis ---
#define INITIAL_BALANCE 100  // N - saldo inicial atribuído a cada cliente na descoberta
// -----------------------

#define TYPE_DESC      1
#define TYPE_DESC_ACK  2
#define TYPE_REQ       3
#define TYPE_REQ_ACK   4

typedef struct client_entry {
    uint32_t ip; // host byte order
    uint32_t last_req;
    uint32_t balance;
    pthread_mutex_t lock; // mutex individual do cliente
    struct client_entry *next;
} client_entry_t;

// Globals for the server state
static client_entry_t *clients_head = NULL;

// Mutex para proteger a lista (estrutura) de clientes durante buscas/inserções.
// Após localizar ponteiros válidos para client_entry_t, liberamos essa mutex e passamos
// a usar os mutexes individuais dos clientes.
static pthread_mutex_t clients_list_mutex = PTHREAD_MUTEX_INITIALIZER;

// rwlock para coordenar leitor(es) de interface e writers ao atualizar totais / enfileirar mensagens.
// Mantém comportamento leitor/escritor para as estruturas de resumo / impressão.
static pthread_rwlock_t table_lock = PTHREAD_RWLOCK_INITIALIZER;

static uint64_t num_transactions = 0;
static uint64_t total_transferred = 0;
static int64_t total_balance = 0; // soma dos saldos de todos os clientes

// Message queue for the "interface reader" to print updates
typedef struct msg_node {
    char *msg;
    struct msg_node *next;
} msg_node_t;
static msg_node_t *msg_head = NULL, *msg_tail = NULL;
static pthread_mutex_t queue_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t queue_cond = PTHREAD_COND_INITIALIZER;

// utility: current timestamp string "YYYY-MM-DD HH:MM:SS"
static void timestamp_now(char *out, size_t n) {
    time_t t = time(NULL);
    struct tm tm;
    localtime_r(&t, &tm);
    strftime(out, n, "%Y-%m-%d %H:%M:%S", &tm);
}

static void enqueue_message(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    char buf[2048];
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    msg_node_t *node = malloc(sizeof(msg_node_t));
    if (!node) return;
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

// interface reader thread: waits for updates and prints them while holding read lock
static void *interface_reader_thread(void *arg) {
    (void)arg;
    for (;;) {
        pthread_mutex_lock(&queue_mutex);
        while (msg_head == NULL) {
            pthread_cond_wait(&queue_cond, &queue_mutex);
        }
        // pop all messages
        msg_node_t *cur = msg_head;
        msg_head = msg_tail = NULL;
        pthread_mutex_unlock(&queue_mutex);

        // Acquire read lock to respect original constraint "enquanto a tabela estiver sendo lida, nenhum escritor poderá modifica-la"
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

// helper: find client by IP (host order).
// Caller must hold clients_list_mutex while calling to ensure safe traversal.
// Returns pointer to client_entry_t (or NULL). Caller may copy pointer and then release clients_list_mutex,
// but must then lock the client's mutex before accessing/modifying its fields.
static client_entry_t *find_client_locked(uint32_t ip) {
    client_entry_t *cur = clients_head;
    while (cur) {
        if (cur->ip == ip) return cur;
        cur = cur->next;
    }
    return NULL;
}

// Discovery handler: add client if new, respond with DESC_ACK
static void handle_discovery(int sockfd, struct sockaddr_in *peer, socklen_t peerlen) {
    uint32_t ip = ntohl(peer->sin_addr.s_addr);
    int added = 0;
    client_entry_t *c = NULL;

    pthread_mutex_lock(&clients_list_mutex);
    c = find_client_locked(ip);
    if (!c) {
        c = malloc(sizeof(client_entry_t));
        if (!c) {
            pthread_mutex_unlock(&clients_list_mutex);
            return;
        }
        c->ip = ip;
        c->last_req = 0;
        c->balance = INITIAL_BALANCE;
        pthread_mutex_init(&c->lock, NULL);
        c->next = clients_head;
        clients_head = c;
        added = 1;
    }
    pthread_mutex_unlock(&clients_list_mutex);

    // If added, update total_balance under table write lock
    if (added) {
        pthread_rwlock_wrlock(&table_lock);
        total_balance += INITIAL_BALANCE;
        // create and enqueue message describing the join
        char tstamp[32];
        timestamp_now(tstamp, sizeof(tstamp));
        char ipstr[INET_ADDRSTRLEN];
        struct in_addr in;
        in.s_addr = htonl(ip);
        inet_ntop(AF_INET, &in, ipstr, sizeof(ipstr));
        enqueue_message("%s client %s joined last_req 0 balance %u num transactions %llu total transferred %llu total balance %lld",
                        tstamp, ipstr, (unsigned)c->balance,
                        (unsigned long long)num_transactions, (unsigned long long)total_transferred, (long long)total_balance);
        pthread_rwlock_unlock(&table_lock);
    } else {
        // enqueue discovery message (already registered) -- use table read to be consistent with prior behavior
        pthread_rwlock_wrlock(&table_lock);
        char tstamp[32];
        timestamp_now(tstamp, sizeof(tstamp));
        char ipstr[INET_ADDRSTRLEN];
        struct in_addr in;
        in.s_addr = htonl(ip);
        inet_ntop(AF_INET, &in, ipstr, sizeof(ipstr));
        enqueue_message("%s client %s discovery (already registered) last_req %u balance %u",
                        tstamp, ipstr, (unsigned)c->last_req, (unsigned)c->balance);
        pthread_rwlock_unlock(&table_lock);
    }

    // send DESC_ACK unicast (2 bytes type)
    char sbuf[8];
    uint16_t t = htons(TYPE_DESC_ACK);
    memcpy(sbuf, &t, 2);
    sendto(sockfd, sbuf, 2, 0, (struct sockaddr*)peer, peerlen);
}

// send REQ_ACK to peer: ack_seqn, new_balance
static void send_req_ack(int sockfd, struct sockaddr_in *peer, socklen_t peerlen, uint32_t ack_seqn, uint32_t new_balance) {
    char sbuf[12];
    uint16_t t = htons(TYPE_REQ_ACK);
    uint32_t s = htonl(ack_seqn);
    uint32_t nb = htonl(new_balance);
    memcpy(sbuf, &t, 2);
    memcpy(sbuf + 2, &s, 4);
    memcpy(sbuf + 6, &nb, 4);
    sendto(sockfd, sbuf, 10, 0, (struct sockaddr*)peer, peerlen);
}

// processing thread args
typedef struct {
    char buf[BUF_SIZE];
    ssize_t len;
    struct sockaddr_in peer;
    socklen_t peerlen;
    int sockfd;
} proc_arg_t;

static void *process_request_thread(void *arg) {
    proc_arg_t *pa = (proc_arg_t*)arg;

    // parse packet
    uint16_t type;
    memcpy(&type, pa->buf, 2);
    type = ntohs(type);
    if (type != TYPE_REQ) {
        free(pa);
        return NULL;
    }
    uint32_t seqn_net;
    memcpy(&seqn_net, pa->buf + 2, 4);
    uint32_t seqn = ntohl(seqn_net);

    uint32_t dest_net, value_net;
    memcpy(&dest_net, pa->buf + 6, 4);
    memcpy(&value_net, pa->buf + 10, 4);
    uint32_t dest_ip = ntohl(dest_net);
    uint32_t value = ntohl(value_net);

    uint32_t origin_ip = ntohl(pa->peer.sin_addr.s_addr);
    char origin_str[INET_ADDRSTRLEN], dest_str[INET_ADDRSTRLEN];
    struct in_addr in;
    in.s_addr = htonl(origin_ip);
    inet_ntop(AF_INET, &in, origin_str, sizeof(origin_str));
    in.s_addr = htonl(dest_ip);
    inet_ntop(AF_INET, &in, dest_str, sizeof(dest_str));

    // Locate origin and dest in the client list safely
    pthread_mutex_lock(&clients_list_mutex);
    client_entry_t *origin = find_client_locked(origin_ip);
    client_entry_t *dest = find_client_locked(dest_ip);
    // we copy pointers; after this we can unlock the list mutex
    pthread_mutex_unlock(&clients_list_mutex);

    if (!origin) {
        // unknown client -> reply with ack seqn 0 new_balance 0 (not registered)
        send_req_ack(pa->sockfd, &pa->peer, pa->peerlen, 0, 0);
        char tstamp[32];
        timestamp_now(tstamp, sizeof(tstamp));
        enqueue_message("%s client %s UNREGISTERED sent id req %u dest %s value %u - ignored (not registered)",
                        tstamp, origin_str, seqn, dest_str, value);
        free(pa);
        return NULL;
    }

    // If dest not registered, dest pointer is NULL - we handle that case later.

    // To update balances and last_req safely while allowing concurrency across different clients,
    // we lock the involved client mutexes in a consistent global order (by ip) to avoid deadlocks.
    client_entry_t *first_lock = NULL, *second_lock = NULL;
    if (dest && origin->ip != dest->ip) {
        if (origin->ip < dest->ip) {
            first_lock = origin; second_lock = dest;
        } else {
            first_lock = dest; second_lock = origin;
        }
        pthread_mutex_lock(&first_lock->lock);
        pthread_mutex_lock(&second_lock->lock);
    } else {
        // either dest == NULL, or origin == dest (self-transfer / balance check)
        pthread_mutex_lock(&origin->lock);
    }

    // Now safe to examine/modify origin and dest balances and last_req
    uint32_t expected = origin->last_req + 1;
    if (seqn == expected) {
        if (!dest) {
            // destination not registered -> mark as processed to avoid infinite retransmit
            origin->last_req = seqn;
            uint32_t curbal = origin->balance;
            // No totals update because transfer not applied.
            // Send ack with current balance
            send_req_ack(pa->sockfd, &pa->peer, pa->peerlen, seqn, curbal);

            // Build and enqueue message: must update shared counters? No. Only print message.
            // For consistent printing relative to totals, acquire table write lock while enqueueing.
            pthread_rwlock_wrlock(&table_lock);
            char tstamp[32];
            timestamp_now(tstamp, sizeof(tstamp));
            enqueue_message("%s client %s id req %u dest %s value %u"
                            " DEST_NOT_REGISTERED - not processed num transactions %llu total transferred %llu total balance %lld",
                            tstamp, origin_str, seqn, dest_str, value,
                            (unsigned long long)num_transactions, (unsigned long long)total_transferred, (long long)total_balance);
            pthread_rwlock_unlock(&table_lock);
            // unlock clients
            if (second_lock) { pthread_mutex_unlock(&second_lock->lock); pthread_mutex_unlock(&first_lock->lock); }
            else pthread_mutex_unlock(&origin->lock);
            free(pa);
            return NULL;
        }

        if (value == 0) {
            // balance check - mark processed; do not change balances
            origin->last_req = seqn;
            uint32_t curbal = origin->balance;
            send_req_ack(pa->sockfd, &pa->peer, pa->peerlen, seqn, curbal);

            pthread_rwlock_wrlock(&table_lock);
            char tstamp[32];
            timestamp_now(tstamp, sizeof(tstamp));
            enqueue_message("%s client %s id req %u dest %s value %u (balance check) num transactions %llu total transferred %llu total balance %lld",
                            tstamp, origin_str, seqn, dest_str, value,
                            (unsigned long long)num_transactions, (unsigned long long)total_transferred, (long long)total_balance);
            pthread_rwlock_unlock(&table_lock);

            if (second_lock) { pthread_mutex_unlock(&second_lock->lock); pthread_mutex_unlock(&first_lock->lock); }
            else pthread_mutex_unlock(&origin->lock);
            free(pa);
            return NULL;
        }

        if (origin->balance < value) {
            // insufficient funds: mark as processed but do not move money
            origin->last_req = seqn;
            uint32_t newbal = origin->balance;
            send_req_ack(pa->sockfd, &pa->peer, pa->peerlen, seqn, newbal);

            pthread_rwlock_wrlock(&table_lock);
            char tstamp[32];
            timestamp_now(tstamp, sizeof(tstamp));
            enqueue_message("%s client %s id req %u dest %s value %u"
                            " INSUFFICIENT_FUNDS new_balance %u num transactions %llu total transferred %llu total balance %lld",
                            tstamp, origin_str, seqn, dest_str, value,
                            (unsigned)newbal,
                            (unsigned long long)num_transactions, (unsigned long long)total_transferred, (long long)total_balance);
            pthread_rwlock_unlock(&table_lock);

            if (second_lock) { pthread_mutex_unlock(&second_lock->lock); pthread_mutex_unlock(&first_lock->lock); }
            else pthread_mutex_unlock(&origin->lock);
            free(pa);
            return NULL;
        }

        // All checks passed: perform transfer
        origin->balance -= value;
        dest->balance += value;
        origin->last_req = seqn;
        // Update totals: num_transactions and total_transferred (and optionally total_balance,
        // but internal transfer doesn't change total_balance)
        // Acquire table write lock to update totals and enqueue message atomically with respect to reader
        pthread_rwlock_wrlock(&table_lock);
        num_transactions++;
        total_transferred += value;
        // total_balance unchanged for internal transfers

        uint32_t newbal = origin->balance;
        send_req_ack(pa->sockfd, &pa->peer, pa->peerlen, seqn, newbal);

        char tstamp[32];
        timestamp_now(tstamp, sizeof(tstamp));
        enqueue_message("%s client %s id req %u dest %s value %u\nnum transactions %llu\ntotal transferred %llu total balance %lld",
                        tstamp, origin_str, seqn, dest_str, value,
                        (unsigned long long)num_transactions, (unsigned long long)total_transferred, (long long)total_balance);

        pthread_rwlock_unlock(&table_lock);

        if (second_lock) { pthread_mutex_unlock(&second_lock->lock); pthread_mutex_unlock(&first_lock->lock); }
        else pthread_mutex_unlock(&origin->lock);
        free(pa);
        return NULL;
    } else if (seqn <= origin->last_req) {
        // duplicate retransmission: do not reprocess; resend ack with last_req
        uint32_t last = origin->last_req;
        uint32_t curbal = origin->balance;
        send_req_ack(pa->sockfd, &pa->peer, pa->peerlen, last, curbal);

        // Print message with DUP!!
        pthread_rwlock_wrlock(&table_lock);
        char tstamp[32];
        timestamp_now(tstamp, sizeof(tstamp));
        enqueue_message("%s client %s DUP!! id req %u dest %s value %u num transactions %llu total transferred %llu total balance %lld",
                        tstamp, origin_str, seqn, dest_str, value,
                        (unsigned long long)num_transactions, (unsigned long long)total_transferred, (long long)total_balance);
        pthread_rwlock_unlock(&table_lock);

        if (second_lock) { pthread_mutex_unlock(&second_lock->lock); pthread_mutex_unlock(&first_lock->lock); }
        else pthread_mutex_unlock(&origin->lock);
        free(pa);
        return NULL;
    } else {
        // seqn > expected -> missing prior request(s): inform client via ack with last processed (origin->last_req)
        uint32_t last = origin->last_req;
        uint32_t curbal = origin->balance;
        send_req_ack(pa->sockfd, &pa->peer, pa->peerlen, last, curbal);

        pthread_rwlock_wrlock(&table_lock);
        char tstamp[32];
        timestamp_now(tstamp, sizeof(tstamp));
        enqueue_message("%s client %s OUT_OF_ORDER id req %u (expected %u) dest %s value %u -> acking last %u",
                        tstamp, origin_str, seqn, expected, dest_str, value, last);
        pthread_rwlock_unlock(&table_lock);

        if (second_lock) { pthread_mutex_unlock(&second_lock->lock); pthread_mutex_unlock(&first_lock->lock); }
        else pthread_mutex_unlock(&origin->lock);
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

    // start interface reader thread
    pthread_t reader_tid;
    pthread_create(&reader_tid, NULL, interface_reader_thread, NULL);
    pthread_detach(reader_tid);

    // Print initial server state as required (timestamp ... zeros)
    char tstamp[32];
    timestamp_now(tstamp, sizeof(tstamp));
    printf("%s num transactions %llu total transferred %llu total balance %lld\n",
           tstamp, (unsigned long long)num_transactions, (unsigned long long)total_transferred, (long long)total_balance);
    fflush(stdout);

    // main loop: receive all packets, dispatch discovery or spawn processing threads
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
        uint16_t type;
        memcpy(&type, buf, 2);
        type = ntohs(type);

        if (type == TYPE_DESC) {
            handle_discovery(sockfd, &peer, peerlen);
        } else if (type == TYPE_REQ) {
            // spawn new thread to process this request
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
        } else {
            // unknown packet type - ignore
            continue;
        }
    }

    close(sockfd);
    return 0;
}
