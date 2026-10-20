param(
    [ValidateRange(0, 512000)][int]$SizeMiB = 51200,
    [ValidateRange(0, 1000000)][int]$SmallFiles = 100000
)
$ErrorActionPreference = 'Stop'
if ($SizeMiB % 4 -ne 0 -or ($SizeMiB -eq 0 -and $SmallFiles -eq 0)) {
    throw 'Use a multiple of 4 MiB and select at least one workload.'
}
$ProjectRoot = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..')).Path
$BinaryPath = Join-Path $ProjectRoot 'out/release/backupctl'
if (-not (Test-Path -LiteralPath $BinaryPath -PathType Leaf)) {
    throw 'Run scripts/dev.ps1 first to build the Linux executable.'
}
$RunId = [guid]::NewGuid().ToString('N')
$ContainerName = "backupctl-restore-$RunId"
$OutputPath = Join-Path $ProjectRoot "out/restore-scale-$RunId"
$ContainerCreated = $false
Push-Location -LiteralPath $ProjectRoot
try {
    $Revision = git rev-parse HEAD
    if ($LASTEXITCODE -ne 0) { throw 'Could not identify the source revision.' }
    docker image inspect backupctl-dev:local *> $null
    if ($LASTEXITCODE -ne 0) { throw 'Run scripts/dev.ps1 first to build the isolated image.' }
    $RunScript = @"
set -eu
cp /input/out/release/backupctl /work/backupctl
chmod 755 /work/backupctl
python3 /input/benchmarks/restore_scale.py /work/backupctl --work /work/scale --output /work/results --size-mib $SizeMiB --small-files $SmallFiles --source-revision $Revision
"@
    docker create --name $ContainerName --cpus 4 --memory 8g `
        --mount "type=bind,source=$ProjectRoot,target=/input,readonly" `
        backupctl-dev:local bash -lc $RunScript
    if ($LASTEXITCODE -ne 0) { throw 'Could not create the measurement container.' }
    $ContainerCreated = $true
    docker start -a $ContainerName
    if ($LASTEXITCODE -ne 0) { throw 'The measurement container could not finish.' }
    $ContainerExit = docker inspect --format '{{.State.ExitCode}}' $ContainerName
    if ($ContainerExit -ne '0') { throw "The workload exited with $ContainerExit." }
} finally {
    if ($ContainerCreated) {
        New-Item -ItemType Directory -Path $OutputPath -Force | Out-Null
        docker cp "${ContainerName}:/work/results/." $OutputPath
        $CopyExit = $LASTEXITCODE
        if ($CopyExit -eq 0) {
            docker rm -f $ContainerName | Out-Null
            Write-Output "Saved measurements to $OutputPath"
        } else {
            Write-Warning "Evidence export failed. Container retained: $ContainerName"
        }
    }
    Pop-Location
}
