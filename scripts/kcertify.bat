@echo off

IF %1.==. GOTO defaultarg
@set os_provided=%1
GOTO certify
:defaultarg
@set os_provided="10_X64"

:certify
makecert -r -pe -ss PrivateCertStore -n CN=ksgldrv ksgldrv.cer
Inf2cat.exe /driver:.\ /os:%os_provided%
Signtool sign /v /fd sha256 /s PrivateCertStore /n ksgldrv /a /t http://timestamp.digicert.com ksgldrv.cat
CertMgr.exe /add ksgldrv.cer /s /r localMachine root
CertMgr.exe /add ksgldrv.cer /s /r localMachine trustedpublisher