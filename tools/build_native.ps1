[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$Sdk,
    [string]$RustCompiler = 'rustc',
    [string]$Build = 'build/native'
)
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path "$PSScriptRoot/..").Path
$lib = (Resolve-Path $Sdk).Path
$rust = (Get-Command $RustCompiler).Source
$env:PATH = "$lib/bin;$env:PATH"
$cmake = (Get-Command cmake -ErrorAction SilentlyContinue).Source
if (!$cmake) {
    $finder = "${env:ProgramFiles(x86)}/Microsoft Visual Studio/Installer/vswhere.exe"
    $cmake = (& $finder -all -products '*' -property installationPath | ForEach-Object {Join-Path $_ 'Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe'} | Where-Object {Test-Path $_} | Select-Object -First 1)
}
if (!$cmake) { throw 'Install CMake or Visual Studio CMake tools' }
& $cmake -S $root -B $Build -G 'Visual Studio 17 2022' -A x64 '-DRAYSCULPT_NATIVE_DESKTOP=ON' "-DCMAKE_PREFIX_PATH=$lib" "-DOpenCV_DIR=$lib/cmake" "-DRUSTC_EXECUTABLE=$rust"
if ($LASTEXITCODE) { exit $LASTEXITCODE }
& $cmake --build $Build --config Release --parallel 2
if ($LASTEXITCODE) { exit $LASTEXITCODE }
& (Join-Path (Split-Path $cmake) 'ctest.exe') --test-dir $Build -C Release --output-on-failure
exit $LASTEXITCODE
