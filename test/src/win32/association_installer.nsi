; Copyright (C) 2026 Vinit Rajpurohit (spidey889)
; SPDX-License-Identifier: GPL-2.0-or-later

; Isolated NSIS regression executable: generated mocks are mandatory. No VLC,
; registry writer, installer payload or elevation request is used by this fixture.
!ifndef ASSOCIATION_TEST_HELPER
  !error "Build with association_installer.py; production helpers must be mocked."
!endif
Unicode true
!include LogicLib.nsh
!include Memento.nsh
!include FileFunc.nsh
!include WinVer.nsh
!include StrFunc.nsh
${StrRep}
${StrCase}
!define VERSION "4.0.0"
!define PRODUCT_DIR_REGKEY "Unused association fixture"
!define PRODUCT_UNINST_ROOT_KEY HKLM
!define PRODUCT_UNINST_KEY "Unused association fixture"
!define MEMENTO_REGISTRY_ROOT HKLM
!define MEMENTO_REGISTRY_KEY "Unused association fixture"
Name "VLC association adapter regressions"
OutFile "association-installer-test.exe"
RequestExecutionLevel user
SilentInstall silent
AutoCloseWindow true
LangString ShellAssociation_Play 1033 "Play with VLC"

Var Report
Var Checks
Var MarkerRead
Var MarkerAt
Var Launches
Var MockExit
Var LaunchFailure
Var MockElevated
Var RegistrationFailure
Var MockCommand
Var MockDetail
Var LegacyCalls
Var ExpectedExtensions
Var RegisteredExtension
Var RegisteredCapabilities
Var RegisteredWrites
Var RegisteredMsiValues
Var RemovedMsiAlias
Var MockMsiCommand
Var MockRegistryRoot
Var MockRegistryKey
Var MockRegistryName
Var MockRegistryValue
Var MockCopyError
Var MockPathError
Var PreviousVersion
Var PreviousVersionState

!include "${ASSOCIATION_TEST_HELPER}"
!include "${ASSOCIATION_TEST_EXTENSIONS}"

!macro Check VALUE EXPECTED MESSAGE
  IntOp $Checks $Checks + 1
  ${If} "${VALUE}" S!= "${EXPECTED}"
    FileWrite $Report "FAIL: ${MESSAGE}$\r$\n"
    FileClose $Report
    SetErrorLevel 1
    Quit
  ${EndIf}
!macroend

Function MockReadMarker
  IntOp $MarkerRead $MarkerRead + 1
  ClearErrors
  StrCpy $0 ""
  ${If} $MarkerRead != $MarkerAt
    SetErrors
  ${EndIf}
FunctionEnd

Function MockExecWait
  IntOp $Launches $Launches + 1
  StrCpy $1 $MockExit
  ${If} $LaunchFailure == 1
    ; ExecWait's output is undefined on launch error: zero must not mean success.
    StrCpy $1 0
    SetErrors
  ${EndIf}
FunctionEnd

Function MockAssociateExtension
  IntOp $LegacyCalls $LegacyCalls + 1
FunctionEnd

; These mocks only record operations; no registry API is called.
Function MockMachineRegistry
  !insertmacro Check "$MockRegistryRoot" "HKLM" "registration and cleanup must not use either user's hive"
  StrLen $0 "Software\Classes\"
  StrCpy $1 $MockRegistryKey $0
  ${If} $1 == "Software\Classes\"
    ; Existing metadata checks use paths relative to the verified machine classes.
    StrCpy $MockRegistryKey $MockRegistryKey "" $0
  ${Else}
    StrLen $0 "Software\Clients\Media\VLC\Capabilities\FileAssociations"
    StrCpy $1 $MockRegistryKey $0
    !insertmacro Check "$1" "Software\Clients\Media\VLC\Capabilities\FileAssociations" "class keys require the explicit Software Classes prefix"
  ${EndIf}
FunctionEnd

Function MockWriteRegStr
  Pop $MockRegistryValue
  Pop $MockRegistryName
  Pop $MockRegistryKey
  Pop $MockRegistryRoot
  Call MockMachineRegistry
  IntOp $RegisteredWrites $RegisteredWrites + 1
  StrLen $0 "VLC$R0.Document"
  StrCpy $1 $MockRegistryKey $0
  ${If} $MockRegistryRoot == "HKLM"
  ${AndIf} $1 == "VLC$R0.Document"
    StrCpy $RegisteredMsiValues "$RegisteredMsiValues|$MockRegistryKey|$MockRegistryName|$MockRegistryValue"
  ${EndIf}
  ${If} $MockRegistryRoot == "HKLM"
  ${AndIf} $MockRegistryKey == "Applications\vlc.exe\SupportedTypes"
    StrCpy $RegisteredExtension $MockRegistryName
  ${EndIf}
  ${If} $MockRegistryRoot == "HKLM"
  ${AndIf} $MockRegistryKey == "Software\Clients\Media\VLC\Capabilities\FileAssociations"
    StrCpy $RegisteredCapabilities $MockRegistryValue
  ${EndIf}
FunctionEnd

Function MockReadRegStr
  Pop $MockRegistryName
  Pop $MockRegistryKey
  Pop $MockRegistryRoot
  Call MockMachineRegistry
  ${If} $MockRegistryRoot == "HKLM"
  ${AndIf} $MockRegistryKey == "VLC$R0.Document\shell\Open\command"
  ${AndIf} $MockRegistryName == ""
    Push $MockMsiCommand
  ${Else}
    Push ""
  ${EndIf}
FunctionEnd

Function MockDeleteRegValue
  Pop $MockRegistryName
  Pop $MockRegistryKey
  Pop $MockRegistryRoot
  Call MockMachineRegistry
FunctionEnd

Function MockDeleteRegKey
  Pop $MockRegistryKey
  Pop $MockRegistryRoot
  Call MockMachineRegistry
  ${If} $MockRegistryRoot == "HKLM"
  ${AndIf} $MockRegistryKey == "VLC$R0.Document"
    IntOp $RemovedMsiAlias $RemovedMsiAlias + 1
  ${EndIf}
FunctionEnd

!macro CheckMsiCompatibility TYPE EXT LABEL
  Call ResetCase
  StrCpy $R0 "${EXT}"
  StrCpy $R1 "${TYPE}"
  Call TestRegisterExtension
  !insertmacro Check "$RegisteredExtension" "${EXT}" "compatibility registration keeps SupportedTypes"
  !insertmacro Check "$RegisteredCapabilities" "VLC${EXT}" "compatibility registration keeps the native capability"
  !insertmacro Check "$RegisteredWrites" 14 "only the native registration and six compatibility values are written"
  !insertmacro Check "$RegisteredMsiValues" '|VLC${EXT}.Document||${LABEL} ${TYPE} File (VLC)|VLC${EXT}.Document\shell||Open|VLC${EXT}.Document\shell\Open||Play with VLC|VLC${EXT}.Document\shell\Open|MultiSelectModel|Player|VLC${EXT}.Document\shell\Open\command||$\"$INSTDIR\vlc.exe$\" --started-from-file $\"%1$\"|VLC${EXT}.Document\DefaultIcon||$\"$INSTDIR\vlc.exe$\",0' "MSI alias has the current executable and full native metadata"

  StrCpy $MockMsiCommand '$\"$INSTDIR\vlc.exe$\" --started-from-file $\"%1$\"'
  Call TestUnRegisterExtension
  !insertmacro Check "$RemovedMsiAlias" 1 "uninstall removes this installation's alias"
  StrCpy $RemovedMsiAlias 0
  StrCpy $MockMsiCommand '$\"D:\Other VLC\vlc.exe$\" --started-from-file $\"%1$\"'
  Call TestUnRegisterExtension
  !insertmacro Check "$RemovedMsiAlias" 0 "uninstall preserves another installation's alias"
  StrCpy $MockMsiCommand ""
  Call TestUnRegisterExtension
  !insertmacro Check "$RemovedMsiAlias" 0 "missing alias is left alone"
!macroend

!macro CheckNoMsiCompatibility EXT
  Call ResetCase
  StrCpy $R0 "${EXT}"
  StrCpy $R1 "Video"
  Call TestRegisterExtension
  !insertmacro Check "$RegisteredCapabilities" "VLC${EXT}" "other extensions keep native capabilities"
  !insertmacro Check "$RegisteredWrites" 8 "other extensions receive no compatibility writes"
  !insertmacro Check "$RegisteredMsiValues" "" "extensions outside the shared inventory receive no aliases"
  StrCpy $MockMsiCommand '$\"$INSTDIR\vlc.exe$\" --started-from-file $\"%1$\"'
  Call TestUnRegisterExtension
  !insertmacro Check "$RemovedMsiAlias" 0 "other MSI identifiers are not removed"
!macroend

Function ResetCase
  SetSilent normal
  StrCpy $AssociationExtensions ""
  StrCpy $AssociationExtensionsOverflow 0
  StrCpy $AssociationSkipDefaults 0
  StrCpy $AssociationCommandAttempted 0
  StrCpy $Launches 0
  StrCpy $MockExit 0
  StrCpy $LaunchFailure 0
  StrCpy $MockPathError 0
  StrCpy $RegistrationFailure 0
  StrCpy $MockElevated 1
  StrCpy $MockDetail ""
  StrCpy $MockCommand ""
  StrCpy $RegisteredExtension ""
  StrCpy $RegisteredCapabilities ""
  StrCpy $RegisteredWrites 0
  StrCpy $RegisteredMsiValues ""
  StrCpy $RemovedMsiAlias 0
  StrCpy $MockMsiCommand ""
  StrCpy $INSTDIR "C:\Program Files\VideoLAN\Ω VLC"
  ClearErrors
FunctionEnd

Function TryCommand
  StrCpy $0 "preserve zero"
  StrCpy $1 "preserve one"
  SetErrors
  Call RunAssociationCommand
  ${If} ${Errors}
    FileWrite $Report "FAIL: optional command leaked an error flag$\r$\n"
    FileClose $Report
    SetErrorLevel 1
    Quit
  ${EndIf}
  !insertmacro Check "$0" "preserve zero" "scratch register 0"
  !insertmacro Check "$1" "preserve one" "scratch register 1"
FunctionEnd

; Real checkbox macros only collect selection. MockAssociateExtension is a
; regression trap: any reintroduced machine-default callback must fail safely.
SectionGroup "Audio"
  !insertmacro MacroAudioExtensions AssociateExtensionSection
SectionGroupEnd
SectionGroup "Video"
  !insertmacro MacroVideoExtensions AssociateExtensionSection
SectionGroupEnd
SectionGroup "Other"
  !insertmacro MacroOtherExtensions AssociateExtensionSection
  !insertmacro MacroSkinExtensions AssociateSkinExtensionSection
  !insertmacro MacroUnassociatedExtensions AssociateExtensionUnselectedSection
SectionGroupEnd
${MementoSectionDone}

Section -AssertSelection
  !insertmacro Check "$LegacyCalls" 0 "checkboxes must never assign machine-wide defaults"
  !insertmacro Check "$AssociationExtensions" "$ExpectedExtensions" "only selected sections collected in UI order"
  SetSilent normal
  Call TryCommand
  SetSilent silent
  !insertmacro Check "$Launches" 1 "one command for selected sections"
  !insertmacro Check "$MockCommand" '$\"$INSTDIR\vlc.exe$\" --win32-set-default-associations-once=$ExpectedExtensions' "exact quoted selection command"
SectionEnd

Function .OnInstSuccess
  ; A repeated callback must not execute the optional operation again.
  SetSilent normal
  Call TryCommand
  SetSilent silent
  !insertmacro Check "$Launches" 1 "single attempt across repeated callbacks"
  FileWrite $Report "$Checks checks passed$\r$\n"
  FileClose $Report
  SetErrorLevel 0
FunctionEnd

Function .onInit
  !ifdef VLC_64BIT_INSTALLER
    SetRegView 64
  !endif
  FileOpen $Report "$EXEDIR\association-installer-results.txt" w
  StrCpy $Checks 0

  ; This one check uses the real read-only token query; RunAssociationCommand's
  ; call is mocked so the fixture never needs administrator privileges.
  Call AssociationCommandIsElevated
  ${If} $0 != 0
  ${AndIf} $0 != 1
    SetErrorLevel 1
    Quit
  ${EndIf}
  IntOp $Checks $Checks + 1

  StrCpy $MarkerRead 0
  StrCpy $MarkerAt 0
  Call InitAssociationCommand
  !insertmacro Check "$AssociationSkipDefaults" 0 "fresh install detection"
  ${If} ${RunningX64}
    !insertmacro Check "$MarkerRead" 6 "both registry views checked"
  ${Else}
    !insertmacro Check "$MarkerRead" 3 "native registry view checked"
  ${EndIf}
  StrCpy $MarkerRead 0
  StrCpy $MarkerAt 1
  Call InitAssociationCommand
  !insertmacro Check "$AssociationSkipDefaults" 1 "empty but existing install marker prevents reclaim"
  ${If} ${RunningX64}
    StrCpy $MarkerRead 0
    StrCpy $MarkerAt 4
    Call InitAssociationCommand
    !insertmacro Check "$AssociationSkipDefaults" 1 "other architecture prevents reclaim"
  ${EndIf}

  StrCpy $PreviousVersion ""
  StrCpy $PreviousVersionState "preserve"
  Call AllowAssociationUpgrade
  !insertmacro Check "$AssociationSkipDefaults" 1 "marker-only MSI migration remains excluded"
  StrCpy $PreviousVersion "5.0.0"
  Call AllowAssociationUpgrade
  !insertmacro Check "$AssociationSkipDefaults" 1 "downgrade remains excluded"
  StrCpy $PreviousVersion "3.0.24"
  Call AllowAssociationUpgrade
  !insertmacro Check "$AssociationSkipDefaults" 0 "recognized NSIS upgrade becomes eligible"
  !insertmacro Check "$PreviousVersionState" "preserve" "upgrade eligibility preserves native detection state"
  StrCpy $AssociationSkipDefaults 1
  StrCpy $PreviousVersion "4.0.0"
  Call AllowAssociationUpgrade
  !insertmacro Check "$AssociationSkipDefaults" 0 "same-version NSIS install uses historical success gate"

  ; Mirror production order: the other architecture is examined first, then
  ; the main target view. Its own markers must not inherit the earlier allow.
  StrCpy $AssociationSkipDefaults 1
  StrCpy $MarkerRead 0
  StrCpy $MarkerAt 1
  StrCpy $PreviousVersion "3.0.24"
  Call AllowAssociationUpgrade
  !insertmacro Check "$AssociationSkipDefaults" 0 "recognized other-view predecessor permits an upgrade"
  StrCpy $MarkerRead 0
  StrCpy $MarkerAt 1
  StrCpy $PreviousVersion "5.0.0"
  Call AllowAssociationUpgrade
  !insertmacro Check "$AssociationSkipDefaults" 1 "main-view downgrade cannot inherit other-view eligibility"

  StrCpy $AssociationSkipDefaults 0
  StrCpy $MarkerRead 0
  StrCpy $MarkerAt 1
  StrCpy $PreviousVersion ""
  Call AllowAssociationUpgrade
  !insertmacro Check "$AssociationSkipDefaults" 1 "unrecognized main-view install cannot inherit earlier eligibility"
  StrCpy $AssociationSkipDefaults 0
  StrCpy $MarkerRead 0
  StrCpy $MarkerAt 0
  Call AllowAssociationUpgrade
  !insertmacro Check "$AssociationSkipDefaults" 0 "empty main view preserves recognized architecture upgrade"
  StrCpy $MarkerRead 0
  StrCpy $MarkerAt 1
  StrCpy $PreviousVersion "3.0.24"
  Call AllowAssociationUpgrade
  !insertmacro Check "$AssociationSkipDefaults" 0 "recognized main-view upgrade remains eligible"
  !insertmacro Check "$PreviousVersionState" "preserve" "real comparator preserves native page state"

  Call ResetCase
  StrCpy $0 "unrelated scratch value"
  StrCpy $R0 ".mp3"
  StrCpy $R1 "Audio"
  Call TestRegisterExtension
  !insertmacro Check "$RegisteredExtension" ".mp3" "SupportedTypes uses the extension, not scratch register zero"

  Call ResetCase
  StrCpy $R0 ".vlt"
  Call TestRegisterSkinExtension
  !insertmacro Check "$RegisteredCapabilities" "VLC.vlt" "skin capability keeps its handler"
  !insertmacro Check "$RegisteredWrites" 6 "skin metadata remains unchanged in machine classes"

  !insertmacro CheckAllMsiCompatibility

  Call ResetCase
  StrCpy $MockCopyError 0
  SetErrors
  !insertmacro TestCopyGuard0
  !insertmacro Check "$AssociationSkipDefaults" 0 "successful extraction clears unrelated stale errors"
  StrCpy $MockCopyError 1
  !insertmacro TestCopyGuard0
  ClearErrors
  StrCpy $AssociationExtensions ".mp3"
  Call TryCommand
  !insertmacro Check "$AssociationSkipDefaults" 1 "copy failure stays latched after later error clearing"
  !insertmacro Check "$Launches" 0 "ignored extraction failure cannot launch stale executable"

  Call ResetCase
  StrCpy $MockCopyError 0
  SetErrors
  !insertmacro TestCopyGuard1
  !insertmacro Check "$AssociationSkipDefaults" 0 "successful extraction clears unrelated stale errors"
  StrCpy $MockCopyError 1
  !insertmacro TestCopyGuard1
  ClearErrors
  StrCpy $AssociationExtensions ".mp3"
  Call TryCommand
  !insertmacro Check "$AssociationSkipDefaults" 1 "copy failure stays latched after later error clearing"
  !insertmacro Check "$Launches" 0 "ignored extraction failure cannot launch stale executable"

  Call ResetCase
  StrCpy $MockCopyError 0
  SetErrors
  !insertmacro TestCopyGuard2
  !insertmacro Check "$AssociationSkipDefaults" 0 "successful extraction clears unrelated stale errors"
  StrCpy $MockCopyError 1
  !insertmacro TestCopyGuard2
  ClearErrors
  StrCpy $AssociationExtensions ".mp3"
  Call TryCommand
  !insertmacro Check "$AssociationSkipDefaults" 1 "copy failure stays latched after later error clearing"
  !insertmacro Check "$Launches" 0 "ignored extraction failure cannot launch stale executable"

  Call ResetCase
  StrCpy $MockCopyError 0
  StrCpy $MockPathError 1
  !insertmacro TestCopyGuard1
  ClearErrors
  StrCpy $AssociationExtensions ".mp3"
  Call TryCommand
  !insertmacro Check "$AssociationSkipDefaults" 1 "failed output directory also suppresses defaults"
  !insertmacro Check "$Launches" 0 "folder setup failure cannot reach the command"

  Call ResetCase
  StrCpy $MockCopyError 0
  StrCpy $MockPathError 1
  !insertmacro TestCopyGuard2
  ClearErrors
  StrCpy $AssociationExtensions ".mp3"
  Call TryCommand
  !insertmacro Check "$AssociationSkipDefaults" 1 "failed output directory also suppresses defaults"
  !insertmacro Check "$Launches" 0 "folder setup failure cannot reach the command"

  Call ResetCase
  StrCpy $R0 ".MP3"
  Call AppendAssociationExtension
  StrCpy $R0 ".iso"
  Call AppendAssociationExtension
  Call AppendAssociationExtension
  !insertmacro Check "$AssociationExtensions" ".MP3,.iso,.iso" "case, order and duplicates preserved"
  Call TryCommand
  !insertmacro Check "$Launches" 1 "fresh interactive install launches once"
  !insertmacro Check "$MockCommand" '$\"$INSTDIR\vlc.exe$\" --win32-set-default-associations-once=.MP3,.iso,.iso' "unicode path and exact payload"
  Call TryCommand
  !insertmacro Check "$Launches" 1 "no second attempt"

  Call ResetCase
  Call TryCommand
  !insertmacro Check "$Launches" 0 "empty selection skips command"
  Call ResetCase
  StrCpy $AssociationExtensions ".mp3"
  SetSilent silent
  Call TryCommand
  !insertmacro Check "$Launches" 0 "silent install never reclaims defaults"
  Call ResetCase
  StrCpy $AssociationExtensions ".mp3"
  StrCpy $AssociationSkipDefaults 1
  Call TryCommand
  !insertmacro Check "$Launches" 0 "latched update remains skipped"

  StrCpy $9 0
status_loop:
  Call ResetCase
  StrCpy $AssociationExtensions ".mp3"
  StrCpy $MockExit $9
  Call TryCommand
  !insertmacro Check "$Launches" 1 "every status is one nonfatal attempt"
  ${If} $9 == 0
    !insertmacro Check "$MockDetail" "Installer default file associations completed or previously recorded; no further changes requested." "success diagnostic"
  ${Else}
    ${If} $MockDetail == "Installer default file associations completed or previously recorded; no further changes requested."
      FileWrite $Report "FAIL: failure reported as success$\r$\n"
      FileClose $Report
      SetErrorLevel 1
      Quit
    ${EndIf}
    IntOp $Checks $Checks + 1
  ${EndIf}
  ${If} $9 == 1
    !insertmacro Check "$MockDetail" "Default file associations incomplete or completion unconfirmed; a worker may still be running. No retry." "unconfirmed diagnostic"
  ${EndIf}
  Call TryCommand
  !insertmacro Check "$Launches" 1 "failure never retries"
  IntOp $9 $9 + 1
  IntCmp $9 6 status_loop status_loop

  Call ResetCase
  StrCpy $AssociationExtensions ".mp3"
  StrCpy $MockExit 3221225477
  Call TryCommand
  !insertmacro Check "$Launches" 1 "crashed command remains nonfatal"
  Call ResetCase
  StrCpy $AssociationExtensions ".mp3"
  StrCpy $LaunchFailure 1
  Call TryCommand
  !insertmacro Check "$Launches" 1 "launch failure never retries"
  !insertmacro Check "$MockDetail" "Default file associations could not be started; VLC installation completed. No retry." "ignore undefined launch-error output"

  Call ResetCase
  StrCpy $AssociationExtensions ".mp3"
  StrCpy $MockElevated 0
  Call TryCommand
  !insertmacro Check "$Launches" 0 "no synchronous unelevated fallback"
  StrCpy $9 1
registration_loop:
  Call ResetCase
  StrCpy $AssociationExtensions ".mp3"
  StrCpy $RegistrationFailure $9
  Call TryCommand
  !insertmacro Check "$Launches" 0 "missing executable or mismatched registration skips command"
  IntOp $9 $9 + 1
  IntCmp $9 5 registration_loop registration_loop

  Call ResetCase
  StrCpy $R0 ".a"
  StrCpy $9 0
overflow_loop:
  Call AppendAssociationExtension
  IntOp $9 $9 + 1
  IntCmp $9 350 overflow_loop overflow_loop
  !insertmacro Check "$AssociationExtensionsOverflow" 1 "list overflow is latched"
  !insertmacro Check "$AssociationExtensions" "" "overflow never keeps a partial batch"
  Call TryCommand
  !insertmacro Check "$Launches" 0 "overlong list skips the entire operation"

  Call ResetCase
  StrCpy $AssociationExtensions ".mp3"
  StrCpy $INSTDIR ""
  StrLen $9 '$\"\vlc.exe$\" --win32-set-default-associations-once=.mp3'
  IntOp $9 ${NSIS_MAX_STRLEN} - $9
  IntOp $9 $9 - 1
path_loop:
  StrCpy $INSTDIR "$INSTDIR\x"
  StrLen $8 $INSTDIR
  IntCmp $8 $9 path_ready path_loop path_ready
path_ready:
  ; The two-character increments can overshoot. Trim to the exact boundary.
  StrCpy $INSTDIR $INSTDIR $9
  Call TryCommand
  !insertmacro Check "$Launches" 1 "maximum complete command fits"
  StrCpy $AssociationCommandAttempted 0
  StrCpy $Launches 0
  StrCpy $INSTDIR "$INSTDIR\x"
  Call TryCommand
  !insertmacro Check "$Launches" 0 "overlong complete command cannot truncate"

  ; Run actual section selection once after unit cases. Default/unselected/skin
  ; sections keep their real headers, flags and SectionIn behavior.
  Call ResetCase
  StrCpy $LegacyCalls 0
  StrCpy $9 0
clear_sections:
  SectionGetFlags $9 $8
  IntOp $8 $8 & 0xFFFFFFFE
  SectionSetFlags $9 $8
  IntOp $9 $9 + 1
  IntCmp $9 ${__ASSOCIATION_SECTION_COUNT} select_sections clear_sections
select_sections:
  ${GetParameters} $8
  ${If} $8 == "/all"
    StrCpy $ExpectedExtensions "${__ASSOCIATION_ALL_EXTENSIONS}"
    StrCpy $9 0
select_all:
    SectionGetFlags $9 $8
    IntOp $8 $8 | ${SF_SELECTED}
    SectionSetFlags $9 $8
    IntOp $9 $9 + 1
    IntCmp $9 ${__ASSOCIATION_SECTION_COUNT} selected_all select_all
selected_all:
  ${Else}
    StrCpy $ExpectedExtensions ".mp3,.vlt,.iso"
    SectionSetFlags ${SEC_EXT_Audio_.mp3} ${SF_SELECTED}
    SectionSetFlags ${SEC_EXT_SKIN_.vlt} ${SF_SELECTED}
    SectionSetFlags ${SEC_EXT_Other_.iso} ${SF_SELECTED}
  ${EndIf}
  SetSilent silent
FunctionEnd
