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
#include <signal.h>
#include <sys/select.h>
#include <stdarg.h>

#define BUF_SIZE 512
#define TYPE_DESC 1
#define TYPE_DESC_ACK 2
#define TYPE_REQ 3
#define TYPE_REQ_ACK 4

// Constante para o número de transações automáticas
#define T 10

// Tabela de IPs de destino disponíveis (exemplo)
static const char *AVAILABLE_IPS[] = {
    "10.67.104.20"
};
#define NUM_IPS (sizeof(AVAILABLE_IPS) / sizeof(AVAILABLE_IPS[0]))

typedef enum {
    ACK_OK = 0,
    ACK_FAILED_INSUF_FUNDS = 1,
    ACK_FAILED_DEST_NOT_REG = 2
} ack_status_t;

static volatile int running = 1;

static void handle_sigint(int signo) {
    (void)signo;
    running = 0;
}

// timestamp
static void timestamp_now(char *out, size_t n) {
    time_t t = time(NULL);
    struct tm tm;
    localtime_r(&t, &tm);
    strftime(out, n, "%Y-%m-%d %H:%M:%S", &tm);
}

// print queue
typedef struct msg_node {
    char *msg;
    struct msg_node *next;
} msg_node_t;

static msg_node_t *msg_head = NULL, *msg_tail = NULL;
static pthread_mutex_t queue_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t queue_cond = PTHREAD_COND_INITIALIZER;

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

// printer thread: prints messages enqueued
static void *printer_thread(void *arg) {
    (void)arg;
    while (running) {
        pthread_mutex_lock(&queue_mutex);
        while (msg_head == NULL && running) {
            pthread_cond_wait(&queue_cond, &queue_mutex);
        }
        msg_node_t *cur = msg_head;
        msg_head = msg_tail = NULL;
        pthread_mutex_unlock(&queue_mutex);

        while (cur) {
            printf("%s\n", cur->msg);
            free(cur->msg);
            msg_node_t *tmp = cur;
            cur = cur->next;
            free(tmp);
        }
        fflush(stdout);
    }
    return NULL;
}

int main(int argc, char *argv[]) {
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <udp_port>\n", argv[0]);
        exit(EXIT_FAILURE);
    }
    int port = atoi(argv[1]);

    // Inicializa o gerador de números aleatórios
    srand(time(NULL));

    signal(SIGINT, handle_sigint);

    int sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd < 0) { perror("socket"); exit(EXIT_FAILURE); }

    // bind to any port (0) so we can receive replies
    struct sockaddr_in local;
    memset(&local, 0, sizeof(local));
    local.sin_family = AF_INET;
    local.sin_addr.s_addr = INADDR_ANY;
    local.sin_port = 0;
    if (bind(sockfd, (struct sockaddr*)&local, sizeof(local)) < 0) {
        perror("bind");
        close(sockfd);
        exit(EXIT_FAILURE);
    }

    // enable broadcast for discovery
    int yes = 1;
    if (setsockopt(sockfd, SOL_SOCKET, SO_BROADCAST, &yes, sizeof(yes)) < 0) {
        perror("setsockopt SO_BROADCAST");
    }

    // build broadcast address
    struct sockaddr_in baddr;
    memset(&baddr, 0, sizeof(baddr));
    baddr.sin_family = AF_INET;
    baddr.sin_port = htons(port);
    inet_pton(AF_INET, "255.255.255.255", &baddr.sin_addr);

    // send discovery packet
    char dbuf[8];
    uint16_t t = htons(TYPE_DESC);
    memcpy(dbuf, &t, 2);
    sendto(sockfd, dbuf, 2, 0, (struct sockaddr*)&baddr, sizeof(baddr));

    // wait for server response (blocking)
    struct sockaddr_in serveraddr;
    socklen_t serverlen = sizeof(serveraddr);
    char rbuf[BUF_SIZE];
    ssize_t rlen;

    while (1) {
        rlen = recvfrom(sockfd, rbuf, sizeof(rbuf), 0, (struct sockaddr*)&serveraddr, &serverlen);
        if (rlen < 0) {
            if (errno == EINTR) { if (!running) break; continue; }
            perror("recvfrom");
            continue;
        }
        if (rlen >= 2) {
            uint16_t rtype;
            memcpy(&rtype, rbuf, 2);
            rtype = ntohs(rtype);
            if (rtype == TYPE_DESC_ACK) {
                // discovered server
                char tstamp[64];
                timestamp_now(tstamp, sizeof(tstamp));
                char saddr_str[INET_ADDRSTRLEN];
                inet_ntop(AF_INET, &serveraddr.sin_addr, saddr_str, sizeof(saddr_str));
                printf("%s server addr %s\n", tstamp, saddr_str);
                fflush(stdout);
                break;
            }
        }
    }

    // start printer thread
    pthread_t pth;
    pthread_create(&pth, NULL, printer_thread, NULL);
    pthread_detach(pth);

    // =========================================================================
    // INÍCIO DA MODIFICAÇÃO: Aguardar comando "start"
    // =========================================================================
    char line[256];
    printf("\nServer encontrado. Digite 'start' para iniciar as %d transacoes: ", T);
    fflush(stdout);
    
    // Loop para ler do stdin até receber o comando "start" ou o programa ser encerrado
    while (running) {
        if (fgets(line, sizeof(line), stdin) == NULL) {
            // EOF ou erro no stdin
            break; 
        }
        // Remove a quebra de linha
        size_t len = strlen(line);
        if (len > 0 && line[len-1] == '\n') {
            line[len-1] = '\0';
        }

        if (strcmp(line, "start") == 0) {
            printf("Comando 'start' recebido. Iniciando as transacoes...\n\n");
            fflush(stdout);
            break; // Sai do loop para iniciar as transações
        } else {
            printf("Comando invalido. Digite 'start': ");
            fflush(stdout);
        }
    }

    if (!running) {
        // Se saiu do loop porque o programa foi interrompido (ex: Ctrl+C)
        goto cleanup;
    }
    // =========================================================================
    // FIM DA MODIFICAÇÃO
    // =========================================================================

    // reading loop: **AUTOMATICALLY** generate T transactions
    uint32_t seqn = 1;
    int transaction_count = 0;

    // Estrutura para o atraso de 50ms
    struct timespec delay = {0, 50 * 1000000}; // 50ms

    while (running && transaction_count < T) {
        // 1. Escolhe um IP de destino aleatório da tabela
        const char *dest_str = AVAILABLE_IPS[rand() % NUM_IPS];
        
        // 2. Gera um valor aleatório de 1 a 30
        uint32_t value = (rand() % 30) + 1;

        struct in_addr dest_in;
        if (inet_pton(AF_INET, dest_str, &dest_in) != 1) {
            // Se o IP for inválido, apenas pula esta iteração e tenta a próxima.
            transaction_count++;
            continue;
        }
        uint32_t dest_ip = ntohl(dest_in.s_addr);

        // prepare request buffer: type(2) seqn(4) dest(4) value(4)
        char sbuf[16];
        uint16_t typ_n = htons(TYPE_REQ);
        uint32_t seqn_n = htonl(seqn);
        uint32_t dest_n = htonl(dest_ip);
        uint32_t val_n = htonl(value);
        memcpy(sbuf, &typ_n, 2);
        memcpy(sbuf + 2, &seqn_n, 4);
        memcpy(sbuf + 6, &dest_n, 4);
        memcpy(sbuf + 10, &val_n, 4);
        ssize_t slen = 14;

        // Log da transação gerada
        enqueue_message("Gerando transacao %d/%d: dest %s value %u", 
                        transaction_count + 1, T, dest_str, value);

        // send and wait for ack (retransmit on timeout). Timeout = 10 ms
        int acknowledged = 0;
        while (running && !acknowledged) {
            ssize_t sent = sendto(sockfd, sbuf, slen, 0, (struct sockaddr*)&serveraddr, sizeof(serveraddr));
            if (sent < 0) perror("sendto");

            // wait for response with select timeout 10ms
            fd_set rfds;
            FD_ZERO(&rfds);
            FD_SET(sockfd, &rfds);
            struct timeval tv;
            tv.tv_sec = 0;
            tv.tv_usec = 10000; // 10 ms
            int rv = select(sockfd + 1, &rfds, NULL, NULL, &tv);

            if (rv > 0 && FD_ISSET(sockfd, &rfds)) {
                struct sockaddr_in peer;
                socklen_t plen = sizeof(peer);
                ssize_t r = recvfrom(sockfd, rbuf, sizeof(rbuf), 0, (struct sockaddr*)&peer, &plen);
                if (r >= 2) {
                    uint16_t rtype;
                    memcpy(&rtype, rbuf, 2);
                    rtype = ntohs(rtype);

                    if (rtype == TYPE_REQ_ACK) {
                        uint32_t ack_seqn_n, newbal_n;
                        uint8_t status_byte = 0;
                        if (r >= 11) {
                            memcpy(&ack_seqn_n, rbuf + 2, 4);
                            memcpy(&newbal_n, rbuf + 6, 4);
                            status_byte = (uint8_t)rbuf[10];

                            uint32_t ack_seqn = ntohl(ack_seqn_n);
                            uint32_t newbal = ntohl(newbal_n);
                            ack_status_t status = (ack_status_t)status_byte;

                            char tstamp[64];
                            timestamp_now(tstamp, sizeof(tstamp));
                            char saddr_str[INET_ADDRSTRLEN];
                            inet_ntop(AF_INET, &serveraddr.sin_addr, saddr_str, sizeof(saddr_str));

                            if (ack_seqn == seqn) {
                                const char *status_str = "ACK_OK";
                                if (status == ACK_FAILED_INSUF_FUNDS) status_str = "FAILED: saldo insuficiente";
                                else if (status == ACK_FAILED_DEST_NOT_REG) status_str = "FAILED: destino nao registrado";
                                
                                enqueue_message("%s server %s id req %u dest %s value %u %s new balance %u",
                                                tstamp, saddr_str, seqn, dest_str, value, status_str, newbal);
                                seqn++;
                                acknowledged = 1;
                                break;
                            } else if (ack_seqn < seqn) {
                                enqueue_message("%s server ack last %u (we sent %u) -> resending",
                                                tstamp, ack_seqn, seqn);
                            } else {
                                enqueue_message("%s server ack unexpected %u (we sent %u) -> advancing",
                                                tstamp, ack_seqn, seqn);
                                seqn = ack_seqn + 1;
                                acknowledged = 1;
                                break;
                            }
                        }
                    }
                }
            }
        } // end resend loop

        // Incrementa o contador de transações realizadas
        if (acknowledged) {
            transaction_count++;
            // 3. Espera no mínimo 50ms antes da próxima transação
            nanosleep(&delay, NULL);
        }
    } // end automatic transaction loop

    cleanup: // Rótulo para o goto em caso de interrupção
    // cleanup
    running = 0;
    pthread_cond_signal(&queue_cond);
    close(sockfd);

    struct timespec ts;
    ts.tv_sec = 0;
    ts.tv_nsec = 50 * 1000 * 1000; // 50ms
    nanosleep(&ts, NULL);

    return 0;
}