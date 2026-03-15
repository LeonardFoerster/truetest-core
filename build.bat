@echo off
setlocal

set BUILD_DIR=build

cmake -S . -B %BUILD_DIR%
if %ERRORLEVEL% neq 0 (
    echo.
    echo CMake configure failed.
    echo Make sure a C++ compiler is on your PATH (MSVC, MinGW, etc.) and that
    echo PostgreSQL is installed with its bin directory on PATH.
    exit /b %ERRORLEVEL%
)

cmake --build %BUILD_DIR% --config Debug
if %ERRORLEVEL% neq 0 (
    echo.
    echo Build failed.
    exit /b %ERRORLEVEL%
)

echo.
echo Build complete.
endlocal
