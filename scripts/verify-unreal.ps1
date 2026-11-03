param(
    [Parameter(Mandatory = $true)][string]$EngineRoot,
    [Parameter(Mandatory = $true)][string]$SdkRoot,
    [Parameter(Mandatory = $true)][string]$Output,
    [ValidateSet("Development", "Shipping")][string]$Configuration = "Development",
    [switch]$BuildOnly,
    [switch]$Package,
    [switch]$RunPackaged,
    [switch]$Pie
)
$ErrorActionPreference = "Stop"
if (-not $IsLinux) {
    throw "The retained Unreal source tuple is Linux-only; native Windows remains unqualified. Run on the matching Linux engine host."
}
$Runner = Join-Path $PSScriptRoot "../integrations/unreal/run.py"
$RunArgs = @($Runner, '--engine-root', $EngineRoot, '--sdk-root', $SdkRoot,
    '--output', $Output, '--configuration', $Configuration)
if ($BuildOnly) { $RunArgs += '--build-only' }
if ($Package) { $RunArgs += '--package' }
if ($RunPackaged) { $RunArgs += '--run-packaged' }
if ($Pie) { $RunArgs += '--pie' }
& python3 @RunArgs
if ($LASTEXITCODE -ne 0) { throw "Unreal validation failed with exit code $LASTEXITCODE" }
