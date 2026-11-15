@echo off
rem Builds EyesProtect.exe with MSVC: no C runtime, size-optimized.
setlocal
cd /d "%~dp0"

set "VSINSTALLER=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer"
set "VSWHERE=%VSINSTALLER%\vswhere.exe"
if not exist "%VSWHERE%" goto novs
rem vcvars64.bat calls vswhere by name, so put it on PATH.
set "PATH=%PATH%;%VSINSTALLER%"
for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%i"
if not defined VSDIR goto novs

call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1

cl /nologo /O1 /GS- /Gy /W4 /utf-8 /DUNICODE /D_UNICODE EyesProtect.c /Fe:EyesProtect.exe ^
   /link /NODEFAULTLIB /ENTRY:Start /SUBSYSTEM:WINDOWS /OPT:REF /OPT:ICF ^
   /MANIFEST:EMBED /MANIFESTUAC:"level='asInvoker' uiAccess='false'" ^
   kernel32.lib user32.lib gdi32.lib shell32.lib advapi32.lib wtsapi32.lib dwmapi.lib
if errorlevel 1 exit /b 1

del EyesProtect.obj 2>nul
for %%F in (EyesProtect.exe) do echo Built EyesProtect.exe (%%~zF bytes)
exit /b 0

:novs
echo Visual Studio C++ build tools not found (needs vswhere + MSVC x64).
exit /b 1
