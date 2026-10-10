@echo off
cd /d "%~dp0"
if exist dist\AgentCore.exe (
    cd dist
) else if exist build\host\AgentCore.exe (
    cd build\host
) else (
    echo Build AgentCore.exe first. See README.md.
    pause
    exit /b 1
)
AgentCore.exe
