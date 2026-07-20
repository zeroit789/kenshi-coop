# Arregla el Plugins_x64.cfg del PC2 (separa la linea pegada)
$f = "D:\SteamLibrary\steamapps\common\Kenshi\Plugins_x64.cfg"
$c = Get-Content $f -Raw
$c = $c -replace 'Plugin_Terrain_x64Plugin=KenshiMP\.Core', "Plugin_Terrain_x64`r`nPlugin=KenshiMP.Core"
# Asegura newline final
if ($c -notmatch "`n$") { $c = $c.TrimEnd() + "`r`n" }
Set-Content -Path $f -Value $c -NoNewline -Encoding ascii
Write-Host "=== Plugins_x64.cfg ARREGLADO ==="
Get-Content $f
