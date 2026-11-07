; Copyright (C) 2026 Vinit Rajpurohit (spidey889)
; SPDX-License-Identifier: GPL-2.0-or-later

!include LogicLib.nsh
!include x64.nsh

Var AssociationExtensions
Var AssociationExtensionsOverflow
Var AssociationSkipDefaults
Var AssociationCommandAttempted

; Latch any prior install before registration or upgrade code changes its values.
; Read both views, including architecture migrations and incomplete old installs.
Function CheckAssociationUpgrade
  Push $0
  ClearErrors
  ReadRegStr $0 HKLM "${PRODUCT_DIR_REGKEY}" "InstallDir"
  ${IfNot} ${Errors}
    StrCpy $AssociationSkipDefaults 1
  ${EndIf}
  ClearErrors
  ReadRegStr $0 HKLM "${PRODUCT_DIR_REGKEY}" "Version"
  ${IfNot} ${Errors}
    StrCpy $AssociationSkipDefaults 1
  ${EndIf}
  ClearErrors
  ReadRegStr $0 ${PRODUCT_UNINST_ROOT_KEY} "${PRODUCT_UNINST_KEY}" "UninstallString"
  ${IfNot} ${Errors}
    StrCpy $AssociationSkipDefaults 1
  ${EndIf}
  Pop $0
  ClearErrors
FunctionEnd

Function InitAssociationCommand
  StrCpy $AssociationExtensions ""
  StrCpy $AssociationExtensionsOverflow 0
  StrCpy $AssociationSkipDefaults 0
  StrCpy $AssociationCommandAttempted 0
  Call CheckAssociationUpgrade
  ${If} ${RunningX64}
    !ifdef VLC_64BIT_INSTALLER
      SetRegView 32
    !else
      SetRegView 64
    !endif
    Call CheckAssociationUpgrade
    SetRegView lastused
  ${EndIf}
FunctionEnd

; The existing installer recognizes NSIS predecessors through InstallDir/Version.
; Lift only the initial prior-install skip for a recognized non-downgrade.
; Marker-only/MSI predecessors stay skipped; packaging behavior is unchanged.
Function AllowAssociationUpgrade
  ; Detection visits the other architecture before the main target view. Latch
  ; this view again so an earlier allow cannot override its own downgrade or
  ; unrecognized predecessor. This changes only optional association eligibility.
  Call CheckAssociationUpgrade
  Push $PreviousVersionState
  ${If} $PreviousVersion != ""
    Push "${VERSION}"
    Push $PreviousVersion
    Call VersionCompare
    ${If} $PreviousVersionState == "newer"
    ${OrIf} $PreviousVersionState == "same"
      StrCpy $AssociationSkipDefaults 0
    ${EndIf}
  ${EndIf}
  Pop $PreviousVersionState
FunctionEnd

; Called only by executed checkbox sections, with their existing extension in R0.
; Reject the entire optional batch on overflow rather than applying a truncated list.
Function AppendAssociationExtension
  Push $0
  Push $1
  ${If} $AssociationExtensionsOverflow != 1
    StrLen $0 $AssociationExtensions
    StrLen $1 $R0
    IntOp $0 $0 + $1
    ${If} $AssociationExtensions != ""
      IntOp $0 $0 + 1
    ${EndIf}
    ${If} $0 >= ${NSIS_MAX_STRLEN}
      StrCpy $AssociationExtensionsOverflow 1
      StrCpy $AssociationExtensions ""
    ${ElseIf} $AssociationExtensions == ""
      StrCpy $AssociationExtensions $R0
    ${Else}
      StrCpy $AssociationExtensions "$AssociationExtensions,$R0"
    ${EndIf}
  ${EndIf}
  Pop $1
  Pop $0
FunctionEnd

; RequestExecutionLevel admin normally guarantees this. Verify it so an unusual
; unelevated launch cannot fall into vlc.exe's synchronous ordinary-user path.
; The command and worker still perform their independent SID/session checks.
Function AssociationCommandIsElevated
  Push $1
  Push $2
  Push $3
  Push $4
  StrCpy $0 0
  System::Call 'kernel32::GetCurrentProcess() p .r1'
  System::Call 'advapi32::OpenProcessToken(p r1, i 0x0008, *p .r2) i .r3'
  ${If} $3 != 0
    StrCpy $3 0
    StrCpy $4 0
    System::Call 'advapi32::GetTokenInformation(p r2, i 20, *i .r3, i 4, *i .r4) i .r1'
    System::Call 'kernel32::CloseHandle(p r2)'
    ${If} $1 != 0
    ${AndIf} $3 == 1
    ${AndIf} $4 == 4
      StrCpy $0 1
    ${EndIf}
  ${EndIf}
  Pop $4
  Pop $3
  Pop $2
  Pop $1
FunctionEnd

; Optional defaults run only after successful sections, including -Post. Every
; failure is nonfatal: code 1 can mean a live worker, so never retry or clean it up.
Function RunAssociationCommand
  Push $0
  Push $1
  ${If} $AssociationCommandAttempted == 1
    Goto association_done
  ${EndIf}
  StrCpy $AssociationCommandAttempted 1
  IfSilent association_done
  ${If} $AssociationSkipDefaults == 1
    Goto association_done
  ${EndIf}
  ${If} $AssociationExtensionsOverflow == 1
    Goto association_too_long
  ${EndIf}
  ${If} $AssociationExtensions == ""
    Goto association_done
  ${EndIf}

  ; Check length before expansion into any NSIS string (including ExecWait).
  StrLen $0 $INSTDIR
  StrLen $1 '"\vlc.exe" --win32-set-default-associations-once='
  IntOp $0 $0 + $1
  StrLen $1 $AssociationExtensions
  IntOp $0 $0 + $1
  ${If} $0 >= ${NSIS_MAX_STRLEN}
    Goto association_too_long
  ${EndIf}

  IfFileExists "$INSTDIR\vlc.exe" 0 association_registration_failed
  ClearErrors
  ReadRegStr $0 HKLM "${PRODUCT_DIR_REGKEY}" ""
  ${If} ${Errors}
  ${OrIf} $0 != "$INSTDIR\vlc.exe"
    Goto association_registration_failed
  ${EndIf}
  ClearErrors
  ReadRegStr $0 HKLM "Software\Microsoft\Windows\CurrentVersion\App Paths\vlc.exe" ""
  ${If} ${Errors}
  ${OrIf} $0 != "$INSTDIR\vlc.exe"
    Goto association_registration_failed
  ${EndIf}
  Call AssociationCommandIsElevated
  ${If} $0 != 1
    DetailPrint "Default file associations skipped: elevated installer context unavailable."
    Goto association_done
  ${EndIf}

  ClearErrors
  ExecWait '"$INSTDIR\vlc.exe" --win32-set-default-associations-once=$AssociationExtensions' $1
  ; A launch error leaves $1 undefined; check the flag before the exit code.
  ${If} ${Errors}
    DetailPrint "Default file associations could not be started; VLC installation completed. No retry."
  ${ElseIf} $1 == 0
    DetailPrint "Installer default file associations completed or previously recorded; no further changes requested."
  ${ElseIf} $1 == 1
    DetailPrint "Default file associations incomplete or completion unconfirmed; a worker may still be running. No retry."
  ${ElseIf} $1 == 2
    DetailPrint "Default file associations skipped: invalid request. No retry."
  ${ElseIf} $1 == 3
    DetailPrint "Default file associations skipped: intended user could not be verified. No retry."
  ${ElseIf} $1 == 4
    DetailPrint "Default file associations incomplete: writer busy; earlier changes may have completed. No retry."
  ${Else}
    DetailPrint "Default file associations failed (exit $1); VLC installation completed. No retry."
  ${EndIf}
  Goto association_done

association_registration_failed:
  DetailPrint "Default file associations skipped: installed executable registration could not be verified."
  Goto association_done
association_too_long:
  DetailPrint "Default file associations skipped: complete request exceeds the installer string limit."
association_done:
  ; Optional association work must not leave an installer failure flag behind.
  ClearErrors
  Pop $1
  Pop $0
FunctionEnd
