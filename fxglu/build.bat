@echo off
rem Builds FXGLU.dll (32-bit) with the Visual Studio (Build Tools) x86 compiler.
rem Needs Visual Studio 2017 or newer with the "Desktop development with C++" workload.
setlocal
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (echo vswhere.exe not found - install Visual Studio or the Build Tools & exit /b 1)
for /f "usebackq delims=" %%i in (`call "%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%i"
if not defined VSDIR (echo no Visual Studio with the C++ x86/x64 tools found & exit /b 1)
call "%VSDIR%\VC\Auxiliary\Build\vcvars32.bat" >nul 2>nul || (echo vcvars32.bat failed & exit /b 1)
cd /d "%~dp0"
cl /nologo /O2 /MT /W3 /D_CRT_SECURE_NO_WARNINGS /LD fxglu.c /Fe:FXGLU.dll /link /DEF:fxglu.def user32.lib gdi32.lib opengl32.lib winmm.lib || exit /b 1
del /q fxglu.obj FXGLU.exp 2>nul
echo built %~dp0FXGLU.dll
