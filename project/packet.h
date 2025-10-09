// Arquivo de cabeçalho definindo constantes e estruturas para comunicação UDP.
// Inclui valores iniciais e tipos de pacotes.
#ifndef PACKET_H
#define PACKET_H

#include <cstdint>

#define INITIAL_BALANCE 100             // Saldo inicial de cada cliente em reais.
#define BUFFER_SIZE 1024                // Tamanho máximo do buffer para pacotes.
#define TIMEOUT_MS 10                   // Tempo de espera em milissegundos para ACK.
#define DESC 1                          // Tipo de pacote para descoberta.
#define REQ 2                           // Tipo de pacote para requisição.
#define DESC_ACK 3                      // Tipo de pacote para confirmação de descoberta.
#define REQ_ACK 4                       // Tipo de pacote para confirmação de requisição.

struct Requisicao {
    uint32_t dest_addr; // IP destino em network byte order
    uint32_t value;     // Valor
};

struct RequisicaoAck {
    uint32_t seqn;       // Seq do ack
    uint32_t new_balance; // Novo saldo
};

struct Packet {
    uint16_t type; // Tipo
    uint32_t seqn; // Seq da req
    union {
        Requisicao req;
        RequisicaoAck ack;
    };
};

#endif