@rem Find and build all *.sln file in current directory
@setlocal enabledelayedexpansion
@echo off
set /a errorno=1
for /F "delims=#" %%E in ('"prompt #$E# & for %%E in (1) do rem"') do set "esc=%%E"

rem https://github.com/Microsoft/vswhere
rem https://github.com/microsoft/vswhere/wiki/Find-VC#batch

set "vswhere=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%vswhere%" (
  echo Failed to find "vswhere.exe".  Please install the latest version of Visual Studio.
  goto :ERROR
)

set "InstallDir="
for /f "usebackq tokens=*" %%i in (
  `"%vswhere%" -latest                                                     ^
               -products *                                                 ^
               -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 ^
               -property installationPath`
) do (
  set "InstallDir=%%i"
)
if "%InstallDir%" == "" (
  echo Failed to find Visual C++.  Please install the latest version of Visual C++.
  goto :ERROR
)

call "%InstallDir%\VC\Auxiliary\Build\vcvars64.bat" || goto :ERROR


rem https://docs.microsoft.com/visualstudio/msbuild/msbuild-command-line-reference

set "sln=lz4.sln"

rmdir /S /Q bin 2>nul

echo msbuild "%sln%" /p:Configuration=Debug /p:Platform="Win32"
msbuild "%sln%"                  ^
        /nologo                  ^
        /v:minimal               ^
        /m                       ^
        /p:Configuration=Debug   ^
        /p:Platform="Win32"      ^
        /t:Clean,Build           ^
        || goto :ERROR

if not exist "bin\Win32_Debug\datagen.exe"       ( echo FAIL: "bin\Win32_Debug\datagen.exe"       && goto :ERROR )
if not exist "bin\Win32_Debug\frametest.exe"     ( echo FAIL: "bin\Win32_Debug\frametest.exe"     && goto :ERROR )
if not exist "bin\Win32_Debug\fullbench-dll.exe" ( echo FAIL: "bin\Win32_Debug\fullbench-dll.exe" && goto :ERROR )
if not exist "bin\Win32_Debug\fullbench.exe"     ( echo FAIL: "bin\Win32_Debug\fullbench.exe"     && goto :ERROR )
if not exist "bin\Win32_Debug\fuzzer.exe"        ( echo FAIL: "bin\Win32_Debug\fuzzer.exe"        && goto :ERROR )
if not exist "bin\Win32_Debug\liblz4.dll"        ( echo FAIL: "bin\Win32_Debug\liblz4.dll"        && goto :ERROR )
if not exist "bin\Win32_Debug\liblz4.lib"        ( echo FAIL: "bin\Win32_Debug\liblz4.lib"        && goto :ERROR )
if not exist "bin\Win32_Debug\liblz4_static.lib" ( echo FAIL: "bin\Win32_Debug\liblz4_static.lib" && goto :ERROR )
if not exist "bin\Win32_Debug\lz4.exe"           ( echo FAIL: "bin\Win32_Debug\lz4.exe"           && goto :ERROR )

echo msbuild "%sln%" /p:Configuration=Release /p:Platform="Win32"
msbuild "%sln%"                  ^
        /nologo                  ^
        /v:minimal               ^
        /m                       ^
        /p:Configuration=Release ^
        /p:Platform="Win32"      ^
        /t:Clean,Build           ^
        || goto :ERROR

if not exist "bin\Win32_Release\datagen.exe"       ( echo FAIL: "bin\Win32_Release\datagen.exe"       && goto :ERROR )
if not exist "bin\Win32_Release\frametest.exe"     ( echo FAIL: "bin\Win32_Release\frametest.exe"     && goto :ERROR )
if not exist "bin\Win32_Release\fullbench-dll.exe" ( echo FAIL: "bin\Win32_Release\fullbench-dll.exe" && goto :ERROR )
if not exist "bin\Win32_Release\fullbench.exe"     ( echo FAIL: "bin\Win32_Release\fullbench.exe"     && goto :ERROR )
if not exist "bin\Win32_Release\fuzzer.exe"        ( echo FAIL: "bin\Win32_Release\fuzzer.exe"        && goto :ERROR )
if not exist "bin\Win32_Release\liblz4.dll"        ( echo FAIL: "bin\Win32_Release\liblz4.dll"        && goto :ERROR )
if not exist "bin\Win32_Release\liblz4.lib"        ( echo FAIL: "bin\Win32_Release\liblz4.lib"        && goto :ERROR )
if not exist "bin\Win32_Release\liblz4_static.lib" ( echo FAIL: "bin\Win32_Release\liblz4_static.lib" && goto :ERROR )
if not exist "bin\Win32_Release\lz4.exe"           ( echo FAIL: "bin\Win32_Release\lz4.exe"           && goto :ERROR )

echo msbuild "%sln%" /p:Configuration=Debug /p:Platform="x64"
msbuild "%sln%"                  ^
        /nologo                  ^
        /v:minimal               ^
        /m                       ^
        /p:Configuration=Debug   ^
        /p:Platform="x64"        ^
        /t:Clean,Build           ^
        || goto :ERROR

if not exist "bin\x64_Debug\datagen.exe"       ( echo FAIL: "bin\x64_Debug\datagen.exe"       && goto :ERROR )
if not exist "bin\x64_Debug\frametest.exe"     ( echo FAIL: "bin\x64_Debug\frametest.exe"     && goto :ERROR )
if not exist "bin\x64_Debug\fullbench-dll.exe" ( echo FAIL: "bin\x64_Debug\fullbench-dll.exe" && goto :ERROR )
if not exist "bin\x64_Debug\fullbench.exe"     ( echo FAIL: "bin\x64_Debug\fullbench.exe"     && goto :ERROR )
if not exist "bin\x64_Debug\fuzzer.exe"        ( echo FAIL: "bin\x64_Debug\fuzzer.exe"        && goto :ERROR )
if not exist "bin\x64_Debug\liblz4.dll"        ( echo FAIL: "bin\x64_Debug\liblz4.dll"        && goto :ERROR )
if not exist "bin\x64_Debug\liblz4.lib"        ( echo FAIL: "bin\x64_Debug\liblz4.lib"        && goto :ERROR )
if not exist "bin\x64_Debug\liblz4_static.lib" ( echo FAIL: "bin\x64_Debug\liblz4_static.lib" && goto :ERROR )
if not exist "bin\x64_Debug\lz4.exe"           ( echo FAIL: "bin\x64_Debug\lz4.exe"           && goto :ERROR )

echo msbuild "%sln%" /p:Configuration=Release /p:Platform="x64"
msbuild "%sln%"                  ^
        /nologo                  ^
        /v:minimal               ^
        /m                       ^
        /p:Configuration=Release ^
        /p:Platform="x64"        ^
        /t:Clean,Build           ^
        || goto :ERROR

if not exist "bin\x64_Release\datagen.exe"       ( echo FAIL: "bin\x64_Release\datagen.exe"       && goto :ERROR )
if not exist "bin\x64_Release\frametest.exe"     ( echo FAIL: "bin\x64_Release\frametest.exe"     && goto :ERROR )
if not exist "bin\x64_Release\fullbench-dll.exe" ( echo FAIL: "bin\x64_Release\fullbench-dll.exe" && goto :ERROR )
if not exist "bin\x64_Release\fullbench.exe"     ( echo FAIL: "bin\x64_Release\fullbench.exe"     && goto :ERROR )
if not exist "bin\x64_Release\fuzzer.exe"        ( echo FAIL: "bin\x64_Release\fuzzer.exe"        && goto :ERROR )
if not exist "bin\x64_Release\liblz4.dll"        ( echo FAIL: "bin\x64_Release\liblz4.dll"        && goto :ERROR )
if not exist "bin\x64_Release\liblz4.lib"        ( echo FAIL: "bin\x64_Release\liblz4.lib"        && goto :ERROR )
if not exist "bin\x64_Release\liblz4_static.lib" ( echo FAIL: "bin\x64_Release\liblz4_static.lib" && goto :ERROR )
if not exist "bin\x64_Release\lz4.exe"           ( echo FAIL: "bin\x64_Release\lz4.exe"           && goto :ERROR )

echo Build Status -%esc%[92m SUCCEEDED %esc%[0m
set /a errorno=0
goto :END


:ERROR
echo Abort by error.
echo Build Status -%esc%[91m ERROR %esc%[0m


:END
exit /B %errorno%
