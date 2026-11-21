<#
.SYNOPSIS
Build a standalone Switch NSP from a local SMNE01 revision 1 disc.
.EXAMPLE
.\make-nsmbw-switch.ps1 -Disc 'D:\Wii dumps\NSMBW (USA) (Rev 1).rvz'
.EXAMPLE
.\make-nsmbw-switch.ps1 -DiscDir 'D:\Wii dumps\NSMBW' -Keys 'D:\Switch\prod.keys'
#>
[CmdletBinding(DefaultParameterSetName = 'Image')]
param(
    [Parameter(Mandatory, ParameterSetName = 'Image')]
    [string]$Disc,

    [Parameter(Mandatory, ParameterSetName = 'Extracted')]
    [string]$DiscDir,

    [string]$Keys = $(if ($env:NSMBW_SWITCH_KEYS) { $env:NSMBW_SWITCH_KEYS } else { Join-Path $HOME '.switch\prod.keys' }),
    [string]$Out = (Join-Path $PSScriptRoot 'build\switch'),
    [ValidatePattern('^[0-9]+\.[0-9]+\.[0-9]+$')]
    [ValidateLength(1, 15)]
    [string]$Version = '1.1.3',
    [string]$Icon,
    [ValidateRange(1, [int]::MaxValue)]
    [int]$Jobs = 4
)

$ErrorActionPreference = 'Stop'

function Invoke-BuildDocker {
    & $Docker @args
    if ($LASTEXITCODE -ne 0) {
        throw "Docker $($args[0]) failed with exit code $LASTEXITCODE."
    }
}

function Get-LocalInput([string]$Path, [string]$Kind, [string]$Name) {
    if (-not (Test-Path -LiteralPath $Path -PathType $Kind)) {
        throw "$Name not found at $Path."
    }
    return (Get-Item -LiteralPath $Path).FullName
}

function New-BuildContainer([string[]]$Options, [string[]]$Stage) {
    return (Invoke-BuildDocker create --platform $Platform --mount $Mount @Options -e "NSMBW_BUILD_JOBS=$Jobs" $Image bash /input/runner.sh @Stage).Trim()
}

function Start-BuildContainer([string]$Container) {
    Invoke-BuildDocker start $Container | Out-Null
    Invoke-BuildDocker logs --follow $Container
    $StageExit = [string](Invoke-BuildDocker wait $Container)
    if ($StageExit -notmatch '^\d+$') { throw 'Docker did not return a build container exit status.' }
    if ([int]$StageExit -ne 0) { throw "The Switch build container exited with code $StageExit." }
}

function Copy-Recipe([string]$Container) {
    Invoke-BuildDocker cp $RecipeArchive "$($Container):/input/recipe.tar"
    Invoke-BuildDocker cp $Runner "$($Container):/input/runner.sh"
}

function Get-TextHash([string]$Text) {
    $Hasher = [Security.Cryptography.SHA256]::Create()
    try {
        $Digest = $Hasher.ComputeHash([Text.Encoding]::UTF8.GetBytes($Text))
        return [BitConverter]::ToString($Digest).Replace('-', '').ToLowerInvariant()
    } finally {
        $Hasher.Dispose()
    }
}

if ($PSCmdlet.ParameterSetName -eq 'Image') {
    $InputPath = Get-LocalInput $Disc 'Leaf' 'Disc image'
    $InputKind = 'disc'
} else {
    $InputPath = Get-LocalInput $DiscDir 'Container' 'Extracted disc'
    $null = Get-LocalInput (Join-Path $InputPath 'sys\boot.bin') 'Leaf' 'Disc header'
    $InputKind = 'disc-dir'
}
$Keys = Get-LocalInput $Keys 'Leaf' 'Local Switch key file (set -Keys)'
if ($Icon) { $Icon = Get-LocalInput $Icon 'Leaf' 'Icon' }

$DockerCommand = Get-Command docker.exe -ErrorAction SilentlyContinue
if (-not $DockerCommand) { throw 'Install Docker Desktop and start it in Linux containers mode.' }
$Docker = $DockerCommand.Source
$Engine = (Invoke-BuildDocker version --format '{{.Server.Os}}/{{.Server.Arch}}').Trim()
if ($Engine -notin 'linux/amd64', 'linux/arm64') {
    throw 'Switch Docker Desktop to Linux containers mode. The builder supports amd64 and arm64 Linux engines.'
}
$Platform = $Engine
$Image = 'nsmbw-switch-windows-builder:1'
$RecipeDir = Join-Path $PSScriptRoot 'switch\standalone'
$Runner = Join-Path $RecipeDir 'windows-container.sh'
$Out = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($Out)
$null = [IO.Directory]::CreateDirectory($Out)
$Logs = Join-Path $Out 'logs'
$null = [IO.Directory]::CreateDirectory($Logs)

# Bind the reusable container cache to this Windows output directory and engine platform.
$CacheId = (Get-TextHash "$env:COMPUTERNAME/$env:USERNAME/$($Out.ToLowerInvariant())/$Platform").Substring(0, 24)
$Mount = "type=volume,source=nsmbw-switch-$CacheId,target=/work"
$Lock = [IO.File]::Open((Join-Path $Out '.building-windows'), 'OpenOrCreate', 'ReadWrite', 'None')
$RecipeArchive = Join-Path $Out ('.recipe-' + [Guid]::NewGuid().ToString('N') + '.tar')
$LocalStamp = "$RecipeArchive.stamp"
$Pending = Join-Path $Out ('.nsp-' + [Guid]::NewGuid().ToString('N') + '.tmp')
$Container = $null

try {
    Write-Host 'Preparing the Switch toolchain'
    Invoke-BuildDocker build --target windows-builder --platform $Platform --tag $Image --file (Join-Path $RecipeDir 'Dockerfile') $RecipeDir

    # Include only build recipes. Disc data and keys are supplied separately.
    $RecipeFiles = @('apply-patches.py', 'switch/nsmbw.nacp')
    $RecipeFiles += @(Get-ChildItem -LiteralPath (Join-Path $PSScriptRoot 'switch') -File |
        Where-Object { $_.Extension -in '.cpp', '.h', '.py', '.sh', '.S', '.json' } |
        ForEach-Object { "switch/$($_.Name)" })
    foreach ($Subdir in 'include', 'patches', 'standalone') {
        $Directory = Join-Path (Join-Path $PSScriptRoot 'switch') $Subdir
        $RecipeFiles += @(Get-ChildItem -LiteralPath $Directory -File -Recurse |
            Where-Object { $_.Extension -in '.h', '.py', '.sh', '.json', '.patch', '.c' } |
            ForEach-Object { $_.FullName.Substring($PSScriptRoot.Length + 1).Replace('\', '/') })
    }
    & tar.exe -cf $RecipeArchive -C $PSScriptRoot @RecipeFiles
    if ($LASTEXITCODE -ne 0) { throw 'Could not stage the Switch build recipes.' }

    if ($InputKind -eq 'disc') {
        $DiscHash = (Get-FileHash -LiteralPath $InputPath).Hash.ToLowerInvariant()
    } else {
        $Entries = Get-ChildItem -LiteralPath $InputPath -File -Recurse -Force | Sort-Object FullName | ForEach-Object {
            $Relative = $_.FullName.Substring($InputPath.Length).TrimStart('\').Replace('\', '/')
            "$Relative $((Get-FileHash -LiteralPath $_.FullName).Hash)"
        }
        $DiscHash = Get-TextHash ($Entries -join [char]0)
    }
    $InputStamp = "$InputKind-$DiscHash"

    Write-Host 'Preparing and compiling the disc'
    $Container = New-BuildContainer -Options @() -Stage @('compile', $InputKind, $InputStamp)
    Copy-Recipe $Container
    # A new volume has no stamp yet. Copy an existing stamp from the stopped container.
    try { & $Docker cp "$($Container):/work/.disc-input-id" $LocalStamp 2>$null } catch { }
    $CachedStamp = if ([IO.File]::Exists($LocalStamp)) { [IO.File]::ReadAllText($LocalStamp).Trim() }
    if ($InputStamp -ne $CachedStamp) {
        Invoke-BuildDocker cp $InputPath "$($Container):/input/disc"
    }
    Start-BuildContainer $Container
    Invoke-BuildDocker rm $Container | Out-Null
    $Container = $null

    Write-Host 'Packaging the standalone NSP'
    $Container = New-BuildContainer -Options @('--network', 'none') -Stage @('pack', $Version)
    Copy-Recipe $Container
    Invoke-BuildDocker cp $Keys "$($Container):/input/keys"
    if ($Icon) { Invoke-BuildDocker cp $Icon "$($Container):/input/icon" }
    Start-BuildContainer $Container
    Invoke-BuildDocker cp "$($Container):/pack/out_rb/01004e53a1b50000.nsp" $Pending
    $Result = Join-Path $Out "aaa-nsmbw-v$Version-standalone.nsp"
    if ([IO.File]::Exists($Result)) {
        [IO.File]::Replace($Pending, $Result, $null)
    } else {
        [IO.File]::Move($Pending, $Result)
    }
    Write-Host "Built $Result"
} finally {
    if ($Container) {
        & $Docker cp "$($Container):/work/logs/." $Logs
        & $Docker rm --force $Container | Out-Null
    }
    foreach ($Temp in $RecipeArchive, $LocalStamp, $Pending) {
        if ([IO.File]::Exists($Temp)) { [IO.File]::Delete($Temp) }
    }
    $Lock.Dispose()
}
