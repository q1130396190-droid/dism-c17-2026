@echo off
setlocal

echo Setting English as the default input language...

:: ´ò¿ª×¢²á±í±à¼­Æ÷
reg add "HKCU\Keyboard Layout\Preload" /v 1 /t REG_SZ /d 00000409 /f

echo English has been set as the default input language.
pause