/*
 * servidor.c — Servidor FTP (etapa 1: ECHO) — WINDOWS
 *
 * - Escuta comandos na porta 20001/TCP (canal de controle).
 * - Modo ATIVO: para enviar dados, o servidor abre a conexão a partir da
 *   porta 20000/TCP até a porta de dados informada pelo cliente.
 * - Aceita até MAX_CLIENTES sessões simultâneas (uma thread por cliente).
 * - Mantém uma tabela de controle com login e endereço socket de cada cliente.
 * - Confere o endereço socket de origem a cada comando recebido.
 * - Cada cliente usa 2 threads no servidor: a de controle (sessao_thread) e
 *   uma thread de transferência criada para cada envio pelo canal de dados.
 *
 * Uso: servidor.exe [pasta_servida]
 */
#include "protocolo.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------- Tabela de controle ---------- */
typedef struct {
    int                ativo;          /* 1 = posição em uso                  */
    int                logado;         /* 1 = já fez LOGIN                    */
    SOCKET             fd;             /* socket do canal de controle         */
    char               usuario[MAX_NOME];
    struct sockaddr_in end;            /* endereço socket do cliente (login)  */
    uint16_t           porta_dados;    /* porta onde o cliente espera dados   */
    int                modo_bin;       /* 1 = BINARY, 0 = ASCII               */
    uint32_t           seq;            /* próximo nº de sequência a enviar    */
} Sessao;

static Sessao           tabela[MAX_CLIENTES];
static CRITICAL_SECTION cs_tabela;     /* "mutex" do Windows */
static CRITICAL_SECTION cs_log;

static char nome_maquina[256];
static char dir_servidor[MAX_PATH];

/* ---------- Utilidades ---------- */
static void logf_(const char *fmt, ...)
{
    va_list ap;
    EnterCriticalSection(&cs_log);
    va_start(ap, fmt);
    printf("[servidor] ");
    vprintf(fmt, ap);
    printf("\n");
    fflush(stdout);
    va_end(ap);
    LeaveCriticalSection(&cs_log);
}

static const char *end_str(const struct sockaddr_in *a, char *buf, size_t n)
{
    snprintf(buf, n, "%s:%u", inet_ntoa(a->sin_addr), ntohs(a->sin_port));
    return buf;
}

static int mesmo_endereco(const struct sockaddr_in *a, const struct sockaddr_in *b)
{
    return a->sin_addr.s_addr == b->sin_addr.s_addr && a->sin_port == b->sin_port;
}

static int sessoes_ativas(void)
{
    int n = 0;
    for (int i = 0; i < MAX_CLIENTES; i++) n += tabela[i].ativo;
    return n;
}

static void imprimir_tabela(void)
{
    char buf[64];
    EnterCriticalSection(&cs_tabela);
    EnterCriticalSection(&cs_log);
    printf("[servidor] ---- Tabela de controle (%d/%d) ----\n", sessoes_ativas(), MAX_CLIENTES);
    for (int i = 0; i < MAX_CLIENTES; i++) {
        if (!tabela[i].ativo) continue;
        printf("[servidor]   #%d  login=%-12s  socket=%s  porta_dados=%u  modo=%s\n",
               i, tabela[i].logado ? tabela[i].usuario : "(sem login)",
               end_str(&tabela[i].end, buf, sizeof buf), tabela[i].porta_dados,
               tabela[i].modo_bin ? "BIN" : "ASCII");
    }
    fflush(stdout);
    LeaveCriticalSection(&cs_log);
    LeaveCriticalSection(&cs_tabela);
}

/* Envia resposta pelo canal de controle, com nº de sequência da sessão. */
static int responder(Sessao *s, uint8_t cod, const char *msg)
{
    return enviar_texto(s->fd, s->seq++, cod, msg);
}

/* ---------- Canal de dados (modo ativo) ---------- */

/* Abre conexão DO servidor (porta local 20000) PARA o cliente. */
static SOCKET abrir_canal_dados(const Sessao *s)
{
    SOCKET fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd == INVALID_SOCKET) return INVALID_SOCKET;

    /* Vários clientes usam a porta 20000 do servidor ao mesmo tempo:
     * SO_REUSEADDR permite vários sockets presos à mesma porta local.
     * Cada conexão continua única porque o destino (IP:porta do cliente) muda. */
    BOOL um = TRUE;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (const char *)&um, sizeof um);

    DWORD tempo = 10000;   /* 10 s (no Windows o timeout é em milissegundos) */
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, (const char *)&tempo, sizeof tempo);
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, (const char *)&tempo, sizeof tempo);

    struct sockaddr_in local;
    memset(&local, 0, sizeof local);
    local.sin_family      = AF_INET;
    local.sin_addr.s_addr = htonl(INADDR_ANY);
    local.sin_port        = htons(PORTA_DADOS);
    if (bind(fd, (struct sockaddr *)&local, sizeof local) == SOCKET_ERROR) {
        logf_("bind na porta %d falhou (erro %d)", PORTA_DADOS, WSAGetLastError());
        closesocket(fd);
        return INVALID_SOCKET;
    }

    struct sockaddr_in destino = s->end;            /* mesmo IP do cliente  */
    destino.sin_port = htons(s->porta_dados);       /* porta de dados dele  */
    if (connect(fd, (struct sockaddr *)&destino, sizeof destino) == SOCKET_ERROR) {
        logf_("connect no canal de dados falhou (erro %d)", WSAGetLastError());
        closesocket(fd);
        return INVALID_SOCKET;
    }
    return fd;
}

/* Fecha o canal de dados deixando o CLIENTE fechar primeiro.
 * Quem fecha primeiro fica em TIME_WAIT (no Windows, até 4 min); se fosse o
 * servidor, a próxima conexão 20000 -> mesma porta do cliente falharia. */
static void fechar_canal_dados(SOCKET fd)
{
    char lixo[64];
    while (recv(fd, lixo, sizeof lixo, 0) > 0) { /* espera o cliente fechar */ }
    closesocket(fd);
}

/* Dados passados para a thread de transferência. */
typedef struct {
    Sessao  *s;
    char     texto[MAX_PAYLOAD + 1];
    int      resultado;      /* 0 = ok, -1 = erro */
} TarefaDados;

/* Thread de transferência: envia o ECHO pelo canal de dados.
 * (Nas próximas etapas: GET envia o arquivo, PUT recebe o arquivo.) */
static DWORD WINAPI thread_dados_echo(LPVOID arg)
{
    TarefaDados *t = (TarefaDados *)arg;
    t->resultado = -1;

    SOCKET fd = abrir_canal_dados(t->s);
    if (fd == INVALID_SOCKET) return 0;

    uint32_t seq = 0;
    if (enviar_texto(fd, seq++, RESP_DADOS, t->texto) == 0 &&
        enviar_pacote(fd, seq++, RESP_FIM, NULL, 0) == 0)
        t->resultado = 0;

    fechar_canal_dados(fd);
    return 0;
}

/* ---------- Tratamento dos comandos ---------- */

static void cmd_login(Sessao *s, const Pacote *p)
{
    if (p->len == 0) { responder(s, RESP_ERRO, "Informe o nome do usuário"); return; }

    struct sockaddr_in origem;
    int tam = sizeof origem;
    getpeername(s->fd, (struct sockaddr *)&origem, &tam);

    EnterCriticalSection(&cs_tabela);
    snprintf(s->usuario, sizeof s->usuario, "%.*s", MAX_NOME - 1, p->payload); /* guarda o usuário */
    s->end    = origem;                                                         /* guarda o socket  */
    s->logado = 1;
    LeaveCriticalSection(&cs_tabela);

    char buf[64];
    logf_("LOGIN de '%s' a partir de %s", s->usuario, end_str(&s->end, buf, sizeof buf));

    /* A resposta leva "maquina:/diretorio/" para o cliente montar o prompt. */
    char prompt[sizeof nome_maquina + sizeof dir_servidor + 2];
    snprintf(prompt, sizeof prompt, "%s:%s/", nome_maquina, dir_servidor);
    responder(s, RESP_OK, prompt);
}

static void cmd_porta(Sessao *s, const Pacote *p)
{
    long porta = strtol(p->payload, NULL, 10);
    if (porta <= 0 || porta > 65535) { responder(s, RESP_ERRO, "Porta de dados inválida"); return; }

    EnterCriticalSection(&cs_tabela);
    s->porta_dados = (uint16_t)porta;
    LeaveCriticalSection(&cs_tabela);

    logf_("'%s' espera dados na porta %ld", s->usuario, porta);
    responder(s, RESP_OK, "Porta de dados registrada");
    imprimir_tabela();
}

static void cmd_echo(Sessao *s, const Pacote *p)
{
    if (s->porta_dados == 0) { responder(s, RESP_ERRO, "Porta de dados não informada"); return; }

    TarefaDados *t = (TarefaDados *)malloc(sizeof *t);
    if (!t) { responder(s, RESP_ERRO, "Sem memória"); return; }
    t->s = s;
    snprintf(t->texto, sizeof t->texto, "%s", p->payload);

    /* Segunda thread: transferência pelo canal de dados. */
    HANDLE th = CreateThread(NULL, 0, thread_dados_echo, t, 0, NULL);
    if (!th) {
        free(t);
        responder(s, RESP_ERRO, "Falha ao criar thread de dados");
        return;
    }
    WaitForSingleObject(th, INFINITE);   /* espera a transferência terminar */
    CloseHandle(th);

    if (t->resultado == 0) {
        logf_("ECHO de '%s' enviado pelo canal de dados (porta %d)", s->usuario, PORTA_DADOS);
        responder(s, RESP_OK, "ECHO enviado pelo canal de dados");
    } else {
        responder(s, RESP_ERRO, "Falha ao abrir o canal de dados");
    }
    free(t);
}

static void cmd_modo(Sessao *s, int bin)
{
    s->modo_bin = bin;
    responder(s, RESP_OK, bin ? "Modo de transferência: BINARY" : "Modo de transferência: ASCII");
}

/* ---------- Thread de controle de cada cliente ---------- */
static DWORD WINAPI sessao_thread(LPVOID arg)
{
    Sessao *s = (Sessao *)arg;
    Pacote *p = (Pacote *)malloc(sizeof *p);
    char    buf[64];

    while (p && receber_pacote(s->fd, p) == 1) {

        /* Requisito: verificar o endereço socket de origem de cada comando. */
        if (s->logado) {
            struct sockaddr_in origem;
            int tam = sizeof origem;
            getpeername(s->fd, (struct sockaddr *)&origem, &tam);
            if (!mesmo_endereco(&origem, &s->end)) {
                logf_("Comando de origem desconhecida (%s) recusado", end_str(&origem, buf, sizeof buf));
                responder(s, RESP_ERRO, "Endereço de origem não confere com o login");
                continue;
            }
        } else if (p->cmd != CMD_LOGIN && p->cmd != CMD_QUIT) {
            responder(s, RESP_ERRO, "Faça LOGIN primeiro");
            continue;
        }

        logf_("<- %s  seq=%u  cmd=%s  payload=\"%s\"",
              s->logado ? s->usuario : "?", p->seq, nome_cmd(p->cmd), p->payload);

        if (p->cmd == CMD_QUIT) {
            responder(s, RESP_OK, "Até logo!");
            break;
        }

        switch (p->cmd) {
        case CMD_LOGIN:  cmd_login(s, p); break;
        case CMD_PORTA:  cmd_porta(s, p); break;
        case CMD_ECHO:   cmd_echo(s, p);  break;
        case CMD_BIN:    cmd_modo(s, 1);  break;
        case CMD_ASCII:  cmd_modo(s, 0);  break;
        case CMD_GET:
        case CMD_PUT:
        case CMD_LS:
        case CMD_DELETE:
            /* TODO (próximas etapas): implementar usando o canal de dados. */
            responder(s, RESP_ERRO, "Comando ainda não implementado");
            break;
        default:
            responder(s, RESP_ERRO, "Comando desconhecido");
        }
    }
    free(p);

    logf_("Sessão de '%s' (%s) encerrada", s->logado ? s->usuario : "?",
          end_str(&s->end, buf, sizeof buf));
    closesocket(s->fd);
    EnterCriticalSection(&cs_tabela);
    memset(s, 0, sizeof *s);              /* libera a posição na tabela */
    LeaveCriticalSection(&cs_tabela);
    imprimir_tabela();
    return 0;
}

/* ---------- main ---------- */
int main(int argc, char **argv)
{
    SetConsoleOutputCP(CP_UTF8);          /* acentos corretos no console */
    InitializeCriticalSection(&cs_tabela);
    InitializeCriticalSection(&cs_log);
    if (iniciar_winsock() < 0) return 1;

    if (argc > 1 && !SetCurrentDirectoryA(argv[1])) {
        printf("Pasta não encontrada: %s\n", argv[1]);
        return 1;
    }
    GetCurrentDirectoryA(sizeof dir_servidor, dir_servidor);
    for (char *c = dir_servidor; *c; c++) if (*c == '\\') *c = '/';   /* C:/pasta/ */
    gethostname(nome_maquina, sizeof nome_maquina);

    SOCKET srv = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (srv == INVALID_SOCKET) { printf("socket falhou (erro %d)\n", WSAGetLastError()); return 1; }

    struct sockaddr_in end;
    memset(&end, 0, sizeof end);
    end.sin_family      = AF_INET;
    end.sin_addr.s_addr = htonl(INADDR_ANY);
    end.sin_port        = htons(PORTA_CONTROLE);
    if (bind(srv, (struct sockaddr *)&end, sizeof end) == SOCKET_ERROR) {
        printf("bind na porta %d falhou (erro %d). Já existe um servidor aberto?\n",
               PORTA_CONTROLE, WSAGetLastError());
        return 1;
    }
    if (listen(srv, 16) == SOCKET_ERROR) { printf("listen falhou (erro %d)\n", WSAGetLastError()); return 1; }

    logf_("Máquina %s, diretório %s", nome_maquina, dir_servidor);
    logf_("Aguardando clientes na porta %d/TCP (dados pela porta %d/TCP, modo ativo)",
          PORTA_CONTROLE, PORTA_DADOS);

    for (;;) {
        struct sockaddr_in cli;
        int tam = sizeof cli;
        SOCKET fd = accept(srv, (struct sockaddr *)&cli, &tam);
        if (fd == INVALID_SOCKET) { logf_("accept falhou (erro %d)", WSAGetLastError()); continue; }

        char buf[64];
        end_str(&cli, buf, sizeof buf);

        /* Procura uma posição livre na tabela de controle. */
        EnterCriticalSection(&cs_tabela);
        int livre = -1;
        for (int i = 0; i < MAX_CLIENTES; i++)
            if (!tabela[i].ativo) { livre = i; break; }
        if (livre >= 0) {
            memset(&tabela[livre], 0, sizeof tabela[livre]);
            tabela[livre].ativo    = 1;
            tabela[livre].fd       = fd;
            tabela[livre].end      = cli;
            tabela[livre].modo_bin = 1;   /* padrão: BINARY */
        }
        LeaveCriticalSection(&cs_tabela);

        if (livre < 0) {
            logf_("Conexão de %s recusada: Número de usuários excedido", buf);
            enviar_texto(fd, 0, RESP_ERRO, "Número de usuários excedido");
            closesocket(fd);
            continue;
        }

        logf_("Conexão de %s aceita (sessão #%d)", buf, livre);
        responder(&tabela[livre], RESP_OK, "Conexão estabelecida com sucesso");

        HANDLE th = CreateThread(NULL, 0, sessao_thread, &tabela[livre], 0, NULL);
        if (!th) {
            closesocket(fd);
            EnterCriticalSection(&cs_tabela);
            memset(&tabela[livre], 0, sizeof tabela[livre]);
            LeaveCriticalSection(&cs_tabela);
            continue;
        }
        CloseHandle(th);   /* a thread continua rodando sozinha (detach) */
    }
}
