#requires -Version 5.1
param(
    [string]$ProjectRoot = ""
)

$ErrorActionPreference = "Stop"

if([string]::IsNullOrWhiteSpace($ProjectRoot)) {
    $Candidate = Split-Path -Parent $PSScriptRoot
    if(Test-Path (Join-Path $Candidate "CMakeLists.txt")) {
        $ProjectRoot = $Candidate
    } else {
        $ProjectRoot = (Get-Location).Path
    }
}
$ProjectRoot = (Resolve-Path $ProjectRoot).Path
$Payload = Join-Path $PSScriptRoot "payload"
if(!(Test-Path (Join-Path $ProjectRoot "CMakeLists.txt"))) {
    throw "Не найден CMakeLists.txt в $ProjectRoot. Укажи -ProjectRoot C:\Vertex-20.1."
}

$Rel = @{
    Controls = "src\game\client\components\controls.cpp"
    ControlsH = "src\game\client\components\controls.h"
    Menu = "src\game\client\components\menus_settings_vertex.cpp"
    GameClientH = "src\game\client\gameclient.h"
    Players = "src\game\client\components\players.cpp"
    AvoidH = "src\game\client\avoidfreeze.h"
    ConfigVars = "src\engine\shared\config_variables.h"
    ConfigCpp = "src\engine\shared\config.cpp"
    CMake = "CMakeLists.txt"
    AimH = "src\game\client\aimbot.h"
    AimCpp = "src\game\client\aimbot.cpp"
}
$Full = @{}
foreach($k in $Rel.Keys) { $Full[$k] = Join-Path $ProjectRoot $Rel[$k] }
foreach($k in @("Controls","ControlsH","Menu","GameClientH","Players","AvoidH","ConfigVars","ConfigCpp","CMake")) {
    if(!(Test-Path $Full[$k])) { throw "Не найден файл: $($Full[$k]). Ничего не изменено." }
}
if(!(Test-Path (Join-Path $ProjectRoot "src\game\client\vertex_gate.h"))) {
    throw "Новый GUI включает src\game\client\vertex_gate.h, но файл не найден. Ничего не изменено."
}
foreach($f in @("aimbot.h","aimbot.cpp","controls.cpp.uploaded","menus_settings_vertex.cpp.uploaded","config_aim_additions.txt")) {
    if(!(Test-Path (Join-Path $Payload $f))) { throw "В пакете отсутствует payload\$f." }
}

# Auto Aled в этой ветке Vertex использует singleton VertexAutoAled(),
# поэтому находим именно его заголовок и не добавляем несуществующий m_AutoAled в CGameClient.
$AledHeaderCandidates = Get-ChildItem (Join-Path $ProjectRoot "src\game\client") -Recurse -File -Filter "aled.h" -ErrorAction SilentlyContinue
$AledHeader = $null
$AledText = $null
$CMakeProbe = [IO.File]::ReadAllText($Full.CMake, [Text.Encoding]::UTF8)
$CompatibleAled = @()
foreach($f in $AledHeaderCandidates) {
    $t = [IO.File]::ReadAllText($f.FullName, [Text.Encoding]::UTF8)
    $Cpp = [IO.Path]::ChangeExtension($f.FullName, '.cpp')
    if($t -match 'class\s+CAutoAled' -and $t -match 'VertexAutoAled\s*\(' -and $t -match 'm_PlanActive' -and (Test-Path $Cpp)) {
        $ClientDir = [IO.Path]::GetFullPath((Join-Path $ProjectRoot 'src\game\client')).TrimEnd([char[]]@('\','/'))
        $CppFull = [IO.Path]::GetFullPath($Cpp)
        if($CppFull.StartsWith($ClientDir + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) {
            $CppRel = $CppFull.Substring($ClientDir.Length + 1).Replace('\','/')
            $Listed = [regex]::IsMatch($CMakeProbe, '(?m)^\s*' + [regex]::Escape($CppRel) + '\s*$')
            $CompatibleAled += [pscustomobject]@{Header=$f.FullName; Text=$t; Cpp=$CppFull; Listed=$Listed}
        }
    }
}
$SelectedAled = $CompatibleAled | Where-Object { $_.Listed } | Select-Object -First 1
if(!$SelectedAled -and $CompatibleAled.Count -eq 1) { $SelectedAled = $CompatibleAled[0] }
if($SelectedAled) {
    $AledHeader = $SelectedAled.Header
    $AledText = $SelectedAled.Text
}
if(!$AledHeader) {
    throw "Не найден aled.h с CAutoAled, VertexAutoAled() и m_PlanActive. Пришли текущий aled.h; проект не изменён."
}

function Read-Source([string]$Path) {
    $Bytes = [IO.File]::ReadAllBytes($Path)
    $Bom = $Bytes.Length -ge 3 -and $Bytes[0] -eq 0xEF -and $Bytes[1] -eq 0xBB -and $Bytes[2] -eq 0xBF
    $Text = [IO.File]::ReadAllText($Path, [Text.Encoding]::UTF8)
    $NL = if($Text.Contains("`r`n")) { "`r`n" } else { "`n" }
    return @{ Path=$Path; Text=$Text; Bom=$Bom; NL=$NL }
}
function Write-Source($Source) {
    [IO.File]::WriteAllText($Source.Path, $Source.Text, [System.Text.UTF8Encoding]::new($Source.Bom))
}
function Read-PayloadText([string]$Name) {
    $Path = Join-Path $Payload $Name
    $Text = [IO.File]::ReadAllText($Path, [Text.Encoding]::UTF8)
    # Uploaded C++ payloads can contain CRCRLF line endings. Normalize these
    # before line-anchored edits so PowerShell/.NET regex sees ordinary CRLF.
    return $Text.Replace("`r`r`n", "`r`n")
}
function Replace-Once([string]$Text, [string]$Pattern, [string]$Replacement, [string]$Description) {
    $Rx = [regex]::new($Pattern)
    $Count = $Rx.Matches($Text).Count
    if($Count -ne 1) { throw "$Description`: ожидалось 1 совпадение, найдено $Count. Файлы проекта ещё не менялись." }
    return $Rx.Replace($Text, $Replacement, 1)
}
function Add-IncludeAfter([string]$Text, [string]$Anchor, [string]$IncludeLine, [string]$NL, [string]$Description) {
    if($Text -match [regex]::Escape($IncludeLine)) { return $Text }
    $Pattern = '(?m)^([ \t]*' + [regex]::Escape($Anchor) + ')[ \t]*\r*$'
    return Replace-Once $Text $Pattern ('$1' + $NL + $IncludeLine) $Description
}
function Merge-ConfigMacros([string]$Text, [string[]]$Lines, [string]$NL, [string]$Label) {
    $Missing = New-Object System.Collections.Generic.List[string]
    foreach($line in $Lines) {
        $m = [regex]::Match($line, '^MACRO_CONFIG_[A-Z0-9_]+\(\s*([A-Za-z0-9_]+)\s*,')
        if(!$m.Success) { throw "Не удалось разобрать строку конфигурации: $line" }
        $Name = $m.Groups[1].Value
        $Pattern = '(?m)^[ \t]*MACRO_CONFIG_[A-Z0-9_]+\(\s*' + [regex]::Escape($Name) + '\s*,[^\r\n]*\r?$'
        if([regex]::IsMatch($Text, $Pattern)) {
            $Text = [regex]::Replace($Text, $Pattern, $line, 1)
        } else {
            $Missing.Add($line)
        }
    }
    if($Missing.Count -gt 0) {
        $Marker = [regex]::Match($Text, '(?m)^\s*// === VERTEX MOD CONFIGS END ===')
        if($Marker.Success) {
            $InsertAt = $Marker.Index
            $Text = $Text.Insert($InsertAt, (($Missing -join $NL) + $NL))
        } else {
            $Anchor = [regex]::Match($Text, '(?m)^[ \t]*MACRO_CONFIG_[A-Z0-9_]+\(\s*VxAfDebug\s*,[^\r\n]*\r?\n')
            if(!$Anchor.Success) {
                $Anchor = [regex]::Match($Text, '(?m)^[ \t]*MACRO_CONFIG_[A-Z0-9_]+\(\s*ClVertexDuSelftest\s*,')
                if(!$Anchor.Success) { throw "$Label`: не найден безопасный якорь для новых Aim config." }
                $Text = $Text.Insert($Anchor.Index, (($Missing -join $NL) + $NL + $NL))
            } else {
                $Text = $Text.Insert($Anchor.Index + $Anchor.Length, (($Missing -join $NL) + $NL))
            }
        }
    }
    return $Text
}

function Add-SortedGameClientEntry([string]$Text, [string]$Entry, [string]$NL) {
    $GameClientSet = [regex]::Match($Text, '(?m)^\s*set_src\(GAME_CLIENT GLOB_RECURSE src/game/client\s*\r?\n')
    if(!$GameClientSet.Success) { throw "Не найден set_src(GAME_CLIENT) для $Entry." }
    $AfterSet = $GameClientSet.Index + $GameClientSet.Length
    $StartMarker = ([regex]::new('(?m)^\s*# tidy-alphabetical-start\s*\r?\n')).Match($Text, $AfterSet)
    if(!$StartMarker.Success) { throw "Не найден tidy-alphabetical-start в GAME_CLIENT для $Entry." }
    $AfterStart = $StartMarker.Index + $StartMarker.Length
    $EndMarker = ([regex]::new('(?m)^\s*# tidy-alphabetical-end\s*(?:\r?\n|$)')).Match($Text, $AfterStart)
    if(!$EndMarker.Success -or $EndMarker.Index -le $StartMarker.Index) {
        throw "Не найдена граница tidy-alphabetical-end в GAME_CLIENT для $Entry."
    }
    $Body = $Text.Substring($AfterStart, $EndMarker.Index - $AfterStart)
    foreach($Line in [regex]::Matches($Body, '(?m)^[ \t]*([^#\r\n][^\r\n]*)\r?$')) {
        $Existing = $Line.Groups[1].Value.Trim()
        if($Existing -and [string]::CompareOrdinal($Entry, $Existing) -lt 0) {
            return $Text.Insert($AfterStart + $Line.Index, ('    ' + $Entry + $NL))
        }
    }
    return $Text.Insert($EndMarker.Index, ('    ' + $Entry + $NL))
}

# ----------------------------------------------------------------------------
# PRE-FLIGHT: читаем всё и готовим правки в памяти. До конца проверок проект
# не записывается. Все исходники из присланного набора ставятся как замена.
# ----------------------------------------------------------------------------
$ControlsH = Read-Source $Full.ControlsH
$GameClientH = Read-Source $Full.GameClientH
$Players = Read-Source $Full.Players
$AvoidH = Read-Source $Full.AvoidH
$ConfigVars = Read-Source $Full.ConfigVars
$ConfigCpp = Read-Source $Full.ConfigCpp
$CMake = Read-Source $Full.CMake
$AledHSource = Read-Source $AledHeader
$NewControls = Read-PayloadText "controls.cpp.uploaded"
$NewMenu = Read-PayloadText "menus_settings_vertex.cpp.uploaded"
$AimCpp = Read-PayloadText "aimbot.cpp"
$AimHText = Read-PayloadText "aimbot.h"
$NL = $ConfigVars.NL

# Both renderer definitions are in the provided replacement menu. This restores
# the HUD leaf / rain / particle symbols instead of disabling their HUD calls.
foreach($Function in @("VertexRenderParticles","VertexRenderWorld")) {
    if(([regex]::Matches($NewMenu, "(?m)^void\s+$Function\s*\(")).Count -ne 1) {
        throw "В новом меню нет единственного определения $Function. Ничего не изменено."
    }
}

# 1. Adapt only the AvoidFreeze calls in the full controls.cpp to the API that
#    exists in this Vertex tree. Other controls / Dummy Unfreeze / laser code
#    from the uploaded file are preserved.
$UpdatePattern = 'GameClient\(\)->m_AvoidFreeze\.Update\(\s*GameClient\(\),\s*Client\(\),\s*Input\(\),\s*&m_aInputData\[g_Config\.m_ClDummy\],\s*m_aInputDirectionLeft\[g_Config\.m_ClDummy\],\s*m_aInputDirectionRight\[g_Config\.m_ClDummy\],\s*g_Config\.m_ClDummy,\s*m_LaserUnfreezeAimActive,\s*m_aInputData\[g_Config\.m_ClDummy\]\.m_Hook\s*==\s*1\s*\);'
$NewControls = Replace-Once $NewControls $UpdatePattern 'GameClient()->m_AvoidFreeze.Update(GameClient(), Client(), &m_aInputData[g_Config.m_ClDummy]);' "AvoidFreeze::Update (переход на текущую сигнатуру)"

$NewControls = Replace-Once $NewControls '(?m)^([ \t]*)GameClient\(\)->m_AutoAled\.Update\(GameClient\(\), Client\(\), &m_aInputData\[g_Config\.m_ClDummy\]\);[ \t]*\r?$' '$1VertexAutoAled().Update(GameClient(), Client(), &m_aInputData[g_Config.m_ClDummy]);' "Auto Aled singleton Update"

$CapturePattern = '(?m)^([ \t]*)GameClient\(\)->m_AvoidFreeze\.CaptureMenuBinds\(Input\(\), GameClient\(\)->m_Chat\.IsActive\(\)\);\r?\n[ \t]*VertexAvoidRender\(\);'
$NewControls = Replace-Once $NewControls $CapturePattern '$1GameClient()->m_AvoidFreeze.RenderBinds(GameClient());' "захват биндов текущего AvoidFreeze"

# The uploaded controls.cpp contains a second AvoidFreeze renderer using members
# absent from this older CAvoidFreeze. Its existing RenderWorld() is already drawn
# by players.cpp, so remove only that duplicate trailing method and its call.
$AvoidRenderMarker = "// Vertex Avoid Freeze visuals:"
$AvoidRenderPos = $NewControls.IndexOf($AvoidRenderMarker)
if($AvoidRenderPos -lt 0) { throw "Не найден trailing VertexAvoidRender() в controls.cpp." }
if($NewControls.Substring($AvoidRenderPos).Trim() -notmatch '^// Vertex Avoid Freeze visuals:[\s\S]*?void CControls::VertexAvoidRender\(\)') {
    throw "Не удалось проверить границы VertexAvoidRender()."
}
$NewControls = $NewControls.Substring(0, $AvoidRenderPos).TrimEnd() + $NL

# Use the project's existing Auto Aled singleton from components/aled.h.
$ClientSourceDir = Join-Path $ProjectRoot 'src\game\client'
$AledHeaderFull = [IO.Path]::GetFullPath($AledHeader)
$ClientSourceFull = [IO.Path]::GetFullPath($ClientSourceDir).TrimEnd([char[]]@('\','/'))
if(!$AledHeaderFull.StartsWith($ClientSourceFull + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) {
    throw "aled.h найден вне src\game\client: $AledHeaderFull"
}
$AledRelativeFromClient = $AledHeaderFull.Substring($ClientSourceFull.Length + 1).Replace('\','/')
$AledDir = [IO.Path]::GetDirectoryName($AledHeaderFull)
$ControlsDir = [IO.Path]::GetFullPath((Split-Path -Parent $Full.Controls))
$AimSourceDir = [IO.Path]::GetFullPath((Split-Path -Parent $Full.AimCpp))
$MenuSourceDir = [IO.Path]::GetFullPath((Split-Path -Parent $Full.Menu))
$AledIncludeForDir = { param([string]$SourceDir) if([string]::Equals($AledDir, $SourceDir, [StringComparison]::OrdinalIgnoreCase)) { '#include "aled.h"' } else { '#include <game/client/' + $AledRelativeFromClient + '>' } }
$AledIncludeForControls = & $AledIncludeForDir $ControlsDir
$AledIncludeForAim = & $AledIncludeForDir $AimSourceDir
$AledIncludeForMenu = & $AledIncludeForDir $MenuSourceDir
$NewControls = Add-IncludeAfter $NewControls '#include "controls.h"' $AledIncludeForControls $NL "include Auto Aled в controls.cpp"
$AimCpp = Add-IncludeAfter $AimCpp '#include "aimbot.h"' $AledIncludeForAim $NL "include Auto Aled в aimbot.cpp"
$MenuAledIncludePattern = '(?m)^[ \t]*#include\s+["<][^">]*aled\.h[">][ \t]*\r?$'
if([regex]::IsMatch($NewMenu, $MenuAledIncludePattern)) {
    $NewMenu = [regex]::Replace($NewMenu, $MenuAledIncludePattern, $AledIncludeForMenu, 1)
} else {
    $NewMenu = Add-IncludeAfter $NewMenu '#include "menus.h"' $AledIncludeForMenu $NL "include Auto Aled в меню"
}

# OnReset resets all input-owning helpers together.
$ResetPattern = '(?m)^([ \t]*)GameClient\(\)->m_AvoidFreeze\.Reset\(\);[ \t]*\r?$'
if(([regex]::Matches($NewControls,$ResetPattern)).Count -ne 1) { throw "Не найден единственный AvoidFreeze::Reset в controls.cpp." }
if($NewControls -notmatch 'VertexAutoAled\(\)\.Reset\(\)') {
    $NewControls = [regex]::Replace($NewControls,$ResetPattern,('$1GameClient()->m_AvoidFreeze.Reset();' + $NL + '$1VertexAutoAled().Reset();' + $NL + '$1GameClient()->m_AimBot.Reset();'),1)
} elseif($NewControls -notmatch 'm_AimBot\.Reset\(\)') {
    $NewControls = [regex]::Replace($NewControls,$ResetPattern,('$1GameClient()->m_AvoidFreeze.Reset();' + $NL + '$1GameClient()->m_AimBot.Reset();'),1)
}

# Ensure no CGameClient::m_AutoAled assumption remains in the delivered source.
if($NewControls -match 'm_AutoAled') { throw "В адаптированном controls.cpp осталась ссылка на несуществующий CGameClient::m_AutoAled." }
$AimCpp = Replace-Once $AimCpp 'pGame->m_AutoAled\.IsBusy\(\)' 'VertexAutoAled().IsBusy()' "Aim priority check для Auto Aled singleton"

# 2. Add IsBusy adapters to the already existing rescue modules. They report
#    whether a rescue plan currently owns / is changing input; no algorithm is replaced.
if($AvoidH.Text -notmatch '\bIsBusy\s*\(') {
    if($AvoidH.Text -notmatch '\bAnyActive\s*\(\s*\)\s*const') { throw "В avoidfreeze.h отсутствует AnyActive() const; невозможно подключить приоритет Aim." }
    $AvoidH.Text = Replace-Once $AvoidH.Text '(?m)^([ \t]*)private:[ \t]*\r?$' ('$1bool IsBusy() const { return AnyActive(); }' + $AvoidH.NL + '$1private:') "AvoidFreeze::IsBusy"
}
if($AledHSource.Text -notmatch '\bIsBusy\s*\(') {
    if($AledHSource.Text -notmatch '\bm_PlanActive\b') { throw "В aled.h нет m_PlanActive — не могу безопасно определить занятость Auto Aled." }
    $BusyExpr = 'm_PlanActive'
    if($AledHSource.Text -match '\bm_ApproachActive\b') { $BusyExpr = 'm_PlanActive || m_ApproachActive' }
    $AledHSource.Text = Replace-Once $AledHSource.Text '(?m)^([ \t]*)const char \*Status\(\) const \{ return m_pStatus; \}[ \t]*\r?$' ('$1const char *Status() const { return m_pStatus; }' + $AledHSource.NL + '$1bool IsBusy() const { return ' + $BusyExpr + '; }') "Auto Aled::IsBusy"
}

# 3. Add Aim state to controls/gameclient and world drawing to players.cpp.
if($ControlsH.Text -notmatch '\bm_VertexAimTargetId\b') {
    $ClassStart = [regex]::Match($ControlsH.Text, 'class\s+CControls\b')
    if(!$ClassStart.Success) { throw "Не найден class CControls в controls.h." }
    $PublicMatch = [regex]::Match($ControlsH.Text.Substring($ClassStart.Index), '(?m)^([ \t]*public:)[ \t]*\r?$')
    if(!$PublicMatch.Success) { throw "Не найдена public-секция CControls в controls.h." }
    $InsertAt = $ClassStart.Index + $PublicMatch.Index + $PublicMatch.Length
    $ControlsH.Text = $ControlsH.Text.Insert($InsertAt, $NL + "`tint m_VertexAimTargetId = -1;" + $NL)
}
if($ControlsH.Text -notmatch '\bm_aMousePos\b' -or $ControlsH.Text -notmatch '\bm_LaserUnfreezeAimActive\b') {
    throw "controls.h не содержит m_aMousePos или m_LaserUnfreezeAimActive, необходимых присланному Aim v2. Файлы не записаны."
}

if($GameClientH.Text -notmatch '#include\s+"aimbot\.h"') {
    $GameClientH.Text = Replace-Once $GameClientH.Text '(?m)^([ \t]*#include "avoidfreeze\.h")[ \t]*\r?$' ('$1' + $GameClientH.NL + '#include "aimbot.h"') "include aimbot.h в gameclient.h"
}
if($GameClientH.Text -notmatch '\bCAimBot\s+m_AimBot\s*;') {
    $GameClientH.Text = Replace-Once $GameClientH.Text '(?m)^([ \t]*CAvoidFreeze\s+m_AvoidFreeze\s*;)[ \t]*\r?$' ('$1' + $GameClientH.NL + '$1CAimBot m_AimBot;') "CAimBot member в CGameClient"
}
if($Players.Text -notmatch 'm_AimBot\.RenderWorld\(') {
    $Players.Text = Replace-Once $Players.Text '(?m)^([ \t]*)GameClient\(\)->m_AvoidFreeze\.RenderWorld\(GameClient\(\)\);[ \t]*\r?$' ('$1GameClient()->m_AvoidFreeze.RenderWorld(GameClient());' + $Players.NL + '$1GameClient()->m_AimBot.RenderWorld(GameClient());') "Aim RenderWorld в players.cpp"
}

# 4. Keep the supplied full V3 menu, including its rain/particles/leaves renderer.
#    Only remove optional Aled controls whose config variables are absent in the
#    installed project; the Aled card's core existing controls/status are preserved.
$DeclaredConfig = New-Object 'System.Collections.Generic.HashSet[string]'
foreach($m in [regex]::Matches($ConfigVars.Text, 'MACRO_CONFIG_[A-Z0-9_]+\(\s*([A-Za-z0-9_]+)\s*,')) { [void]$DeclaredConfig.Add($m.Groups[1].Value) }
$OptionalAledRows = @{
    VxAledSafeTicks = '(?m)^[ \t]*SliderRow\("Проверка безопасности",[^\r\n]*\r?\n'
    VxAledCooldown = '(?m)^[ \t]*SliderRow\("Пауза после попытки",[^\r\n]*\r?\n'
    VxAledMaxPresses = '(?m)^[ \t]*SliderRow\("Лимит нажатий молотка",[^\r\n]*\r?\n'
}
foreach($field in $OptionalAledRows.Keys) {
    if(!$DeclaredConfig.Contains($field)) {
        $Pattern = $OptionalAledRows[$field]
        if([regex]::IsMatch($NewMenu,$Pattern)) { $NewMenu = [regex]::Replace($NewMenu,$Pattern,'',1) }
    }
}

# 5. Merge the exact 34 Aim V2 config lines, preserving unrelated legacy vars.
$AimConfigLines = @(Get-Content (Join-Path $Payload "config_aim_additions.txt") | Where-Object { $_ -match '^MACRO_CONFIG_' })
if($AimConfigLines.Count -ne 34) { throw "Ожидалось 34 Aim config lines, найдено $($AimConfigLines.Count)." }
$LegacyAimPattern = '(?m)^[ \t]*MACRO_CONFIG_[A-Z0-9_]+\([^\r\n]*,\s*vx_aimbot_[^,\r\n]*,[^\r\n]*(?:\r?\n|$)'
$ConfigVars.Text = [regex]::Replace($ConfigVars.Text,$LegacyAimPattern,'')
$ConfigVars.Text = Merge-ConfigMacros $ConfigVars.Text $AimConfigLines $ConfigVars.NL "config_variables.h"

# config.cpp in this DDNet branch may hold a second manual MACRO_CONFIG list.
# If it does, update that list too; if it includes config_variables.h, do not duplicate it.
$ConfigCppHasManualVertexList = $ConfigCpp.Text -match 'MACRO_CONFIG_[A-Z0-9_]+\([^\r\n]*(VxAled|VxAf|VxRain|VxLeaves)'
$ConfigCppIncludesVariables = $ConfigCpp.Text -match '#include\s*["<][^">]*config_variables\.h[">]'
if($ConfigCppHasManualVertexList) {
    $ConfigCpp.Text = [regex]::Replace($ConfigCpp.Text,$LegacyAimPattern,'')
    $ConfigCpp.Text = Merge-ConfigMacros $ConfigCpp.Text $AimConfigLines $ConfigCpp.NL "config.cpp"
} elseif(!$ConfigCppIncludesVariables) {
    throw "В config.cpp не найден ни второй список Vertex config, ни include config_variables.h. Нужна проверка config.cpp; проект не изменён."
}

# 6. Confirm every g_Config field referenced by the replacement menu, controls,
#    and Aim cpp exists in config_variables.h after merging.
$AllNewCode = $NewMenu + $NewControls + $AimCpp
$Missing = New-Object System.Collections.Generic.List[string]
foreach($m in [regex]::Matches($AllNewCode,'g_Config\.m_([A-Za-z0-9_]+)')) {
    $Name = $m.Groups[1].Value
    if(!$DeclaredConfig.Contains($Name) -and $ConfigVars.Text -notmatch ('MACRO_CONFIG_[A-Z0-9_]+\(\s*' + [regex]::Escape($Name) + '\s*,')) {
        $Missing.Add($Name)
    }
}
$MissingUnique = @($Missing | Select-Object -Unique)
if($MissingUnique.Count -gt 0) {
    throw "Не объявлены конфиги, используемые новыми файлами: $($MissingUnique -join ', '). Проект не изменён."
}

# 7. CMake uses a curated GAME_CLIENT list, not an automatic directory glob.
#    The old components/aimbot.cpp is a previous CAimBot implementation. If it
#    is still listed, remove only its build-list entries so its definitions do
#    not collide with the supplied replacement src/game/client/aimbot.cpp.
foreach($LegacyAimEntry in @('components/aimbot.cpp', 'components/aimbot.h')) {
    $LegacyPattern = '(?m)^[ \t]*' + [regex]::Escape($LegacyAimEntry) + '[ \t]*\r?\n'
    if([regex]::IsMatch($CMake.Text, $LegacyPattern)) {
        $CMake.Text = [regex]::Replace($CMake.Text, $LegacyPattern, '', 1)
    }
}
foreach($Entry in @('aimbot.cpp', 'aimbot.h')) {
    if($CMake.Text -notmatch ('(?m)^\s*' + [regex]::Escape($Entry) + '\s*$')) {
        $CMake.Text = Add-SortedGameClientEntry $CMake.Text $Entry $CMake.NL
    }
}

# Ensure the Auto Aled implementation and header are included in GAME_CLIENT.
$AledCpp = [IO.Path]::ChangeExtension($AledHeader,'.cpp')
if(!(Test-Path $AledCpp)) { throw "Рядом с $AledHeader не найден aled.cpp." }
$AledCppFull = [IO.Path]::GetFullPath($AledCpp)
$AledCppRel = $AledCppFull.Substring($ClientSourceFull.Length + 1).Replace('\','/')
$AledHeaderRel = $AledHeaderFull.Substring($ClientSourceFull.Length + 1).Replace('\','/')
foreach($Entry in @($AledCppRel, $AledHeaderRel)) {
    if($CMake.Text -notmatch ('(?m)^\s*' + [regex]::Escape($Entry) + '\s*$')) {
        $CMake.Text = Add-SortedGameClientEntry $CMake.Text $Entry $CMake.NL
    }
}

# 8. Verify there is only one integration call per stage and renderer definitions.
foreach($check in @(
    @{Text=$NewControls; Pattern='m_AimBot\.Update\('; Name='Aim Update'},
    @{Text=$NewControls; Pattern='m_AimBot\.RenderBinds\('; Name='Aim RenderBinds'},
    @{Text=$Players.Text; Pattern='m_AimBot\.RenderWorld\('; Name='Aim RenderWorld'},
    @{Text=$GameClientH.Text; Pattern='CAimBot\s+m_AimBot'; Name='CGameClient Aim member'}
)) {
    if(([regex]::Matches($check.Text,$check.Pattern)).Count -ne 1) { throw "$($check.Name): ожидалось ровно одно подключение." }
}

# 9. Prepare write targets and backups. No file is written until every preflight passes.
$AimHeaderTarget = $Full.AimH
$AimCppTarget = $Full.AimCpp
$Targets = @(
    $Full.Controls, $Full.ControlsH, $Full.Menu, $Full.GameClientH,
    $Full.Players, $Full.AvoidH, $Full.ConfigVars, $Full.ConfigCpp,
    $Full.CMake, $AledHeader, $AimHeaderTarget, $AimCppTarget
)
$Stamp = Get-Date -Format "yyyyMMdd_HHmmss"
foreach($p in $Targets) {
    if(Test-Path $p) { Copy-Item -LiteralPath $p -Destination ($p + ".before_aimv2_" + $Stamp + ".bak") -Force }
}

# Write the replacement files and all integration points.
[IO.File]::WriteAllText($Full.Controls,$NewControls,[System.Text.UTF8Encoding]::new($false))
[IO.File]::WriteAllText($Full.Menu,$NewMenu,[System.Text.UTF8Encoding]::new($false))
Copy-Item (Join-Path $Payload "aimbot.h") $AimHeaderTarget -Force
Copy-Item (Join-Path $Payload "aimbot.cpp") $AimCppTarget -Force
Write-Source $ControlsH
Write-Source $GameClientH
Write-Source $Players
Write-Source $AvoidH
Write-Source $AledHSource
Write-Source $ConfigVars
Write-Source $ConfigCpp
Write-Source $CMake

Write-Host "`nVERTEX AIM SYSTEM V2 УСТАНОВЛЕН" -ForegroundColor Green
Write-Host "Заменены controls.cpp и menus_settings_vertex.cpp файлами из твоего набора."
Write-Host "Подключены CAimBot, 34 vx_aim_* настройки, Aim Update/RenderBinds/RenderWorld."
Write-Host "Сохранены текущие AvoidFreeze и Auto Aled; адаптированы только их вызовы/IsBusy к старому API."
Write-Host "Новое меню содержит VertexRenderParticles и VertexRenderWorld — HUD-вызовы листьев/дождя/частиц оставлены активными."
Write-Host "Резервные копии всех изменённых файлов: *.before_aimv2_$Stamp.bak"
Write-Host "`nСледующий шаг: cmake --build .\build --config Release --parallel 4"
