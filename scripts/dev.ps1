param([ValidateSet('release', 'clang', 'asan', 'tsan')][string]$Preset = 'release')
$ErrorActionPreference = 'Stop'
$ProjectRoot = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..')).Path
$ContainerName = "backupctl-build-$PID"
$BuildScript = @'
set -eu
mkdir -p /work
tar -C /input --exclude=.git --exclude=.cache --exclude=.deps --exclude=build --exclude=out -cf - . | tar -C /work -xf -
cmake --preset PRESET -DVCPKG_INSTALLED_DIR=/opt/dependencies/vcpkg_installed -DVCPKG_MANIFEST_INSTALL=OFF
cmake --build --preset PRESET -j4
ctest --preset PRESET
'@
$BuildScript = $BuildScript.Replace('PRESET', $Preset)
Push-Location -LiteralPath $ProjectRoot
try {
    docker build -t backupctl-dev:local .
    if ($LASTEXITCODE -ne 0) { throw 'The isolated development image did not build.' }
    docker create --name $ContainerName --mount "type=bind,source=$ProjectRoot,target=/input,readonly" backupctl-dev:local bash -lc $BuildScript
    if ($LASTEXITCODE -ne 0) { throw 'Could not create the project container.' }
    docker start -a $ContainerName
    if ($LASTEXITCODE -ne 0) { throw 'Build or tests failed. See the container output.' }
    $OutputPath = Join-Path $ProjectRoot "out\$Preset"
    New-Item -ItemType Directory -Path $OutputPath -Force | Out-Null
    docker cp "${ContainerName}:/work/build/$Preset/backupctl" $OutputPath
    if ($LASTEXITCODE -ne 0) { throw 'Could not export the Linux executable.' }
} finally {
    docker rm -f $ContainerName 2>$null | Out-Null
    Pop-Location
}
