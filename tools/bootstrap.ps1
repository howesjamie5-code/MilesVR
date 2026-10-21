param([switch]$Observer)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$destination = Join-Path $root 'third_party\OpenXR-SDK'
$commit = '977f6675bc0057d5a54ed290cb5c71c699b1c0ab'
if (-not (Test-Path -LiteralPath $destination)) {
    git clone --depth 1 --branch release-1.1.49 https://github.com/KhronosGroup/OpenXR-SDK.git $destination
    if ($LASTEXITCODE) { throw 'OpenXR SDK download failed' }
}
$actual = git -c "safe.directory=$($destination.Replace('\','/'))" -C $destination rev-parse HEAD
if ($LASTEXITCODE -or $actual -ne $commit) { throw 'OpenXR SDK revision differs from the pinned revision. Existing files were preserved.' }
Write-Output "OpenXR SDK 1.1.49 verified: $actual"
# The launcher window (apps/launcher) is drawn with Dear ImGui.
$imguiDestination = Join-Path $root 'third_party\imgui'
$imguiCommit = 'f1cc2ae15e53a861a874c3034aae6798fde194ab'
if (-not (Test-Path -LiteralPath $imguiDestination)) {
    git clone --depth 1 --branch v1.92.9b https://github.com/ocornut/imgui.git $imguiDestination
    if ($LASTEXITCODE) { throw 'Dear ImGui download failed' }
}
$imguiActual = git -c "safe.directory=$($imguiDestination.Replace('\','/'))" -C $imguiDestination rev-parse HEAD
if ($LASTEXITCODE -or $imguiActual -ne $imguiCommit) { throw 'Dear ImGui revision differs from the pinned revision. Existing files were preserved.' }
Write-Output "Dear ImGui 1.92.9b verified: $imguiActual"
# Body haptics (src/bhaptics_output.cpp) play through bHaptics' own SDK: its header, and its library, which the
# release zip ships as it comes (its bytes are pinned too: the SDK's CHANGELOG.md lists them).
$bhapticsDestination = Join-Path $root 'third_party\tact-cpp2'
$bhapticsCommit = 'd016a3f22a4c1e5b6b2192e3ed2224512a045a19'
if (-not (Test-Path -LiteralPath $bhapticsDestination)) {
    git clone --depth 1 --branch v2.7.1 https://github.com/bhaptics/tact-cpp2.git $bhapticsDestination
    if ($LASTEXITCODE) { throw 'bHaptics SDK download failed' }
}
$bhapticsActual = git -c "safe.directory=$($bhapticsDestination.Replace('\','/'))" -C $bhapticsDestination rev-parse HEAD
if ($LASTEXITCODE -or $bhapticsActual -ne $bhapticsCommit) { throw 'bHaptics SDK revision differs from the pinned revision. Existing files were preserved.' }
$bhapticsLibrary = Join-Path $bhapticsDestination 'sdk\lib\x64\bhaptics_library.dll'
if ((Get-FileHash -LiteralPath $bhapticsLibrary -Algorithm SHA256).Hash -ne '399992988DC899173E921BE23409BED6DC8E7E0CBE78F505452F311FEAD06B26') {
    throw 'The bHaptics library differs from the one pinned. Existing files were preserved.'
}
Write-Output "bHaptics SDK 2.7.1 verified: $bhapticsActual"
if ($Observer) {
    $hookDestination = Join-Path $root 'third_party\minhook'
    $hookCommit = 'c3fcafdc10146beb5919319d0683e44e3c30d537'
    if (-not (Test-Path -LiteralPath $hookDestination)) {
        git clone --depth 1 --branch v1.3.4 https://github.com/TsudaKageyu/minhook.git $hookDestination
        if ($LASTEXITCODE) { throw 'MinHook download failed' }
    }
    $hookActual = git -c "safe.directory=$($hookDestination.Replace('\','/'))" -C $hookDestination rev-parse HEAD
    if ($LASTEXITCODE -or $hookActual -ne $hookCommit) { throw 'MinHook revision differs from the pinned revision. Existing files were preserved.' }
    Write-Output "MinHook 1.3.4 verified: $hookActual"
}
