# ES: Reconfigura el cliente del PC2: reescribe APPDATA/KenshiMP/client.json para apuntar a la IP local
#     del host, 192.168.1.123:27800, en la misma red, y lo muestra. Uso: powershell -File reconfig_pc2.ps1
# EN: Reconfigures PC2's client: rewrites APPDATA/KenshiMP/client.json to point to the host's local IP,
#     192.168.1.123:27800, on the same network, and shows it. Usage: powershell -File reconfig_pc2.ps1

# Reapunta el PC2 a la IP LOCAL del host (PC1) para conexion en la misma red
# EN: Points PC2 at the host's (PC1) LOCAL IP for a same-network connection
$f = "$env:APPDATA\KenshiMP\client.json"
$cfg = '{ "lastServer": "192.168.1.123", "lastPort": 27800, "playerName": "Umi" }'
Set-Content -Path $f -Value $cfg -Encoding ascii
Write-Host "client.json actualizado:"
Get-Content $f
