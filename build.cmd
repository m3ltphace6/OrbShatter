@echo off
setlocal
if not defined VCVARS set "VCVARS=C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvarsall.bat"
call "%VCVARS%" x86 >nul
if errorlevel 1 exit /b 1
cd /d "%~dp0"
if not exist out mkdir out
cl /nologo /W4 /WX /O2 /MT /LD /Fo:out\OrbContainerTargeting.obj src\OrbContainerTargeting.cpp /link /INCREMENTAL:NO /OUT:out\OrbContainerTargeting.dsdll
exit /b %errorlevel%
