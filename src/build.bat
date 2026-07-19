@echo off
setlocal
cd /d "%~dp0"
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 ( echo VCVARS FAILED & exit /b 1 )

if not exist "..\dist" mkdir "..\dist"

cl /nologo /O2 /MT /W3 /EHsc /LD dllmain.cpp /Fe:..\dist\SoraSaveSlots.asi /Fo:..\dist\ /link /DLL
if errorlevel 1 ( echo BUILD FAILED & exit /b 1 )

del /q ..\dist\*.obj 2>nul
del /q ..\dist\*.exp 2>nul
del /q ..\dist\*.lib 2>nul

echo.
echo BUILD OK -^> dist\SoraSaveSlots.asi
