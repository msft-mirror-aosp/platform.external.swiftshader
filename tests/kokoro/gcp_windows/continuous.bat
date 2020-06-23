@echo on

SETLOCAL ENABLEDELAYEDEXPANSION

SET PATH=C:\python36;C:\Program Files\cmake\bin;%PATH%
SET SRC=%cd%\git\SwiftShader

cd %SRC% || goto :error

IF "%LLVM_VERSION%"=="10.0" (
  ECHO "TODO(b/152339534): LLVM 10 migration is still in progress"
  EXIT 0
)

# Lower the amount of debug info, to reduce Kokoro build times.
SET LESS_DEBUG_INFO=1

cd %SRC%\build || goto :error

REM Update CMake
choco upgrade cmake -y --limit-output --no-progress
cmake --version

cmake .. ^
    -G "Visual Studio 15 2017 Win64" ^
    -Thost=x64 ^
    "-DCMAKE_BUILD_TYPE=%BUILD_TYPE%" ^
    "-DREACTOR_BACKEND=%REACTOR_BACKEND%" ^
    "-DSWIFTSHADER_LLVM_VERSION=%LLVM_VERSION%" ^
    "-DREACTOR_VERIFY_LLVM_IR=1" ^
    "-DLESS_DEBUG_INFO=%LESS_DEBUG_INFO%" || goto :error

cmake --build . || goto :error

REM Run the unit tests. Some must be run from project root
cd %SRC% || goto :error
SET SWIFTSHADER_DISABLE_DEBUGGER_WAIT_DIALOG=1

build\Debug\ReactorUnitTests.exe || goto :error
build\Debug\gles-unittests.exe || goto :error
build\Debug\system-unittests.exe || goto :error
build\Debug\vk-unittests.exe || goto :error

REM Incrementally build and run rr::Print unit tests
cd %SRC%\build || goto :error
cmake "-DREACTOR_ENABLE_PRINT=1" .. || goto :error
cmake --build . --target ReactorUnitTests || goto :error
cd %SRC% || goto :error
build\Debug\ReactorUnitTests.exe --gtest_filter=ReactorUnitTests.Print* || goto :error

exit /b 0

:error
exit /b !ERRORLEVEL!
