@echo off
rem Dekompiliert Funktionen aus dem Ghidra-Projekt von LEGO Batman 2 (runs\batman2\ghidra, siehe docs\batman2-phase0.md)
rem decompile.bat <ausgabe.txt> 0xADRESSE [0xADRESSE ...]
setlocal
for %%I in ("%~dp0..\..") do set ROOT=%%~fI
call "C:\Users\hidaj\Tools\ghidra_12.1.4_PUBLIC\support\analyzeHeadless.bat" "%ROOT%\runs\batman2\ghidra" LB2 -process LEGOBatman2_entpackt.exe -noanalysis -readOnly -scriptPath "%ROOT%\tools\ghidra" -postScript Decompile.java %* > "%ROOT%\runs\batman2\decompile.log" 2>&1 < nul
