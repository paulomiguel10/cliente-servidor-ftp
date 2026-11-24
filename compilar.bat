@echo off
REM Compila servidor e cliente com o GCC (MinGW-w64)
gcc -Wall -Wextra -O2 -o servidor.exe servidor.c protocolo.c -lws2_32
if errorlevel 1 goto erro
gcc -Wall -Wextra -O2 -o cliente.exe cliente.c protocolo.c -lws2_32
if errorlevel 1 goto erro
echo.
echo Pronto: servidor.exe e cliente.exe gerados.
goto fim
:erro
echo.
echo Erro na compilacao. O gcc esta instalado e no PATH? Teste: gcc --version
:fim
