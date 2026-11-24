# Projeto RC 2026/S2 — Cliente-Servidor FTP (Windows, C)

Etapa 1 pronta: **ECHO** com canal de controle (20001) e canal de dados em **modo ativo** (o servidor conecta a partir da porta 20000).

Usa **Winsock2** (sockets do Windows) e **threads da API do Windows** (`CreateThread`, `CRITICAL_SECTION`). Compila no **GCC/MinGW** e no **Visual Studio**.

## Arquivos

| Arquivo | O que tem |
|---|---|
| `protocolo.h` | Portas, limites, códigos de comando/resposta e a struct `Pacote` (seq, cmd, len, payload) |
| `protocolo.c` | `iniciar_winsock()`, `enviar_pacote()`, `enviar_texto()`, `receber_pacote()`, usados pelos dois lados |
| `servidor.c` | Thread por cliente, tabela de controle protegida, limite de 10, LOGIN, PORTA, ECHO, BIN, ASCII, QUIT |
| `cliente.c` | Thread de comandos + thread de dados, prompt `maquina:/dir/>` |
| `compilar.bat` | Compila tudo com o GCC |
| `servidor.exe`, `cliente.exe` | Já compilados (64 bits), para testar direto |

## Compilar

**Opção A — GCC (MinGW-w64).** Se ainda não tiver o gcc, instale pelo MSYS2 (https://www.msys2.org) ou use o w64devkit. Depois, na pasta do projeto:

```bat
compilar.bat
```

**Opção B — Visual Studio.** Abra o "Developer Command Prompt for VS" na pasta do projeto:

```bat
cl /utf-8 /W3 servidor.c protocolo.c
cl /utf-8 /W3 cliente.c protocolo.c
```

## Rodar

Terminal 1 (servidor, servindo uma pasta):

```bat
.\servidor.exe
```

Terminal 2 (cliente):

```bat
.\cliente.exe
```

Exemplo:

```
[OK] Conexão estabelecida com sucesso
[OK] Porta de dados registrada
MAQUINA01:C:/ftp/> echo ola mundo
[canal de dados 127.0.0.1:20000] ola mundo
[OK] ECHO enviado pelo canal de dados
MAQUINA01:C:/ftp/> quit
[OK] Até logo!
```

**Firewall:** na primeira execução, o Windows pergunta se pode liberar o servidor **e o cliente**. Clique em **Permitir acesso**. O cliente também precisa ser liberado porque, no modo ativo, é o servidor que conecta nele. Para testar entre dois PCs, use o IP do servidor (descubra com `ipconfig`) e deixe as duas máquinas na mesma rede.

## Como funciona a sessão

1. O cliente conecta na porta 20001. O servidor responde `OK "Conexão estabelecida com sucesso"` ou `ERRO "Número de usuários excedido"`.
2. `CMD_LOGIN <usuario>`: o servidor guarda o usuário e o endereço socket na tabela e responde `maquina:/dir/` (usado no prompt).
3. `CMD_PORTA <porta>`: o cliente informa em que porta a thread de dados está escutando.
4. Em cada transferência, o servidor cria uma thread de dados, faz `bind(20000)` + `connect(ip_cliente:porta)`, envia `RESP_DADOS`... e por último `RESP_FIM`.
5. O **cliente fecha primeiro** o canal de dados. Assim o `TIME_WAIT` (que no Windows pode durar até 4 minutos) fica do lado do cliente, e o servidor pode reconectar da porta 20000 logo em seguida. Bom item para "problemas e soluções" no documento.

## Diferenças Windows x Linux (bom para o documento)
- É preciso chamar `WSAStartup()` antes de usar qualquer socket, e `WSACleanup()` no fim.
- O socket é do tipo `SOCKET`, e não `int`. Fecha com `closesocket()`, não com `close()`.
- Erros vêm de `WSAGetLastError()`, não de `errno`.
- Threads: `CreateThread` e `WaitForSingleObject` no lugar de `pthread_create` e `pthread_join`. O mutex é a `CRITICAL_SECTION`.
- Os timeouts de `SO_RCVTIMEO` são um `DWORD` em milissegundos.

## O que falta (próximas etapas)

Os pontos marcados com `TODO` no código:

**Pessoa A — servidor (`servidor.c`)**
- [ ] `LS`: `FindFirstFileA`/`FindNextFileA` na pasta servida, lista enviada pelo canal de dados
- [ ] `DELETE <arq>`: `remove()` e resposta OK/ERRO
- [ ] `GET <arq>`: abrir o arquivo (`"rb"` em BIN, `"r"` em ASCII) e enviar em blocos de `MAX_PAYLOAD` pelo canal de dados
- [ ] `PUT <arq>`: receber os blocos pelo canal de dados e gravar
- [ ] Não aceitar `..`, `\`, `/` ou `:` no nome do arquivo (para não sair da pasta servida)

**Pessoa B — cliente (`cliente.c`)**
- [ ] `thread_dados`: em GET, gravar os `RESP_DADOS` num arquivo local em vez de imprimir
- [ ] `PUT`: avisar a thread de dados que ela deve **enviar** o arquivo local quando o servidor conectar
- [ ] Guardar o modo atual (BIN/ASCII) para abrir os arquivos do jeito certo
- [ ] Modo ASCII: no Windows, `"r"`/`"w"` já convertem `\r\n` ↔ `\n` sozinhos. Documentar isso

**Juntos**
- [ ] Testes: 3+ clientes, 11º cliente recusado, arquivo `.txt` e arquivo binário (imagem). Comparar com `certutil -hashfile arquivo MD5` antes e depois
- [ ] Documento: cliente, servidor, protocolo, referências, problemas e soluções

## Referências úteis
- Microsoft — Getting Started with Winsock: https://learn.microsoft.com/windows/win32/winsock/getting-started-with-winsock
- Beej's Guide to Network Programming (tem seção sobre Windows): https://beej.us/guide/bgnet/
- RFC 959 (FTP), seção de modo ativo/PORT
