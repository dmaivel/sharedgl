@echo off
@cd /d "%~dp0"
@set "ERRORLEVEL="
@CMD /C EXIT 0
@"%SYSTEMROOT%\system32\cacls.exe" "%SYSTEMROOT%\system32\config\system" >nul 2>&1
@if NOT "%ERRORLEVEL%"=="0" (
@IF "%*"=="" powershell -Command Start-Process """%0""" -Verb runAs 2>nul
@IF NOT "%*"=="" powershell -Command Start-Process """%0""" """%*""" -Verb runAs 2>nul
@GOTO exit
)

@IF EXIST "%windir%\System32\sharedgl.dll" del "%windir%\System32\sharedgl.dll"
@IF EXIST "%windir%\SysWOW64\sharedgl.dll" del "%windir%\SysWOW64\sharedgl.dll"

@set "ERRORLEVEL="
@CMD /C EXIT 0
@REG QUERY "HKEY_LOCAL_MACHINE\SOFTWARE\Microsoft\Windows NT\CurrentVersion\OpenGLDrivers\MSOGL" /d /f sharedgl.dll >nul 2>&1
@if "%ERRORLEVEL%"=="0" REG DELETE "HKEY_LOCAL_MACHINE\SOFTWARE\Microsoft\Windows NT\CurrentVersion\OpenGLDrivers\MSOGL" /f
@CMD /C EXIT 0
@IF /I %PROCESSOR_ARCHITECTURE%==AMD64 REG QUERY "HKEY_LOCAL_MACHINE\SOFTWARE\Wow6432Node\Microsoft\Windows NT\CurrentVersion\OpenGLDrivers\MSOGL" /d /f sharedgl.dll >nul 2>&1
@if "%ERRORLEVEL%"=="0" IF /I %PROCESSOR_ARCHITECTURE%==AMD64 REG DELETE "HKEY_LOCAL_MACHINE\SOFTWARE\Wow6432Node\Microsoft\Windows NT\CurrentVersion\OpenGLDrivers\MSOGL" /f
@CMD /C EXIT 0

@echo Uninstalled SharedGL!

:exit