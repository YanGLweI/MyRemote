@echo off
rem Windows tags files that came from another machine or out of a zip with a
rem Zone.Identifier stream. Qt's file logging then fails without saying so and
rem control_server.log never shows up. Run this once after unzipping.
powershell -NoProfile -ExecutionPolicy Bypass -Command "Get-ChildItem -LiteralPath '%~dp0' -Recurse -File | Unblock-File"
echo unblocked: %~dp0
pause
