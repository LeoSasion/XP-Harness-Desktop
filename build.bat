@echo off
setlocal
pushd "%~dp0"
cl /? >nul 2>nul
if errorlevel 1 goto compiler
if not exist build mkdir build
if not exist dist mkdir dist
cl /nologo /W3 /O1 /MT /DWIN32 /DCJSON_NESTING_LIMIT=64 /I"dep\curl-8.20\include" /Fo"build\\" src\main.c src\cJSON.c src\ipc.c /Fe"dist\AgentCore.exe" /link /subsystem:console,5.01 /machine:I386 /libpath:"dep\curl-8.20\lib" libcurl.lib user32.lib advapi32.lib
if errorlevel 1 goto failed
cl /nologo /W3 /O1 /MT /DWIN32 /DCJSON_NESTING_LIMIT=64 /Fo"build\\" src\desktop.c src\cJSON.c src\ipc.c /Fe"dist\XP-Harness-Desktop.exe" /link /subsystem:windows,5.01 /machine:I386 user32.lib gdi32.lib shell32.lib advapi32.lib ole32.lib comctl32.lib comdlg32.lib
if errorlevel 1 goto failed
copy /y dep\curl-8.20\bin\libcurl-4.dll dist\ >nul
copy /y curl-ca-bundle.crt dist\ >nul
copy /y vc6ai.sample.ini dist\ >nul
if not exist dist\vc6ai.ini copy vc6ai.sample.ini dist\vc6ai.ini >nul
copy /y dep\busybox-1.38\busybox.exe dist\ >nul
copy /y dep\jq-1.8.2\jq.exe dist\ >nul
copy /y dep\curl-8.20\bin\curl.exe dist\ >nul
copy /y LICENSE dist\ >nul
copy /y NOTICE dist\ >nul
if not exist dist\licenses mkdir dist\licenses
copy /y dep\busybox-1.38\LICENSE dist\licenses\busybox.txt >nul
copy /y dep\curl-8.20\COPYING dist\licenses\curl.txt >nul
copy /y dep\jq-1.8.2\COPYING dist\licenses\jq.txt >nul
copy /y dep\jq-1.8.2\COPYING.oniguruma dist\licenses\oniguruma.txt >nul
echo Built dist\XP-Harness-Desktop.exe and dist\AgentCore.exe.
popd
exit /b 0
:compiler
echo Run this from a Visual C++ 6.0 SP6 command prompt after VCVARS32.BAT.
:failed
popd
exit /b 1
