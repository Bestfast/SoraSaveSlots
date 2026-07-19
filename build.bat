@echo off
setlocal
cd /d "%~dp0"
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 ( echo VCVARS FAILED & exit /b 1 )
cl /nologo /O2 /MT /W3 /EHsc /LD sora_saveslots.cpp /Fe:sora_saveslots.dll
if errorlevel 1 ( echo BUILD FAILED & exit /b 1 )
copy /y sora_saveslots.dll sora_saveslots.asi >nul
echo BUILD OK
