$ErrorActionPreference = 'Stop'

$ProjectRoot = 'C:\Vertex-20.1'
$Path = Join-Path $ProjectRoot 'src\engine\shared\config_variables.h'

if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
    throw "File not found: $Path"
}

$Bytes = [System.IO.File]::ReadAllBytes($Path)
if ($Bytes.Length -ge 2 -and (($Bytes[0] -eq 0xFF -and $Bytes[1] -eq 0xFE) -or ($Bytes[0] -eq 0xFE -and $Bytes[1] -eq 0xFF))) {
    throw 'The file is UTF-16, not UTF-8/legacy single-byte text. No changes were made.'
}

$Cp1251 = [System.Text.Encoding]::GetEncoding(1251)
$Out = New-Object 'System.Collections.Generic.List[byte]'
$Fixed = 0
$i = 0

while ($i -lt $Bytes.Length) {
    $b = [int]$Bytes[$i]
    if ($b -lt 0x80) {
        $Out.Add([byte]$b)
        $i++
        continue
    }
    $Len = 0

    if ($b -ge 0xC2 -and $b -le 0xDF) { $Len = 2 }
    elseif ($b -ge 0xE0 -and $b -le 0xEF) { $Len = 3 }
    elseif ($b -ge 0xF0 -and $b -le 0xF4) { $Len = 4 }

    $Valid = ($Len -gt 0 -and ($i + $Len) -le $Bytes.Length)
    if ($Valid) {
        for ($j = 1; $j -lt $Len; $j++) {
            $Next = [int]$Bytes[$i + $j]
            if ($Next -lt 0x80 -or $Next -gt 0xBF) { $Valid = $false; break }
        }
    }

    if ($Valid -and $Len -ge 3) {
        $Second = [int]$Bytes[$i + 1]
        if (($b -eq 0xE0 -and $Second -lt 0xA0) -or
            ($b -eq 0xED -and $Second -gt 0x9F)) { $Valid = $false }
    }
    if ($Valid -and $Len -eq 4) {
        $Second = [int]$Bytes[$i + 1]
        if (($b -eq 0xF0 -and $Second -lt 0x90) -or
            ($b -eq 0xF4 -and $Second -gt 0x8F)) { $Valid = $false }
    }

    if ($Valid) {
        for ($j = 0; $j -lt $Len; $j++) { $Out.Add([byte]$Bytes[$i + $j]) }
        $i += $Len
    }
    else {
        # Only malformed UTF-8 bytes are interpreted as Windows-1251.
        # Already-valid UTF-8 sequences and all ASCII source text stay untouched.
        $OneByte = [byte[]]@([byte]$b)
        $Char = $Cp1251.GetString($OneByte)
        $Encoded = [System.Text.Encoding]::UTF8.GetBytes($Char)
        foreach ($Value in $Encoded) { $Out.Add([byte]$Value) }
        $Fixed++
        $i++
    }
}

if ($Fixed -eq 0) {
    Write-Host 'config_variables.h already contains valid UTF-8. No changes made.'
    exit 0
}

$Utf8Strict = [System.Text.UTF8Encoding]::new($false, $true)
$null = $Utf8Strict.GetString($Out.ToArray())

$Stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$Backup = "$Path.before-utf8-fix-$Stamp.bak"
[System.IO.File]::Copy($Path, $Backup, $false)
[System.IO.File]::WriteAllBytes($Path, $Out.ToArray())

# Verify the saved file strictly decodes as UTF-8.
$Saved = [System.IO.File]::ReadAllBytes($Path)
$null = $Utf8Strict.GetString($Saved)

Write-Host "Fixed invalid UTF-8 byte(s): $Fixed"
Write-Host "Backup: $Backup"
Write-Host 'config_variables.h is now valid UTF-8.'
