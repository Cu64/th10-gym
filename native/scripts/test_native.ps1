# Build and run the offline C++ tests against the shipped sources. No game is required.
param([string]$OutputDirectory)

$ErrorActionPreference = 'Stop'
$root = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '../..')).Path
$native = Join-Path $root 'native'
$tests = Join-Path $native 'tests'
$include = Join-Path $native 'include/th10'
$src = Join-Path $native 'src'
$out = [IO.Path]::GetFullPath($(if ($OutputDirectory) { $OutputDirectory } else { Join-Path $root 'build/native/tests' }))
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$vs = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vs) { throw 'Visual Studio C++ x86 build tools are required.' }
$vcvars = Join-Path $vs 'VC/Auxiliary/Build/vcvarsall.bat'
if ("$root$out$vcvars" -match '[%!"]') { throw 'Build paths must not contain %, !, or double quotes.' }
$null = New-Item -ItemType Directory -Force -Path $out

$compile = "/nologo /std:c++17 /EHsc /MT /W4 /O2 /I`"$include`""
# Windows treats executables with names like "update" as installers unless the
# manifest says otherwise, so every test helper embeds asInvoker.
$link = '/link /MACHINE:X86 bcrypt.lib user32.lib /MANIFEST:EMBED /MANIFESTUAC:"level=''asInvoker'' uiAccess=''false''"'
$fixtureExports = '/EXPORT:FixtureGetMetadata=_FixtureGetMetadata@4 /EXPORT:FixtureReadPathAfterRelease=_FixtureReadPathAfterRelease@4'
$installerFlags = "/I`"$src/installer`""
$installerSources = @('report', 'remote_process', 'pe_file', 'hook_loader', 'call_patcher') | ForEach-Object { "$src/installer/$_.cpp" }

$targets = @(
    @{ Name = 'tick_input_control_tests'; Flags = ''; Sources = @("$tests/tick_input_control_tests.cpp") },
    @{ Name = 'input_controller_tests'; Flags = ''; Sources = @("$src/hook/input_controller.cpp", "$tests/input_controller_tests.cpp") },
    @{ Name = 'tick_gate_tests'; Flags = ''; Sources = @("$src/hook/input_controller.cpp", "$src/hook/tick_gate.cpp", "$tests/tick_gate_tests.cpp") },
    @{ Name = 'snapshot_capture_tests'; Flags = "/I`"$src/hook`""; Sources = @("$src/hook/input_controller.cpp", "$src/hook/tick_gate.cpp", "$src/hook/snapshot_capture.cpp", "$src/hook/snapshot_writer.cpp", "$tests/snapshot_capture_tests.cpp") },
    @{ Name = 'tick_hooks_tests'; Flags = '/DTH10_HOOK_WRAPPER_FIXTURE'; Sources = @("$src/hook/tick_hooks.cpp", "$tests/tick_hooks_tests.cpp") },
    @{ Name = 'installer_module_retry_tests'; Flags = $installerFlags; Sources = @("$src/installer/report.cpp", "$src/installer/remote_process.cpp", "$tests/installer_module_retry_tests.cpp") },
    @{ Name = 'pe_file_tests'; Flags = $installerFlags; Sources = @("$src/installer/report.cpp", "$src/installer/remote_process.cpp", "$src/installer/pe_file.cpp", "$tests/pe_file_tests.cpp") },
    @{ Name = 'installer_fixture'; Flags = ''; Sources = @("$tests/installer_fixture.cpp"); LinkExtra = $fixtureExports },
    @{ Name = 'installer_patch_tests'; Flags = $installerFlags; Sources = $installerSources + @("$tests/installer_patch_tests.cpp") }
)

$lines = @('@echo off', "call `"$vcvars`" x86 >nul || exit /b 1", "cd /d `"$out`" || exit /b 1")
foreach ($target in $targets) {
    $sources = ($target.Sources | ForEach-Object { "`"$_`"" }) -join ' '
    $lines += "cl $compile $($target.Flags) $sources /Fe:$($target.Name).exe $link $($target.LinkExtra) || exit /b 1"
}
$build = Join-Path $out 'build_native_tests.cmd'
$lines | Set-Content -LiteralPath $build -Encoding Ascii
& $env:ComSpec /d /c $build
if ($LASTEXITCODE -ne 0) { throw 'Native test build failed.' }

$env:PYTHONPATH = $root
foreach ($target in $targets) {
    $exe = Join-Path $out "$($target.Name).exe"
    & python -B -c 'import sys; from pathlib import Path; from th10_gym._win32.manifest import verify; verify(Path(sys.argv[1]))' $exe
    if ($LASTEXITCODE -ne 0) { throw "Refusing to run a helper without an asInvoker manifest: $exe" }
}

$failed = @()
foreach ($target in $targets) {
    if ($target.Name -eq 'installer_fixture') { continue }
    $exe = Join-Path $out "$($target.Name).exe"
    Write-Output "== $($target.Name)"
    if ($target.Name -eq 'installer_patch_tests') {
        & $exe (Join-Path $out 'installer_fixture.exe')
    } else {
        & $exe
    }
    if ($LASTEXITCODE -ne 0) { $failed += $target.Name }
}
if ($failed) { throw "Native tests failed: $($failed -join ', ')" }
Write-Output 'All native tests passed.'
