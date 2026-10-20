param([ValidateSet('release', 'clang', 'asan', 'tsan')][string]$Preset = 'release')
$ErrorActionPreference = 'Stop'
$ProjectRoot = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..')).Path
$ContainerName = "backupctl-build-$PID"
$ContainerCreated = $false
$BuildScript = @'
set -eu
mkdir -p /work
tar -C /input --exclude=.git --exclude=.cache --exclude=.deps --exclude=build --exclude=out --exclude=demo-data --exclude=benchmark-data --exclude=benchmark-results --exclude=vcpkg_installed --exclude=.env --exclude=.aws -cf - . | tar -C /work -xf -
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
    $ContainerCreated = $true
    docker start -a $ContainerName
    if ($LASTEXITCODE -ne 0) { throw 'Build or tests failed. See the container output.' }
    $ContainerExit = docker inspect --format '{{.State.ExitCode}}' $ContainerName
    if ($ContainerExit -ne '0') { throw "The Linux build exited with $ContainerExit." }
    $OutputPath = Join-Path $ProjectRoot "out\$Preset"
    New-Item -ItemType Directory -Path $OutputPath -Force | Out-Null
    docker cp "${ContainerName}:/work/build/$Preset/backupctl" $OutputPath
    if ($LASTEXITCODE -ne 0) { throw 'Could not export the Linux executable.' }
} finally {
    if ($ContainerCreated) { docker rm -f $ContainerName 2>$null | Out-Null }
    Pop-Location
}
