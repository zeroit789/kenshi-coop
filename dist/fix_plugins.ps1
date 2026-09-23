# ES: Arreglo puntual para el segundo PC de pruebas, PC2: en su Plugins_x64.cfg la linea
#     Plugin_Terrain_x64 quedo pegada a Plugin=KenshiMP.Core; las separa, asegura salto de linea final y
#     muestra el resultado. Ruta fija D:\SteamLibrary. Uso: powershell -File fix_plugins.ps1
# EN: One-off fix for the second test PC, PC2: in its Plugins_x64.cfg the Plugin_Terrain_x64 line got
#     glued to Plugin=KenshiMP.Core; it splits them, ensures a trailing newline and shows the result.
#     Hardcoded path D:\SteamLibrary. Usage: powershell -File fix_plugins.ps1

# Arregla el Plugins_x64.cfg del PC2 (separa la linea pegada)
# EN: Fixes PC2's Plugins_x64.cfg (splits the glued line)
$f = "D:\SteamLibrary\steamapps\common\Kenshi\Plugins_x64.cfg"
$c = Get-Content $f -Raw
$c = $c -replace 'Plugin_Terrain_x64Plugin=KenshiMP\.Core', "Plugin_Terrain_x64`r`nPlugin=KenshiMP.Core"
# Asegura newline final
# EN: Ensure a trailing newline
if ($c -notmatch "`n$") { $c = $c.TrimEnd() + "`r`n" }
Set-Content -Path $f -Value $c -NoNewline -Encoding ascii
Write-Host "=== Plugins_x64.cfg ARREGLADO ==="
Get-Content $f
