# Instalador KenshiMP para el PC2 (ejecutado por Onyx via SSH)
$ErrorActionPreference = "Stop"
$KENSHI = "D:\SteamLibrary\steamapps\common\Kenshi"
$SRC    = "C:\Users\umi\kmp"
$LAYOUT = "$KENSHI\data\gui\layout"
$BK     = "$KENSHI\KenshiMP_backup"

Write-Host "=== Instalando KenshiMP en $KENSHI ==="
if (-not (Test-Path "$KENSHI\kenshi_x64.exe")) { throw "No se encuentra Kenshi en $KENSHI" }
New-Item -ItemType Directory -Force -Path $BK | Out-Null

# 1) Backups de lo que vamos a tocar
if ((Test-Path "$KENSHI\Plugins_x64.cfg") -and -not (Test-Path "$BK\Plugins_x64.cfg.bak")) {
    Copy-Item "$KENSHI\Plugins_x64.cfg" "$BK\Plugins_x64.cfg.bak" -Force
}
if ((Test-Path "$LAYOUT\Kenshi_MainMenu.layout") -and -not (Test-Path "$BK\Kenshi_MainMenu.layout.bak")) {
    Copy-Item "$LAYOUT\Kenshi_MainMenu.layout" "$BK\Kenshi_MainMenu.layout.bak" -Force
}
Write-Host "[OK] Backups en $BK"

# 2) DLL del mod
Copy-Item "$SRC\KenshiMP.Core.dll" "$KENSHI\KenshiMP.Core.dll" -Force
Write-Host "[OK] KenshiMP.Core.dll copiado"

# 3) Plugins_x64.cfg -> activar el plugin
$plug = "$KENSHI\Plugins_x64.cfg"
if (-not (Select-String -Path $plug -Pattern "Plugin=KenshiMP.Core" -Quiet -EA SilentlyContinue)) {
    Add-Content -Path $plug -Value "Plugin=KenshiMP.Core"
    Write-Host "[OK] Plugin=KenshiMP.Core anadido"
} else { Write-Host "[--] Plugin ya estaba" }

# 4) Layouts (menu MP + HUD + boton MULTIPLAYER ya parcheado)
Copy-Item "$SRC\Kenshi_MainMenu.layout"         "$LAYOUT\Kenshi_MainMenu.layout" -Force
Copy-Item "$SRC\Kenshi_MultiplayerPanel.layout" "$LAYOUT\Kenshi_MultiplayerPanel.layout" -Force
Copy-Item "$SRC\Kenshi_MultiplayerHUD.layout"   "$LAYOUT\Kenshi_MultiplayerHUD.layout" -Force
Write-Host "[OK] Layouts copiados"

# 5) El .mod (faccion Nameless) en data/ y mods/
Copy-Item "$SRC\kenshi-online.mod" "$KENSHI\data\kenshi-online.mod" -Force
New-Item -ItemType Directory -Force -Path "$KENSHI\mods\kenshi-online" | Out-Null
Copy-Item "$SRC\kenshi-online.mod" "$KENSHI\mods\kenshi-online\kenshi-online.mod" -Force
$ml = "$KENSHI\data\__mods.list"
if (-not (Test-Path $ml) -or -not (Select-String -Path $ml -Pattern "kenshi-online" -Quiet -EA SilentlyContinue)) {
    Add-Content -Path $ml -Value "kenshi-online"
    Write-Host "[OK] kenshi-online anadido a __mods.list"
} else { Write-Host "[--] kenshi-online ya en __mods.list" }
Write-Host "[OK] Mod instalado"

# 6) Pre-configurar conexion al server de Zero (auto-connect)
$cfgDir = "$env:APPDATA\KenshiMP"
New-Item -ItemType Directory -Force -Path $cfgDir | Out-Null
$cfg = '{ "lastServer": "85.57.86.232", "lastPort": 27800, "playerName": "Umi" }'
Set-Content -Path "$cfgDir\client.json" -Value $cfg -Encoding ascii
Write-Host "[OK] client.json -> server 85.57.86.232:27800 (auto-connect)"

Write-Host ""
Write-Host "=== INSTALACION COMPLETA EN EL PC2 ==="
