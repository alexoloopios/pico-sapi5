# Builds both architectures and stages a complete installation under dist\.
#
# Pico compiles for x86 and x64 alike, so this is two ordinary builds rather than
# the engine-plus-helper arrangement a 32-bit-only engine would need. Both DLLs
# end up in the same directory and share one copy of the lingware.
#
#   .\tools\build_all.ps1                  build, stage and run the tests
#   .\tools\build_all.ps1 -SkipTests       build and stage only
#   .\tools\build_all.ps1 -Installer       also compile the Inno Setup installer

[CmdletBinding()]
param(
    [string] $Configuration = 'Release',
    [switch] $SkipTests,
    [switch] $Installer,
    [switch] $Clean
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$root = Split-Path -Parent $PSScriptRoot
$distDir = Join-Path $root 'dist'
$generator = 'Visual Studio 17 2022'

function Invoke-Step {
    param([string] $Name, [scriptblock] $Body)
    Write-Host ""
    Write-Host "== $Name ==" -ForegroundColor Cyan
    & $Body
    if ($LASTEXITCODE -ne 0 -and $null -ne $LASTEXITCODE) {
        throw "$Name failed with exit code $LASTEXITCODE"
    }
}

if ($Clean) {
    Invoke-Step 'Cleaning' {
        foreach ($path in @((Join-Path $root 'build'), $distDir)) {
            if (Test-Path $path) { Remove-Item -Recurse -Force $path }
        }
        $global:LASTEXITCODE = 0
    }
}

# --- build both architectures ------------------------------------------------
$architectures = @(
    @{ Name = 'x64';   Platform = 'x64'   },
    @{ Name = 'x86';   Platform = 'Win32' }
)

foreach ($arch in $architectures) {
    $buildDir = Join-Path $root "build\$($arch.Name)"
    Invoke-Step "Configuring $($arch.Name)" {
        cmake -S $root -B $buildDir -G $generator -A $arch.Platform | Out-Null
    }
    Invoke-Step "Building $($arch.Name)" {
        cmake --build $buildDir --config $Configuration -- /verbosity:minimal /nologo
    }
}

# --- stage -------------------------------------------------------------------
Invoke-Step 'Staging dist\' {
    & (Join-Path $PSScriptRoot 'stage.ps1') -Configuration $Configuration
}

# --- test --------------------------------------------------------------------
if (-not $SkipTests) {
    # The suite is run once per voice, not just once. Pico suballocates from a
    # fixed arena, and at least one fault -- a voice failing to load after a
    # differently sized one had been used -- only appeared for particular
    # starting voices. Which voice the engine begins on is therefore part of the
    # state under test.
    $voices = @('en-US', 'en-GB', 'de-DE', 'es-ES', 'fr-FR', 'it-IT')
    foreach ($arch in $architectures) {
        $binDir = Join-Path $root "build\$($arch.Name)\bin\$Configuration"
        $dll = Join-Path $binDir $(if ($arch.Name -eq 'x86') { 'PicoSAPI5_x86.dll' } else { 'PicoSAPI5.dll' })
        Invoke-Step "Testing $($arch.Name)" {
            Push-Location $binDir
            try {
                foreach ($voice in $voices) {
                    $output = & .\pico_sapitest.exe --dll $dll --voice $voice
                    $verdict = $output | Select-String -Pattern 'ALL CHECKS PASSED|FAILURES'
                    Write-Host ("  {0,-6} {1}" -f $voice, $verdict.Line.Trim())
                    if ($LASTEXITCODE -ne 0) {
                        $output | Select-String -Pattern '\[FAIL\]' | ForEach-Object {
                            Write-Host "    $($_.Line.Trim())" -ForegroundColor Red
                        }
                        throw "the $($arch.Name) engine failed its checks with the $voice voice"
                    }
                }
                $global:LASTEXITCODE = 0
            } finally {
                Pop-Location
            }
        }
    }
}

# --- installer ---------------------------------------------------------------
if ($Installer) {
    $iscc = @(
        "${env:ProgramFiles(x86)}\Inno Setup 6\ISCC.exe",
        "$env:ProgramFiles\Inno Setup 6\ISCC.exe"
    ) | Where-Object { Test-Path $_ } | Select-Object -First 1
    if (-not $iscc) {
        throw 'Inno Setup 6 was not found; install it or run without -Installer.'
    }
    Invoke-Step 'Compiling the installer' {
        & $iscc (Join-Path $root 'installer\pico_sapi5.iss')
    }

    # The installer is attached to a release rather than committed, so this is
    # the last chance to see what was actually produced. The hash is mainly a
    # check that the file being uploaded is the one this run built and not one
    # left over in dist\; publishing it alongside the download is optional.
    $setup = Get-ChildItem (Join-Path $distDir 'PicoSAPI5-*-setup.exe') |
             Sort-Object LastWriteTime | Select-Object -Last 1
    if ($setup) {
        $hash = (Get-FileHash -Algorithm SHA256 -Path $setup.FullName).Hash.ToLower()
        Write-Host ""
        Write-Host "  $($setup.Name)  ($([math]::Round($setup.Length / 1MB, 2)) MB)"
        Write-Host "  SHA-256: $hash"
    }
}

Write-Host ""
Write-Host "Done. Staged installation in $distDir" -ForegroundColor Green
Write-Host "To install it, from an elevated prompt:" -ForegroundColor Green
Write-Host "  regsvr32 `"$distDir\PicoSAPI5.dll`"" -ForegroundColor Green
Write-Host "  regsvr32 `"$distDir\PicoSAPI5_x86.dll`"   (for 32-bit applications)" -ForegroundColor Green
