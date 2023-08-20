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

@set regglkey="HKEY_LOCAL_MACHINE\SOFTWARE\Microsoft\Windows NT\CurrentVersion\OpenGLDrivers\MSOGL"
@set regglwowkey="HKEY_LOCAL_MACHINE\SOFTWARE\Wow6432Node\Microsoft\Windows NT\CurrentVersion\OpenGLDrivers\MSOGL"

@set CD=
@set curloc=%CD%
@IF %curloc:~0,1%%curloc:~-1%=="" set curloc=%curloc:~1,-1%
@IF "%curloc:~-1%"=="\" set curloc=%curloc:~0,-1%

@IF /I %PROCESSOR_ARCHITECTURE%==AMD64 IF EXIST "%curloc%\sharedgl32.dll" copy "%curloc%\sharedgl32.dll" "%windir%\SysWOW64\sharedgl.dll"
@IF /I %PROCESSOR_ARCHITECTURE%==AMD64 IF EXIST "%curloc%\sharedgl64.dll" copy "%curloc%\sharedgl64.dll" "%windir%\System32\sharedgl.dll"
@IF /I %PROCESSOR_ARCHITECTURE%==X86 IF EXIST "%curloc%\sharedgl32.dll" copy "%curloc%\sharedgl32.dll" "%windir%\SysWOW64\sharedgl.dll"

@IF EXIST "%windir%\System32\sharedgl.dll" REG ADD %regglkey% /v DLL /t REG_SZ /d sharedgl.dll /f
@IF EXIST "%windir%\System32\sharedgl.dll" REG ADD %regglkey% /v DriverVersion /t REG_DWORD /d 1 /f
@IF EXIST "%windir%\System32\sharedgl.dll" REG ADD %regglkey% /v Flags /t REG_DWORD /d 1 /f
@IF EXIST "%windir%\System32\sharedgl.dll" REG ADD %regglkey% /v Version /t REG_DWORD /d 2 /f
@IF /I %PROCESSOR_ARCHITECTURE%==AMD64 IF EXIST "%windir%\SysWOW64\sharedgl.dll" REG ADD %regglwowkey% /v DLL /t REG_SZ /d sharedgl.dll /f
@IF /I %PROCESSOR_ARCHITECTURE%==AMD64 IF EXIST "%windir%\SysWOW64\sharedgl.dll" REG ADD %regglwowkey% /v DriverVersion /t REG_DWORD /d 1 /f
@IF /I %PROCESSOR_ARCHITECTURE%==AMD64 IF EXIST "%windir%\SysWOW64\sharedgl.dll" REG ADD %regglwowkey% /v Flags /t REG_DWORD /d 1 /f
@IF /I %PROCESSOR_ARCHITECTURE%==AMD64 IF EXIST "%windir%\SysWOW64\sharedgl.dll" REG ADD %regglwowkey% /v Version /t REG_DWORD /d 2 /f

@echo Installed SharedGL!

:exit