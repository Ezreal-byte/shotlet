# Shotlet Windows Static Build Script
# Prerequisites: CMake 3.20+, Visual Studio 2019/2022/2026
# Dependencies: ./imgui, ./sdl3

param(
    [Parameter(ValueFromRemainingArguments = $true)]
    [string[]]$BuildArgs
)

$ErrorActionPreference = "Stop"
$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
Set-Location $ScriptDir

function Show-Usage {
    Write-Host "Usage:"
    Write-Host "  .\build.ps1"
    Write-Host "  .\build.ps1 --app|-a|app"
    Write-Host "  .\build.ps1 --node|-n|node"
    Write-Host "  .\build.ps1 --tests|-t|tests"
    Write-Host "  .\build.ps1 --all|all"
    Write-Host ""
    Write-Host "Commands:"
    Write-Host "  --app, -a, app       build shotlet app/executable"
    Write-Host "  --node, -n, node     build Node-API addon"
    Write-Host "  --tests, -t, tests   build shotlet_tests"
    Write-Host "  --all, all           build app, Node addon, and tests"
    Write-Host ""
    Write-Host "Outputs:"
    Write-Host "  app                  build/Release/shotlet.exe"
    Write-Host "  node                 build/Release/shotlet.node"
    Write-Host "  tests                build/tests/Release/shotlet_tests.exe"
}

function Test-CMakeGeneratorSupportsPlatform {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Generator
    )

    return $Generator -match "^Visual Studio"
}

function Find-VisualStudioGenerator {
    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    if (-not (Test-Path $vswhere)) {
        return $null
    }

    $installationVersions = & $vswhere -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationVersion 2>$null
    if (-not $installationVersions) {
        return $null
    }

    $cmakeHelp = cmake --help 2>$null
    if (-not $cmakeHelp) {
        return $null
    }

    $installedMajorVersions = @($installationVersions |
        ForEach-Object { [int]($_ -split "\.")[0] } |
        Sort-Object -Descending)

    $generatorByMajorVersion = @{
        18 = "Visual Studio 18 2026"
        17 = "Visual Studio 17 2022"
        16 = "Visual Studio 16 2019"
    }

    foreach ($majorVersion in $installedMajorVersions) {
        $candidate = $generatorByMajorVersion[$majorVersion]
        if ($candidate -and ($cmakeHelp -match [regex]::Escape($candidate))) {
            return $candidate
        }
    }

    return $null
}

function Get-CachedCMakeGenerator {
    param(
        [Parameter(Mandatory = $true)]
        [string]$CachePath
    )

    if (-not (Test-Path $CachePath)) {
        return $null
    }

    $generatorLine = Get-Content -Path $CachePath |
        Where-Object { $_ -like "CMAKE_GENERATOR:INTERNAL=*" } |
        Select-Object -First 1

    if (-not $generatorLine) {
        return $null
    }

    return $generatorLine.Substring("CMAKE_GENERATOR:INTERNAL=".Length)
}

function Get-CachedCMakeValue {
    param(
        [Parameter(Mandatory = $true)]
        [string]$CachePath,
        [Parameter(Mandatory = $true)]
        [string]$Name
    )

    if (-not (Test-Path $CachePath)) {
        return $null
    }

    $line = Get-Content -Path $CachePath |
        Where-Object { $_ -match "^$([regex]::Escape($Name)):[^=]*=" } |
        Select-Object -First 1

    if (-not $line) {
        return $null
    }

    return ($line -split "=", 2)[1]
}

function Clear-CMakeConfigurationCache {
    param(
        [Parameter(Mandatory = $true)]
        [string]$BuildDir,
        [Parameter(Mandatory = $true)]
        [string]$Reason
    )

    Write-Host $Reason -ForegroundColor DarkYellow
    Write-Host "Clearing CMake configuration cache so it can be regenerated." -ForegroundColor DarkYellow

    $cachePath = Join-Path $BuildDir "CMakeCache.txt"
    if (Test-Path $cachePath) {
        Remove-Item -LiteralPath $cachePath -Force
    }

    $cmakeFilesPath = Join-Path $BuildDir "CMakeFiles"
    if (Test-Path $cmakeFilesPath) {
        Remove-Item -LiteralPath $cmakeFilesPath -Recurse -Force
    }
}

function Find-VcpkgToolchain {
    if ($env:VCPKG_ROOT) {
        $toolchain = Join-Path $env:VCPKG_ROOT "scripts\buildsystems\vcpkg.cmake"
        if (Test-Path $toolchain) {
            return (Resolve-Path $toolchain).Path
        }
    }

    $vcpkgCommand = Get-Command vcpkg -ErrorAction SilentlyContinue
    if ($vcpkgCommand) {
        $vcpkgRoot = Split-Path -Parent $vcpkgCommand.Source
        $toolchain = Join-Path $vcpkgRoot "scripts\buildsystems\vcpkg.cmake"
        if (Test-Path $toolchain) {
            return (Resolve-Path $toolchain).Path
        }
    }

    return $null
}

function Find-NodeIncludeDir {
    $candidates = @()

    if ($env:NODE_INCLUDE_DIR) {
        $candidates += $env:NODE_INCLUDE_DIR
    }

    $nodeCommand = Get-Command node -ErrorAction SilentlyContinue
    if ($nodeCommand) {
        $nodeRoot = Split-Path -Parent $nodeCommand.Source
        $candidates += "$nodeRoot\include\node"
    }

    $nodeVersion = $null
    if ($nodeCommand) {
        $nodeVersion = (& $nodeCommand.Source -p "process.versions.node" 2>$null)
    }

    $nodeGypRoots = @(
        "$env:LOCALAPPDATA\node-gyp\Cache",
        "$env:USERPROFILE\.node-gyp"
    )

    foreach ($root in $nodeGypRoots) {
        if (-not (Test-Path $root)) {
            continue
        }

        if ($nodeVersion) {
            $candidates += "$root\$nodeVersion\include\node"
        }

        Get-ChildItem -Path $root -Directory -ErrorAction SilentlyContinue |
            Sort-Object Name -Descending |
            ForEach-Object { $candidates += "$($_.FullName)\include\node" }
    }

    foreach ($candidate in $candidates | Select-Object -Unique) {
        if ($candidate -and (Test-Path (Join-Path $candidate "node_api.h"))) {
            return $candidate
        }
    }

    return $null
}

function Find-NodeLibrary {
    $candidates = @()

    if ($env:NODE_LIBRARY) {
        $candidates += $env:NODE_LIBRARY
    }

    $nodeCommand = Get-Command node -ErrorAction SilentlyContinue
    $nodeVersion = $null
    if ($nodeCommand) {
        $nodeVersion = (& $nodeCommand.Source -p "process.versions.node" 2>$null)
    }

    $nodeGypRoots = @(
        "$env:LOCALAPPDATA\node-gyp\Cache",
        "$env:USERPROFILE\.node-gyp"
    )

    foreach ($root in $nodeGypRoots) {
        if (-not (Test-Path $root)) {
            continue
        }

        if ($nodeVersion) {
            $candidates += "$root\$nodeVersion\x64\node.lib"
        }

        Get-ChildItem -Path $root -Directory -ErrorAction SilentlyContinue |
            Sort-Object Name -Descending |
            ForEach-Object { $candidates += "$($_.FullName)\x64\node.lib" }
    }

    foreach ($candidate in $candidates | Select-Object -Unique) {
        if ($candidate -and (Test-Path $candidate)) {
            return $candidate
        }
    }

    return $null
}

$BuildApp = $true
$BuildNode = $false
$BuildTests = $false

if ($BuildArgs.Count -gt 0) {
    $BuildApp = $false
    foreach ($arg in $BuildArgs) {
        switch ($arg) {
            { $_ -in @("--app", "-a", "app") } {
                $BuildApp = $true
                break
            }
            { $_ -in @("--node", "-n", "node") } {
                $BuildNode = $true
                break
            }
            { $_ -in @("--tests", "-t", "tests") } {
                $BuildTests = $true
                break
            }
            { $_ -in @("--all", "all") } {
                $BuildApp = $true
                $BuildNode = $true
                $BuildTests = $true
                break
            }
            { $_ -in @("-h", "--help") } {
                Show-Usage
                exit 0
            }
            default {
                Write-Error "Unknown argument '$arg'"
                Show-Usage
                exit 1
            }
        }
    }
}

# ─── Check dependencies ───
$NodeIncludeDir = $null
$NodeLibrary = $null
if ($BuildNode) {
    $NodeIncludeDir = Find-NodeIncludeDir
    if (-not $NodeIncludeDir) {
        Write-Error "node_api.h not found. Install Node.js headers with 'node-gyp install' or set NODE_INCLUDE_DIR to a directory containing node_api.h."
        exit 1
    }

    $NodeLibrary = Find-NodeLibrary
    if (-not $NodeLibrary) {
        Write-Error "node.lib not found. Install Node.js headers with 'node-gyp install' or set NODE_LIBRARY to the Node.js import library path."
        exit 1
    }
}

$missing = @()
if (-not (Test-Path "./imgui/imgui.h"))  { $missing += "imgui" }
if (-not (Test-Path "./sdl3/include/SDL3/SDL.h")) { $missing += "sdl3" }
if ($missing.Count -gt 0) {
    Write-Error "Missing dependencies: $($missing -join ', ')"
    Write-Host "Expected structure:"
    Write-Host "  shotlet/"
    Write-Host "  ├── imgui/          (ImGui source)"
    Write-Host "  ├── sdl3/           (SDL3 source)"
    exit 1
}

# ─── Configure ───
$BuildDir = "$ScriptDir/build"
New-Item -ItemType Directory -Force -Path $BuildDir | Out-Null
Set-Location $BuildDir

Write-Host "=== Configuring Shotlet (Windows x64 Static) ===" -ForegroundColor Cyan

$BuildTestsOption = if ($BuildTests) { "ON" } else { "OFF" }
$BuildNodeOption = if ($BuildNode) { "ON" } else { "OFF" }
$MsvcRuntimeLibrary = if ($BuildTests -and -not $BuildApp -and -not $BuildNode) { "MultiThreadedDLL" } else { "MultiThreaded" }
$VcpkgToolchain = $null
$VcpkgTriplet = $null
if ($BuildTests) {
    $VcpkgToolchain = Find-VcpkgToolchain
    if (-not $VcpkgToolchain) {
        Write-Error "vcpkg toolchain not found. Set VCPKG_ROOT or add vcpkg.exe to PATH."
        exit 1
    }
    $VcpkgTriplet = if ($env:VCPKG_TARGET_TRIPLET) { $env:VCPKG_TARGET_TRIPLET } else { "x64-windows" }
}

$Generator = $env:CMAKE_GENERATOR
if (-not $Generator) {
    $Generator = Find-VisualStudioGenerator
}

$CMakeArgs = @(
    "$ScriptDir",
    "-DCMAKE_BUILD_TYPE=Release",
    "-DIMGUI_DIR=$ScriptDir/imgui",
    "-DSDL3_DIR=$ScriptDir/sdl3",
    "-DSHOTLET_BUILD_TESTS=$BuildTestsOption",
    "-DSHOTLET_BUILD_NODE_ADDON=$BuildNodeOption",
    "-DCMAKE_MSVC_RUNTIME_LIBRARY=$MsvcRuntimeLibrary"
)

if ($BuildTests) {
    $CMakeArgs += @(
        "-DCMAKE_TOOLCHAIN_FILE=$VcpkgToolchain",
        "-DVCPKG_TARGET_TRIPLET=$VcpkgTriplet"
    )
}

if ($Generator) {
    Write-Host "CMake generator: $Generator" -ForegroundColor DarkCyan

    $CachedGenerator = Get-CachedCMakeGenerator "$BuildDir/CMakeCache.txt"
    if ($CachedGenerator -and $CachedGenerator -ne $Generator) {
        Write-Error "Existing build directory uses CMake generator '$CachedGenerator', but this run selected '$Generator'. Remove '$BuildDir' and run build.ps1 again."
        exit 1
    }

    $CMakeArgs += @("-G", $Generator)

    $RequestedGeneratorPlatform = $null
    if (Test-CMakeGeneratorSupportsPlatform $Generator) {
        $RequestedGeneratorPlatform = "x64"
        $CMakeArgs += @("-A", $RequestedGeneratorPlatform)
    } else {
        Write-Host "Generator '$Generator' does not support -A x64; use an x64 Developer PowerShell/Command Prompt for this generator." -ForegroundColor DarkYellow
    }

    $CachedGeneratorPlatform = Get-CachedCMakeValue "$BuildDir/CMakeCache.txt" "CMAKE_GENERATOR_PLATFORM"
    if ($null -ne $CachedGeneratorPlatform -and $CachedGeneratorPlatform -ne $RequestedGeneratorPlatform) {
        Clear-CMakeConfigurationCache `
            -BuildDir $BuildDir `
            -Reason "Existing build directory uses CMake generator platform '$CachedGeneratorPlatform', but this run selected '$RequestedGeneratorPlatform'."
    }
} else {
    Write-Host "CMake generator: default (no Visual Studio generator found)" -ForegroundColor DarkYellow
    Write-Host "If configuration cannot find MSVC, run from an x64 Developer PowerShell or install Visual Studio C++ tools." -ForegroundColor DarkYellow
}

if ($BuildTests) {
    $CachedToolchain = Get-CachedCMakeValue "$BuildDir/CMakeCache.txt" "CMAKE_TOOLCHAIN_FILE"
    if ($CachedToolchain -and ((Resolve-Path $CachedToolchain -ErrorAction SilentlyContinue).Path -ne $VcpkgToolchain)) {
        Write-Error "Existing build directory uses CMAKE_TOOLCHAIN_FILE='$CachedToolchain', but this run selected '$VcpkgToolchain'. Remove '$BuildDir' and run build.ps1 again."
        exit 1
    }
    if (-not $CachedToolchain -and (Test-Path "$BuildDir/CMakeCache.txt")) {
        Write-Error "Existing build directory was configured without vcpkg. Remove '$BuildDir' and run build.ps1 again."
        exit 1
    }

    Write-Host "vcpkg toolchain: $VcpkgToolchain" -ForegroundColor DarkCyan
    Write-Host "vcpkg triplet:   $VcpkgTriplet" -ForegroundColor DarkCyan
}
Write-Host "MSVC runtime:    $MsvcRuntimeLibrary" -ForegroundColor DarkCyan

if ($NodeIncludeDir) {
    $CMakeArgs += "-DNODE_INCLUDE_DIR=$NodeIncludeDir"
}
if ($NodeLibrary) {
    $CMakeArgs += "-DNODE_LIBRARY=$NodeLibrary"
}

cmake @CMakeArgs

if ($LASTEXITCODE -ne 0) {
    Write-Error "CMake configuration failed!"
    exit 1
}

# ─── Build ───
if ($BuildApp) {
    Write-Host ""
    Write-Host "=== Building Shotlet (Release, Static CRT) ===" -ForegroundColor Cyan

    cmake --build . --config Release --target shotlet --parallel

    if ($LASTEXITCODE -ne 0) {
        Write-Error "Build failed!"
        exit 1
    }
}

# ─── Verify output ───
$exePath = "$BuildDir/Release/shotlet.exe"
if ($BuildApp -and (Test-Path $exePath)) {
    # Locate dumpbin (requires VS Developer Command Prompt environment)
    $dumpbin = Get-Command dumpbin -ErrorAction SilentlyContinue
    if (-not $dumpbin) {
        # Try to find via vswhere (VS 2017+)
        $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
        if (Test-Path $vswhere) {
            $vsPath = & $vswhere -latest -products * -property installationPath 2>$null
            if ($vsPath) {
                $dumpbinCandidate = Get-ChildItem -Path $vsPath -Recurse -Filter "dumpbin.exe" -ErrorAction SilentlyContinue | Select-Object -First 1
                if ($dumpbinCandidate) { $dumpbin = $dumpbinCandidate.FullName }
            }
        }
    }

    if ($dumpbin) {
        # Check DLL dependencies
        Write-Host ""
        Write-Host "=== DLL Dependencies ===" -ForegroundColor Cyan
        & $dumpbin /dependents $exePath 2>$null | Select-String "\.dll"

        # Check if CRT is statically linked
        $hasVCR = & $dumpbin /dependents $exePath 2>$null | Select-String "vcruntime"
        if ($hasVCR) {
            Write-Host ""
            Write-Host "WARNING: CRT is dynamically linked (depends on vcruntime140.dll)" -ForegroundColor Yellow
            Write-Host "  To distribute, include the Visual C++ Redistributable" -ForegroundColor Yellow
        } else {
            Write-Host ""
            Write-Host "OK: CRT is statically linked - standalone executable!" -ForegroundColor Green
        }
    } else {
        Write-Host ""
        Write-Host "=== DLL Check Skipped ===" -ForegroundColor DarkYellow
        Write-Host "dumpbin not found. Run from 'Developer PowerShell for VS' to see DLL dependencies." -ForegroundColor DarkYellow
    }

    $size = (Get-Item $exePath).Length / 1MB
    Write-Host ""
    Write-Host "=== Build Successful! ===" -ForegroundColor Green
    Write-Host "Output: $exePath" -ForegroundColor Yellow
    Write-Host "Size:   $([math]::Round($size, 1)) MB" -ForegroundColor Yellow
} else {
    if ($BuildApp) {
        Write-Error "shotlet.exe not found at expected location!"
        exit 1
    }
}

if ($BuildNode) {
    Write-Host ""
    Write-Host "=== Building Node-API addon (Release) ===" -ForegroundColor Cyan

    cmake --build . --config Release --target shotlet_node --parallel

    if ($LASTEXITCODE -ne 0) {
        Write-Error "Node-API addon build failed!"
        exit 1
    }

    $nodePath = "$BuildDir/Release/shotlet.node"
    if (Test-Path $nodePath) {
        $size = (Get-Item $nodePath).Length / 1MB
        Write-Host ""
        Write-Host "=== Node-API Addon Build Successful! ===" -ForegroundColor Green
        Write-Host "Output: $nodePath" -ForegroundColor Yellow
        Write-Host "Size:   $([math]::Round($size, 1)) MB" -ForegroundColor Yellow
    } else {
        Write-Error "shotlet.node not found at expected location!"
        exit 1
    }
}

if ($BuildTests) {
    Write-Host ""
    Write-Host "=== Building tests (Release) ===" -ForegroundColor Cyan

    cmake --build . --config Release --target shotlet_tests --parallel

    if ($LASTEXITCODE -ne 0) {
        Write-Error "Tests build failed!"
        exit 1
    }

    $testsPath = "$BuildDir/tests/Release/shotlet_tests.exe"
    if (Test-Path $testsPath) {
        Write-Host ""
        Write-Host "=== Tests Build Successful! ===" -ForegroundColor Green
        Write-Host "Output: $testsPath" -ForegroundColor Yellow
    } else {
        Write-Error "shotlet_tests.exe not found at expected location!"
        exit 1
    }
}
