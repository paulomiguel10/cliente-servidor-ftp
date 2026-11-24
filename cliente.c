/*
 * cliente.c — Cliente FTP (etapa 1: ECHO) — WINDOWS
 *
 * - Conecta no servidor pela porta 20001/TCP (canal de controle).
 * - Dois threads:
 *     1) principal: lê comandos, envia ao servidor e mostra as respostas;
 *     2) dados: espera o SERVIDOR conectar (modo ativo) e recebe os dados.
 * - Prompt no formato  maquina:/diretorio/>
 *
 * Uso: cliente.exe [ip_servidor] [usuario]
 *      ex.: cliente.exe 127.0.0.1 joao
 */
#include "protocolo.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static SOCKET           fd_controle = INVALID_SOCKET;
static SOCKET           fd_escuta   = INVALID_SOCKET;  /* onde o servidor conecta */
static uint32_t         seq         = 0;               /* nº de sequência         */
static char             prompt[MAX_PAYLOAD + 1] = "?:/";
static CRITICAL_SECTION cs_tela;

/* ---------- Thread de dados ---------- */
static DWORD WINAPI thread_dados(LPVOID arg)
{
    (void)arg;
    Pacote *p = (Pacote *)malloc(sizeof *p);
    if (!p) return 0;

    for (;;) {
        struct sockaddr_in origem;
        int tam = sizeof origem;
        SOCKET fd = accept(fd_escuta, (struct sockaddr *)&origem, &tam);
        if (fd == INVALID_SOCKET) break;   /* socket de escuta fechado (QUIT) */

        while (receber_pacote(fd, p) == 1) {
            if (p->cmd == RESP_DADOS) {
                EnterCriticalSection(&cs_tela);
                printf("[canal de dados %s:%u] %s\n",
                       inet_ntoa(origem.sin_addr), ntohs(origem.sin_port), p->payload);
                fflush(stdout);
                LeaveCriticalSection(&cs_tela);
                /* TODO (GET): gravar p->payload no arquivo local. */
            } else if (p->cmd == RESP_FIM) {
                break;
            }
        }
        closesocket(fd);   /* o cliente fecha primeiro (ver fechar_canal_dados no servidor) */
    }
    free(p);
    return 0;
}

/* Cria o socket de escuta de dados numa porta livre e devolve a porta. */
static int criar_escuta_dados(void)
{
    fd_escuta = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd_escuta == INVALID_SOCKET) return -1;

    struct sockaddr_in end;
    memset(&end, 0, sizeof end);
    end.sin_family      = AF_INET;
    end.sin_addr.s_addr = htonl(INADDR_ANY);
    end.sin_port        = 0;            /* 0 = o sistema escolhe uma porta livre */
    if (bind(fd_escuta, (struct sockaddr *)&end, sizeof end) == SOCKET_ERROR) return -1;
    if (listen(fd_escuta, 4) == SOCKET_ERROR) return -1;

    int tam = sizeof end;
    getsockname(fd_escuta, (struct sockaddr *)&end, &tam);
    return ntohs(end.sin_port);
}

/* ---------- Canal de controle ---------- */

/* Espera a resposta do servidor. Retorna o código (RESP_OK/RESP_ERRO). */
static int esperar_resposta(Pacote *p)
{
    if (receber_pacote(fd_controle, p) != 1) {
        printf("Conexão com o servidor perdida.\n");
        exit(1);
    }
    return p->cmd;
}

static void mostrar(const Pacote *p)
{
    EnterCriticalSection(&cs_tela);
    printf("%s %s\n", p->cmd == RESP_OK ? "[OK]" : "[ERRO]", p->payload);
    fflush(stdout);
    LeaveCriticalSection(&cs_tela);
}

/* Converte o texto digitado no código do comando. */
static int codigo_do_comando(const char *palavra)
{
    if (!_stricmp(palavra, "quit"))   return CMD_QUIT;
    if (!_stricmp(palavra, "get"))    return CMD_GET;
    if (!_stricmp(palavra, "put"))    return CMD_PUT;
    if (!_stricmp(palavra, "bin"))    return CMD_BIN;
    if (!_stricmp(palavra, "ascii"))  return CMD_ASCII;
    if (!_stricmp(palavra, "ls"))     return CMD_LS;
    if (!_stricmp(palavra, "delete")) return CMD_DELETE;
    if (!_stricmp(palavra, "echo"))   return CMD_ECHO;
    return -1;
}

static void ajuda(void)
{
    printf("Comandos: echo <texto> | bin | ascii | ls | get <arq> | put <arq> | delete <arq> | quit\n");
}

int main(int argc, char **argv)
{
    SetConsoleOutputCP(CP_UTF8);          /* acentos corretos no console */
    InitializeCriticalSection(&cs_tela);
    if (iniciar_winsock() < 0) return 1;

    const char *ip = argc > 1 ? argv[1] : "127.0.0.1";
    char usuario[MAX_NOME];

    if (argc > 2) {
        snprintf(usuario, sizeof usuario, "%s", argv[2]);
    } else {
        printf("Usuário: ");
        fflush(stdout);
        if (!fgets(usuario, sizeof usuario, stdin)) return 1;
        usuario[strcspn(usuario, "\r\n")] = '\0';
    }

    /* 1) Conecta no canal de controle (porta 20001) */
    fd_controle = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    struct sockaddr_in srv;
    memset(&srv, 0, sizeof srv);
    srv.sin_family      = AF_INET;
    srv.sin_port        = htons(PORTA_CONTROLE);
    srv.sin_addr.s_addr = inet_addr(ip);
    if (srv.sin_addr.s_addr == INADDR_NONE) { printf("IP inválido: %s\n", ip); return 1; }
    if (connect(fd_controle, (struct sockaddr *)&srv, sizeof srv) == SOCKET_ERROR) {
        printf("Não foi possível conectar em %s:%d (erro %d)\n", ip, PORTA_CONTROLE, WSAGetLastError());
        return 1;
    }

    /* 2) Mensagem de boas-vindas: conexão estabelecida ou recusada */
    Pacote *p = (Pacote *)malloc(sizeof *p);
    if (!p) return 1;
    esperar_resposta(p);
    mostrar(p);
    if (p->cmd != RESP_OK) { closesocket(fd_controle); WSACleanup(); return 1; }

    /* 3) Prepara o canal de dados e inicia a thread de dados */
    int porta_dados = criar_escuta_dados();
    if (porta_dados < 0) { printf("Falha ao abrir escuta de dados (erro %d)\n", WSAGetLastError()); return 1; }
    HANDLE th = CreateThread(NULL, 0, thread_dados, NULL, 0, NULL);

    /* 4) LOGIN -> o servidor responde "maquina:/diretorio/" */
    enviar_texto(fd_controle, seq++, CMD_LOGIN, usuario);
    if (esperar_resposta(p) != RESP_OK) { mostrar(p); return 1; }
    snprintf(prompt, sizeof prompt, "%s", p->payload);

    /* 5) Informa a porta de dados (o servidor conectará nela a partir da 20000) */
    char txt[16];
    snprintf(txt, sizeof txt, "%d", porta_dados);
    enviar_texto(fd_controle, seq++, CMD_PORTA, txt);
    esperar_resposta(p);
    mostrar(p);

    ajuda();

    /* 6) Laço de comandos */
    char linha[MAX_PAYLOAD + 64];
    for (;;) {
        EnterCriticalSection(&cs_tela);
        printf("%s> ", prompt);
        fflush(stdout);
        LeaveCriticalSection(&cs_tela);

        if (!fgets(linha, sizeof linha, stdin)) strcpy(linha, "quit");
        linha[strcspn(linha, "\r\n")] = '\0';

        char *cmd = linha;
        while (isspace((unsigned char)*cmd)) cmd++;
        if (*cmd == '\0') continue;

        char *arg = cmd;
        while (*arg && !isspace((unsigned char)*arg)) arg++;
        if (*arg) { *arg++ = '\0'; while (isspace((unsigned char)*arg)) arg++; }

        if (!_stricmp(cmd, "help") || !strcmp(cmd, "?")) { ajuda(); continue; }

        int codigo = codigo_do_comando(cmd);
        if (codigo < 0) { printf("Comando desconhecido: %s\n", cmd); ajuda(); continue; }

        enviar_texto(fd_controle, seq++, (uint8_t)codigo, arg);
        esperar_resposta(p);
        mostrar(p);

        if (codigo == CMD_QUIT) break;
    }

    closesocket(fd_escuta);               /* acorda o accept() da thread de dados */
    if (th) { WaitForSingleObject(th, 3000); CloseHandle(th); }
    closesocket(fd_controle);
    free(p);
    WSACleanup();
    return 0;
}
