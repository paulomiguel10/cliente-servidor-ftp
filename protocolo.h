/*
 * protocolo.h — Definição do protocolo do Projeto FTP (Redes de Computadores)
 * Versão para WINDOWS (Winsock2).
 *
 * Formato do pacote (exigido no enunciado):
 *   - seq     : código do pacote (número de sequência)
 *   - cmd     : código do comando
 *   - payload : dados (texto, nome de arquivo, pedaço de arquivo...)
 *
 * Também existe o campo "len" (tamanho do payload): o TCP entrega um fluxo
 * contínuo de bytes, sem divisão entre mensagens. Sem o tamanho, quem recebe
 * não sabe onde um pacote termina e o próximo começa.
 *
 * No fio, o cabeçalho tem 9 bytes: seq (4) + cmd (1) + len (4), com seq e len
 * em ordem de rede (big-endian). Logo depois vêm "len" bytes de payload.
 */
#ifndef PROTOCOLO_H
#define PROTOCOLO_H

#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS        /* evita avisos do Visual Studio */
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <stdint.h>
#include <stddef.h>

#ifdef _MSC_VER
#pragma comment(lib, "ws2_32.lib")     /* Visual Studio: liga a Winsock sozinho */
#endif

/* ---------- Portas e limites ---------- */
#define PORTA_CONTROLE  20001   /* comandos, controle e respostas (servidor escuta) */
#define PORTA_DADOS     20000   /* canal de dados: servidor conecta A PARTIR desta porta */
#define MIN_CLIENTES    2
#define MAX_CLIENTES    10
#define MAX_PAYLOAD     4096
#define MAX_NOME        64

/* ---------- Códigos de comando (cliente -> servidor) ---------- */
enum {
    CMD_LOGIN  = 1,   /* payload: nome do usuário                         */
    CMD_PORTA  = 2,   /* payload: porta de dados do cliente, ex. "45123"  */
    CMD_QUIT   = 3,
    CMD_GET    = 4,   /* payload: nome do arquivo                         */
    CMD_PUT    = 5,   /* payload: nome do arquivo                         */
    CMD_BIN    = 6,
    CMD_ASCII  = 7,
    CMD_LS     = 8,
    CMD_DELETE = 9,   /* payload: nome do arquivo                         */
    CMD_ECHO   = 10   /* payload: texto (etapa inicial sugerida pelo prof.) */
};

/* ---------- Códigos de resposta (servidor -> cliente) ---------- */
enum {
    RESP_OK    = 100, /* payload: mensagem de sucesso                     */
    RESP_ERRO  = 101, /* payload: mensagem de erro                        */
    RESP_DADOS = 102, /* payload: pedaço de dados (canal de dados)        */
    RESP_FIM   = 103  /* fim da transferência no canal de dados           */
};

/* ---------- Estrutura do pacote ---------- */
typedef struct {
    uint32_t seq;                      /* número de sequência  */
    uint8_t  cmd;                      /* código do comando    */
    uint32_t len;                      /* tamanho do payload   */
    char     payload[MAX_PAYLOAD + 1]; /* +1 para '\0' quando for texto */
} Pacote;

/* ---------- Funções (implementadas em protocolo.c) ---------- */

/* Inicializa a Winsock (obrigatório no Windows antes de usar sockets). */
int iniciar_winsock(void);

/* Envia um pacote. Retorna 0 em sucesso, -1 em erro. */
int enviar_pacote(SOCKET fd, uint32_t seq, uint8_t cmd, const void *payload, uint32_t len);

/* Atalho para enviar texto (payload = string terminada em '\0'). */
int enviar_texto(SOCKET fd, uint32_t seq, uint8_t cmd, const char *texto);

/* Recebe um pacote completo. Retorna 1 em sucesso, 0 se a conexão
 * foi fechada, -1 em erro. O payload sempre termina com '\0'. */
int receber_pacote(SOCKET fd, Pacote *p);

/* Nome legível de um código (útil para logs). */
const char *nome_cmd(uint8_t cmd);

#endif
