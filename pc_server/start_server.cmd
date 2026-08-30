@echo off
setlocal
set "SERVER_DIR=%~dp0"
set "PYTHON_EXE=%SERVER_DIR%.venv\Scripts\python.exe"
if not exist "%PYTHON_EXE%" (
  echo Run setup_server.ps1 first.
  pause
  exit /b 2
)
if not exist "%SERVER_DIR%server_config.local.json" (
  echo Run configure_server.ps1 first.
  pause
  exit /b 3
)
"%PYTHON_EXE%" -B "%SERVER_DIR%lan_dialogue_server.py"
pause
