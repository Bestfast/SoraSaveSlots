@echo off
setlocal
cd /d "%~dp0"

call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 ( echo VCVARS FAILED & exit /b 1 )

if not exist "..\dist" mkdir "..\dist"

cl /nologo /O2 /MT /W3 /EHsc /LD dllmain.cpp core.cpp sora1.cpp sora2.cpp /Fe:..\dist\SoraSaveSlots.asi /Fo:..\dist\ /link /DLL
if errorlevel 1 ( echo BUILD FAILED & exit /b 1 )

del /q ..\dist\dllmain.obj ..\dist\core.obj ..\dist\sora1.obj ..\dist\sora2.obj 2>nul
del /q ..\dist\*.exp 2>nul
del /q ..\dist\*.lib 2>nul

rem -- refresh the Special K hardlink so the games always see the fresh build
set "SKDIR=C:\Users\Salvatore\AppData\Local\Programs\Special K"
set "SKLINK=%SKDIR%\PlugIns\ThirdParty\SoraSaveSlots\SoraSaveSlots.asi"
if exist "%SKDIR%\PlugIns\ThirdParty" (
    del /f /q "%SKLINK%" 2>nul
    mklink /H "%SKLINK%" "..\dist\SoraSaveSlots.asi" >nul 2>&1
    if exist "%SKLINK%" ( echo HARDLINK OK -^> Special K ) else ( echo WARNING: hardlink refresh failed - copy dist\SoraSaveSlots.asi manually )
)

echo.
echo BUILD OK -^> dist\SoraSaveSlots.asi
