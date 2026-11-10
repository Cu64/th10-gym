param(
    [string]$OutputDirectory = (Join-Path $PSScriptRoot 'build/native'),
    [switch]$WithSceneBridge
)

$ErrorActionPreference = 'Stop'
$native = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot 'native')).Path
$out = [IO.Path]::GetFullPath($OutputDirectory)
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$vs = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vs) { throw 'Visual Studio C++ x86/x64 build tools are required.' }
$vcvars = Join-Path $vs 'VC/Auxiliary/Build/vcvarsall.bat'
$include = Join-Path $native 'include/th10'
$sources = Get-Content -Raw -LiteralPath (Join-Path $native 'gym_sources.json') | ConvertFrom-Json
$hookSources = ($sources.hook | ForEach-Object { '"' + (Join-Path $native $_) + '"' }) -join ' '
$definition = Join-Path $native $sources.definition
$installerSources = ($sources.installer | ForEach-Object { '"' + (Join-Path $native $_) + '"' }) -join ' '
$atomic = Join-Path $native $sources.atomic
if ("$native$out$vcvars" -match '[%!"]') { throw 'Build paths must not contain %, !, or double quotes.' }
$null = New-Item -ItemType Directory -Force -Path $out
$compile = "/nologo /std:c++17 /EHsc /MT /W4 /O2 /I`"$include`""
$manifest = '/MANIFEST:EMBED /MANIFESTUAC:"level=''asInvoker'' uiAccess=''false''"'
$build = Join-Path $out 'build_th10_gym.cmd'
@(
    '@echo off',
    "call `"$vcvars`" x86 || exit /b 1",
    "cd /d `"$out`" || exit /b 1",
    "cl $compile /LD $hookSources /link /MACHINE:X86 /OPT:NOREF /DYNAMICBASE:NO /FIXED /BASE:0x50000000 bcrypt.lib user32.lib /DEF:`"$definition`" /OUT:th10_hook.dll || exit /b 1",
    "cl $compile $installerSources /Fe:th10_hook_installer.exe /link /MACHINE:X86 bcrypt.lib $manifest || exit /b 1"
) | Set-Content -LiteralPath $build -Encoding Ascii
& $env:ComSpec /d /c $build
if ($LASTEXITCODE -ne 0) { throw 'TH10 x86 native build failed.' }
$build = Join-Path $out 'build_th10_gym_atomic.cmd'
@(
    '@echo off',
    "call `"$vcvars`" x64 || exit /b 1",
    "cd /d `"$out`" || exit /b 1",
    "cl $compile /LD `"$atomic`" /link /MACHINE:X64 /OUT:atomic_mapping_x64.dll || exit /b 1"
) | Set-Content -LiteralPath $build -Encoding Ascii
& $env:ComSpec /d /c $build
if ($LASTEXITCODE -ne 0) { throw 'TH10 x64 atomic helper build failed.' }
if ($WithSceneBridge) {
    & (Join-Path $native 'scripts/build_scene_bridge.ps1') `
        -AtomicHelper (Join-Path $out 'atomic_mapping_x64.dll') -Fetch
}
