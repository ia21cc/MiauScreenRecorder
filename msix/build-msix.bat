@echo off
setlocal EnableDelayedExpansion
cd /d "%~dp0"

if not exist "Miau Screen Recorder.exe" goto noexe

set "PF86=%ProgramFiles(x86)%"
set "MAKEAPPX="
for /f "delims=" %%i in ('dir /b /s "%PF86%\Windows Kits\10\bin\makeappx.exe" 2^>nul ^| findstr /i "\\x64\\"') do set "MAKEAPPX=%%i"
if "!MAKEAPPX!"=="" goto nosdk
echo Usando: !MAKEAPPX!

if exist stage rmdir /s /q stage
mkdir stage
copy /y "Miau Screen Recorder.exe" stage\ >nul
copy /y AppxManifest.xml stage\ >nul
xcopy /e /i /y Assets stage\Assets >nul
if exist ..\LICENSE copy /y ..\LICENSE stage\ >nul
if exist ..\THIRD_PARTY_NOTICES.md copy /y ..\THIRD_PARTY_NOTICES.md stage\ >nul

if exist MiauScreenRecorder.msix del /q MiauScreenRecorder.msix
"!MAKEAPPX!" pack /d stage /p MiauScreenRecorder.msix /o
if errorlevel 1 goto packfail
echo.
echo LISTO: se creo MiauScreenRecorder.msix. Subelo a Partner Center.
goto fin

:noexe
echo Falta el archivo "Miau Screen Recorder.exe" en esta carpeta.
echo Copialo junto a este .bat y vuelve a ejecutarlo.
goto fin

:nosdk
echo No se encontro makeappx.exe.
echo Instala el Windows SDK desde developer.microsoft.com/windows/downloads/windows-sdk
echo y vuelve a ejecutar este archivo.
goto fin

:packfail
echo ERROR al empaquetar. Revisa que AppxManifest.xml tenga los 3 datos de Partner Center.

:fin
echo.
pause
