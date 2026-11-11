$ErrorActionPreference = 'Stop'

# Run this script from the Vertex project root (C:\Vertex-20.1).
# Restores the source state from before the assistant's AvoidFreeze UI/range edits,
# Toggle Blatant bind, full AvoidFreeze removal, and HUD call suppression.

$Controls = '.\src\game\client\components\controls.cpp'
$Menu = '.\src\game\client\components\menus_settings_vertex.cpp'
$Config = '.\src\engine\shared\config_variables.h'
$AfCpp = '.\src\game\client\avoidfreeze.cpp'
$AfH = '.\src\game\client\avoidfreeze.h'
$Hud = '.\src\game\client\components\hud.cpp'

# Find the exact snapshot made immediately before full AvoidFreeze removal.
$RemovalMarker = Get-ChildItem -LiteralPath (Split-Path $Controls) -Filter 'controls.cpp.before_full_avoid_remove_*.bak' |
    Sort-Object LastWriteTime -Descending |
    Select-Object -First 1
if(!$RemovalMarker) { throw 'Cannot find controls.cpp.before_full_avoid_remove_*.bak' }
$RemovalStamp = [regex]::Match($RemovalMarker.Name, '\.before_full_avoid_remove_(\d{8}_\d{6})\.bak$').Groups[1].Value
if(!$RemovalStamp) { throw 'Cannot read the full-removal backup timestamp.' }

$Restore = @(
    @{ File = $Controls; Backup = "$Controls.before_full_avoid_remove_$RemovalStamp.bak" },
    @{ File = '.\src\game\client\components\players.cpp'; Backup = ".\src\game\client\components\players.cpp.before_full_avoid_remove_$RemovalStamp.bak" },
    @{ File = '.\src\game\client\components\nameplates.cpp'; Backup = ".\src\game\client\components\nameplates.cpp.before_full_avoid_remove_$RemovalStamp.bak" },
    @{ File = '.\src\game\client\gameclient.h'; Backup = ".\src\game\client\gameclient.h.before_full_avoid_remove_$RemovalStamp.bak" },
    @{ File = '.\src\game\client\gameclient.cpp'; Backup = ".\src\game\client\gameclient.cpp.before_full_avoid_remove_$RemovalStamp.bak" },
    @{ File = '.\CMakeLists.txt'; Backup = ".\CMakeLists.txt.before_full_avoid_remove_$RemovalStamp.bak" },
    @{ File = '.\src\game\client\components\avoidfreeze.cpp'; Backup = ".\src\game\client\components\avoidfreeze.cpp.before_full_avoid_remove_$RemovalStamp.bak" },
    @{ File = '.\src\game\client\components\avoidfreeze.h'; Backup = ".\src\game\client\components\avoidfreeze.h.before_full_avoid_remove_$RemovalStamp.bak" },

    # These three files have earlier backups from before the first AvoidFreeze UI/range edit.
    @{ File = $Menu; Backup = "$Menu.vertex_20261102_075249.bak" },
    @{ File = $Config; Backup = "$Config.vertex_20261102_075249.bak" },
    @{ File = $AfCpp; Backup = "$AfCpp.vertex_20261102_075249.bak" },

    # The header was backed up by the Toggle Blatant patch before it edited the enum.
    @{ File = $AfH; Backup = $null },

    # Restore the HUD before the two missing-renderer calls were wrapped in #if 0.
    @{ File = $Hud; Backup = "$Hud.vertex_before_link_fix.bak" }
)

$HeaderBackup = Get-ChildItem -LiteralPath (Split-Path $AfH) -Filter 'avoidfreeze.h.vertex_blatant_bind_*.bak' |
    Sort-Object LastWriteTime |
    Select-Object -First 1
if(!$HeaderBackup) { throw 'Cannot find avoidfreeze.h.vertex_blatant_bind_*.bak' }
$Restore | Where-Object { $_.File -eq $AfH } | ForEach-Object { $_.Backup = $HeaderBackup.FullName }

# Verify every backup before changing anything.
foreach($Item in $Restore) {
    if(!(Test-Path -LiteralPath $Item.Backup)) {
        throw "Missing recovery copy: $($Item.Backup). No files have been changed."
    }
}

# Make safety copies of files that currently exist.
$Now = Get-Date -Format 'yyyyMMdd_HHmmss'
foreach($Item in $Restore) {
    if(Test-Path -LiteralPath $Item.File) {
        Copy-Item -LiteralPath $Item.File -Destination "$($Item.File).before_original_restore_$Now.bak" -Force
    }
}

# Restore all selected files, including AvoidFreeze files deleted by the earlier script.
foreach($Item in $Restore) {
    $Parent = Split-Path -Parent $Item.File
    if(!(Test-Path -LiteralPath $Parent)) { New-Item -ItemType Directory -Path $Parent -Force | Out-Null }
    Copy-Item -LiteralPath $Item.Backup -Destination $Item.File -Force
    Write-Host "RESTORED: $($Item.File)" -ForegroundColor Green
}

Write-Host ''
Write-Host 'RESTORE FINISHED.' -ForegroundColor Green
Write-Host 'Restored the original menu/config/AvoidFreeze source snapshots and original HUD calls.'
Write-Host 'No build was started.'
Write-Host ''
Write-Host 'NOTE: The old HUD calls are restored. If the leaf/weather renderer implementations are truly absent from the source tree, linking may still fail; this script does not disable those calls.' -ForegroundColor Yellow
