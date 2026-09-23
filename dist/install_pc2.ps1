# ES: Instalador de KenshiMP para el segundo PC de pruebas, PC2: copia desde C:\Users\umi\kmp la DLL,
#     los layouts y el .mod a la instalacion de Kenshi en D:, activa el plugin, anade el mod a
#     __mods.list, hace copias de seguridad y deja client.json apuntando al servidor para autoconexion.
#     Rutas fijas. Ojo: incluye la IP publica del servidor en un repo publico.
#     Uso: powershell -File install_pc2.ps1
# EN: KenshiMP installer for the second test PC, PC2: copies the DLL, layouts and .mod from
#     C:\Users\umi\kmp into the Kenshi install on D:, enables the plugin, adds the mod to __mods.list,
#     makes backups and writes client.json pointing to the server for auto-connect.
#     Hardcoded paths. Note: it contains the server's public IP in a public repo.
#     Usage: powershell -File install_pc2.ps1

# Instalador KenshiMP para el PC2 (ejecutado de forma remota via SSH)
# EN: KenshiMP installer for PC2 (run remotely over SSH)
$ErrorActionPreference = "Stop"
$KENSHI = "D:\SteamLibrary\steamapps\common\Kenshi"
$SRC    = "C:\Users\umi\kmp"
$LAYOUT = "$KENSHI\data\gui\layout"
$BK     = "$KENSHI\KenshiMP_backup"

Write-Host "=== Instalando KenshiMP en $KENSHI ==="
if (-not (Test-Path "$KENSHI\kenshi_x64.exe")) { throw "No se encuentra Kenshi en $KENSHI" }
New-Item -ItemType Directory -Force -Path $BK | Out-Null

# 1) Backups de lo que vamos a tocar
# EN: 1) Backups of what we are going to touch
if ((Test-Path "$KENSHI\Plugins_x64.cfg") -and -not (Test-Path "$BK\Plugins_x64.cfg.bak")) {
    Copy-Item "$KENSHI\Plugins_x64.cfg" "$BK\Plugins_x64.cfg.bak" -Force
}
if ((Test-Path "$LAYOUT\Kenshi_MainMenu.layout") -and -not (Test-Path "$BK\Kenshi_MainMenu.layout.bak")) {
    Copy-Item "$LAYOUT\Kenshi_MainMenu.layout" "$BK\Kenshi_MainMenu.layout.bak" -Force
}
Write-Host "[OK] Backups en $BK"

# 2) DLL del mod
# EN: 2) The mod DLL
Copy-Item "$SRC\KenshiMP.Core.dll" "$KENSHI\KenshiMP.Core.dll" -Force
Write-Host "[OK] KenshiMP.Core.dll copiado"

# 3) Plugins_x64.cfg -> activar el plugin
# EN: 3) Plugins_x64.cfg -> enable the plugin
$plug = "$KENSHI\Plugins_x64.cfg"
if (-not (Select-String -Path $plug -Pattern "Plugin=KenshiMP.Core" -Quiet -EA SilentlyContinue)) {
    Add-Content -Path $plug -Value "Plugin=KenshiMP.Core"
    Write-Host "[OK] Plugin=KenshiMP.Core anadido"
} else { Write-Host "[--] Plugin ya estaba" }

# 4) Layouts (menu MP + HUD + boton MULTIPLAYER ya parcheado)
# EN: 4) Layouts (MP menu + HUD + already patched MULTIPLAYER button)
Copy-Item "$SRC\Kenshi_MainMenu.layout"         "$LAYOUT\Kenshi_MainMenu.layout" -Force
Copy-Item "$SRC\Kenshi_MultiplayerPanel.layout" "$LAYOUT\Kenshi_MultiplayerPanel.layout" -Force
Copy-Item "$SRC\Kenshi_MultiplayerHUD.layout"   "$LAYOUT\Kenshi_MultiplayerHUD.layout" -Force
Write-Host "[OK] Layouts copiados"

# 5) El .mod (faccion Nameless) en data/ y mods/
# EN: 5) The .mod (Nameless faction) into data/ and mods/
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
# EN: 6) Pre-configure the connection to the host server (auto-connect)
$cfgDir = "$env:APPDATA\KenshiMP"
New-Item -ItemType Directory -Force -Path $cfgDir | Out-Null
$cfg = '{ "lastServer": "85.57.86.232", "lastPort": 27800, "playerName": "Umi" }'
Set-Content -Path "$cfgDir\client.json" -Value $cfg -Encoding ascii
Write-Host "[OK] client.json -> server 85.57.86.232:27800 (auto-connect)"

Write-Host ""
Write-Host "=== INSTALACION COMPLETA EN EL PC2 ==="
