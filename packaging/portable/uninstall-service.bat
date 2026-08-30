@echo off
rem Stops and removes the MyRemoteAgent service. The configuration and the logs
rem under ProgramData\MyRemote are kept on purpose: a reinstall picks them up.
"%~dp0agent.exe" --uninstall-service
echo.
echo Files in this folder are still there - delete them when you are sure.
pause
