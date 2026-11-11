param(
    [string]$ProjectRoot = 'C:\Vertex-20.1'
)
$ErrorActionPreference = 'Stop'

$File = Join-Path $ProjectRoot 'src\game\client\components\aimbot.cpp'
if(-not (Test-Path -LiteralPath $File -PathType Leaf)) { throw 'aimbot.cpp was not found. Check ProjectRoot.' }
$File = (Resolve-Path -LiteralPath $File).Path

# Read UTF-8. Remove any duplicate leading BOMs, then preserve one BOM if present.
$Bytes = [IO.File]::ReadAllBytes($File)
$Offset = 0
while(($Offset + 2) -lt $Bytes.Length -and $Bytes[$Offset] -eq 0xEF -and $Bytes[$Offset + 1] -eq 0xBB -and $Bytes[$Offset + 2] -eq 0xBF) {
    $Offset += 3
}
$HadBom = $Offset -gt 0
$Utf8Strict = [Text.UTF8Encoding]::new($false, $true)
$Text = $Utf8Strict.GetString($Bytes, $Offset, $Bytes.Length - $Offset)
if(-not $Text.StartsWith('#include "aimbot.h"')) { throw 'Unexpected aimbot.cpp header. No changes made.' }
$NL = if($Text.Contains("`r`n")) { "`r`n" } else { "`n" }

# Fix the const Core() access by copying the core, changing its tuning, and setting it back.
$TunePattern = '(?m)^([ \t]*)pSimLocal->Core\(\)->m_Tuning = \*SimWorld\.GetTuning\(pSimLocal->GetOverriddenTuneZone\(\)\);\r?$'
$TuneRx = [regex]::new($TunePattern)
$TuneMatches = $TuneRx.Matches($Text)
if($TuneMatches.Count -ne 1) { throw "Expected one const m_Tuning assignment; found $($TuneMatches.Count). No changes made." }
$TuneMatch = $TuneMatches[0]
$Indent = $TuneMatch.Groups[1].Value
$TuneReplacement = @(
    ($Indent + 'CCharacterCore PredictedCore = pSimLocal->GetCore();')
    ($Indent + 'PredictedCore.m_Tuning = *SimWorld.GetTuning(pSimLocal->GetOverriddenTuneZone());')
    ($Indent + 'pSimLocal->SetCore(PredictedCore);')
) -join $NL
$Text = $Text.Substring(0, $TuneMatch.Index) + $TuneReplacement + $Text.Substring($TuneMatch.Index + $TuneMatch.Length)

# Restore the candidate-angle generation block missing from v2.
if($Text -match '(?m)^\s*const int BaseSamples\s*=') { throw 'Candidate generation already exists. No changes made.' }
$AnchorPattern = '(?m)^([ \t]*\};)\r?\n[ \t]*\r?\n([ \t]*)struct SHookRayResult'
$AnchorRx = [regex]::new($AnchorPattern)
$AnchorMatches = $AnchorRx.Matches($Text)
if($AnchorMatches.Count -ne 1) { throw "Could not find the unique SHookRayResult insertion point; found $($AnchorMatches.Count). No changes made." }
$Anchor = $AnchorMatches[0]

$CandidateBase64 = 'ICAgICAgICBjb25zdCBpbnQgQmFzZVNhbXBsZXMgPSBzdGQ6OmNsYW1wKHN0ZDo6bWF4KEhvb2tTZWFyY2hTYW1wbGVzLCA4NSksIDg1LCAxMDI0KTsKICAgICAgICAvLyBHZW5lcmF0ZSBjYW5kaWRhdGUgcGFja2V0IGRpcmVjdGlvbnMgYXJvdW5kIGV2ZXJ5IHByZWRpY3RlZCB0YXJnZXQgcG9zaXRpb24uCiAgICAgICAgY29uc3QgZmxvYXQgTWF4QW5ndWxhclN0ZXAgPSBzdGQ6OmNsYW1wKDAuMzVmIC8gc3RkOjptYXgoMS4wZiwgQWltSW5wdXRTY2FsZSksIDAuMDAwMWYsIDAuMDAyZik7CiAgICAgICAgc3RhdGljIHN0ZDo6dmVjdG9yPHN0ZDo6cGFpcjxmbG9hdCwgZmxvYXQ+PiBzX3ZBbmdsZXM7CiAgICAgICAgc3RhdGljIHN0ZDo6dW5vcmRlcmVkX3NldDxzdGQ6OnVpbnQ2NF90PiBzX1NlZW5QYWNrZXREaXJlY3Rpb25zOwogICAgICAgIGF1dG8gJnZBbmdsZXMgPSBzX3ZBbmdsZXM7CiAgICAgICAgYXV0byAmU2VlblBhY2tldERpcmVjdGlvbnMgPSBzX1NlZW5QYWNrZXREaXJlY3Rpb25zOwogICAgICAgIHZBbmdsZXMuY2xlYXIoKTsKICAgICAgICBTZWVuUGFja2V0RGlyZWN0aW9ucy5jbGVhcigpOwogICAgICAgIHZBbmdsZXMucmVzZXJ2ZShzdGF0aWNfY2FzdDxzaXplX3Q+KE1BWF9DTElFTlRTICogMTI4KSk7CiAgICAgICAgU2VlblBhY2tldERpcmVjdGlvbnMucmVzZXJ2ZSg0MDk2KTsKCiAgICAgICAgYXV0byBBZGRDYW5kaWRhdGVBbmdsZSA9IFsmXShmbG9hdCBSZWxhdGl2ZUFuZ2xlKSB7CiAgICAgICAgICAgIGZsb2F0IENhbmRpZGF0ZUFuZ2xlID0gQWltQW5nbGUgKyBSZWxhdGl2ZUFuZ2xlOwogICAgICAgICAgICBpbnQgVGFyZ2V0WCA9IDA7CiAgICAgICAgICAgIGludCBUYXJnZXRZID0gMDsKICAgICAgICAgICAgdmVjMiBQYWNrZXREaXJlY3Rpb247CiAgICAgICAgICAgIGlmKCFRdWFudGl6ZUhvb2tJbnB1dERpcmVjdGlvbihDYW5kaWRhdGVBbmdsZSwgQWltSW5wdXRTY2FsZSwgJlRhcmdldFgsICZUYXJnZXRZLCAmUGFja2V0RGlyZWN0aW9uKSkKICAgICAgICAgICAgICAgIHJldHVybjsKICAgICAgICAgICAgQ2FuZGlkYXRlQW5nbGUgPSBzdGQ6OmF0YW4yKFBhY2tldERpcmVjdGlvbi55LCBQYWNrZXREaXJlY3Rpb24ueCk7CiAgICAgICAgICAgIFJlbGF0aXZlQW5nbGUgPSBzdGQ6OnJlbWFpbmRlcihDYW5kaWRhdGVBbmdsZSAtIEFpbUFuZ2xlLCBWRVJURVhfVFdPX1BJKTsKICAgICAgICAgICAgaWYoc3RkOjpmYWJzKFJlbGF0aXZlQW5nbGUpID4gSGFsZkZvdiArIDAuMDAwMDAxZikKICAgICAgICAgICAgICAgIHJldHVybjsKICAgICAgICAgICAgY29uc3Qgc3RkOjp1aW50NjRfdCBLZXkgPSAoc3RhdGljX2Nhc3Q8c3RkOjp1aW50NjRfdD4oc3RhdGljX2Nhc3Q8c3RkOjp1aW50MzJfdD4oVGFyZ2V0WCkpIDw8IDMyKSB8CiAgICAgICAgICAgICAgICBzdGF0aWNfY2FzdDxzdGQ6OnVpbnQzMl90PihUYXJnZXRZKTsKICAgICAgICAgICAgaWYoIVNlZW5QYWNrZXREaXJlY3Rpb25zLmluc2VydChLZXkpLnNlY29uZCkKICAgICAgICAgICAgICAgIHJldHVybjsKICAgICAgICAgICAgdkFuZ2xlcy5lbXBsYWNlX2JhY2soc3RkOjpmYWJzKFJlbGF0aXZlQW5nbGUpLCBDYW5kaWRhdGVBbmdsZSk7CiAgICAgICAgfTsKCiAgICAgICAgLy8gQWx3YXlzIHRlc3QgdGhlIGN1cnNvciBkaXJlY3Rpb24gYWZ0ZXIgY29udmVyc2lvbiB0byB0aGUgcmVhbCBwYWNrZXQgdmVjdG9yLgogICAgICAgIEFkZENhbmRpZGF0ZUFuZ2xlKDAuMGYpOwogICAgICAgIGZvcihpbnQgQ2xpZW50SWQgPSAwOyBDbGllbnRJZCA8IE1BWF9DTElFTlRTOyArK0NsaWVudElkKQogICAgICAgIHsKICAgICAgICAgICAgaWYoQ2xpZW50SWQgPT0gTG9jYWxJZCB8fCAhcEdhbWVDbGllbnQtPm1fYUNsaWVudHNbQ2xpZW50SWRdLm1fQWN0aXZlIHx8CiAgICAgICAgICAgICAgICAhcEdhbWVDbGllbnQtPm1fU25hcC5tX2FDaGFyYWN0ZXJzW0NsaWVudElkXS5tX0FjdGl2ZSB8fCAhYVByZWRpY3RlZFRhcmdldFZhbGlkW0NsaWVudElkXVswXSkKICAgICAgICAgICAgICAgIGNvbnRpbnVlOwogICAgICAgICAgICBDQ2hhcmFjdGVyICpwVGFyZ2V0ID0gcEdhbWVDbGllbnQtPm1fUHJlZGljdGVkV29ybGQuR2V0Q2hhcmFjdGVyQnlJZChDbGllbnRJZCk7CiAgICAgICAgICAgIGlmKCFwVGFyZ2V0KQogICAgICAgICAgICAgICAgY29udGludWU7CgogICAgICAgICAgICBmb3IoaW50IFRhcmdldFRpY2sgPSAwOyBUYXJnZXRUaWNrIDw9IFByZWRpY3Rpb25UaWNrczsgKytUYXJnZXRUaWNrKQogICAgICAgICAgICB7CiAgICAgICAgICAgICAgICBpZighUHJlZGljdEhvb2tNb3ZlbWVudCAmJiBUYXJnZXRUaWNrID4gMCkKICAgICAgICAgICAgICAgICAgICBicmVhazsKICAgICAgICAgICAgICAgIGlmKCFhUHJlZGljdGVkVGFyZ2V0VmFsaWRbQ2xpZW50SWRdW1RhcmdldFRpY2tdIHx8ICFhUHJlZGljdGVkSG9va0FsbG93ZWRbQ2xpZW50SWRdW1RhcmdldFRpY2tdKQogICAgICAgICAgICAgICAgICAgIGNvbnRpbnVlOwogICAgICAgICAgICAgICAgY29uc3QgdmVjMiBUYXJnZXRDZW50ZXIgPSBhUHJlZGljdGVkVGFyZ2V0UG9zW0NsaWVudElkXVtUYXJnZXRUaWNrXTsKICAgICAgICAgICAgICAgIGNvbnN0IHZlYzIgVG9DZW50ZXIgPSBUYXJnZXRDZW50ZXIgLSBMb2NhbFBvczsKICAgICAgICAgICAgICAgIGNvbnN0IGZsb2F0IENlbnRlckRpc3RhbmNlID0gbGVuZ3RoKFRvQ2VudGVyKTsKICAgICAgICAgICAgICAgIGNvbnN0IGZsb2F0IFNlYXJjaFJhZGl1cyA9IEhPT0tfUExBWUVSX0hJVF9SQURJVVMgKyAxLjBmOwogICAgICAgICAgICAgICAgaWYoQ2VudGVyRGlzdGFuY2UgPD0gQUlNX0VQU0lMT04pCiAgICAgICAgICAgICAgICAgICAgY29udGludWU7CgogICAgICAgICAgICAgICAgY29uc3QgZmxvYXQgQ2VudGVyQW5nbGUgPSBzdGQ6OmF0YW4yKFRvQ2VudGVyLnksIFRvQ2VudGVyLngpOwogICAgICAgICAgICAgICAgY29uc3QgZmxvYXQgUmVsYXRpdmVDZW50ZXIgPSBzdGQ6OnJlbWFpbmRlcihDZW50ZXJBbmdsZSAtIEFpbUFuZ2xlLCBWRVJURVhfVFdPX1BJKTsKICAgICAgICAgICAgICAgIGNvbnN0IGZsb2F0IEFuZ3VsYXJSYWRpdXMgPSBDZW50ZXJEaXN0YW5jZSA8PSBTZWFyY2hSYWRpdXMgPyBWRVJURVhfUEkgOgogICAgICAgICAgICAgICAgICAgIHN0ZDo6YXNpbihzdGQ6OmNsYW1wKFNlYXJjaFJhZGl1cyAvIENlbnRlckRpc3RhbmNlLCAwLjBmLCAwLjk5OTk5ZikpOwogICAgICAgICAgICAgICAgY29uc3QgZmxvYXQgTG93ID0gc3RkOjptYXgoLUhhbGZGb3YsIFJlbGF0aXZlQ2VudGVyIC0gQW5ndWxhclJhZGl1cyk7CiAgICAgICAgICAgICAgICBjb25zdCBmbG9hdCBIaWdoID0gc3RkOjptaW4oSGFsZkZvdiwgUmVsYXRpdmVDZW50ZXIgKyBBbmd1bGFyUmFkaXVzKTsKICAgICAgICAgICAgICAgIGlmKEhpZ2ggPCBMb3cpCiAgICAgICAgICAgICAgICAgICAgY29udGludWU7CgogICAgICAgICAgICAgICAgQWRkQ2FuZGlkYXRlQW5nbGUoUmVsYXRpdmVDZW50ZXIpOwogICAgICAgICAgICAgICAgY29uc3QgaW50IEFkYXB0aXZlU2FtcGxlcyA9IHN0YXRpY19jYXN0PGludD4oc3RkOjpjZWlsKChIaWdoIC0gTG93KSAvIE1heEFuZ3VsYXJTdGVwKSk7CiAgICAgICAgICAgICAgICBjb25zdCBpbnQgU2FtcGxlcyA9IHN0ZDo6Y2xhbXAoc3RkOjptYXgoQmFzZVNhbXBsZXMsIEFkYXB0aXZlU2FtcGxlcyksIDE2LCA1MTIpOwogICAgICAgICAgICAgICAgZm9yKGludCBTYW1wbGUgPSAwOyBTYW1wbGUgPD0gU2FtcGxlczsgKytTYW1wbGUpCiAgICAgICAgICAgICAgICB7CiAgICAgICAgICAgICAgICAgICAgY29uc3QgZmxvYXQgVCA9IHN0YXRpY19jYXN0PGZsb2F0PihTYW1wbGUpIC8gc3RhdGljX2Nhc3Q8ZmxvYXQ+KFNhbXBsZXMpOwogICAgICAgICAgICAgICAgICAgIEFkZENhbmRpZGF0ZUFuZ2xlKExvdyArIChIaWdoIC0gTG93KSAqIFQpOwogICAgICAgICAgICAgICAgfQogICAgICAgICAgICB9CiAgICAgICAgfQogICAgICAgIHN0ZDo6c29ydCh2QW5nbGVzLmJlZ2luKCksIHZBbmdsZXMuZW5kKCksIFtdKGNvbnN0IGF1dG8gJkEsIGNvbnN0IGF1dG8gJkIpIHsKICAgICAgICAgICAgaWYoQS5maXJzdCAhPSBCLmZpcnN0KQogICAgICAgICAgICAgICAgcmV0dXJuIEEuZmlyc3QgPCBCLmZpcnN0OwogICAgICAgICAgICByZXR1cm4gQS5zZWNvbmQgPCBCLnNlY29uZDsKICAgICAgICB9KTsKCiAgICAgICAgY29uc3QgYm9vbCBOZWVkQWxsSG9va2FibGUgPSBnX0NvbmZpZy5tX1Z4QWltYm90U2tpbkhpZ2hsaWdodCB8fCBnX0NvbmZpZy5tX1Z4QWltYm90RXNwUGxheWVyczsKICAgICAgICBjb25zdCBpbnQgTWF4UmF5QnVkZ2V0ID0gTmVlZEFsbEhvb2thYmxlID8gMTAyNCA6IDUxMjsKICAgICAgICBjb25zdCBpbnQgUmF5QnVkZ2V0ID0gc3RkOjpjbGFtcChzdGF0aWNfY2FzdDxpbnQ+KHN0ZDo6Y2VpbCgKICAgICAgICAgICAgMi4wZiAqIEhhbGZGb3YgKiBzdGQ6Om1heCgxLjBmLCBBaW1JbnB1dFNjYWxlKSAqIDEuMTVmKSksIDk2LCBNYXhSYXlCdWRnZXQpOw=='
$CandidateBlock = [Text.Encoding]::UTF8.GetString([Convert]::FromBase64String($CandidateBase64))
$CandidateBlock = [regex]::Replace($CandidateBlock, "`r`n|`n|`r", $NL)
$Replacement = $Anchor.Groups[1].Value + $NL + $CandidateBlock + $NL + $NL + $Anchor.Groups[2].Value + 'struct SHookRayResult'
$Text = $Text.Substring(0, $Anchor.Index) + $Replacement + $Text.Substring($Anchor.Index + $Anchor.Length)

# Verify all required symbols are now present exactly once.
foreach($Token in @('const int BaseSamples =', 'const float MaxAngularStep =', 'auto &vAngles =', 'const int RayBudget =')) {
    if(([regex]::Matches($Text, [regex]::Escape($Token))).Count -ne 1) { throw "Post-check failed for $Token. No changes made." }
}
if($Text.Contains('pSimLocal->Core()->m_Tuning =')) { throw 'The invalid const Core() assignment remains. No changes made.' }
if(([regex]::Matches($Text, 'struct SHookRayResult')).Count -ne 1) { throw 'Unexpected SHookRayResult count. No changes made.' }

# Backup and write only after every check passed.
$Stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$Backup = "$File.before-build-repair-$Stamp.bak"
Copy-Item -LiteralPath $File -Destination $Backup
$Utf8Out = [Text.UTF8Encoding]::new($false)
$Payload = $Utf8Out.GetBytes($Text)
if($HadBom) {
    $OutBytes = New-Object byte[] ($Payload.Length + 3)
    $OutBytes[0] = 0xEF; $OutBytes[1] = 0xBB; $OutBytes[2] = 0xBF
    [Array]::Copy($Payload, 0, $OutBytes, 3, $Payload.Length)
} else {
    $OutBytes = $Payload
}
[IO.File]::WriteAllBytes($File, $OutBytes)

Write-Host 'Repair applied successfully.' -ForegroundColor Green
Write-Host "Backup: $Backup" -ForegroundColor Cyan
Write-Host 'Restored candidate generation and fixed the const Core() tuning assignment.' -ForegroundColor Green
