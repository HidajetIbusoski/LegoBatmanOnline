@echo off
rem Fuehrt einen Befehl in der MSVC-x86-Umgebung aus: tools\vs.bat <befehl...>
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars32.bat" >nul
%*
