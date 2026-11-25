[CmdletBinding()]
param([Parameter(Mandatory)][string]$Package)
$ErrorActionPreference='Stop'
$packageDir=(Resolve-Path -LiteralPath $Package).Path
$project=(Resolve-Path "$PSScriptRoot/..").Path
$work=(New-Item -ItemType Directory -Path (Join-Path ([IO.Path]::GetTempPath()) ("RaySculpt native "+[Guid]::NewGuid()))).FullName
function Assert-That($condition,[string]$message){if(!$condition){throw $message}}
function Invoke-Native([string]$file,[string[]]$arguments,[int]$expected=0){
    $config=[Diagnostics.ProcessStartInfo]::new((Join-Path $packageDir $file))
    $config.WorkingDirectory=$work
    $config.UseShellExecute=$false
    $config.CreateNoWindow=$true
    $config.Environment['PATH']="$env:SystemRoot/System32;$env:SystemRoot"
    foreach($key in @('QT_PLUGIN_PATH','PYTHONPATH','PYTHONHOME','CSF_SHMessage','CSF_OCCTResourcePath')){[void]$config.Environment.Remove($key)}
    foreach($arg in $arguments){$config.ArgumentList.Add($arg)}
    $proc=[Diagnostics.Process]::Start($config)
    if(!$proc.WaitForExit(120000)){$proc.Kill($true);throw "Timeout in $file $arguments"}
    Assert-That ($proc.ExitCode -eq $expected) "$file $arguments returned $($proc.ExitCode)"
    $proc.Dispose()
}
function Get-Scene {Get-Content "$work/extracted/scene.json" -Raw | ConvertFrom-Json -AsHashtable}
function Check-Job([string]$label,[hashtable]$scene,[string]$task,[hashtable]$extra=@{},[int]$expected=0){
    $folder=(New-Item -ItemType Directory -Path "$work/$label").FullName
    $request=@{scene=$scene;out=$folder;assets="$work/extracted/assets";task=$task;quality=0;rays=120000;resolution=64}+$extra
    $request | ConvertTo-Json -Depth 100 -WarningAction Stop | Set-Content -Encoding utf8NoBOM "$folder/input.json"
    Invoke-Native RaySculpt.exe @('--job',"$folder/input.json","$folder/result.json") $expected
    Get-Content "$folder/result.json" -Raw | ConvertFrom-Json -AsHashtable
}
function Assets-Match([string]$a,[string]$b){
    $first=@(Get-ChildItem -LiteralPath $a -File)
    Assert-That ($first.Count -eq @(Get-ChildItem -LiteralPath $b -File).Count) 'Attachment count changed'
    foreach($f in $first){Assert-That ((Get-FileHash $f.FullName).Hash -eq (Get-FileHash (Join-Path $b $f.Name)).Hash) 'Attachment changed'}
}
try{
    Assert-That (-not (Get-ChildItem -LiteralPath $packageDir -File -Recurse | Where-Object {$_.Name -match '^(python(w|\d*)\.(dll|exe)|pyside|shiboken)' -or $_.Extension -in '.py','.pyc'})) 'Unexpected Python component in package'
    $model="$project/examples/mona_lisa.ccad"
    Invoke-Native RaySculpt.exe @('--roundtrip',$model,"$work/Мона Лиза.ccad")
    [IO.Compression.ZipFile]::ExtractToDirectory($model,"$work/extracted")
    [IO.Compression.ZipFile]::ExtractToDirectory("$work/Мона Лиза.ccad","$work/restored")
    Assets-Match "$work/extracted/assets" "$work/restored/assets"
    $before=Get-Scene
    $round=Get-Content "$work/restored/scene.json" -Raw | ConvertFrom-Json -AsHashtable
    Assert-That ($before.objects[0].id -eq $round.objects[0].id -and $before.version -eq $round.version) 'CCAD identity changed'
    $trace=Check-Job 'trace' $before 'trace'
    Assert-That ($trace.screens[0].flux -gt 0 -and (Test-Path $trace.screens[0].image)) 'Empty projection'
    $design=Check-Job 'design' $before 'design'
    Assert-That ((Test-Path $design.profile) -and (Test-Path $design.screens[0].target)) 'No photographic surface or target'
    $flat=Get-Scene
    foreach($key in @('profile','min_relief','max_relief','design_n')){$flat.objects[0].Remove($key)}
    $layout=Check-Job 'layout' $flat 'placement' @{mode='sun';screen=$flat.objects[2].id;min_gap=40;max_gap=100;max_lamp=300;size=2}
    Assert-That ($layout.choices.Count -ge 1 -and $layout.choices[0].gap -ge 40) 'No layout'
    $bad=Get-Scene
    $bad.objects[1].pos=$bad.objects[2].pos
    $invalid=Check-Job 'rejected' $bad 'trace' @{} 1
    Assert-That (!$invalid.screens -and $invalid.error) 'Invalid source not rejected'
    $turn=Get-Scene
    foreach($key in @('profile','min_relief','max_relief','design_n')){$turn.objects[0].Remove($key)}
    $turn.objects[2].Remove('image')
    $turn.objects[2].dims=@{w=50;d=50}
    $model3="$project/examples/sample_asymmetric.obj"
    $asset=(Get-FileHash -LiteralPath $model3).Hash.ToLowerInvariant()+'.obj'
    Copy-Item -LiteralPath $model3 -Destination "$work/extracted/assets/$asset"
    $turn.turntable=@{name='sample';asset=$asset;angles=@(-8,8);screen=$turn.objects[2].id}
    $views=Check-Job 'views' $turn 'design'
    Assert-That ($views.screens.Count -eq 3 -and $views.screens[1].intermediate -and (Test-Path $views.profile)) 'Missing intermediate turntable view'
    $two=Get-Scene
    foreach($key in @('profile','min_relief','max_relief','design_n')){$two.objects[0].Remove($key)}
    $other=($two.objects[2] | ConvertTo-Json -Depth 20 | ConvertFrom-Json -AsHashtable)
    $other.id='other-screen'
    $other.name='Второй экран'
    $two.objects+=@($other)
    for($i=0;$i -lt 2;$i++){
        $recv=$two.objects[2+$i]
        $recv.pos=@((-12.5+25*$i),0,125)
        $recv.dims=@{w=25;d=40}
        $path="$work/target$i.pgm"
        $bytes=[Collections.Generic.List[byte]]::new()
        $bytes.AddRange([Text.Encoding]::ASCII.GetBytes("P5`n40 64`n255`n"))
        for($y=0;$y -lt 64;$y++){for($x=0;$x -lt 40;$x++){
            $u=$x/19.5-1;$v=$y/31.5-1-0.45*(1-2*$i)
            $bytes.Add([byte](60+140*[Math]::Exp(-4*($u*$u+$v*$v))))
        }}
        [IO.File]::WriteAllBytes($path,$bytes.ToArray())
        $key=(Get-FileHash $path).Hash.ToLowerInvariant()+'.pgm'
        Copy-Item $path "$work/extracted/assets/$key"
        $recv.image=$key
    }
    $joint=Check-Job 'two-screens' $two 'design'
    Assert-That ($joint.screens.Count -eq 2 -and $joint.screens[0].flux -gt 0 -and $joint.screens[1].flux -gt 0) 'Missing a receiver in joint fit'
    $cli='caustic_cli.exe'
    $picture="$work/extracted/assets/"+$before.objects[2].image
    Invoke-Native $cli @('design-photo',$picture,'--out',"$work/photo",'--grid','48','--map-size','48','--resolution','64','--cells','94','--rays','60000','--corrections','0','--formats','step,brep,stl,3mf,obj')
    foreach($ext in @('step','brep','stl','3mf','obj')){Assert-That ((Get-Item "$work/photo/02_plate.$ext").Length -gt 128) "Empty $ext"}
    Invoke-Native $cli @('verify-solid',"$work/photo","$work/photo/02_plate.step")
    Invoke-Native $cli @('verify-stl',"$work/photo",'--side','120')
    Invoke-Native $cli @('trace-facets',"$work/photo",'--strata','2','--source-size','0.5')
    Invoke-Native $cli @('export-plate',"$work/photo",'--out',"$work/plate.stp")
    Invoke-Native $cli @('export-plate',"$work/photo",'--out',"$work/plate.stp") 1
    Write-Output 'Verified portable Windows runtime, optical jobs, CCAD v1, CAD export and optical verification.'
}finally{
    $full=[IO.Path]::GetFullPath($work)
    $temp=[IO.Path]::GetFullPath([IO.Path]::GetTempPath())
    if($full.StartsWith($temp,[StringComparison]::OrdinalIgnoreCase) -and [IO.Path]::GetFileName($full).StartsWith('RaySculpt native ')){Remove-Item -LiteralPath $full -Recurse -Force}
}
