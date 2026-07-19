@echo off
setlocal EnableExtensions

cd /d "%~dp0"

set "BUILD_DIR=build-windows"
set "CONFIG=Release"
set "BUILD_POSTGRES=OFF"
set "CTEST_PARALLEL=--parallel"

if /I "%~1"=="postgres" (
    set "BUILD_POSTGRES=ON"
    set "CTEST_PARALLEL=--parallel 1"
)
if not "%~1"=="" if /I not "%~1"=="postgres" goto :usage

where cmake >nul 2>nul
if errorlevel 1 (
    echo ERROR: cmake was not found in PATH.
    exit /b 1
)

where python >nul 2>nul
if errorlevel 1 (
    echo ERROR: python was not found in PATH. MCP stdio tests require it.
    exit /b 1
)

if not defined OPENSSL_ROOT_DIR (
    if exist "C:\Program Files\OpenSSL-Win64\include\openssl\ssl.h" (
        set "OPENSSL_ROOT_DIR=C:\Program Files\OpenSSL-Win64"
    )
)

if not defined OPENSSL_ROOT_DIR (
    echo ERROR: OpenSSL was not found.
    echo Install it with: choco install openssl -y
    echo Or set OPENSSL_ROOT_DIR to the OpenSSL installation directory.
    exit /b 1
)

echo Configuring NeoGraph in %BUILD_DIR%...
cmake -S . -B "%BUILD_DIR%" ^
    -G "Visual Studio 17 2022" -A x64 ^
    -DOPENSSL_ROOT_DIR="%OPENSSL_ROOT_DIR%" ^
    -DNEOGRAPH_USE_LIBCURL=OFF ^
    -DNEOGRAPH_BUILD_LLM=ON ^
    -DNEOGRAPH_BUILD_MCP=ON ^
    -DNEOGRAPH_BUILD_UTIL=ON ^
    -DNEOGRAPH_BUILD_POSTGRES=%BUILD_POSTGRES% ^
    -DNEOGRAPH_BUILD_SQLITE=OFF ^
    -DNEOGRAPH_BUILD_TESTS=ON ^
    -DNEOGRAPH_BUILD_EXAMPLES=OFF
if errorlevel 1 exit /b 1

echo Building NeoGraph %CONFIG%...
cmake --build "%BUILD_DIR%" --config "%CONFIG%" --parallel
if errorlevel 1 exit /b 1

echo Running tests...
ctest --test-dir "%BUILD_DIR%" -C "%CONFIG%" --output-on-failure %CTEST_PARALLEL%
if errorlevel 1 exit /b 1

echo All Windows tests passed.
exit /b 0

:usage
echo Usage: test.bat [postgres]
echo   postgres  Build PostgreSQL support. Set NEOGRAPH_TEST_POSTGRES_URL
echo             to run the PostgreSQL integration tests against a live server.
exit /b 2
