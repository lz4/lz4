:: Requires 1 parameter == GENERATOR
@echo off
setlocal

:: Set paths for CMake and the build script directory
set "CMAKE_PATH=C:\Program Files\CMake\bin"
set "CMAKELIST_DIR=..\..\cmake"
set "BUILD_BASE_DIR=%~dp0"  :: Use the directory where the script is located

if "%~1"=="" (
    echo No generator specified as first parameter
    exit /b 1
)
set "GENERATOR=%~1"

:: Set the build directory to a subdirectory named after the generator
set "BUILD_DIR=%BUILD_BASE_DIR%\%GENERATOR%"

:: Create the build directory if it doesn't exist
if not exist "%BUILD_DIR%" mkdir "%BUILD_DIR%"

:: Run CMake to configure the project and generate the solution
pushd "%BUILD_DIR%"
"%CMAKE_PATH%\cmake.exe" -G "%GENERATOR%" "%CMAKELIST_DIR%"
if %ERRORLEVEL% neq 0 goto :error

:: If successful, end script
echo Build configuration successful for %GENERATOR%.
goto :end

:error
echo Failed to configure build for %GENERATOR%.
exit /b 1

:end
popd
endlocal
@echo on
