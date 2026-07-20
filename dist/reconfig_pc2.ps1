# Reapunta el PC2 a la IP LOCAL del host (PC1) para conexion en la misma red
$f = "$env:APPDATA\KenshiMP\client.json"
$cfg = '{ "lastServer": "192.168.1.123", "lastPort": 27800, "playerName": "Umi" }'
Set-Content -Path $f -Value $cfg -Encoding ascii
Write-Host "client.json actualizado:"
Get-Content $f
