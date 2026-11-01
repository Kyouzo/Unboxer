@echo off
rem Builds Lootline Debug (x64\Debug\LL_Debug.exe) and Release (x64\Release\Lootline.exe),
rem with whatever is in the offsets folder built in. See offsets\README.txt.
setlocal
cd /d "%~dp0"

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
set "MSBUILD="
if exist "%VSWHERE%" for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -prerelease -requires Microsoft.Component.MSBuild -find MSBuild\**\Bin\MSBuild.exe`) do set "MSBUILD=%%i"
if not defined MSBUILD (
    echo Could not find Visual Studio / MSBuild.
    pause
    exit /b 1
)

for %%f in (offsets.json buttons.json info.json patterns.txt) do (
    if not exist "offsets\%%f" (
        echo Missing offsets\%%f - copy it from the dump first ^(see offsets\README.txt^).
        pause
        exit /b 1
    )
)

echo === Debug ===
"%MSBUILD%" Unboxer.sln /m /nologo /v:minimal /p:Configuration=Debug;Platform=x64
if errorlevel 1 goto failed

echo === Release ===
"%MSBUILD%" Unboxer.sln /m /nologo /v:minimal /p:Configuration=Release;Platform=x64
if errorlevel 1 goto failed

echo.
echo BUILD OK
echo   x64\Debug\LL_Debug.exe
echo   x64\Release\Lootline.exe
pause
exit /b 0

:failed
echo.
echo BUILD FAILED - scroll up for the first error.
pause
exit /b 1
