@REM Snapmaker_Orca build script for Windows
@echo off
set WP=%CD%
if not defined ORCA_PYTHON_PLUGINS set "ORCA_PYTHON_PLUGINS=OFF"
if /I not "%ORCA_PYTHON_PLUGINS%"=="ON" if /I not "%ORCA_PYTHON_PLUGINS%"=="OFF" (
    echo ORCA_PYTHON_PLUGINS must be ON or OFF.
    exit /b 1
)

@REM Pack deps
if "%1"=="pack" (
    setlocal ENABLEDELAYEDEXPANSION 
    cd %WP%/deps/build
    for /f "tokens=2-4 delims=/ " %%a in ('date /t') do set build_date=%%c%%b%%a
    echo packing deps: OrcaSlicer_dep_win64_!build_date!_vs2022.zip

    %WP%/tools/7z.exe a OrcaSlicer_dep_win64_!build_date!_vs2022.zip OrcaSlicer_dep
    exit /b 0
)

set debug=OFF
set debuginfo=OFF
if "%1"=="debug" set debug=ON
if "%2"=="debug" set debug=ON
if "%1"=="debuginfo" set debuginfo=ON
if "%2"=="debuginfo" set debuginfo=ON
if "%debug%"=="ON" (
    set build_type=Debug
    set build_dir=build-dbg
) else (
    if "%debuginfo%"=="ON" (
        set build_type=RelWithDebInfo
        set build_dir=build-dbginfo
    ) else (
        set build_type=Release
        set build_dir=build
    )
)
echo build type set to %build_type%

setlocal DISABLEDELAYEDEXPANSION 
cd deps
mkdir %build_dir%
cd %build_dir%
set DEPS=%CD%/OrcaSlicer_dep
set "SIG_FLAG="
if defined ORCA_UPDATER_SIG_KEY set "SIG_FLAG=-DORCA_UPDATER_SIG_KEY=%ORCA_UPDATER_SIG_KEY%"

if "%1"=="slicer" (
    GOTO :slicer
)
echo "building deps.."

echo on
cmake ../ -G "Visual Studio 17 2022" -A x64 -DDESTDIR="%DEPS%" -DCMAKE_BUILD_TYPE=%build_type% -DDEP_DEBUG=%debug% -DORCA_INCLUDE_DEBUG_INFO=%debuginfo%
cmake --build . --config %build_type% --target deps -- -m
if errorlevel 1 exit /b 1
if not exist "%DEPS%\usr\local\lib\libcrypto.lib" goto openssl_failed
if not exist "%DEPS%\usr\local\lib\libssl.lib" goto openssl_failed
if not exist "%DEPS%\usr\local\include\openssl\opensslv.h" goto openssl_failed
goto openssl_checked

:openssl_failed
    echo OpenSSL dependency was not built successfully.
    exit /b 1

:openssl_checked
@echo off

if "%1"=="deps" exit /b 0

:slicer
echo "building Snapmaker Orca..."
cd %WP%
mkdir %build_dir%
cd %build_dir%

set "PLUGIN_CMAKE_ARGS=-DORCA_PYTHON_PLUGINS=OFF -DORCA_LOCAL_PLUGIN_SPIKE=OFF"
if /I "%ORCA_PYTHON_PLUGINS%"=="ON" (
    if not defined ORCA_PLUGIN_PYTHON_ROOT echo Set ORCA_PLUGIN_PYTHON_ROOT to CPython 3.12.13.
    if not defined ORCA_PLUGIN_PYTHON_ROOT exit /b 1
    if not defined ORCA_PLUGIN_UV_EXECUTABLE echo Set ORCA_PLUGIN_UV_EXECUTABLE to uv 0.11.16.
    if not defined ORCA_PLUGIN_UV_EXECUTABLE exit /b 1
    python "%WP%\scripts\packaging\integrate_plugin_runtime.py" prepare --platform windows --architecture x86_64 --python-root "%ORCA_PLUGIN_PYTHON_ROOT%" --uv "%ORCA_PLUGIN_UV_EXECUTABLE%" --work-dir "%CD%\plugin-release"
    if errorlevel 1 exit /b 1
    set PLUGIN_CMAKE_ARGS=-C "%CD%\plugin-release\plugin-options.cmake"
)

python ..\scripts\packaging\stage_connect_bundle.py --lock ..\.github\connect-bundle.lock.json --platform windows-x64 --resources-dir ..\resources
if errorlevel 1 exit /b 1

echo on
cmake .. -G "Visual Studio 17 2022" -A x64 -DBBL_RELEASE_TO_PUBLIC=1 -DORCA_TOOLS=ON %PLUGIN_CMAKE_ARGS% %SIG_FLAG% -DCMAKE_PREFIX_PATH="%DEPS%/usr/local" -DOPENSSL_ROOT_DIR="%DEPS%/usr/local" -DCMAKE_INSTALL_PREFIX="./Snapmaker_Orca" -DCMAKE_BUILD_TYPE=%build_type% -DWIN10SDK_PATH="%WindowsSdkDir%Include\%WindowsSDKVersion%\"
if errorlevel 1 exit /b 1
cmake --build . --config %build_type% --target ALL_BUILD -- -m
if errorlevel 1 exit /b 1
@echo off
cd ..
call scripts/run_gettext.bat
cd %build_dir%
cmake --build . --target install --config %build_type%
if errorlevel 1 exit /b 1
if /I "%ORCA_PYTHON_PLUGINS%"=="ON" (
    python "%WP%\scripts\packaging\integrate_plugin_runtime.py" deploy --work-dir "%CD%\plugin-release" --app-dir "%CD%\Snapmaker_Orca" --host Snapmaker_Orca.dll --host snapmaker-orca.exe --cache "%CD%\CMakeCache.txt" --cpack-config "%CD%\CPackConfig.cmake"
    if errorlevel 1 exit /b 1
) else (
    if exist "Snapmaker_Orca\plugin-runtime.json" (
        echo Stale plugin runtime in install tree; remove the build\Snapmaker_Orca directory and rerun the OFF build.
        exit /b 1
    )
)
