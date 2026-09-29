@echo off
setlocal EnableExtensions DisableDelayedExpansion

mode con cols=132 lines=45
chcp 65001 >nul
cls

set "PAYLOAD=%~dp0"
set "PRODUCT=%PAYLOAD%ytec-winpe-app.exe"
set "SCRIPT=X:\ydc-msr-diagnostic.txt"

echo === Y-TEC TSUMUGI DRIVE VM-ONLY MSR DIAGNOSTIC ===
echo Target contract: only the retained disposable 64 GiB converted VDI is attached as disk 0.
echo.
echo === BEFORE READ-ONLY INVENTORY ===
"%PRODUCT%" --text
echo === END BEFORE INVENTORY ===
echo.

>"%SCRIPT%" echo select disk 0
>>"%SCRIPT%" echo create partition msr size=16 offset=58718208
>>"%SCRIPT%" echo list partition
>>"%SCRIPT%" echo exit

X:\Windows\System32\diskpart.exe /s "%SCRIPT%"
set "DISKPART_EXIT=%ERRORLEVEL%"
del "%SCRIPT%"

echo.
echo DISKPART_EXIT=%DISKPART_EXIT%
echo === AFTER READ-ONLY INVENTORY ===
"%PRODUCT%" --text
echo === END AFTER INVENTORY ===
echo.
echo Diagnostic evidence will remain visible for two minutes.
ping -n 121 127.0.0.1 >nul
wpeutil shutdown
exit /b %DISKPART_EXIT%
