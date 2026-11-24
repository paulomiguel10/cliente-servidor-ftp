/*
 * protocolo.c — Envio e recebimento de pacotes sobre TCP (Windows / Winsock2).
 *
 * send()/recv() podem enviar/receber MENOS bytes do que o pedido,
 * por isso as funções enviar_tudo()/receber_tudo() repetem até completar.
 */
#include "protocolo.h"

#include <stdio.h>
#include <string.h>

#define TAM_CABECALHO 9   /* seq(4) + cmd(1) + len(4) */

int iniciar_winsock(void)
{
    WSADATA wsa;
    int r = WSAStartup(MAKEWORD(2, 2), &wsa);
    if (r != 0) {
        printf("WSAStartup falhou (erro %d)\n", r);
        return -1;
    }
    return 0;
}

static int enviar_tudo(SOCKET fd, const void *buf, size_t n)
{
    const char *p = (const char *)buf;
    while (n > 0) {
        int r = send(fd, p, (int)n, 0);
        if (r == SOCKET_ERROR) {
            if (WSAGetLastError() == WSAEINTR) continue;
            return -1;
        }
        p += r;
        n -= (size_t)r;
    }
    return 0;
}

/* Retorna 1 = ok, 0 = conexão fechada, -1 = erro */
static int receber_tudo(SOCKET fd, void *buf, size_t n)
{
    char *p = (char *)buf;
    while (n > 0) {
        int r = recv(fd, p, (int)n, 0);
        if (r == 0) return 0;
        if (r == SOCKET_ERROR) {
            if (WSAGetLastError() == WSAEINTR) continue;
            return -1;
        }
        p += r;
        n -= (size_t)r;
    }
    return 1;
}

int enviar_pacote(SOCKET fd, uint32_t seq, uint8_t cmd, const void *payload, uint32_t len)
{
    if (len > MAX_PAYLOAD) return -1;

    unsigned char cab[TAM_CABECALHO];
    uint32_t seq_n = htonl(seq);
    uint32_t len_n = htonl(len);
    memcpy(cab, &seq_n, 4);
    cab[4] = cmd;
    memcpy(cab + 5, &len_n, 4);

    if (enviar_tudo(fd, cab, TAM_CABECALHO) < 0) return -1;
    if (len > 0 && enviar_tudo(fd, payload, len) < 0) return -1;
    return 0;
}

int enviar_texto(SOCKET fd, uint32_t seq, uint8_t cmd, const char *texto)
{
    if (!texto) texto = "";
    size_t n = strlen(texto);
    if (n > MAX_PAYLOAD) n = MAX_PAYLOAD;
    return enviar_pacote(fd, seq, cmd, texto, (uint32_t)n);
}

int receber_pacote(SOCKET fd, Pacote *p)
{
    unsigned char cab[TAM_CABECALHO];
    int r = receber_tudo(fd, cab, TAM_CABECALHO);
    if (r <= 0) return r;

    uint32_t seq_n, len_n;
    memcpy(&seq_n, cab, 4);
    memcpy(&len_n, cab + 5, 4);
    p->seq = ntohl(seq_n);
    p->cmd = cab[4];
    p->len = ntohl(len_n);

    if (p->len > MAX_PAYLOAD) return -1;   /* pacote inválido */

    if (p->len > 0) {
        r = receber_tudo(fd, p->payload, p->len);
        if (r <= 0) return r;
    }
    p->payload[p->len] = '\0';
    return 1;
}

const char *nome_cmd(uint8_t cmd)
{
    switch (cmd) {
    case CMD_LOGIN:  return "LOGIN";
    case CMD_PORTA:  return "PORTA";
    case CMD_QUIT:   return "QUIT";
    case CMD_GET:    return "GET";
    case CMD_PUT:    return "PUT";
    case CMD_BIN:    return "BIN";
    case CMD_ASCII:  return "ASCII";
    case CMD_LS:     return "LS";
    case CMD_DELETE: return "DELETE";
    case CMD_ECHO:   return "ECHO";
    case RESP_OK:    return "OK";
    case RESP_ERRO:  return "ERRO";
    case RESP_DADOS: return "DADOS";
    case RESP_FIM:   return "FIM";
    default:         return "?";
    }
}
