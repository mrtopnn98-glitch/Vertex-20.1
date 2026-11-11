$ErrorActionPreference = 'Stop'
$Path = 'C:\Vertex-20.1\src\game\client\components\aimbot.cpp'

if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
    throw "Не найден файл: $Path"
}

$Bytes = [System.IO.File]::ReadAllBytes($Path)
if ($Bytes.Length -lt 8) {
    throw 'aimbot.cpp слишком короткий — файл не изменён.'
}

$OriginalPrefix = [BitConverter]::ToString($Bytes, 0, [Math]::Min(16, $Bytes.Length))
$Offset = 0
while ($Bytes.Length -ge ($Offset + 3) -and
       $Bytes[$Offset] -eq 0xEF -and
       $Bytes[$Offset + 1] -eq 0xBB -and
       $Bytes[$Offset + 2] -eq 0xBF) {
    $Offset += 3
}

if ($Offset -gt 0) {
    $Backup = "$Path.before-bom-fix-$(Get-Date -Format 'yyyyMMdd-HHmmss').bak"
    Copy-Item -LiteralPath $Path -Destination $Backup
    $Fixed = New-Object byte[] ($Bytes.Length - $Offset)
    [Array]::Copy($Bytes, $Offset, $Fixed, 0, $Fixed.Length)
    [System.IO.File]::WriteAllBytes($Path, $Fixed)
    $Bytes = $Fixed
    Write-Host "Удалено BOM в начале файла: $Offset байт."
    Write-Host "Резервная копия: $Backup"
} else {
    Write-Host 'BOM в начале файла не найден; файл не изменялся.'
}

$NewPrefix = [BitConverter]::ToString($Bytes, 0, [Math]::Min(16, $Bytes.Length))
$FirstLine = [System.Text.Encoding]::UTF8.GetString($Bytes).Split("`n")[0].TrimEnd("`r")
Write-Host "Было:  $OriginalPrefix"
Write-Host "Стало: $NewPrefix"
Write-Host "Первая строка: $FirstLine"

if (-not $FirstLine.StartsWith('#include')) {
    throw 'После очистки первая строка всё ещё не начинается с #include. Файл не прошёл проверку; пришли вывод байтов выше.'
}
if ($Bytes[0] -ne 0x23) {
    throw 'Проверка провалена: первый байт не # (0x23).'
}
Write-Host 'OK: первый байт файла — # (0x23), BOM отсутствует.' -ForegroundColor Green
