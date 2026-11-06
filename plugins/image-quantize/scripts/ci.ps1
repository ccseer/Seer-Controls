# ci.ps1
#
# CI wrapper for the seer_image_quantize package (SPEC-003 Phase 0, 11.4).
# Runs the three documented commands from the package directory, in order:
#   cmake --preset default
#   cmake --build --preset default
#   ctest --preset default --output-on-failure
# A failure in any step stops the run and is propagated as a non-zero exit
# code (the failing tool's own exit code). No interactive prompts are used and
# no Start-Process is used: native tools are invoked directly in-process.

# Neutralise any $ErrorActionPreference = 'Stop' inherited from the caller.
# With 'Stop', PowerShell 5.1 can turn a native tool's ordinary stderr output
# into a terminating error, which would abort a build that only warned.
$ErrorActionPreference = 'Continue'

$packageDir = Split-Path -Parent $PSScriptRoot

$steps = @(
    @{ Exe = 'cmake'; Arguments = @('--preset', 'default') },
    @{ Exe = 'cmake'; Arguments = @('--build', '--preset', 'default') },
    @{ Exe = 'ctest'; Arguments = @('--preset', 'default', '--output-on-failure') }
)

$exitCode = 0

Push-Location -Path $packageDir
try {
    foreach ($step in $steps) {
        $exe = $step.Exe
        $arguments = $step.Arguments
        Write-Host ('==> ' + $exe + ' ' + ($arguments -join ' '))
        & $exe @arguments
        $stepCode = $LASTEXITCODE
        if ($stepCode -ne 0) {
            Write-Host ('CI FAILED (exit ' + $stepCode + '): ' + $exe + ' ' +
                        ($arguments -join ' ')) -ForegroundColor Red
            $exitCode = $stepCode
            break
        }
    }
}
finally {
    Pop-Location
}

if ($exitCode -eq 0) {
    Write-Host 'CI OK: configure, build and test all passed.'
}
exit $exitCode
