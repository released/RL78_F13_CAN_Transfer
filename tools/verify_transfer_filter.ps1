# Project-local source and linked-image guard. No COM/programmer operation.
param([string]$ProjectRoot=(Split-Path -Parent $PSScriptRoot),[switch]$CheckImage)
$ErrorActionPreference='Stop'
$root=(Resolve-Path -LiteralPath $ProjectRoot).Path
$config=[IO.File]::ReadAllText((Join-Path $root 'can_transfer_config.h'))
$controller=[IO.File]::ReadAllText((Join-Path $root 'hal\can\RSCANlite.h'))
$rules=[IO.File]::ReadAllText((Join-Path $root 'hal\can\RSCANlite.c'))
function Read-Define([string]$Name,[string]$Source,[int]$Depth=0) {
    if($Depth -gt 8){throw ('Recursive Transfer definition: '+$Name)}
    $matches=[regex]::Matches($Source,'(?m)^\s*#define\s+'+[regex]::Escape($Name)+'\s+([^\r\n]+)')
    if($matches.Count -ne 1){throw ('Missing/ambiguous Transfer definition: '+$Name)}
    $value=($matches[0].Groups[1].Value -replace '/\*.*?\*/','').Trim().Trim('(',')').Trim()
    if($value -match '^0x([0-9A-Fa-f]+)[uUlL]*$'){return [Convert]::ToUInt32($Matches[1],16)}
    if($value -match '^([0-9]+)[uUlL]*$'){return [Convert]::ToUInt32($Matches[1],10)}
    if($value -match '^[A-Za-z_]\w*$'){return Read-Define $value $Source ($Depth+1)}
    throw ('Unsupported Transfer definition: '+$Name)
}
$expected=@(
    (Read-Define 'CAN_TRANSFER_RX_RULE_ID_LOW' $config),
    (Read-Define 'CAN_TRANSFER_RX_RULE_ID_HIGH' $config),
    (Read-Define 'CAN_TRANSFER_RX_RULE_MASK_LOW' $config),
    (Read-Define 'CAN_TRANSFER_RX_RULE_MASK_HIGH' $config),1,0)
if($expected[0] -ne (Read-Define 'CAN_TRANSFER_REQUEST_ID' $config) -or
   $expected[0] -gt 0x7FF -or $expected[1] -ne 0 -or $expected[2] -ne 0x7FF -or $expected[3] -ne 0xC000 -or
   (Read-Define 'CAN_RX_RULE_NUM_CH0' $controller) -ne 1){throw 'Transfer requires one exact standard-data RX FIFO0 rule.'}
$code=[regex]::Replace($rules,'(?s)/\*.*?\*/|//[^\r\n]*','')
if($code -notmatch '#include\s+"can_transfer_config.h"' -or $code -match 'CAN_BOOT_|can_boot_config.h'){
    throw 'Transfer CAN rules must use the local Transfer configuration, never Boot configuration.'
}
Write-Host ('PASS Transfer source RX filter:0x{0:X3}/0x7FF,one rule,RX FIFO0.' -f $expected[0])
if(!$CheckImage){return}
$name=Split-Path -Leaf $root
$map=[IO.File]::ReadAllText((Join-Path $root ('DefaultBuild\'+$name+'.map')))
$symbol=[regex]::Matches($map,'(?m)^\s*_g_rxrule_table\s*\r?\n\s*([0-9A-Fa-f]{8})\s+([0-9A-Fa-f]+)\s+data\b')
if($symbol.Count -ne 1){throw 'Linked Transfer RX rule symbol is missing/ambiguous.'}
$address=[Convert]::ToUInt32($symbol[0].Groups[1].Value,16)
if([Convert]::ToUInt32($symbol[0].Groups[2].Value,16) -ne 12){throw 'Linked Transfer RX table must contain exactly six16-bit words.'}
$memory=@{}
foreach($line in [IO.File]::ReadAllLines((Join-Path $root ('DefaultBuild\'+$name+'.mot')))) {
    if($line -notmatch '^S([123])([0-9A-Fa-f]+)$'){continue}
    $kind=[int]$Matches[1];$text=$Matches[2]
    if(($text.Length -band 1) -ne 0){throw 'Odd-length S-record.'}
    $record=New-Object byte[] ($text.Length/2)
    $sum=0
    for($i=0;$i -lt $record.Length;$i++){$record[$i]=[Convert]::ToByte($text.Substring($i*2,2),16);$sum+=$record[$i]}
    $addressBytes=$kind+1
    if($record.Length -ne ($record[0]+1) -or $record.Length -lt ($addressBytes+2) -or ($sum -band 255) -ne 255){throw 'Invalid S-record length/checksum.'}
    [uint32]$base=0
    for($i=1;$i -le $addressBytes;$i++){$base=($base -shl 8) -bor $record[$i]}
    for($i=$addressBytes+1;$i -lt $record.Length-1;$i++) {
        $at=[uint32]($base+$i-$addressBytes-1)
        if($memory.ContainsKey($at) -and $memory[$at] -ne $record[$i]){throw 'Conflicting S-record bytes.'}
        $memory[$at]=$record[$i]
    }
}
for($word=0;$word -lt $expected.Count;$word++) {
    $at=[uint32]($address+$word*2)
    $next=[uint32]($at+1)
    if(!$memory.ContainsKey($at) -or !$memory.ContainsKey($next)){throw 'Linked RX rule bytes are missing from MOT.'}
    $actual=[uint32]$memory[$at]+([uint32]$memory[$next] -shl 8)
    if($actual -ne $expected[$word]){throw ('Linked RX filter mismatch at word'+$word+':expected='+$expected[$word]+',actual='+$actual)}
}
Write-Host ('PASS linked Transfer MOT RX table@0x{0:X}:ID0x{1:X3},mask0x7FF,standard data,RX FIFO0.' -f $address,$expected[0])
