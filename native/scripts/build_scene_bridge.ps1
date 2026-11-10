param(
    [string]$SourceRoot = (Join-Path $PSScriptRoot "..\..\tools\thprac_scene_bridge_source"),
    [string]$AtomicHelper,
    [switch]$Fetch,
    [switch]$FixtureOnly,
    [string]$FixtureProjectRoot
)

$ErrorActionPreference = "Stop"
$projectRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
. (Join-Path $PSScriptRoot "build_support.ps1")
$componentRoot = Join-Path $projectRoot "native/thprac_scene_bridge"
if (-not $FixtureProjectRoot) { $FixtureProjectRoot = $projectRoot }
$fixtureOut = Resolve-NativeBuildOutput -ProjectRoot $FixtureProjectRoot -Variant "scene_bridge_fixture"

function Invoke-SceneBridgeFixtures {
    param([string]$ComponentRoot,[string]$FixtureOut,[string]$VsDevCmd,[string]$MtPath)

    $fixture = Join-Path $FixtureOut "protocol_fixture.exe"
    $fixtureCmd = 'call "' + $VsDevCmd + '" -arch=x86 -host_arch=x64 >nul && cd /d "' + $FixtureOut + '" && cl /nologo /EHsc /std:c++20 /I"' + $ComponentRoot + '" "' + (Join-Path $ComponentRoot "protocol_fixture.cpp") + '" /Fe:"' + $fixture + '" /link /MANIFEST:EMBED /MANIFESTUAC:"level=''asInvoker'' uiAccess=''false''"'
    & $env:ComSpec /d /s /c $fixtureCmd | Out-Host
    if ($LASTEXITCODE -ne 0) { throw "Failed to build the native protocol fixture." }
    $fixtureManifest = Join-Path $FixtureOut "protocol_fixture.manifest.xml"
    & $MtPath "-inputresource:$fixture;#1" "-out:$fixtureManifest" | Out-Host
    if ($LASTEXITCODE -ne 0 -or -not (Select-String -Quiet -Path $fixtureManifest -Pattern 'level="asInvoker" uiAccess="false"')) { throw "Protocol fixture is missing its required asInvoker manifest." }
    & $fixture | Out-Host
    if ($LASTEXITCODE -ne 0) { throw "Native protocol layout fixture failed." }

    $dialogueFixture = Join-Path $FixtureOut "dialogue_harness_fixture.exe"
    $dialogueFixtureCmd = 'call "' + $VsDevCmd + '" -arch=x86 -host_arch=x64 >nul && cd /d "' + $FixtureOut + '" && cl /nologo /EHsc /std:c++20 /I"' + $ComponentRoot + '" "' + (Join-Path $ComponentRoot "dialogue_harness_fixture.cpp") + '" /Fe:"' + $dialogueFixture + '" /link /MANIFEST:EMBED /MANIFESTUAC:"level=''asInvoker'' uiAccess=''false''"'
    & $env:ComSpec /d /s /c $dialogueFixtureCmd | Out-Host
    if ($LASTEXITCODE -ne 0) { throw "Failed to build the native dialogue harness fixture." }
    $dialogueFixtureManifest = Join-Path $FixtureOut "dialogue_harness_fixture.manifest.xml"
    & $MtPath "-inputresource:$dialogueFixture;#1" "-out:$dialogueFixtureManifest" | Out-Host
    if ($LASTEXITCODE -ne 0 -or -not (Select-String -Quiet -Path $dialogueFixtureManifest -Pattern 'level="asInvoker" uiAccess="false"')) { throw "Dialogue fixture is missing its required asInvoker manifest." }
    & $dialogueFixture | Out-Host
    if ($LASTEXITCODE -ne 0) { throw "Native dialogue harness fixture failed." }

    return [pscustomobject]@{
        protocol_fixture = $fixture
        protocol_fixture_manifest = $fixtureManifest
        dialogue_fixture = $dialogueFixture
        dialogue_fixture_manifest = $dialogueFixtureManifest
    }
}

$vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path $vswhere)) { throw "Visual Studio Installer vswhere.exe was not found." }
$vsRoot = (& $vswhere -latest -products * -property installationPath).Trim()
if (-not $vsRoot) { throw "No Visual Studio installation was found." }
$vsDevCmd = Join-Path $vsRoot "Common7\Tools\VsDevCmd.bat"
$msbuild = Join-Path $vsRoot "MSBuild\Current\Bin\MSBuild.exe"
if (-not (Test-Path $vsDevCmd) -or -not (Test-Path $msbuild)) { throw "Visual Studio build tools are incomplete." }
if ($FixtureOnly) {
    $mt = Get-ChildItem (Join-Path ${env:ProgramFiles(x86)} "Windows Kits\10\bin") -Filter mt.exe -Recurse |
        Where-Object { $_.FullName -match "\\x86\\mt\.exe$" } | Sort-Object FullName -Descending | Select-Object -First 1
    if (-not $mt) { throw "Windows SDK mt.exe was not found." }
    $fixtures = Invoke-SceneBridgeFixtures -ComponentRoot $componentRoot -FixtureOut $fixtureOut -VsDevCmd $vsDevCmd -MtPath $mt.FullName
    Write-Output "Offline scene-bridge fixtures passed in $fixtureOut."
    return
}

$expectedCommit = "ffb077707e7779b8437608e2fbdb7d6e5775b863"
$protocolVersion = 13
if (-not $AtomicHelper) {
    $AtomicHelper = Join-Path $projectRoot "build/native/atomic_mapping_x64.dll"
}
$atomicHelper = (Resolve-Path -LiteralPath $AtomicHelper).Path
if ($Fetch -and -not (Test-Path -LiteralPath $SourceRoot)) {
    & git clone --no-checkout https://github.com/touhouworldcup/thprac.git $SourceRoot
    if ($LASTEXITCODE -ne 0) { throw "Could not clone THPrac." }
    & git -C $SourceRoot checkout --detach $expectedCommit
    if ($LASTEXITCODE -ne 0) { throw "Could not check out pinned THPrac commit $expectedCommit." }
    & git -C $SourceRoot submodule update --init --recursive
    if ($LASTEXITCODE -ne 0) { throw "Could not initialize THPrac submodules." }
}
$sourceRoot = (Resolve-Path $SourceRoot).Path
$actualCommit = (& git -C $sourceRoot rev-parse HEAD).Trim()
if ($actualCommit -ne $expectedCommit) { throw "Expected thprac $expectedCommit, found $actualCommit." }

$patchPath = Join-Path $componentRoot "thprac_scene_bridge.patch"
$forkFile = Join-Path $sourceRoot "thprac\src\thprac_th10.cpp"
$forkHeader = Join-Path $sourceRoot "thprac\src\thprac_scene_bridge_protocol.h"
$forkDialogueHeader = Join-Path $sourceRoot "thprac\src\thprac_scene_bridge_dialogue_harness.h"
$gamesHeader = Join-Path $sourceRoot "thprac\src\thprac_games.h"
$generatorBatch = Join-Path $sourceRoot "thprac\loc_json_build_msvc.bat"
$patchMarkersPresent = (Select-String -Quiet -Path $forkFile -Pattern "th10_scene_frontend") -and (Select-String -Quiet -Path $forkFile -Pattern "SceneDialogueBranch") -and (Select-String -Quiet -Path $gamesHeader -Pattern "HooksMatch") -and (Select-String -Quiet -Path $generatorBatch -Pattern "MANIFESTUAC")
$anyPatchMarker = (Select-String -Quiet -Path $forkFile -Pattern "th10_scene_frontend") -or (Select-String -Quiet -Path $gamesHeader -Pattern "HooksMatch")
if (-not $patchMarkersPresent -and $anyPatchMarker) { throw "The pinned checkout contains a partial scene bridge patch." }
if (-not $patchMarkersPresent) {
    & git -C $sourceRoot apply --check $patchPath
    if ($LASTEXITCODE -ne 0) { throw "The pinned checkout has unrelated changes; cannot apply the private bridge patch." }
    & git -C $sourceRoot apply $patchPath
    if ($LASTEXITCODE -ne 0) { throw "Could not apply the private bridge patch." }
}
& git -C $sourceRoot apply --reverse --check $patchPath
if ($LASTEXITCODE -ne 0) {
    throw "The bridge checkout does not match the current patch. Build from a fresh checkout of the pinned commit."
}
# The patched sources include these headers. This directory holds their only copies.
Copy-Item (Join-Path $componentRoot "protocol.h") $forkHeader -Force
Copy-Item (Join-Path $componentRoot "dialogue_harness.h") $forkDialogueHeader -Force

$generatorCmd = 'call "' + $vsDevCmd + '" -arch=x86 -host_arch=x64 >nul && cd /d "' + (Join-Path $sourceRoot "thprac") + '" && call .\loc_json_build_msvc.bat'
& $env:ComSpec /d /s /c $generatorCmd
if ($LASTEXITCODE -ne 0) { throw "Failed to build the upstream locale generator." }

$mt = Get-ChildItem (Join-Path ${env:ProgramFiles(x86)} "Windows Kits\10\bin") -Filter mt.exe -Recurse |
    Where-Object { $_.FullName -match "\\x86\\mt\.exe$" } | Sort-Object FullName -Descending | Select-Object -First 1
if (-not $mt) { throw "Windows SDK mt.exe was not found." }
$manifest = Join-Path $fixtureOut "loc_json.manifest.xml"
New-Item -ItemType Directory -Force -Path (Split-Path $manifest) | Out-Null
& $mt.FullName "-inputresource:$(Join-Path $sourceRoot 'thprac\loc_json.exe');#1" "-out:$manifest"
if ($LASTEXITCODE -ne 0) { throw "Could not inspect the locale generator manifest." }
if (-not (Select-String -Quiet -Path $manifest -Pattern 'level="asInvoker" uiAccess="false"')) { throw "Locale generator does not declare asInvoker." }

$fixtures = Invoke-SceneBridgeFixtures -ComponentRoot $componentRoot -FixtureOut $fixtureOut -VsDevCmd $vsDevCmd -MtPath $mt.FullName
$fixture = $fixtures.protocol_fixture
$fixtureManifest = $fixtures.protocol_fixture_manifest
$dialogueFixture = $fixtures.dialogue_fixture
$dialogueFixtureManifest = $fixtures.dialogue_fixture_manifest

$solutionDir = $sourceRoot.TrimEnd('\') + '\'
& $msbuild (Join-Path $sourceRoot "thprac.sln") /p:SolutionDir=$solutionDir /p:Configuration=Release /p:Platform=x86 /p:PlatformToolset=v143 /p:WindowsTargetPlatformVersion=10.0 /m /v:minimal
if ($LASTEXITCODE -ne 0) { throw "Private TH10 x86 DLL build failed." }
$dll = Join-Path $sourceRoot "Release\thprac32.dll"
$launcher = Join-Path $sourceRoot "Release\thprac.exe"
if (-not (Test-Path $dll) -or -not (Test-Path $launcher)) { throw "MSBuild did not produce the private x86 DLL and launcher at the expected paths." }
$launcherManifest = Join-Path $fixtureOut "thprac_launcher.manifest.xml"
& $mt.FullName "-inputresource:$launcher;#1" "-out:$launcherManifest"
if ($LASTEXITCODE -ne 0 -or -not (Select-String -Quiet -Path $launcherManifest -Pattern 'level="asInvoker" uiAccess="false"')) { throw "Private launcher is missing its required asInvoker manifest." }
$privateData = Join-Path $sourceRoot "Release\.thprac_data"
New-Item -ItemType Directory -Force -Path $privateData | Out-Null
Set-Content -Path (Join-Path $privateData "settings.json") -Value '{"check_update":2,"dont_search_ongoing_game":true,"thprac_admin_rights":false}' -Encoding ascii
Get-FileHash -Algorithm SHA256 $dll, $launcher, $atomicHelper | Format-Table -AutoSize
$privateBuild = @{
    upstream_commit = $expectedCommit
    protocol_version = $protocolVersion
    mapping_size_bytes = 384
    normal_game_section_kind = 6
    private_patch_sha256 = (Get-FileHash -Algorithm SHA256 $patchPath).Hash.ToLowerInvariant()
    protocol_fixture_sha256 = (Get-FileHash -Algorithm SHA256 $fixture).Hash.ToLowerInvariant()
    protocol_fixture_manifest_sha256 = (Get-FileHash -Algorithm SHA256 $fixtureManifest).Hash.ToLowerInvariant()
    dialogue_fixture_sha256 = (Get-FileHash -Algorithm SHA256 $dialogueFixture).Hash.ToLowerInvariant()
    dialogue_fixture_manifest_sha256 = (Get-FileHash -Algorithm SHA256 $dialogueFixtureManifest).Hash.ToLowerInvariant()
    launcher_manifest_sha256 = (Get-FileHash -Algorithm SHA256 $launcherManifest).Hash.ToLowerInvariant()
    launcher_sha256 = (Get-FileHash -Algorithm SHA256 $launcher).Hash.ToLowerInvariant()
    thprac32_sha256 = (Get-FileHash -Algorithm SHA256 $dll).Hash.ToLowerInvariant()
    thprac64_sha256 = (Get-FileHash -Algorithm SHA256 (Join-Path $sourceRoot "Release\thprac64.dll")).Hash.ToLowerInvariant()
    atomic_helper_sha256 = (Get-FileHash -Algorithm SHA256 $atomicHelper).Hash.ToLowerInvariant()
} | ConvertTo-Json
Set-Content -Path (Join-Path $sourceRoot "Release\scene_bridge_private_build.json") -Value $privateBuild -Encoding ascii
Write-Output "Built private THPrac x86 DLL: $dll"
Write-Output "Built private launcher: $launcher"
Write-Output "Installed game and installed thprac files were not used as build outputs."
