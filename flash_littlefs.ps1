# Script para generar y flashear imagen LittleFS al ESP32-C6
# Requisitos:
#   1. mklittlefs.exe descargado (https://github.com/earlephilhower/mklittlefs/releases)
#   2. esptool instalado: python -m pip install esptool
#   3. Puerto COM conocido (típicamente COM6)

param(
    [string]$MklittlefsPath = "C:\Tools\mklittlefs.exe",
    [string]$Port = "COM6",
    [string]$DataDir = ".\SchedulingShips\data",
    [string]$OutputImage = ".\littlefs.bin"
)

# Colores para output
$ColorSuccess = 'Green'
$ColorError = 'Red'
$ColorInfo = 'Cyan'

Write-Host "╔════════════════════════════════════════════════════════════╗" -ForegroundColor $ColorInfo
Write-Host "║        LittleFS Image Generator & Flasher para ESP32-C6   ║" -ForegroundColor $ColorInfo
Write-Host "╚════════════════════════════════════════════════════════════╝" -ForegroundColor $ColorInfo

# Verificar que mklittlefs existe
if (-not (Test-Path $MklittlefsPath)) {
    Write-Host "❌ ERROR: mklittlefs.exe no encontrado en: $MklittlefsPath" -ForegroundColor $ColorError
    Write-Host ""
    Write-Host "Descarga mklittlefs desde:" -ForegroundColor $ColorInfo
    Write-Host "  https://github.com/earlephilhower/mklittlefs/releases" -ForegroundColor $ColorInfo
    Write-Host ""
    Write-Host "Y coloca el archivo en: $MklittlefsPath" -ForegroundColor $ColorInfo
    exit 1
}

# Verificar que data/ existe
if (-not (Test-Path $DataDir)) {
    Write-Host "❌ ERROR: Carpeta data/ no encontrada: $DataDir" -ForegroundColor $ColorError
    exit 1
}

Write-Host "✓ Archivos encontrados" -ForegroundColor $ColorSuccess

# Listar archivos en data/
Write-Host "`n📂 Archivos a incluir en la imagen:" -ForegroundColor $ColorInfo
Get-ChildItem -Path $DataDir -File | ForEach-Object {
    $size = $_.Length
    Write-Host "   • $($_.Name) ($size bytes)" -ForegroundColor $ColorInfo
}

# Generar imagen LittleFS
Write-Host "`n⚙️  Generando imagen LittleFS..." -ForegroundColor $ColorInfo

# Parámetros típicos para ESP32-C6
# -p 256 (page size)
# -b 4096 (block size)
# -s 0x160000 (filesystem size, ~1.4 MB)

$GenerateCmd = "& '$MklittlefsPath' -c '$DataDir' -p 256 -b 4096 -s 0x160000 '$OutputImage'"

Write-Host "  Ejecutando: $GenerateCmd" -ForegroundColor $ColorInfo

try {
    Invoke-Expression $GenerateCmd
} catch {
    Write-Host "❌ Error al generar imagen: $_" -ForegroundColor $ColorError
    exit 1
}

if (-not (Test-Path $OutputImage)) {
    Write-Host "❌ ERROR: La imagen no fue generada" -ForegroundColor $ColorError
    exit 1
}

$ImageSize = (Get-Item $OutputImage).Length
Write-Host "✓ Imagen generada: $OutputImage ($ImageSize bytes)" -ForegroundColor $ColorSuccess

# Flashear con esptool
Write-Host "`n⚙️  Flasheando al ESP32-C6..." -ForegroundColor $ColorInfo
Write-Host "  Puerto: $Port" -ForegroundColor $ColorInfo
Write-Host "  Offset: 0x290000 (típico para ESP32-C6)" -ForegroundColor $ColorInfo

$FlashCmd = "python -m esptool --chip esp32c6 --port $Port write_flash 0x290000 '$OutputImage'"

Write-Host "`n  Ejecutando: $FlashCmd" -ForegroundColor $ColorInfo
Write-Host "`n  ⚠️  Desconecta y reconecta el ESP32-C6 después de que termine." -ForegroundColor 'Yellow'

try {
    Invoke-Expression $FlashCmd
    Write-Host "`n✓ Flasheo completado" -ForegroundColor $ColorSuccess
} catch {
    Write-Host "❌ Error al flashear: $_" -ForegroundColor $ColorError
    exit 1
}

Write-Host "`n╔════════════════════════════════════════════════════════════╗" -ForegroundColor $ColorInfo
Write-Host "║                        ✓ Completado                        ║" -ForegroundColor $ColorInfo
Write-Host "╚════════════════════════════════════════════════════════════╝" -ForegroundColor $ColorInfo

Write-Host "`n📋 Próximos pasos:" -ForegroundColor $ColorInfo
Write-Host "  1. Reconecta el ESP32-C6 (o presiona RESET)" -ForegroundColor $ColorInfo
Write-Host "  2. Abre Monitor Serial a 115200 baud" -ForegroundColor $ColorInfo
Write-Host "  3. Verifica que los logs muestren:" -ForegroundColor $ColorInfo
Write-Host "     [CONFIG] Cargado channel_config.txt desde SPIFFS" -ForegroundColor $ColorInfo
Write-Host "     Demo entries cargados: 2" -ForegroundColor $ColorInfo
