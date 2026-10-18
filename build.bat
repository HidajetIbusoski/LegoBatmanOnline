@echo off
rem Baut lbo-launch.exe und lbo.dll (x86) nach build\
setlocal
cd /d "%~dp0"
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars32.bat" >nul
if not exist build mkdir build
cl /nologo /O2 /W3 /MT /D_CRT_SECURE_NO_WARNINGS /Fo:build\ src\launcher\lbo_launch.c src\launcher\online.c /Fe:build\lbo-launch.exe /link /nologo ws2_32.lib bcrypt.lib advapi32.lib shell32.lib || exit /b 1
cl /nologo /O2 /W3 /MT /LD /D_CRT_SECURE_NO_WARNINGS /Fo:build\ src\dll\lbo.c src\dll\net.c src\dll\input.c src\dll\render.c /Fe:build\lbo.dll /link /nologo user32.lib gdi32.lib winmm.lib ws2_32.lib dinput8.lib dxguid.lib || exit /b 1
rem Analysehilfe: Speicherabbild von Spielen mit Steam-Kopierschutzhuelle (siehe src\dump\dump.c)
cl /nologo /O2 /W3 /MT /LD /D_CRT_SECURE_NO_WARNINGS /Fo:build\ src\dump\dump.c /Fe:build\dump.dll /link /nologo || exit /b 1
echo Build OK
