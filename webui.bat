@echo off
REM Start the web console (Windows). Extra arguments are passed on, e.g. webui.bat --port 9000
where py >nul 2>&1
if %errorlevel%==0 (
    py -3 "%~dp0webui\server.py" %*
) else (
    python "%~dp0webui\server.py" %*
)
