Set-ExecutionPolicy -ExecutionPolicy Bypass -Scope Process
Write-Host "Starting build script..." -ForegroundColor Cyan
Start-Sleep -Seconds 1

try {
    $ErrorActionPreference = "Stop"

    Write-Host "USDTweak Automated Build Script" -ForegroundColor Cyan
    Write-Host "================================" -ForegroundColor Cyan
    Write-Host ""
    Write-Host "Prerequisites:" -ForegroundColor White
    Write-Host "  - Git          https://git-scm.com/download/win" -ForegroundColor White
    Write-Host "  - CMake 3.14+  https://cmake.org/download/" -ForegroundColor White
    Write-Host "  - Visual Studio 2022 with C++ desktop workload" -ForegroundColor White
    Write-Host "  - ~3 GB free disk space" -ForegroundColor White
    Write-Host ""

    # Function to check if a command exists
    function Test-Command($cmdname) {
        return [bool](Get-Command -Name $cmdname -ErrorAction SilentlyContinue)
    }

    # Check git
    if (-not (Test-Command "git")) {
        Write-Host "Error: Git is not installed or not in PATH." -ForegroundColor Red
        Write-Host "Download from: https://git-scm.com/download/win" -ForegroundColor Yellow
        pause
        exit 1
    }

    # Check cmake
    if (-not (Test-Command "cmake")) {
        Write-Host "Error: CMake is not installed or not in PATH." -ForegroundColor Red
        Write-Host "Download from: https://cmake.org/download/" -ForegroundColor Yellow
        pause
        exit 1
    }

    # Check cmake version
    $cmakeVersionOutput = & cmake --version 2>&1 | Select-Object -First 1
    if ($cmakeVersionOutput -match "(\d+\.\d+)") {
        $cmakeVer = [version]$Matches[1]
        if ($cmakeVer -lt [version]"3.14") {
            Write-Host "Error: CMake 3.14 or higher is required (found $cmakeVersionOutput)." -ForegroundColor Red
            Write-Host "Download from: https://cmake.org/download/" -ForegroundColor Yellow
            pause
            exit 1
        }
        Write-Host "Found CMake $($Matches[1])" -ForegroundColor Green
    }

    # Check Visual Studio 2022 with C++ workload
    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    if (-not (Test-Path $vswhere)) {
        Write-Host "Error: Visual Studio Installer not found. Visual Studio 2022 is required." -ForegroundColor Red
        Write-Host "Download from: https://visualstudio.microsoft.com/downloads/" -ForegroundColor Yellow
        pause
        exit 1
    }

    $vsInstall = & $vswhere -latest -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath 2>$null
    if (-not $vsInstall) {
        $vsAny = & $vswhere -latest -property installationPath 2>$null
        if ($vsAny) {
            Write-Host "Error: Visual Studio found at '$vsAny' but the C++ desktop development workload is not installed." -ForegroundColor Red
            Write-Host "Open Visual Studio Installer and add 'Desktop development with C++'." -ForegroundColor Yellow
        } else {
            Write-Host "Error: Visual Studio 2022 is not installed." -ForegroundColor Red
            Write-Host "Download from: https://visualstudio.microsoft.com/downloads/" -ForegroundColor Yellow
        }
        pause
        exit 1
    }

    $vsVersion = & $vswhere -latest -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property catalog_productLineVersion 2>$null
    if ($vsVersion -ne "2022") {
        Write-Host "Error: Visual Studio 2022 is required (found VS $vsVersion)." -ForegroundColor Red
        Write-Host "Download from: https://visualstudio.microsoft.com/downloads/" -ForegroundColor Yellow
        pause
        exit 1
    }
    Write-Host "Found Visual Studio 2022 with C++ workload" -ForegroundColor Green

    # Check disk space (~3 GB needed)
    $scriptDrive = (Resolve-Path $PSScriptRoot).Drive
    $freeGB = [math]::Round((Get-PSDrive $scriptDrive.Name).Free / 1GB, 1)
    if ($freeGB -lt 3) {
        Write-Host "Error: Insufficient disk space. Need ~3 GB free, only $freeGB GB available on drive $($scriptDrive.Name):." -ForegroundColor Red
        pause
        exit 1
    }
    Write-Host "Disk space: $freeGB GB free" -ForegroundColor Green

    Write-Host "`nAll prerequisites satisfied." -ForegroundColor Green

    # Create build directory
    $buildDir = Join-Path $PSScriptRoot "build"
    New-Item -ItemType Directory -Force -Path $buildDir | Out-Null

    # Define USD directory path
    $usdDir = Join-Path $buildDir "usd"

    # USD 25.08 with Python 3.12 from NVIDIA
    $usdVersion = "25.08"
    $pythonVersion = "3.12"
    $pythonLibVersion = "312"
    $usdUrl = "https://developer.nvidia.com/downloads/usd/usd_binaries/25.08/usd.py312.windows-x86_64.usdview.release-v25.08.71e038c1.zip"

    # Check if USD directory exists and has the expected version
    $usdPresent = $false
    if (Test-Path $usdDir) {
        if (Test-Path (Join-Path $usdDir "pxrConfig.cmake")) {
            Write-Host "`nFound existing USD installation in build/usd" -ForegroundColor Green
            $usdPresent = $true
        } else {
            Write-Host "`nUSD directory exists but appears incomplete, re-downloading..." -ForegroundColor Yellow
            Remove-Item -Path $usdDir -Recurse -Force
        }
    }

    if (-not $usdPresent) {
        New-Item -ItemType Directory -Force -Path $usdDir | Out-Null

        $usdZip = Join-Path $buildDir "usd.zip"

        Write-Host "`nDownloading USD $usdVersion from NVIDIA (~464 MB)..." -ForegroundColor Yellow
        Write-Host "This may take several minutes depending on your connection." -ForegroundColor DarkGray
        try {
            $ProgressPreference = 'SilentlyContinue'
            Invoke-WebRequest -Uri $usdUrl -OutFile $usdZip
            $ProgressPreference = 'Continue'
        } catch {
            Write-Host "Error: Failed to download USD. Please check your internet connection or download manually from:" -ForegroundColor Red
            Write-Host $usdUrl -ForegroundColor Yellow
            pause
            exit 1
        }

        # Verify the download isn't empty or truncated
        if (-not (Test-Path $usdZip) -or (Get-Item $usdZip).Length -lt 100000000) {
            Write-Host "Error: Downloaded file appears to be incomplete or corrupted. Please try again." -ForegroundColor Red
            if (Test-Path $usdZip) { Remove-Item -Path $usdZip -Force }
            pause
            exit 1
        }

        # Extract USD
        Write-Host "Extracting USD..." -ForegroundColor Yellow
        $sevenZip = "${env:ProgramFiles}\7-Zip\7z.exe"
        if (Test-Path $sevenZip) {
            Write-Host "Using 7-Zip for extraction..." -ForegroundColor Green
            $extractProcess = Start-Process -FilePath $sevenZip -ArgumentList "x", "`"$usdZip`"", "-o`"$usdDir`"", "-y" -NoNewWindow -Wait -PassThru
            if ($extractProcess.ExitCode -ne 0) {
                Write-Host "7-Zip extraction failed, falling back to PowerShell extraction..." -ForegroundColor Yellow
                Expand-Archive -Path $usdZip -DestinationPath $usdDir -Force
            }
        } else {
            Write-Host "7-Zip not found, using PowerShell extraction (this may be slow for large archives)..." -ForegroundColor Yellow
            Expand-Archive -Path $usdZip -DestinationPath $usdDir -Force
        }

        # Remove the zip file after extraction
        if (Test-Path $usdZip) {
            Remove-Item -Path $usdZip -Force
        }

        # Verify extraction succeeded
        if (-not (Test-Path (Join-Path $usdDir "pxrConfig.cmake"))) {
            Write-Host "Error: USD extraction failed - pxrConfig.cmake not found in build/usd." -ForegroundColor Red
            Write-Host "Try deleting the build/usd directory and running this script again." -ForegroundColor Yellow
            pause
            exit 1
        }

        Write-Host "USD $usdVersion extracted successfully" -ForegroundColor Green
    } else {
        Write-Host "Using existing USD installation in build/usd" -ForegroundColor Green
    }

    # ----------------------------------------------------------------
    # The NVIDIA USD package ships TBB, OpenSubdiv, and Imath libraries
    # but does not include CMake config files for them. The pxrConfig.cmake
    # expects to find these via find_dependency(), so we create minimal
    # config files that define the required imported targets.
    # ----------------------------------------------------------------

    # Create TBB CMake config
    $tbbCmakeDir = Join-Path $usdDir "lib\cmake\TBB"
    if (-not (Test-Path (Join-Path $tbbCmakeDir "TBBConfig.cmake"))) {
        Write-Host "Creating TBB CMake config..." -ForegroundColor Yellow
        New-Item -ItemType Directory -Force -Path $tbbCmakeDir | Out-Null

        @'
get_filename_component(_TBB_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../.." ABSOLUTE)

if(NOT TARGET TBB::tbb)
    add_library(TBB::tbb SHARED IMPORTED)
    set_target_properties(TBB::tbb PROPERTIES
        IMPORTED_IMPLIB "${_TBB_ROOT}/lib/tbb.lib"
        IMPORTED_LOCATION "${_TBB_ROOT}/bin/tbb.dll"
        INTERFACE_INCLUDE_DIRECTORIES "${_TBB_ROOT}/include"
    )
endif()

set(TBB_FOUND TRUE)
set(TBB_VERSION "2020.3")
'@ | Set-Content (Join-Path $tbbCmakeDir "TBBConfig.cmake") -Encoding UTF8

        @'
set(PACKAGE_VERSION "2020.3")
if("${PACKAGE_FIND_VERSION}" VERSION_GREATER "${PACKAGE_VERSION}")
    set(PACKAGE_VERSION_COMPATIBLE FALSE)
else()
    set(PACKAGE_VERSION_COMPATIBLE TRUE)
    if("${PACKAGE_FIND_VERSION}" VERSION_EQUAL "${PACKAGE_VERSION}")
        set(PACKAGE_VERSION_EXACT TRUE)
    endif()
endif()
'@ | Set-Content (Join-Path $tbbCmakeDir "TBBConfigVersion.cmake") -Encoding UTF8
    }

    # Create OpenSubdiv CMake config
    $osdCmakeDir = Join-Path $usdDir "lib\cmake\OpenSubdiv"
    if (-not (Test-Path (Join-Path $osdCmakeDir "OpenSubdivConfig.cmake"))) {
        Write-Host "Creating OpenSubdiv CMake config..." -ForegroundColor Yellow
        New-Item -ItemType Directory -Force -Path $osdCmakeDir | Out-Null

        @'
get_filename_component(_OSD_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../.." ABSOLUTE)

if(NOT TARGET opensubdiv::opensubdiv)
    add_library(opensubdiv::opensubdiv INTERFACE IMPORTED)

    add_library(opensubdiv::osdCPU STATIC IMPORTED)
    set_target_properties(opensubdiv::osdCPU PROPERTIES
        IMPORTED_LOCATION "${_OSD_ROOT}/lib/osdCPU.lib"
    )

    add_library(opensubdiv::osdGPU STATIC IMPORTED)
    set_target_properties(opensubdiv::osdGPU PROPERTIES
        IMPORTED_LOCATION "${_OSD_ROOT}/lib/osdGPU.lib"
    )

    set_target_properties(opensubdiv::opensubdiv PROPERTIES
        INTERFACE_LINK_LIBRARIES "opensubdiv::osdCPU;opensubdiv::osdGPU"
        INTERFACE_INCLUDE_DIRECTORIES "${_OSD_ROOT}/include"
    )
endif()

set(OpenSubdiv_FOUND TRUE)
set(OpenSubdiv_VERSION "3.6.0")
'@ | Set-Content (Join-Path $osdCmakeDir "OpenSubdivConfig.cmake") -Encoding UTF8

        @'
set(PACKAGE_VERSION "3.6.0")
if("${PACKAGE_FIND_VERSION}" VERSION_GREATER "${PACKAGE_VERSION}")
    set(PACKAGE_VERSION_COMPATIBLE FALSE)
else()
    set(PACKAGE_VERSION_COMPATIBLE TRUE)
    if("${PACKAGE_FIND_VERSION}" VERSION_EQUAL "${PACKAGE_VERSION}")
        set(PACKAGE_VERSION_EXACT TRUE)
    endif()
endif()
'@ | Set-Content (Join-Path $osdCmakeDir "OpenSubdivConfigVersion.cmake") -Encoding UTF8
    }

    # Create Imath CMake config
    $imathCmakeDir = Join-Path $usdDir "lib\cmake\Imath"
    if (-not (Test-Path (Join-Path $imathCmakeDir "ImathConfig.cmake"))) {
        Write-Host "Creating Imath CMake config..." -ForegroundColor Yellow
        New-Item -ItemType Directory -Force -Path $imathCmakeDir | Out-Null

        @'
get_filename_component(_IMATH_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../.." ABSOLUTE)

if(NOT TARGET Imath::Imath)
    add_library(Imath::Imath SHARED IMPORTED)
    set_target_properties(Imath::Imath PROPERTIES
        IMPORTED_IMPLIB "${_IMATH_ROOT}/lib/Imath-3_1.lib"
        IMPORTED_LOCATION "${_IMATH_ROOT}/bin/Imath-3_1.dll"
        INTERFACE_INCLUDE_DIRECTORIES "${_IMATH_ROOT}/include;${_IMATH_ROOT}/include/Imath"
    )
endif()

if(NOT TARGET Imath::ImathConfig)
    add_library(Imath::ImathConfig INTERFACE IMPORTED)
    set_target_properties(Imath::ImathConfig PROPERTIES
        INTERFACE_INCLUDE_DIRECTORIES "${_IMATH_ROOT}/include;${_IMATH_ROOT}/include/Imath"
    )
endif()

set(Imath_FOUND TRUE)
set(Imath_VERSION "3.1.12")
'@ | Set-Content (Join-Path $imathCmakeDir "ImathConfig.cmake") -Encoding UTF8

        @'
set(PACKAGE_VERSION "3.1.12")
if("${PACKAGE_FIND_VERSION}" VERSION_GREATER "${PACKAGE_VERSION}")
    set(PACKAGE_VERSION_COMPATIBLE FALSE)
else()
    set(PACKAGE_VERSION_COMPATIBLE TRUE)
    if("${PACKAGE_FIND_VERSION}" VERSION_EQUAL "${PACKAGE_VERSION}")
        set(PACKAGE_VERSION_EXACT TRUE)
    endif()
endif()
'@ | Set-Content (Join-Path $imathCmakeDir "ImathConfigVersion.cmake") -Encoding UTF8
    }

    # Move to build directory
    Push-Location $buildDir

    # Configure with CMake
    Write-Host "`nConfiguring with CMake..." -ForegroundColor Yellow
    & cmake `
        -G "Visual Studio 17 2022" `
        -A x64 `
        "-DCMAKE_PREFIX_PATH=$usdDir\lib\cmake" `
        "-Dpxr_DIR=$usdDir" `
        "-DMaterialX_DIR=$usdDir\lib\cmake\MaterialX" `
        "-DImath_DIR=$usdDir\lib\cmake\Imath" `
        "-DPython3_EXECUTABLE=$usdDir\python\python.exe" `
        "-DPython3_LIBRARY=$usdDir\python\libs\python${pythonLibVersion}.lib" `
        "-DPython3_INCLUDE_DIR=$usdDir\python\include" `
        "-DPython3_VERSION=$pythonVersion" `
        ..
    if ($LASTEXITCODE -ne 0) {
        Write-Host "Error: CMake configuration failed." -ForegroundColor Red
        Write-Host "Check the output above for details." -ForegroundColor Yellow
        pause
        exit 1
    }

    # Build the project
    Write-Host "`nBuilding project..." -ForegroundColor Yellow
    & cmake --build . --config RelWithDebInfo --parallel
    if ($LASTEXITCODE -ne 0) {
        Write-Host "Error: Build failed." -ForegroundColor Red
        Write-Host "Check the compiler output above for details." -ForegroundColor Yellow
        pause
        exit 1
    }

    # Return to original directory
    Pop-Location

    # Verify the executable was produced
    $exePath = Join-Path $buildDir "RelWithDebInfo\usdtweak.exe"
    if (-not (Test-Path $exePath)) {
        Write-Host "Error: Build reported success but usdtweak.exe was not found at:" -ForegroundColor Red
        Write-Host "  $exePath" -ForegroundColor Yellow
        pause
        exit 1
    }

    Write-Host "`nBuild completed successfully!" -ForegroundColor Green
    Write-Host "Executable: $exePath" -ForegroundColor Green
    Write-Host "Run usdtweak with: .\windows-start.bat" -ForegroundColor Green
} catch {
    Write-Host "`nError occurred:" -ForegroundColor Red
    Write-Host $_.Exception.Message -ForegroundColor Red
    Write-Host "`nStack trace:" -ForegroundColor Red
    Write-Host $_.ScriptStackTrace -ForegroundColor Red
    pause
    exit 1
} finally {
    if ($buildDir -and (Get-Location).Path -eq $buildDir) {
        Pop-Location
    }
}

Write-Host "`nPress any key to exit..." -ForegroundColor Cyan
pause
