@echo off
rem Installs this folder's agent.exe as the Windows service MyRemoteAgent:
rem LocalSystem, auto-start, works on the logon screen. A UAC prompt appears.
rem Without a service the agent only runs after someone logs in.
"%~dp0agent.exe" --install-service
echo.
echo Check with: agent.exe --service-state
pause
