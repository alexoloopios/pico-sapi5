# Assembles dist\ from the two build trees.
#
# The layout is flat on purpose: both engine DLLs sit beside a single lang\
# directory, which is what lets the 32-bit and 64-bit builds share the six
# megabytes of lingware rather than each carrying a copy.

[CmdletBinding()]
param(
    [string] $Configuration = 'Release'
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$root = Split-Path -Parent $PSScriptRoot
$distDir = Join-Path $root 'dist'
$x64Bin = Join-Path $root "build\x64\bin\$Configuration"
$x86Bin = Join-Path $root "build\x86\bin\$Configuration"

foreach ($required in @($x64Bin, $x86Bin)) {
    if (-not (Test-Path $required)) {
        throw "missing build output: $required. Run tools\build_all.ps1 first."
    }
}

if (Test-Path $distDir) { Remove-Item -Recurse -Force $distDir }
New-Item -ItemType Directory -Path $distDir | Out-Null

# The engines. Each registers itself for applications of its own bitness.
Copy-Item (Join-Path $x64Bin 'PicoSAPI5.dll')     $distDir
Copy-Item (Join-Path $x86Bin 'PicoSAPI5_x86.dll') $distDir

# The lingware, shared by both.
Copy-Item (Join-Path $root 'lang') $distDir -Recurse

# The tools, 64-bit, for checking an installation after the fact.
foreach ($tool in @('pico_speak.exe', 'pico_render.exe', 'pico_sapitest.exe')) {
    Copy-Item (Join-Path $x64Bin $tool) $distDir
}

foreach ($document in @('README.md', 'CHANGELOG.md', 'LICENSE', 'NOTICE')) {
    $path = Join-Path $root $document
    if (Test-Path $path) { Copy-Item $path $distDir }
}

$size = (Get-ChildItem $distDir -Recurse -File | Measure-Object -Property Length -Sum).Sum
Write-Host ("staged {0} files, {1:N1} MB" -f `
    (Get-ChildItem $distDir -Recurse -File).Count, ($size / 1MB))
