[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$Sdk,
    [string]$Build = 'build/native',
    [string]$Destination = 'build/native/portable',
    [string]$PackageCache,
    [string]$RustCompiler = 'rustc'
)
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path "$PSScriptRoot/..").Path
$lib = (Resolve-Path $Sdk).Path
$trees = @("$lib/bin",(Split-Path $lib))
$buildDir = (Resolve-Path $Build).Path
if (Test-Path -LiteralPath $Destination) { throw "Choose a new output directory: $Destination" }
$dest = (New-Item -ItemType Directory -Path $Destination).FullName
$vswhere = "${env:ProgramFiles(x86)}/Microsoft Visual Studio/Installer/vswhere.exe"
$dumpbin = (& $vswhere -all -products '*' -property installationPath | ForEach-Object {
    Get-ChildItem "$_/VC/Tools/MSVC/*/bin/Hostx64/x64/dumpbin.exe" -ErrorAction SilentlyContinue
} | Sort-Object LastWriteTime -Descending | Select-Object -First 1 -ExpandProperty FullName)
if (!$dumpbin) { throw 'Visual Studio x64 C++ tools are required' }
$queue = [Collections.Generic.Queue[string]]::new()
$owners = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
function Copy-Runtime([string]$source, [string]$relative) {
    $target = Join-Path $dest $relative
    if (Test-Path -LiteralPath $target) { return }
    [void][IO.Directory]::CreateDirectory((Split-Path $target))
    Copy-Item -LiteralPath $source -Destination $target
    $queue.Enqueue($target)
    if ($source.StartsWith((Split-Path $lib),[StringComparison]::OrdinalIgnoreCase)) {
        [void]$owners.Add([IO.Path]::GetRelativePath((Split-Path $lib),$source).Replace('\','/'))
    }
}
Copy-Runtime "$buildDir/src/desktop/Release/raysculpt.exe" 'RaySculpt.exe'
Copy-Runtime "$buildDir/Release/caustic_cli.exe" 'caustic_cli.exe'
Copy-Runtime "$buildDir/Release/caustic_trace_demo.exe" 'caustic_trace_demo.exe'
$plugins = "$lib/lib/qt6/plugins"
foreach ($file in @('platforms/qwindows.dll','iconengines/qsvgicon.dll','imageformats/qjpeg.dll','imageformats/qsvg.dll','imageformats/qtiff.dll','imageformats/qwebp.dll')) {
    if (Test-Path "$plugins/$file") { Copy-Runtime "$plugins/$file" "plugins/$file" }
}
while ($queue.Count) {
    $path = $queue.Dequeue()
    $deps = & $dumpbin /nologo /dependents $path
    if ($LASTEXITCODE) { throw "Cannot inspect dependencies of $path" }
    foreach ($line in $deps) {
        if ($line -notmatch '^\s+([-\w.]+\.dll)\s*$') { continue }
        $name = $Matches[1]
        if ($name -match '^(python|pyside|shiboken)') { throw "Unexpected runtime dependency $name in $path" }
        if ($name -match '^(api-ms-|ext-ms-|ucrtbase\.)') { continue }
        $found = $false
        foreach ($tree in $trees) {
            $candidate = Join-Path $tree $name
            if (Test-Path -LiteralPath $candidate) { Copy-Runtime $candidate $name; $found = $true; break }
        }
        if (!$found -and !(Test-Path -LiteralPath "$env:SystemRoot/System32/$name")) { throw "Missing $name for $path" }
    }
}
"[Paths]`nPlugins=plugins" | Set-Content -Encoding ascii "$dest/qt.conf"
Copy-Item "$root/LICENSE","$root/THIRD_PARTY_NOTICES.md","$root/VERSION" $dest
Copy-Item "$root/examples/mona_lisa.ccad" $dest
$resource = "$lib/share/opencascade/resources"
if (Test-Path $resource) {
    $resources = New-Item -ItemType Directory -Path "$dest/occt-resources"
    foreach ($name in @('Shaders','StdResource','UnitsAPI','SHMessage','XSMessage','XSTEPResource','Textures')) {
        if (Test-Path "$resource/$name") {Copy-Item -LiteralPath "$resource/$name" -Destination $resources -Recurse}
    }
}
$notice = New-Item -ItemType Directory -Path "$dest/licenses"
$meta = Join-Path (Split-Path $lib) 'conda-meta'
$cache = if ($PackageCache) { (Resolve-Path $PackageCache).Path } else { '' }
foreach ($entry in Get-ChildItem $meta -Filter '*.json') {
    $pkg = Get-Content -Raw -LiteralPath $entry.FullName | ConvertFrom-Json
    if (!($pkg.files | Where-Object { $owners.Contains($_) -or $_ -like 'Library/share/opencascade/resources/*' } | Select-Object -First 1)) {continue}
    $id = "$($pkg.name)-$($pkg.version)-$($pkg.build)"
    $folder = $pkg.link.source
    if (!(Test-Path -LiteralPath "$folder/info") -and $cache) {
        $folder = (Get-ChildItem -Path $cache -Directory -Filter $id -Recurse | Select-Object -First 1).FullName
    }
    if (!(Test-Path -LiteralPath "$folder/info")) {throw "Missing conda license metadata for $id"}
    $to = New-Item -ItemType Directory -Path "$notice/$id"
    Copy-Item "$folder/info/index.json" $to
    if (Test-Path -LiteralPath "$folder/info/licenses") {Copy-Item -LiteralPath "$folder/info/licenses" $to -Recurse}
}
$rust = (& $RustCompiler --print sysroot)
if ($LASTEXITCODE) {throw 'Cannot locate Rust license notices'}
$rustNotice = New-Item -ItemType Directory -Path "$notice/Rust"
$docs = @(Get-ChildItem "$rust/share/doc/rust" | Where-Object Name -Match '^(COPYRIGHT|LICENSE)')
if (!$docs.Count) { throw 'Rust license files are missing' }
foreach ($doc in $docs) {Copy-Item -LiteralPath $doc.FullName -Destination $rustNotice -Recurse}
Write-Output $dest
