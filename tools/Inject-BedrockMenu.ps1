[CmdletBinding()]
param(
    [string]$Dll = "",
    [string[]]$ProcessName = @("Minecraft.Windows", "Minecraft"),
    [switch]$WaitForProcess,
    [int]$WaitSeconds = 60,
    [switch]$KeepLog,
    [switch]$ResetConfig,
    [switch]$CopyIni,
    [switch]$EnableOverlay,
    [switch]$LateHook
)

$ErrorActionPreference = "Stop"

function Write-Step([string]$message) {
    Write-Host "[inject] $message"
}

function Write-Fail([string]$message) {
    Write-Host "[inject] ERROR: $message" -ForegroundColor Red
}

if ([IntPtr]::Size -ne 8) {
    throw "Run this from 64-bit PowerShell (the game and DLL are x64)."
}

if (-not $Dll) {
    $Dll = Join-Path (Split-Path $PSScriptRoot -Parent) "build\Release\BedrockMenu.dll"
}

$dllPath = (Resolve-Path -LiteralPath $Dll -ErrorAction Stop).Path
Write-Step "DLL: $dllPath"

# Verify it is an x64 PE before touching the game.
$pe = [System.IO.File]::ReadAllBytes($dllPath)
if ($pe.Length -lt 0x40 -or $pe[0] -ne 0x4D -or $pe[1] -ne 0x5A) {
    throw "Not a valid PE file: $dllPath"
}
$peOffset = [BitConverter]::ToInt32($pe, 0x3C)
$machine = [BitConverter]::ToUInt16($pe, $peOffset + 4)
if ($machine -ne 0x8664) {
    throw ("DLL is not x64 (machine=0x{0:X4}). Build the x64 target." -f $machine)
}
Write-Step "DLL architecture: x64"

# Locate the Minecraft package root. The DLL is copied into AC\LocalState so the
# AppContainer can read/load it without relying on arbitrary filesystem access.
$packagesRoot = Join-Path $env:LOCALAPPDATA "Packages"
$package = Get-ChildItem -LiteralPath $packagesRoot -Directory -ErrorAction SilentlyContinue |
    Where-Object {
        $_.Name -like "Microsoft.MinecraftUWP_*" -or
        $_.Name -like "Microsoft.MinecraftWindowsBeta_*" -or
        $_.Name -like "*Minecraft*"
    } |
    Select-Object -First 1

if ($package) {
    $targetDir = Join-Path $package.FullName "AC\LocalState\BedrockMenu"
} else {
    Write-Step "Package root not found; using %TEMP%\BedrockMenu"
    $targetDir = Join-Path $env:TEMP "BedrockMenu"
}

New-Item -ItemType Directory -Force -Path $targetDir | Out-Null
$targetDll = Join-Path $targetDir "BedrockMenu.dll"
Copy-Item -LiteralPath $dllPath -Destination $targetDll -Force
Write-Step "Copied to package-writable path: $targetDll"

if ($CopyIni) {
    $sourceIni = Join-Path (Split-Path $dllPath -Parent) "BedrockMenu.ini"
    if (Test-Path -LiteralPath $sourceIni) {
        Copy-Item -LiteralPath $sourceIni -Destination (Join-Path $targetDir "BedrockMenu.ini") -Force
        Write-Step "Copied BedrockMenu.ini"
    }
}

# Language-independent SID for ALL APPLICATION PACKAGES.
$aclResult = & icacls $targetDir /grant "*S-1-15-2-1:(RX)" /T 2>&1
if ($LASTEXITCODE -ne 0) {
    Write-Step "Warning: icacls could not set AppContainer ACL ($LASTEXITCODE)."
    $aclResult | ForEach-Object { Write-Step $_ }
} else {
    Write-Step "Granted ALL APPLICATION PACKAGES read/execute"
}

$targetIni = Join-Path $targetDir "BedrockMenu.ini"
if ($ResetConfig) {
    $safeConfig = @(
        "[General]",
        "ShowConsole=0",
        "DisableImGuiInput=1",
        "DisableLateHook=1",
        "DisableD3DHooks=1"
    )
    Set-Content -LiteralPath $targetIni -Value $safeConfig -Encoding UTF8
    Write-Step "Wrote safe defaults to BedrockMenu.ini"
} elseif ($EnableOverlay) {
    $lateValue = if ($LateHook) { "0" } else { "1" }
    $overlayConfig = @(
        "[General]",
        "ShowConsole=0",
        "DisableImGuiInput=1",
        "DisableLateHook=$lateValue",
        "DisableD3DHooks=0"
    )
    Set-Content -LiteralPath $targetIni -Value $overlayConfig -Encoding UTF8
    Write-Step ("Wrote overlay config (DisableLateHook={0})" -f $lateValue)
}

$logPath = Join-Path $targetDir "BedrockMenu.log"
if (-not $KeepLog -and (Test-Path -LiteralPath $logPath)) {
    Remove-Item -LiteralPath $logPath -Force
    Write-Step "Cleared old BedrockMenu.log"
}

# Use the compiled helper. PowerShell Add-Type is blocked by some antivirus products.
$projectRoot = Split-Path $PSScriptRoot -Parent
$helper = @(
    (Join-Path $PSScriptRoot "Injector.exe"),
    (Join-Path $projectRoot "build\Release\Injector.exe"),
    (Join-Path $projectRoot "build-test\Injector.exe")
) | Where-Object { Test-Path -LiteralPath $_ } | Select-Object -First 1

if (-not $helper) {
    throw "BedrockInjector.exe not found. Build it with: cmake --build build --target BedrockInjector"
}

# Find a game process we can actually open. Stale/protected UWP processes are skipped.
$targetProcessId = 0
$targetProcessName = ""
$deadline = (Get-Date).AddSeconds($WaitSeconds)

while ($true) {
    $candidates = @(Get-CimInstance -ClassName Win32_Process -ErrorAction SilentlyContinue |
        Where-Object {
            $current = $_.Name
            $ProcessName | Where-Object {
                $current -ieq $_ -or $current -ieq ($_ + ".exe")
            }
        })

    if ($candidates.Count -gt 0) {
        foreach ($candidate in ($candidates | Sort-Object CreationDate -Descending)) {
            $candidateId = [int]$candidate.ProcessId
            & $helper "probe" $candidateId | Out-Null
            if ($LASTEXITCODE -eq 0) {
                $targetProcessId = $candidateId
                $targetProcessName = $candidate.Name
                break
            }
        }
        if ($targetProcessId -ne 0) { break }
    }

    if (-not $WaitForProcess -or (Get-Date) -ge $deadline) {
        $running = @(Get-CimInstance -ClassName Win32_Process -ErrorAction SilentlyContinue |
            Where-Object { $_.Name -match "(?i)minecraft|bedrock" } |
            Select-Object -ExpandProperty Name -Unique)
        $detail = if ($running.Count) { "Running matching processes: $($running -join ', ')" } else { "No usable Minecraft process." }
        throw "Minecraft process not found or not injectable. $detail Start the game, or use -WaitForProcess."
    }
    Start-Sleep -Milliseconds 500
}

Write-Step ("Target: {0} PID {1}" -f $targetProcessName, $targetProcessId)

& $helper "inject" $targetProcessId $targetDll
if ($LASTEXITCODE -ne 0) {
    throw "Injector helper failed (exit $LASTEXITCODE)."
}

if (Test-Path -LiteralPath $logPath) {
    Write-Step "--- BedrockMenu.log (tail) ---"
    Get-Content -LiteralPath $logPath -Tail 20
} else {
    Write-Fail "No BedrockMenu.log was created at $logPath"
}
