<#
.SYNOPSIS
    Fail the build when the Windows payload is not self-contained.

.DESCRIPTION
    Walks the import table of every PE in the deploy directory (dumpbin
    /dependents) and classifies each imported DLL:

      * present in the deploy directory        -> ships with us, fine
      * an MSVC runtime (vcomp/msvcp/...)      -> MUST ship with us, else FAIL
      * a Windows system DLL or API set        -> fine, the OS provides it
      * anything else                          -> FAIL, nothing provides it

    The MSVC-runtime rule is deliberately checked BEFORE the system-DLL rule.
    Build machines have the VC++ redistributable installed, so those DLLs do
    resolve from System32 on the runner and a plain "does it resolve here?"
    test passes while users' machines fail. That is exactly how #4781 shipped:
    AetherSDR.exe imports vcomp140.dll (ggml is built with /openmp), nothing
    staged it, and it went unnoticed through every release since ASR landed
    because CI never inspected the payload. Same class as the OpenSSL crash in
    #1341/#1362.

    Delay-load imports are held to the runtime rule but only warned about
    otherwise: a missing delay-load is a run-time feature failure rather than a
    startup failure, and Qt/ONNX Runtime delay-load optional OS components.

.PARAMETER DeployDir
    Directory holding the final payload (post runtime staging).

.PARAMETER DumpbinPath
    Override the dumpbin executable. Defaults to dumpbin on PATH (MSVC dev
    environment), then a vswhere lookup.

.PARAMETER SystemDir
    Override the Windows system directory used to resolve OS DLLs.
#>

# Keep this file ASCII-only -- see the note at the top of stage-msvc-runtime.ps1.
# Both scripts are invoked with `powershell` (5.1), where a UTF-8 em-dash inside
# a double-quoted string terminates that string under CP1252 decoding and the
# file fails to parse.

[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$DeployDir,
    [string]$DumpbinPath,
    [string]$SystemDir = (Join-Path $env:SystemRoot "System32")
)

$ErrorActionPreference = "Stop"

# msvcrt.dll (no digits after the prefix) is the OS-provided legacy CRT and is
# intentionally NOT matched -- only the redistributable, versioned runtimes are.
#
# An "mfc" hit means "add the Microsoft.VC<nnn>.MFC redist directory to
# stage-msvc-runtime.ps1", not "the payload is broken": MFC ships in a third
# sibling of the CRT/OpenMP dirs that the staging script does not copy. Very
# unlikely for a Qt app, but the entry stays so it fails loudly rather than
# resolving from the machine's redistributable.
$RuntimeDllPattern = '^(msvcp|msvcr|vcruntime|vcomp|concrt|vccorlib|mfc)\d'
$ApiSetPattern     = '^(api-ms-win-|ext-ms-)'

function Resolve-Dumpbin {
    param([string]$Explicit)

    if ($Explicit) {
        if (-not (Test-Path -LiteralPath $Explicit)) {
            throw "dumpbin not found at $Explicit"
        }
        return $Explicit
    }

    $onPath = Get-Command dumpbin -ErrorAction SilentlyContinue
    if ($onPath) {
        return $onPath.Source
    }

    $vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
    if (Test-Path -LiteralPath $vswhere) {
        $vsRoot = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
        if ($vsRoot) {
            $found = Get-ChildItem -LiteralPath (Join-Path $vsRoot "VC\Tools\MSVC") -Recurse -Filter "dumpbin.exe" -ErrorAction SilentlyContinue |
                Where-Object { $_.FullName -match "\\bin\\Hostx64\\x64\\dumpbin\.exe$" } |
                Select-Object -First 1
            if ($found) {
                return $found.FullName
            }
        }
    }

    throw "dumpbin.exe not found. Run this from an MSVC developer environment or pass -DumpbinPath."
}

function Get-PeDependencies {
    param(
        [string]$Dumpbin,
        [string]$ImagePath
    )

    # Scope ErrorActionPreference around the native call. Windows PowerShell 5.1
    # (what the CI step invokes, not pwsh 7) turns each stderr line of a native
    # command redirected with 2>&1 into an ErrorRecord, and under "Stop" that is
    # a terminating NativeCommandError raised before $LASTEXITCODE is read -- so
    # a benign dumpbin warning (LNK4048 on a file that is not a well-formed PE)
    # would kill the job with an opaque error instead of the message below.
    $output = & {
        $ErrorActionPreference = "Continue"
        & $Dumpbin /nologo /dependents $ImagePath 2>&1
    }
    if ($LASTEXITCODE -ne 0) {
        throw "dumpbin failed on ${ImagePath}: $($output -join [Environment]::NewLine)"
    }

    $direct = @()
    $delayed = @()
    $section = "none"

    foreach ($rawLine in $output) {
        $line = "$rawLine".Trim()

        if ($line -match 'following delay load dependencies:') { $section = "delay"; continue }
        if ($line -match 'following dependencies:')            { $section = "direct"; continue }
        if ($line -eq "Summary")                               { $section = "none"; continue }
        if ($section -eq "none" -or -not $line)                { continue }

        # Dependency lines are bare file names; anything else ends the section.
        if ($line -notmatch '^[^\s]+\.dll$') {
            $section = "none"
            continue
        }

        if ($section -eq "direct") { $direct += $line } else { $delayed += $line }
    }

    return [pscustomobject]@{
        Direct  = $direct
        Delayed = $delayed
    }
}

function Test-DependencySatisfied {
    <#
        Returns 'shipped', 'system', 'missing-runtime' or 'missing'.
    #>
    param(
        [string]$Name,
        [hashtable]$ShippedFiles,
        [string]$SystemDirectory
    )

    if ($ShippedFiles.ContainsKey($Name.ToLowerInvariant())) {
        return "shipped"
    }

    # Checked before the System32 probe on purpose -- see the header comment.
    if ($Name -match $RuntimeDllPattern) {
        return "missing-runtime"
    }

    if ($Name -match $ApiSetPattern) {
        return "system"
    }

    if ($SystemDirectory -and (Test-Path -LiteralPath (Join-Path $SystemDirectory $Name))) {
        return "system"
    }

    return "missing"
}

$resolvedDeployDir = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($DeployDir)
if (-not (Test-Path -LiteralPath $resolvedDeployDir)) {
    throw "Deploy directory not found: $resolvedDeployDir"
}

$dumpbin = Resolve-Dumpbin -Explicit $DumpbinPath
Write-Host "Auditing $resolvedDeployDir with $dumpbin"

$images = Get-ChildItem -LiteralPath $resolvedDeployDir -Recurse -File |
    Where-Object { $_.Extension -in ".dll", ".exe" } |
    Sort-Object FullName

if (-not $images) {
    throw "No .dll/.exe images found under $resolvedDeployDir."
}

# Index every shipped file by bare name, including the ones in subdirectories.
# That is deliberately more permissive than the real loader -- a DLL sitting only
# in deploy\imageformats\ would not satisfy an import from deploy\platforms\ --
# and it errs toward passing on purpose: a false failure blocking a release is
# worse than missing an exotic cross-plugin case.
$shippedFiles = @{}
foreach ($image in $images) {
    $shippedFiles[$image.Name.ToLowerInvariant()] = $true
}

$failures = @()
$warnings = @()

foreach ($image in $images) {
    $deps = Get-PeDependencies -Dumpbin $dumpbin -ImagePath $image.FullName

    foreach ($dep in $deps.Direct) {
        $verdict = Test-DependencySatisfied -Name $dep -ShippedFiles $shippedFiles -SystemDirectory $SystemDir
        if ($verdict -eq "missing-runtime") {
            $failures += "$($image.Name) imports $dep -- MSVC runtime not staged app-local"
        } elseif ($verdict -eq "missing") {
            $failures += "$($image.Name) imports $dep -- not in the payload and not a system DLL"
        }
    }

    foreach ($dep in $deps.Delayed) {
        $verdict = Test-DependencySatisfied -Name $dep -ShippedFiles $shippedFiles -SystemDirectory $SystemDir
        if ($verdict -eq "missing-runtime") {
            $failures += "$($image.Name) delay-loads $dep -- MSVC runtime not staged app-local"
        } elseif ($verdict -eq "missing") {
            $warnings += "$($image.Name) delay-loads $dep -- not in the payload and not a system DLL"
        }
    }
}

foreach ($warning in $warnings) {
    Write-Warning $warning
}

if ($failures) {
    foreach ($failure in $failures) {
        Write-Host "MISSING: $failure"
    }
    throw "Windows payload is not self-contained: $($failures.Count) unresolved dependenc$(if ($failures.Count -eq 1) { 'y' } else { 'ies' })."
}

Write-Host "Payload is self-contained: $($images.Count) images audited, every import resolves app-local or to the OS."
