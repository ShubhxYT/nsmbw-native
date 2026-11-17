# make-nsmbw.ps1 — build New Super Mario Bros. Wii into a native Windows program
#
# STATUS: scaffolded, not yet run on Windows. It follows the upstream project's own BUILDING.md
# (projects/nsmbw/BUILDING.md) step for step, and shares the same patch file the macOS script uses.
# Expect to fix small path or tool-name issues on first run; the stages are printed so it is obvious
# where it stops.
#
# What it does:
#   1. checks Windows x64, the .NET 8 SDK, Python 3 and git
#   2. clones the recompilation source, applies this port's fixes
#   3. downloads nodtool and extracts your disc
#   4. unpacks and prelinks the game code
#   5. fetches the pinned toolchain and dependencies, translates, compiles
#   6. collects the result into dist\NSMBW-Windows\ with the game folder beside the exe
#
# You need your own copy of New Super Mario Bros. Wii (USA, SMNE01, revision 1). Nothing here
# downloads, contains or redistributes any game code or assets.
#
# Usage (PowerShell, from this folder):
#   .\make-nsmbw.ps1 -Disc "D:\dumps\New Super Mario Bros. Wii (USA) (Rev 1).rvz"
#
# Options:
#   -Disc PATH      disc image (.rvz .iso .wbfs .gcz .wia)        [required unless -DiscDir]
#   -DiscDir PATH   an already extracted disc (sys\ + files\)    [optional]
#   -Out DIR        work and output folder     [default: $HOME\nsmbw-build]
#   -Repo DIR       existing checkout          [default: <Out>\repo]
#   -Jobs N         parallel compile jobs      [default: half the cores]
#   -Clean          delete <Out> first

[CmdletBinding()]
param(
    [string]$Disc,
    [string]$DiscDir,
    [string]$Out = (Join-Path $HOME 'nsmbw-build'),
    [string]$Repo,
    [int]$Jobs = [Math]::Max(2, [Environment]::ProcessorCount / 2),
    [switch]$Clean
)

$ErrorActionPreference = 'Stop'
$RepoUrl = 'https://github.com/WitherBlitz/NSMB-Wii-Multiplayer-Recompilation.git'
$AppName = 'New Super Mario Bros. Wii'

function Step($text) { Write-Host "`n==> $text" -ForegroundColor Cyan }
function Ok($text)   { Write-Host "    ok $text" -ForegroundColor Green }
function Die($text)  { Write-Host "`nerror: $text" -ForegroundColor Red; exit 1 }

if ([Environment]::OSVersion.Platform -ne 'Win32NT') { Die 'this script builds the Windows program; run it on Windows' }
if ([Environment]::Is64BitOperatingSystem -eq $false) { Die 'a 64-bit Windows is required' }
if (-not $Disc -and -not $DiscDir) { Die 'give -Disc <image> or -DiscDir <extracted disc>' }

$Repo      = if ($Repo) { $Repo } else { Join-Path $Out 'repo' }
$DiscDir   = if ($DiscDir) { $DiscDir } else { Join-Path $Out 'disc' }
$Dist      = Join-Path $Out 'dist\NSMBW-Windows'
$Logs      = Join-Path $Out 'logs'

if ($Clean -and (Test-Path $Out)) { Step "Cleaning $Out"; Remove-Item -Recurse -Force $Out }
New-Item -ItemType Directory -Force -Path $Out, $Logs | Out-Null

# ---------------------------------------------------------------- 1. tools
Step 'Checking the toolchain'
foreach ($tool in 'git', 'python', 'dotnet') {
    if (-not (Get-Command $tool -ErrorAction SilentlyContinue)) {
        Die "$tool is missing. Install it first (git, Python 3, the .NET 8 SDK)."
    }
}
$runtimes = & dotnet --list-runtimes
if (-not ($runtimes -match 'Microsoft\.NETCore\.App 8\.')) { Die 'the .NET 8 runtime is missing' }
$pythonArch = & python -c "import platform;print(platform.architecture()[0])"
if ($pythonArch -notmatch '64bit') { Die 'a 64-bit Python is required' }
Ok "python $(& python -V), dotnet $(& dotnet --version), git $(& git --version)"

# A machine can have .NET installed but no package sources at all (this one did), and then every
# restore fails with NU1100. Put nuget.org back if the list is empty.
$sources = & dotnet nuget list source
if ($sources -match 'No sources found') {
    & dotnet nuget add source https://api.nuget.org/v3/index.json -n nuget.org | Out-Null
    Ok 'added the nuget.org package source (none were configured)'
}

# ---------------------------------------------------------------- 2. source + fixes
Step 'Source code'
if (Test-Path (Join-Path $Repo '.git')) {
    Ok "using the checkout at $Repo"
} else {
    New-Item -ItemType Directory -Force -Path (Split-Path $Repo) | Out-Null
    & git clone --depth 1 -q $RepoUrl $Repo
    if ($LASTEXITCODE -ne 0) { Die 'could not clone the source' }
    Ok "cloned into $Repo"
}
if (-not (Test-Path (Join-Path $Repo 'projects\nsmbw\recomp.yml'))) {
    Die "$Repo does not look like the NSMBW recompilation"
}

# ---------------------------------------------------------------- 3. nodtool + disc
Step 'Disc extraction tool'
$Nodtool = Join-Path $Out 'tools\nodtool.exe'
New-Item -ItemType Directory -Force -Path (Split-Path $Nodtool) | Out-Null
if (-not (Test-Path $Nodtool)) {
    # Asset name from encounter/nod's release list; adjust if the release renames it.
    $url = 'https://github.com/encounter/nod/releases/download/v2.0.0-alpha.10/nodtool-windows-x86_64.exe'
    try {
        Invoke-WebRequest -Uri $url -OutFile $Nodtool -UseBasicParsing
        Ok 'downloaded nodtool'
    } catch {
        Die "could not download nodtool ($url). Download it by hand to $Nodtool and run again."
    }
} else {
    Ok 'nodtool already present'
}

Step 'Your disc'
if (Test-Path (Join-Path $DiscDir 'sys\main.dol')) {
    Ok "already extracted: $DiscDir"
} else {
    if (-not (Test-Path $Disc)) { Die "disc image not found: $Disc" }
    New-Item -ItemType Directory -Force -Path $DiscDir | Out-Null
    Write-Host "    extracting $(Split-Path $Disc -Leaf) ..."
    & $Nodtool extract $Disc $DiscDir *> (Join-Path $Logs 'extract.log')
    if ($LASTEXITCODE -ne 0 -or -not (Test-Path (Join-Path $DiscDir 'sys\main.dol'))) {
        Die "nodtool could not read the disc (see $Logs\extract.log)"
    }
    Ok 'extracted'
}

# Runs after extraction: besides the source fixes, it points the [E1] checksum at this disc's
# main.dol, which is what the Mac script does too.
Step "Applying this port's fixes"
& python (Join-Path $PSScriptRoot 'apply-patches.py') $Repo --disc $DiscDir
if ($LASTEXITCODE -ne 0) { Die "could not apply the port's fixes" }

# ---------------------------------------------------------------- 4. translator inputs
Step "Preparing the translator's inputs"
Push-Location $Repo
try {
    & python 'projects/nsmbw/tools/unpack_game.py' $DiscDir *> (Join-Path $Logs 'unpack.log')
    if ($LASTEXITCODE -ne 0) { Get-Content (Join-Path $Logs 'unpack.log') -Tail 10; Die 'unpack_game.py failed' }
    Ok 'game code unpacked'

    & python 'projects/nsmbw/tools/prelink_rels.py' *> (Join-Path $Logs 'prelink.log')
    if ($LASTEXITCODE -ne 0) { Get-Content (Join-Path $Logs 'prelink.log') -Tail 10; Die 'prelink_rels.py failed' }
    Ok 'boot RELs linked'
} finally {
    Pop-Location
}

# ---------------------------------------------------------------- 5. toolchain, translate, build
Step 'Pinned toolchain'
Push-Location $Repo
try {
    if (-not (Test-Path 'Launcher\artifacts\portable-tools')) {
        & 'Launcher\Prepare-PortableTools.ps1'
        if ($LASTEXITCODE -ne 0) { Die 'Prepare-PortableTools.ps1 failed' }
    }
    Ok 'toolchain ready'

    # An absolute destination: a relative one is resolved against the script that writes it,
    # which put the sources beside this script instead of inside the checkout.
    $depsDir = Join-Path $Repo 'Dependencies'
    if (-not (Test-Path (Join-Path $depsDir 'SDL'))) {
        & 'Launcher\Prepare-Dependencies.ps1' -Destination $depsDir
        if ($LASTEXITCODE -ne 0) { Die 'Prepare-Dependencies.ps1 failed' }
    }
    Ok 'third-party sources ready'

    Step 'Translating and compiling (the long one)'
    # rebuild-all.ps1 runs the translator, then configure-native.ps1 and build-native.ps1.
    & 'projects\nsmbw\tools\rebuild-all.ps1' -Threads $Jobs -Jobs $Jobs *> (Join-Path $Logs 'build.log')
    if ($LASTEXITCODE -ne 0) {
        Get-Content (Join-Path $Logs 'build.log') -Tail 25
        Die "the build failed (see $Logs\build.log)"
    }
} finally {
    Pop-Location
}

$exe = Join-Path $Repo 'native-build\WiiCompiled.exe'
if (-not (Test-Path $exe)) { Die "the build produced no exe (expected $exe)" }

# ---------------------------------------------------------------- 6. collect
Step 'Collecting the result'
if (Test-Path $Dist) { Remove-Item -Recurse -Force $Dist }
New-Item -ItemType Directory -Force -Path $Dist | Out-Null
# Only what the program needs at run time. A wholesale copy of native-build also brings the
# compiler's .a libraries, .ninja bookkeeping and CMake files, which nobody should ship.
$native = Join-Path $Repo 'native-build'
Copy-Item (Join-Path $native 'WiiCompiled.exe') $Dist -Force
Get-ChildItem $native -Filter '*.dll' -File | Copy-Item -Destination $Dist -Force
foreach ($name in 'dsp_coef.bin', 'initial_pipeline_cache.db') {
    $src = Join-Path $native $name
    if (Test-Path $src) { Copy-Item $src $Dist -Force }
}
$boot = Join-Path $native 'wii_bootstrap'
if (Test-Path $boot) { Copy-Item $boot $Dist -Recurse -Force }
Copy-Item $DiscDir (Join-Path $Dist 'game') -Recurse -Force

$renamed = Join-Path $Dist 'New Super Mario Bros. Wii.exe'
Move-Item (Join-Path $Dist 'WiiCompiled.exe') $renamed -Force
# The runtime looks for its game folder beside the exe; it is there as "game".
$readme = @'
New Super Mario Bros. Wii — native Windows build

Run "New Super Mario Bros. Wii.exe". The game files are in the "game" folder beside it.
In game, F10 opens settings: resolution, aspect (4:3 / 16:10 / 16:9 / fill), display mode,
controls and button labels (PlayStation / Xbox / Nintendo).

Saves: %LOCALAPPDATA%\WiiCompiled  (or a UserData folder here if a portable.txt sits beside the exe)
LAN play: Select a File > Play: LAN > Create Room / Join. Tailscale works for remote players.
'@
Set-Content -Path (Join-Path $Dist 'README.txt') -Value $readme -Encoding UTF8

# Desktop shortcut, so the build is one double-click away. Rebuilt runs replace it.
$desktop = [Environment]::GetFolderPath('Desktop')
$lnk = Join-Path $desktop 'New Super Mario Bros. Wii.lnk'
$shell = New-Object -ComObject WScript.Shell
$shortcut = $shell.CreateShortcut($lnk)
$shortcut.TargetPath = $renamed
$shortcut.WorkingDirectory = $Dist
$shortcut.IconLocation = "$renamed,0"
$shortcut.Description = 'New Super Mario Bros. Wii (native recompilation)'
$shortcut.Save()
Ok "desktop shortcut: $lnk"

Step 'Done'
Write-Host "    exe:  $renamed"
Write-Host "    size: $([math]::Round((Get-ChildItem $Dist -Recurse | Measure-Object Length -Sum).Sum / 1GB, 2)) GB"
Write-Host "    logs: $Logs"
