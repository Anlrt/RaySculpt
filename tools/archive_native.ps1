[CmdletBinding()]
param([Parameter(Mandatory)][string]$Package,[string]$Output='build/releases')
$ErrorActionPreference='Stop'
$version=(Get-Content "$PSScriptRoot/../VERSION" -Raw).Trim()
$dir=[IO.Directory]::CreateDirectory([IO.Path]::GetFullPath($Output))
$name="RaySculpt-$version-Windows-x64.zip"
$dest=Join-Path $dir.FullName $name
if(Test-Path -LiteralPath $dest){throw "Archive already exists: $dest"}
[IO.Compression.ZipFile]::CreateFromDirectory((Resolve-Path $Package).Path,$dest,[IO.Compression.CompressionLevel]::Optimal,$false)
"$((Get-FileHash $dest).Hash.ToLowerInvariant())  $name" | Set-Content -Encoding ascii "$dest.sha256"
Write-Output $dest
