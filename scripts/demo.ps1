param(
    [int]$Duration = 30,
    [switch]$NoBrowser,
    [switch]$SkipBuild,
    [string]$Preset = "windows-mingw",
    [ValidateSet("Debug", "Release")]
    [string]$BuildType = "Release",
    [string]$Pipeline = ""
)

$ErrorActionPreference = "Continue"
# CMake and third-party dependencies legitimately emit warnings on stderr.
# Native commands below must therefore be checked via $LASTEXITCODE instead
# of PowerShell's stderr-to-error conversion.
# Windows: CRT maps "/tmp/..." -> "<drive>:\tmp\..." (cwd drive). Ensure it exists.
foreach ($d in @('D:\tmp','C:\tmp', (Join-Path $env:TEMP 'flow_logs'))) {
    $driveRoot = [System.IO.Path]::GetPathRoot($d)
    if ($driveRoot -and (Test-Path $driveRoot)) {
        New-Item -ItemType Directory -Force -Path $d | Out-Null
    }
}
$runtimeTemp = Join-Path $env:LOCALAPPDATA 'FlowEngine\tmp'
New-Item -ItemType Directory -Force -Path $runtimeTemp | Out-Null
$env:FLOWENGINE_TEMP_DIR = $runtimeTemp
$env:FLOWENGINE_STATE_FILE = Join-Path $runtimeTemp 'flow_topology.json'
$env:FLOW_LOG_DIR = Join-Path $runtimeTemp 'flow_logs'
New-Item -ItemType Directory -Force -Path $env:FLOW_LOG_DIR | Out-Null
$Root = Resolve-Path (Join-Path $PSScriptRoot "..")
Set-Location $Root

function Add-PathFront([string]$p) {
    if ($p -and (Test-Path $p) -and (($env:PATH -split ';') -notcontains $p)) {
        $env:PATH = "$p;$env:PATH"
    }
}

function Find-BinDir([string[]]$candidates) {
    foreach ($c in $candidates) {
        if ($c -and (Test-Path $c)) { return $c }
    }
    return $null
}

# 鈹€鈹€ Toolchain discovery (MinGW primary) 鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€
$winlibs = Join-Path $Root ".tools\winlibs\mingw64\bin"
Add-PathFront $winlibs

# winget WinLibs is the preferred native toolchain.  Do not silently fall back
# to an unrelated old C:\mingw64 installation when the managed package exists.
$wingetWinLibs = Get-ChildItem "$env:LOCALAPPDATA\Microsoft\WinGet\Packages" `
    -Directory -Filter 'BrechtSanders.WinLibs.POSIX.UCRT*' -ErrorAction SilentlyContinue |
    ForEach-Object { Join-Path $_.FullName 'mingw64\bin' } |
    Where-Object { Test-Path (Join-Path $_ 'g++.exe') } |
    Select-Object -First 1
Add-PathFront $wingetWinLibs

$cmakeBin = Find-BinDir @(
    "C:\Program Files\CMake\bin",
    "C:\Program Files (x86)\CMake\bin"
)
Add-PathFront $cmakeBin

$ninjaHint = Get-ChildItem "$env:LOCALAPPDATA\Microsoft\WinGet\Packages" -Filter ninja.exe -Recurse -ErrorAction SilentlyContinue |
    Select-Object -First 1 -ExpandProperty DirectoryName
Add-PathFront $ninjaHint
Add-PathFront (Join-Path $env:LOCALAPPDATA "Microsoft\WinGet\Links")

$pyHint = Find-BinDir @(
    (Join-Path $env:LOCALAPPDATA "Programs\Python\Python312"),
    (Join-Path $env:LOCALAPPDATA "Programs\Python\Python311"),
    "C:\Python312",
    "C:\Python311"
)
Add-PathFront $pyHint

if ((Test-Path $winlibs) -or $wingetWinLibs) {
    $preferredMingw = if (Test-Path $winlibs) { $winlibs } else { $wingetWinLibs }
    $parts = $env:PATH -split ';' | Where-Object {
        $_ -and ($_ -ne $preferredMingw) -and
        ($_ -notmatch '(?i)[\\/]mingw64[\\/]mingw64[\\/]bin$') -and
        ($_ -notmatch '(?i)^C:\\mingw64\\')
    }
    $env:PATH = ($preferredMingw + ';' + ($parts -join ';'))
}

if (-not (Get-Command cmake -ErrorAction SilentlyContinue)) {
    throw "cmake not found. Install CMake and ensure it is on PATH."
}

$isMingw = $Preset -eq "windows-mingw"
$isVs = $Preset -eq "windows-vs"
if ($isMingw) {
    if (-not (Get-Command gcc -ErrorAction SilentlyContinue) -or -not (Get-Command g++ -ErrorAction SilentlyContinue)) {
        throw @"
MinGW g++/gcc not found.
Place WinLibs under: $winlibs
  or install WinLibs (POSIX+UCRT, GCC>=11) and put its bin/ on PATH.
  Download: https://github.com/brechtsanders/winlibs_mingw/releases
"@
    }
    $ver = (& g++ -dumpversion)
    Write-Host "[demo] MinGW g++ $ver  (primary toolchain)"
    if ([int]($ver.Split('.')[0]) -lt 11) {
        throw "MinGW GCC >= 11 is required; found $ver at $((Get-Command g++).Source)"
    }
    if (-not (Get-Command ninja -ErrorAction SilentlyContinue)) {
        throw "ninja not found (required by windows-mingw preset)."
    }
}

$BuildDirName = switch ($Preset) {
    "windows-vs" { "build-win-vs" }
    "windows-mingw" { "build-win" }
    "windows-ninja" { "build-win-ninja" }
    default { "build-win" }
}
$BuildDir = Join-Path $Root $BuildDirName

if (-not $SkipBuild) {
    if ($isMingw -and (Test-Path (Join-Path $BuildDir 'CMakeCache.txt'))) {
        $cachedCompiler = (Select-String -Path (Join-Path $BuildDir 'CMakeCache.txt') `
            -Pattern '^CMAKE_C_COMPILER:.*=(.+)$' | Select-Object -First 1).Matches.Groups[1].Value
        $expectedCompiler = Join-Path $preferredMingw 'gcc.exe'
        if ($cachedCompiler -and -not [string]::Equals(
                [System.IO.Path]::GetFullPath($cachedCompiler),
                [System.IO.Path]::GetFullPath($expectedCompiler),
                [System.StringComparison]::OrdinalIgnoreCase)) {
            Write-Host "[demo] compiler changed; clearing stale CMake cache ..."
            Remove-Item -Recurse -Force $BuildDir
        }
    }
    Write-Host "[demo] configure preset=$Preset ..."
    if ($isMingw) {
        cmake --preset $Preset "-DCMAKE_BUILD_TYPE=$BuildType"
    } elseif ($isVs) {
        cmake --preset $Preset -DCMAKE_BUILD_TYPE=Release
    } else {
        cmake --preset $Preset
    }
    if ($LASTEXITCODE -ne 0) { throw "failed to configure FlowEngine" }
    Write-Host "[demo] build core ..."
    if ($isVs) {
        cmake --build --preset $Preset --config Release
    } else {
        cmake --build --preset $Preset
    }
    if ($LASTEXITCODE -ne 0) { throw "failed to build FlowEngine core" }

    Write-Host "[demo] build node plugins ..."
    $nodesB = Join-Path $BuildDir "modules\adas_nodes"
    $cfgArgs = @(
        "-S", (Join-Path $Root "modules\adas_nodes"),
        "-B", $nodesB,
        "-G", $(if ($isVs) { "Visual Studio 17 2022" } else { "Ninja" }),
        "-DFLOWENGINE_BUILD=$BuildDir",
        "-DCMAKE_BUILD_TYPE=$BuildType"
    )
    if ($isMingw) {
        $cfgArgs += @("-DCMAKE_C_COMPILER=gcc", "-DCMAKE_CXX_COMPILER=g++")

        $esminiRoot = Join-Path $Root 'third_party\esmini'
        if (-not (Test-Path (Join-Path $esminiRoot 'CMakeLists.txt'))) {
            Write-Host "[demo] initializing esmini submodule ..."
            & git submodule update --init --depth 1 third_party/esmini
            if ($LASTEXITCODE -ne 0) { throw "failed to initialize esmini submodule" }
        }
        $esminiBuild = Join-Path $BuildDir 'esmini'
        $compatHeader = (Join-Path $Root 'cmake\esmini_mingw_compat.h') -replace '\\','/'
        Write-Host "[demo] build esmini RoadManager ..."
        & cmake -S $esminiRoot -B $esminiBuild -G Ninja `
            -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=gcc -DCMAKE_CXX_COMPILER=g++ `
            "-DCMAKE_CXX_FLAGS=-include $compatHeader" `
            -DDOWNLOAD_EXTERNALS=OFF -DUSE_OSG=OFF -DUSE_OSI=OFF -DUSE_SUMO=OFF `
            -DUSE_GTEST=OFF -DUSE_IMPLOT=OFF -DBUILD_ODRPLOT=OFF `
            -DBUILD_REPLAYER=OFF -DBUILD_EXAMPLES=OFF -DENABLE_INCLUDE_WHAT_YOU_USE=OFF
        if ($LASTEXITCODE -ne 0) { throw "failed to configure esmini RoadManager" }
        & cmake --build $esminiBuild --target esminiRMLib --parallel 4
        if ($LASTEXITCODE -ne 0) { throw "failed to build esmini RoadManager" }

        $esminiLibDir = Join-Path $esminiBuild 'EnvironmentSimulator\Libraries\esminiRMLib'
        $esminiImport = Get-ChildItem $esminiLibDir -Filter '*esminiRMLib*.dll.a' |
            Select-Object -First 1 -ExpandProperty FullName
        $esminiDll = Get-ChildItem $esminiLibDir -Filter '*esminiRMLib*.dll' |
            Select-Object -First 1 -ExpandProperty FullName
        if (-not $esminiImport -or -not $esminiDll) {
            throw "esmini build completed without import library/DLL in $esminiLibDir"
        }
        $cfgArgs += "-DESMINI_RMLIB=$esminiImport"
        New-Item -ItemType Directory -Force -Path (Join-Path $BuildDir 'lib') | Out-Null
        Copy-Item $esminiDll (Join-Path $BuildDir 'lib') -Force
    }
    & cmake @cfgArgs
    if ($LASTEXITCODE -ne 0) { throw "failed to configure node plugins" }
    if ($isVs) {
        cmake --build $nodesB --config Release
    } else {
        cmake --build $nodesB
    }
    if ($LASTEXITCODE -ne 0) { throw "failed to build node plugins" }
} else {
    Write-Host "[demo] SkipBuild: using existing artifacts in $BuildDirName"
}

$BinDir = $null
$LibDir = $null
foreach ($cand in @(
    (Join-Path $BuildDir "bin"),
    (Join-Path $BuildDir "bin\Release")
)) {
    if (Test-Path (Join-Path $cand "flow_launcher.exe")) { $BinDir = $cand; break }
}
foreach ($cand in @(
    (Join-Path $BuildDir "lib"),
    (Join-Path $BuildDir "lib\Release")
)) {
    if (Test-Path $cand) {
        $hasDll = Get-ChildItem $cand -Filter "*_node.dll" -ErrorAction SilentlyContinue | Select-Object -First 1
        if ($hasDll) { $LibDir = $cand; break }
    }
}
if (-not $BinDir) { throw "flow_launcher.exe not found under $BuildDir\bin" }
if (-not $LibDir) { throw "node plugins (*_node.dll) not found under $BuildDir\lib" }

if ($isMingw) {
    # Windows loader checks the executable directory before PATH.  Deploy the
    # runtime from the compiler used above so an older system-wide MinGW (for
    # example GCC 8 under C:\mingw64) cannot satisfy these imports incorrectly.
    foreach ($runtimeDll in @('libatomic-1.dll', 'libgcc_s_seh-1.dll', 'libstdc++-6.dll', 'libwinpthread-1.dll')) {
        $runtimeSource = Join-Path $preferredMingw $runtimeDll
        if (-not (Test-Path $runtimeSource)) {
            throw "MinGW runtime missing: $runtimeSource"
        }
        Copy-Item $runtimeSource $BinDir -Force
    }
}

Write-Host "[demo] bin=$BinDir"
Write-Host "[demo] lib=$LibDir"

$env:PATH = "$BinDir;$LibDir;$env:PATH"
$env:FLOWENGINE_PLUGIN_DIR = $LibDir
$python = Get-Command python -ErrorAction SilentlyContinue
if ($python) { $env:FLOWENGINE_PYTHON = $python.Source }

if (-not $Pipeline) {
    $Pipeline = Join-Path $Root "config\pipeline_windows.json"
}
if (-not (Test-Path $Pipeline)) {
    throw "pipeline not found: $Pipeline"
}

$pipelineObj = Get-Content $Pipeline -Raw | ConvertFrom-Json
foreach ($proc in $pipelineObj.processes) {
    if (-not $proc.library_path) { continue }
    $base = [System.IO.Path]::GetFileName(($proc.library_path -replace '\\','/'))
    $base = $base -replace '^lib','' -replace '\.so$','.dll'
    $candidates = @(
        (Join-Path $LibDir $base),
        (Join-Path $LibDir ("lib" + $base))
    )
    $abs = $candidates | Where-Object { Test-Path $_ } | Select-Object -First 1
    if (-not $abs) {
        $abs = $candidates[0]
        Write-Warning "plugin missing: $abs (also tried lib$base)"
    }
    $proc.library_path = $abs
}
$runtimePipeline = Join-Path $env:TEMP "flow_pipeline_windows_runtime.json"
($pipelineObj | ConvertTo-Json -Depth 30) | ForEach-Object {
    [IO.File]::WriteAllText($runtimePipeline, $_, (New-Object System.Text.UTF8Encoding $false))
}
Write-Host "[demo] pipeline => $runtimePipeline"

$Flowmond = Join-Path $BinDir "flowmond.exe"
$Launcher = Join-Path $BinDir "flow_launcher.exe"
$Html = Join-Path $Root "tools\flowboard\index.html"

Get-Process flowmond, flow_launcher -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue

Write-Host "[demo] starting flowmond..."
$flowmondProc = Start-Process -FilePath $Flowmond `
    -ArgumentList @("--html-path", $Html) `
    -PassThru -WindowStyle Minimized

$ready = $false
for ($i = 0; $i -lt 20; $i++) {
    Start-Sleep -Milliseconds 250
    try {
        $r = Invoke-WebRequest -Uri "http://127.0.0.1:8800/api/health" -UseBasicParsing -TimeoutSec 1
        if ($r.StatusCode -eq 200) { $ready = $true; break }
    } catch {}
}
if ($ready) {
    Write-Host "[demo] flowmond ready http://localhost:8800"
} else {
    Write-Warning "flowmond health check not ready yet; continuing anyway"
}

if (-not $NoBrowser) {
    Start-Process "http://localhost:8800"
}

try {
    Write-Host "[demo] running flow_launcher duration=${Duration}s ..."
    & $Launcher $runtimePipeline --duration $Duration
    $code = $LASTEXITCODE
} finally {
    if ($flowmondProc -and -not $flowmondProc.HasExited) {
        Stop-Process -Id $flowmondProc.Id -Force -ErrorAction SilentlyContinue
    }
}

if ($code -ne 0 -and $null -ne $code) {
    throw "flow_launcher exited with code $code"
}
Write-Host "[demo] finished OK"