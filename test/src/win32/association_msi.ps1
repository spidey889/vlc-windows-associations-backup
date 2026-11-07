# Copyright (C) 2026 VideoLAN and its authors
# Copyright (C) 2026 Vinit Rajpurohit (spidey889)
# SPDX-License-Identifier: GPL-2.0-or-later
# Native half of association_msi.py. Never invoke an install/UI/execute sequence.
param([Parameter(Mandatory=$true)][string]$Fixture)
$ErrorActionPreference='Stop'
$Fixture=(Resolve-Path -LiteralPath $Fixture).Path
function Require($condition, $message) { if(-not $condition){throw $message} }

$engine=New-Object -ComObject WindowsInstaller.Installer
$db=$engine.OpenDatabase((Join-Path $Fixture 'fixture.msi'),0)
function Rows($table, $columns) {
    $fields=($columns|ForEach-Object {'`'+$_+'`'}) -join ','
    $view=$db.OpenView(('SELECT '+$fields+' FROM `'+$table+'`'))
    try {
        $null=$view.Execute()
        while($record=$view.Fetch()) {
            $row=@{}
            for($i=0;$i -lt $columns.Count;$i++){$row[$columns[$i]]=$record.StringData($i+1)}
            [pscustomobject]$row
            [void][Runtime.InteropServices.Marshal]::FinalReleaseComObject($record)
        }
    } finally {
        $null=$view.Close()
        [void][Runtime.InteropServices.Marshal]::FinalReleaseComObject($view)
    }
}
try {
    $components=@(Rows Component @('Component','Condition'))
    $registry=@(Rows Registry @('Root','Key','Name','Value','Component_'))
    $featureComponents=@(Rows FeatureComponents @('Feature_','Component_'))
    $actions=@(Rows CustomAction @('Action','Type','Source','Target'))
    $sequence=@(Rows InstallUISequence @('Action','Condition','Sequence'))
    $execute=@(Rows InstallExecuteSequence @('Action'))
    $events=@(Rows ControlEvent @('Dialog_','Control_','Event','Argument','Condition','Ordering'))
} finally {
    [void][Runtime.InteropServices.Marshal]::FinalReleaseComObject($db)
    [void][Runtime.InteropServices.Marshal]::FinalReleaseComObject($engine)
}
# Check compiled registrations after the old NSIS keys have disappeared. The
# legacy ProgID string must still resolve without changing UserChoice or history.
[xml]$inventory=Get-Content (Join-Path $Fixture 'extensions.wxs') -Raw
foreach($group in 'Audio','Video','Other') {
    $component=$inventory.Wix.Fragment.DirectoryRef.Component|Where-Object Id -eq "Comp${group}FileAssociation"
    $owner="Comp${group}NSISFileAssociation"
    $compatibility=@($components|Where-Object Component -eq $owner)
    Require ($compatibility.Count -eq 1 -and -not $compatibility[0].Condition) 'Compatibility registration must be independent of defaults eligibility/history'
    Require (@($featureComponents|Where-Object {$_.Feature_ -eq ($group.ToUpper()+'FILEASSOCIATION') -and $_.Component_ -eq $owner}).Count -eq 1) "Compatibility registrations must follow the $group selection"
    foreach($mapping in @($component.RegistryValue|Where-Object Key -eq 'Software\Clients\Media\VLC\Capabilities\FileAssociations')) {
        $legacy='VLC'+$mapping.Name
        $native=$mapping.Value
        $command=@($registry|Where-Object {$_.Root -eq '0' -and $_.Key -eq ($legacy+'\shell\open\command') -and -not $_.Name -and $_.Component_ -eq $owner})
        $original=@($registry|Where-Object {$_.Root -eq '0' -and $_.Key -eq ($native+'\shell\open\command') -and -not $_.Name -and $_.Component_ -eq $component.Id})
        Require ($command.Count -eq 1) "Missing NSIS-compatible open command: $legacy"
        Require ($original.Count -eq 1 -and $command[0].Value -ceq $original[0].Value) "Compatibility command must use the installed VLC: $legacy"
        foreach($suffix in @('','\DefaultIcon')) {
            $alias=@($registry|Where-Object {$_.Root -eq '0' -and $_.Key -eq ($legacy+$suffix) -and -not $_.Name -and $_.Component_ -eq $owner})
            $original=@($registry|Where-Object {$_.Root -eq '0' -and $_.Key -eq ($native+$suffix) -and -not $_.Name -and $_.Component_ -eq $component.Id})
            Require ($alias.Count -eq 1 -and $original.Count -eq 1 -and $alias[0].Value -ceq $original[0].Value) "Compatibility name/icon must match MSI: $legacy"
        }
    }
}
Require (@($registry|Where-Object {($_.Root -eq '0' -and $_.Key -match '^\.[^\\]+$' -and -not $_.Name) -or $_.Key -match '\\(UserChoice|UserChoiceLatest|InstallerDefaults)(\\|$)'}).Count -eq 0) 'Registration must not assign defaults or alter completion history'
Write-Output 'PASS: MSI NSIS ProgID compatibility registrations and feature ownership'

$action=@($actions|Where-Object Action -eq 'VlcSetDefaultAssociations')
Require ($action.Count -eq 1 -and $action[0].Type -eq '210' -and $action[0].Source -eq 'vlc.exe') 'Expected immediate impersonating asyncNoWait installed-EXE action'
Require ($action[0].Target -ceq '--win32-set-default-associations-once=[VlcAssociationExtensions]') 'Once-only CLI contract changed'
Require (@($execute|Where-Object Action -like '*Association*').Count -eq 0) 'Association action must stay out of the elevated execute sequence'
$capture=$sequence|Where-Object Action -eq 'VlcSetAssociationEligibility'
$reset=$sequence|Where-Object Action -eq 'VlcResetAssociationEligibility'
foreach($prior in 'AppSearch','FindRelatedProducts','CostFinalize') {
    Require ([int]$capture.Sequence -gt [int]($sequence|Where-Object Action -eq $prior).Sequence) "Eligibility must follow $prior"
}
Require ([int]$reset.Sequence -lt [int]($sequence|Where-Object Action -eq 'AppSearch').Sequence) 'Eligibility reset must precede searches'
Require ([int]$capture.Sequence -lt [int]($sequence|Where-Object Action -eq 'ExecuteAction').Sequence) 'Eligibility must be captured before installation'
$finish=@($events|Where-Object {$_.Dialog_ -eq 'ExitDialog' -and $_.Control_ -eq 'Finish' -and ($_.Event -like '[[]Vlc*' -or $_.Argument -eq 'VlcSetDefaultAssociations')}|Sort-Object {[int]$_.Ordering})
Require (@($finish|Where-Object Event -eq 'DoAction').Count -eq 1) 'Expected one Finish handoff'

Add-Type -TypeDefinition @'
using System;using System.Text;using System.Runtime.InteropServices;
public static class MsiAssociationFixture {
 [DllImport("msi.dll")] public static extern uint MsiSetInternalUI(uint level,IntPtr hwnd);
 [DllImport("msi.dll",CharSet=CharSet.Unicode)] public static extern uint MsiOpenPackageExW(string path,uint flags,out uint handle);
 [DllImport("msi.dll",CharSet=CharSet.Unicode)] public static extern uint MsiSetPropertyW(uint h,string name,string value);
 [DllImport("msi.dll",CharSet=CharSet.Unicode)] public static extern uint MsiGetPropertyW(uint h,string name,StringBuilder value,ref uint size);
 [DllImport("msi.dll",CharSet=CharSet.Unicode)] public static extern uint MsiDoActionW(uint h,string action);
 [DllImport("msi.dll",CharSet=CharSet.Unicode)] public static extern int MsiEvaluateConditionW(uint h,string condition);
 [DllImport("msi.dll",CharSet=CharSet.Unicode)] public static extern uint MsiSetFeatureStateW(uint h,string name,int state);
 [DllImport("msi.dll",CharSet=CharSet.Unicode)] public static extern uint MsiGetFeatureStateW(uint h,string name,out int installed,out int action);
 [DllImport("msi.dll",CharSet=CharSet.Unicode)] public static extern uint MsiSetTargetPathW(uint h,string directory,string path);
 [DllImport("msi.dll")] public static extern uint MsiCloseHandle(uint h);
 [DllImport("msi.dll")] public static extern uint MsiCreateRecord(uint fields);
 [DllImport("msi.dll",CharSet=CharSet.Unicode)] public static extern uint MsiRecordSetStringW(uint h,uint field,string value);
 [DllImport("msi.dll",CharSet=CharSet.Unicode)] public static extern uint MsiFormatRecordW(uint h,uint record,StringBuilder value,ref uint size);
 public static void Set(uint h,string name,string value){if(MsiSetPropertyW(h,name,value)!=0)throw new Exception("Set: "+name);}
 public static string Get(uint h,string name){var b=new StringBuilder(32768);uint z=(uint)b.Capacity;if(MsiGetPropertyW(h,name,b,ref z)!=0)throw new Exception("Get: "+name);return b.ToString();}
 public static string Format(uint h,string text){uint r=MsiCreateRecord(0);try{if(MsiRecordSetStringW(r,0,text)!=0)throw new Exception("Record");var b=new StringBuilder(32768);uint z=(uint)b.Capacity;if(MsiFormatRecordW(h,r,b,ref z)!=0)throw new Exception("Format");return b.ToString();}finally{MsiCloseHandle(r);}}
 public static void Action(uint h,string name){
  if(name!="CostInitialize"&&name!="FileCost"&&name!="CostFinalize"&&name!="VlcResetAssociationEligibility"&&name!="VlcSetAssociationEligibility"&&name!="VlcSetDefaultAssociations")throw new Exception("Disallowed action: "+name);
  if(MsiDoActionW(h,name)!=0)throw new Exception("Action: "+name);
 }
}
'@
$oldUI=[MsiAssociationFixture]::MsiSetInternalUI(2,[IntPtr]::Zero)
[uint32]$h=0
$payload=(Join-Path $Fixture 'payload')+'\'
$groups=@('AUDIOFILEASSOCIATION','VIDEOFILEASSOCIATION','OTHERFILEASSOCIATION')
$guards=@('Installed','REINSTALL','REMOVE','PATCH','UPGRADINGPRODUCTCODE','RESUME','Preselected')
$requests=[Collections.Generic.List[string]]::new()
function Set-Features($mask) {
    for($i=0;$i -lt 3;$i++) {
        $state=if($mask -band (1 -shl $i)){3}else{2}
        Require ([MsiAssociationFixture]::MsiSetFeatureStateW($h,$groups[$i],$state) -eq 0) 'Feature selection failed'
    }
}
function Eligible {
    $result=[MsiAssociationFixture]::MsiEvaluateConditionW($h,$capture.Condition)
    Require ($result -ne 3) 'Invalid MSI eligibility condition'
    return $result -eq 1
}
function Finish([bool]$launch=$false) {
    # Exercise compiled events in order, with MSI's own condition/format engine.
    # Do not run EndDialog, normal VLC launch or any other installer action.
    foreach($event in $finish) {
        $condition=[MsiAssociationFixture]::MsiEvaluateConditionW($h,$event.Condition)
        Require ($condition -ne 3) 'Invalid Finish condition'
        if($condition -ne 1){continue}
        if($event.Event.StartsWith('[')) {
            $value=if($event.Argument -eq '{}'){''}else{[MsiAssociationFixture]::Format($h,$event.Argument)}
            [MsiAssociationFixture]::Set($h,$event.Event.Trim('[',']'),$value)
        } else {
            $requests.Add([MsiAssociationFixture]::Get($h,'VlcAssociationExtensions'))
            if($launch) {
                Require ([MsiAssociationFixture]::Format($h,'[#vlc.exe]') -eq ($payload+'vlc.exe')) 'Unsafe mock-worker path'
                [MsiAssociationFixture]::Action($h,'VlcSetDefaultAssociations')
            }
        }
    }
}
$started=$null;$gate=$null
$oldStarted=$env:VLC_MSI_TEST_STARTED;$oldGate=$env:VLC_MSI_TEST_GATE
try {
    # OpenPackage/costing changes session memory only; no install is performed.
    Require ([MsiAssociationFixture]::MsiOpenPackageExW((Join-Path $Fixture 'fixture.msi'),0,[ref]$h) -eq 0) 'Open fixture failed'
    [MsiAssociationFixture]::Set($h,'APPLICATIONFOLDER',$payload)
    [MsiAssociationFixture]::Set($h,'ACTION','INSTALL')
    foreach($name in 'CostInitialize','FileCost','CostFinalize'){[MsiAssociationFixture]::Action($h,$name)}
    foreach($group in $groups) {
        [int]$installed=0;[int]$state=0
        Require ([MsiAssociationFixture]::MsiGetFeatureStateW($h,$group,[ref]$installed,[ref]$state) -eq 0 -and $state -eq 3) "Quick Install must select $group"
    }
    foreach($guard in $guards){[MsiAssociationFixture]::Set($h,$guard,'')}
    foreach($level in '2','3','4','5') {
        [MsiAssociationFixture]::Set($h,'UILevel',$level)
        Require ((Eligible) -eq ($level -eq '5')) "Wrong eligibility for UILevel $level"
    }
    foreach($guard in $guards) {
        [MsiAssociationFixture]::Set($h,$guard,'1')
        Require (-not (Eligible)) "$guard must block eligibility"
        [MsiAssociationFixture]::Set($h,$guard,'')
    }
    foreach($upgrade in 'WIX_UPGRADE_DETECTED','NSISLOCATION') {
        [MsiAssociationFixture]::Set($h,$upgrade,'1')
        Require (Eligible) "$upgrade must permit a supported full-UI install"
        [MsiAssociationFixture]::Set($h,'VlcAssociationEligible','1')
        [MsiAssociationFixture]::Set($h,'VlcAssociationAttempted','')
        $count=$requests.Count;Finish
        Require ($requests.Count -eq $count+1) "$upgrade must also permit the Finish request"
        [MsiAssociationFixture]::Set($h,$upgrade,'')
    }
    [MsiAssociationFixture]::Set($h,'ACTION','ADMIN')
    Require (-not (Eligible)) 'Administrative image must be excluded'
    [MsiAssociationFixture]::Set($h,'ACTION','INSTALL')
    [MsiAssociationFixture]::Set($h,'INSTALLLEVEL','1')
    Require (Eligible) 'Feature INSTALLLEVEL must not be confused with UILevel'
    [MsiAssociationFixture]::Set($h,'VlcAssociationEligible','stale')
    [MsiAssociationFixture]::Action($h,'VlcResetAssociationEligibility')
    Require ([MsiAssociationFixture]::Get($h,'VlcAssociationEligible') -eq '') 'Initial reset must clear stale eligibility'
    [MsiAssociationFixture]::Action($h,'VlcSetAssociationEligibility')
    Require ([MsiAssociationFixture]::Get($h,'VlcAssociationEligible') -eq '1') 'Capture must set client eligibility'
    Write-Output 'PASS: MSI eligibility'

    $catalog=@('Audio','Video','Other')|ForEach-Object {
        $component=$inventory.Wix.Fragment.DirectoryRef.Component|Where-Object Id -eq "Comp${_}FileAssociation"
        ,@($component.RegistryValue|Where-Object Key -eq 'Software\Clients\Media\VLC\Capabilities\FileAssociations'|ForEach-Object Name)
    }
    for($mask=0;$mask -lt 8;$mask++) {
        Set-Features $mask
        $expected=@();for($i=0;$i -lt 3;$i++){if($mask -band (1 -shl $i)){$expected+=$catalog[$i]}}
        $expected=@($expected|Where-Object {$_ -notin @('.mid','.bin')})
        [MsiAssociationFixture]::Set($h,'VlcAssociationEligible','1')
        [MsiAssociationFixture]::Set($h,'VlcAssociationAttempted','')
        [MsiAssociationFixture]::Set($h,'VlcAssociationExtensions','.stale')
        $count=$requests.Count;Finish
        Require ($requests.Count -eq $count+[int]($mask -ne 0)) "Wrong request count for group mask $mask"
        if($mask) {
            $actual=$requests[$count].Split(',')
            Require ('.mid' -notin $actual -and '.bin' -notin $actual) "Excluded automatic defaults in group mask $mask"
            Require ($requests[$count] -ceq ($expected -join ',')) "Wrong selected extensions for group mask $mask"
        }
        Finish
        Require ($requests.Count -eq $count+[int]($mask -ne 0)) "Repeated Finish retried mask $mask"
        Require ([MsiAssociationFixture]::Get($h,'VlcAssociationExtensions') -eq '') 'Prepared list must be cleared'
    }
    Set-Features 7
    foreach($guard in @('')+@($guards|Where-Object {$_ -ne 'Preselected'})) {
        [MsiAssociationFixture]::Set($h,'VlcAssociationEligible',$(if($guard){'1'}else{''}))
        [MsiAssociationFixture]::Set($h,'VlcAssociationAttempted','')
        if($guard){[MsiAssociationFixture]::Set($h,$guard,'1')}
        $count=$requests.Count;Finish
        Require ($requests.Count -eq $count) "Finish must not grant eligibility: $guard"
        if($guard){[MsiAssociationFixture]::Set($h,$guard,'')}
    }
    Write-Output 'PASS: MSI Audio/Video/Others selection and single-attempt Finish'

    # A gate proves handoff without a fragile subsecond timing threshold.
    $id=[guid]::NewGuid().ToString('N')
    $env:VLC_MSI_TEST_STARTED='Local\VlcMsiStarted'+$id
    $env:VLC_MSI_TEST_GATE='Local\VlcMsiGate'+$id
    $started=[Threading.EventWaitHandle]::new($false,0,$env:VLC_MSI_TEST_STARTED)
    $gate=[Threading.EventWaitHandle]::new($false,1,$env:VLC_MSI_TEST_GATE)
    [MsiAssociationFixture]::Set($h,'VlcAssociationEligible','1')
    [MsiAssociationFixture]::Set($h,'VlcAssociationAttempted','')
    $count=$requests.Count;Finish $true
    Require ($started.WaitOne(5000)) 'MSI did not launch the mock worker'
    Require (-not (Test-Path (Join-Path $payload 'completed.txt'))) 'MSI waited for worker completion'
    $lines=@(Get-Content (Join-Path $payload 'invocations.txt'))
    Require ($lines.Count -eq 1 -and $lines[0] -ceq ('--win32-set-default-associations-once='+$requests[$count])) 'Wrong native worker arguments'
    $null=$gate.Set()
    for($i=0;$i -lt 50 -and -not (Test-Path (Join-Path $payload 'completed.txt'));$i++){Start-Sleep -Milliseconds 100}
    Require ((Get-Content (Join-Path $payload 'completed.txt') -Raw).Trim() -eq '0') 'Mock worker did not finish after gate release'
    Finish $true
    Require ($requests.Count -eq $count+1) 'Nonzero worker exit must not retry'
    $missing=Join-Path $Fixture 'missing-worker';$null=New-Item -ItemType Directory -Path $missing
    $payload=$missing+'\'
    Require ([MsiAssociationFixture]::MsiSetTargetPathW($h,'APPLICATIONFOLDER',$payload) -eq 0) 'Missing-worker target failed'
    [MsiAssociationFixture]::Set($h,'VlcAssociationEligible','1')
    [MsiAssociationFixture]::Set($h,'VlcAssociationAttempted','')
    $count=$requests.Count;Finish $true;Finish $true
    Require ($requests.Count -eq $count+1) 'Launch failure must be nonfatal and must not retry'
    Write-Output 'PASS: MSI asynchronous handoff and nonfatal worker/launch failure'
} finally {
    if($gate){$null=$gate.Set();$gate.Dispose()}
    if($started){$started.Dispose()}
    $env:VLC_MSI_TEST_STARTED=$oldStarted;$env:VLC_MSI_TEST_GATE=$oldGate
    if($h){$null=[MsiAssociationFixture]::MsiCloseHandle($h)}
    $null=[MsiAssociationFixture]::MsiSetInternalUI($oldUI,[IntPtr]::Zero)
}
