@echo off
setlocal

set "SRC_FILE=%~1"
set "BUILD_DIR=%~2"
if "%SRC_FILE%"=="" set "SRC_FILE=.\test_program.c"
if "%BUILD_DIR%"=="" set "BUILD_DIR=.\build"
if not exist "%BUILD_DIR%" mkdir "%BUILD_DIR%"

set "PASS_LIB=.\build\lib\ChakravyuhaStringEncryptionPass.dll"
set "LLVM_IR=%BUILD_DIR%\test_program.ll"
set "OBFUSCATED_IR=%BUILD_DIR%\obfuscated.ll"
set "FINAL_BINARY=%BUILD_DIR%\obfuscated_program.exe"
set "REPORT_FILE=%BUILD_DIR%\report.json"

if not exist "%PASS_LIB%" ( echo Error: Pass library not found at %PASS_LIB% >&2 & exit /b 1 )

clang -O0 -S -emit-llvm "%SRC_FILE%" -o "%LLVM_IR%"
if %errorlevel% neq 0 exit /b %errorlevel%

rem
opt -load-pass-plugin="%PASS_LIB%" -passes=chakravyuha-string-encrypt ^
    "%LLVM_IR%" -S -o "%OBFUSCATED_IR%" > "%REPORT_FILE%" 2>nul
if %errorlevel% neq 0 exit /b %errorlevel%

clang "%OBFUSCATED_IR%" -o "%FINAL_BINARY%"
if %errorlevel% neq 0 exit /b %errorlevel%

endlocal
exit /b 0