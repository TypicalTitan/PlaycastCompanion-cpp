; Playcast Companion (native build) - NSIS installer (personal use).
;
; Behavior:
;   - installs to %ProgramFiles%\PlaycastCompanion (admin-writable only, so the
;     guest account cannot tamper with the binary or its config)
;   - same APP_ID / install dir / Run key / uninstall key as the .NET build, so
;     this setup upgrades the .NET version IN PLACE and keeps its config.json
;   - enables autostart for ALL accounts via HKLM Run - including the guest
;     session, where the app runs headless
;   - registers in Settings > Apps with a real uninstall.exe
;   - preserves an existing config.json across reinstalls/upgrades
;   - migrates automatically from the old "Chroma Blackout" install (keeps its
;     config, removes its autostart/app entry/folder)
;   - uninstall is guarded: it only deletes a folder that actually contains
;     PlaycastCompanion.exe
;
; Build: makensis installer.nsi   (payload\ must exist - see build-setup.cmd)
; Supports silent runs: PlaycastCompanionSetup-x.y.z.exe /S  and  uninstall.exe /S

!include "MUI2.nsh"
!include "FileFunc.nsh"

!define APP_NAME      "Playcast Companion"
!define APP_ID        "PlaycastCompanion"
!define APP_VERSION   "3.2.2"
!define APP_PUBLISHER "TypicalTitan"
!define APP_EXE       "PlaycastCompanion.exe"
!define OLD_ID        "ChromaBlackout"
!define OLD_EXE       "ChromaBlackout.exe"
!define UNINST_KEY    "Software\Microsoft\Windows\CurrentVersion\Uninstall\${APP_ID}"
!define OLD_UNINST_KEY "Software\Microsoft\Windows\CurrentVersion\Uninstall\${OLD_ID}"
!define RUN_KEY       "Software\Microsoft\Windows\CurrentVersion\Run"

Name "${APP_NAME}"
OutFile "PlaycastCompanionSetup-${APP_VERSION}.exe"
Unicode true
ManifestDPIAware true
RequestExecutionLevel admin        ; one UAC prompt; the installed app runs unelevated
InstallDir "$PROGRAMFILES64\${APP_ID}"   ; fixed path by design - no directory page
SetCompressor /SOLID lzma

VIProductVersion "${APP_VERSION}.0"
VIAddVersionKey "ProductName" "${APP_NAME}"
VIAddVersionKey "FileDescription" "${APP_NAME} Setup"
VIAddVersionKey "FileVersion" "${APP_VERSION}"
VIAddVersionKey "ProductVersion" "${APP_VERSION}"
VIAddVersionKey "CompanyName" "${APP_PUBLISHER}"
VIAddVersionKey "LegalCopyright" "MIT License - github.com/TypicalTitan/PlaycastCompanion-cpp"

; --- UI ----------------------------------------------------------------------
!define MUI_ICON "${NSISDIR}\Contrib\Graphics\Icons\modern-install.ico"
!define MUI_UNICON "${NSISDIR}\Contrib\Graphics\Icons\modern-uninstall.ico"
!define MUI_WELCOMEPAGE_TITLE "${APP_NAME} ${APP_VERSION}"
!define MUI_WELCOMEPAGE_TEXT "Your PC's stealth sidekick for Playcast hosting - native build, no .NET runtime needed.$\r$\n$\r$\nWhile a guest is signed in: every Razer light goes dark and (optionally) your Discord shows what's happening. When they leave, everything snaps back to normal.$\r$\n$\r$\nInstalls to Program Files and starts automatically for every account. An existing Playcast Companion (.NET) or Chroma Blackout install is upgraded in place and its settings carried over.$\r$\n$\r$\nClick Install to continue."
!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_INSTFILES
!define MUI_FINISHPAGE_RUN
!define MUI_FINISHPAGE_RUN_TEXT "Open ${APP_NAME} now"
!define MUI_FINISHPAGE_RUN_FUNCTION LaunchUnelevated
!insertmacro MUI_PAGE_FINISH

!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES

!insertmacro MUI_LANGUAGE "English"

; -----------------------------------------------------------------------------
Function .onInit
  SetRegView 64
FunctionEnd

Function un.onInit
  SetRegView 64
FunctionEnd

Function LaunchUnelevated
  ; explorer starts it with the normal shell token instead of the admin one
  Exec '"$WINDIR\explorer.exe" "$INSTDIR\${APP_EXE}"'
FunctionEnd

; -----------------------------------------------------------------------------
Section "Install"
  SetShellVarContext all

  ; stop running instances (new and old name) in every session
  nsExec::Exec 'taskkill /F /IM "${APP_EXE}"'
  Pop $0
  nsExec::Exec 'taskkill /F /IM "${OLD_EXE}"'
  Pop $0
  Sleep 700

  SetOutPath "$INSTDIR"

  ; migrate the old Chroma Blackout config before anything else, so the
  ; preserve-existing-config logic below picks it up
  IfFileExists "$INSTDIR\config.json" ConfigInPlace 0
  IfFileExists "$PROGRAMFILES64\${OLD_ID}\config.json" 0 ConfigInPlace
    CopyFiles /SILENT "$PROGRAMFILES64\${OLD_ID}\config.json" "$INSTDIR\config.json"
  ConfigInPlace:

  ; preserve an existing config.json across upgrades (incl. from the .NET build)
  IfFileExists "$INSTDIR\config.json" +2
    File "payload\config.json"
  File "payload\${APP_EXE}"

  ; remove the old Chroma Blackout install entirely
  IfFileExists "$PROGRAMFILES64\${OLD_ID}\${OLD_EXE}" 0 NoOldInstall
    RMDir /r "$PROGRAMFILES64\${OLD_ID}"
  NoOldInstall:
  DeleteRegValue HKLM "${RUN_KEY}" "${OLD_ID}"
  DeleteRegValue HKCU "${RUN_KEY}" "${OLD_ID}"
  DeleteRegKey HKLM "${OLD_UNINST_KEY}"
  Delete "$SMPROGRAMS\Chroma Blackout.lnk"

  ; harden ACLs: re-inherit Program Files defaults
  ; (admins/system: full control; standard users incl. the guest: read+execute)
  nsExec::Exec 'icacls "$INSTDIR" /reset /t /c'
  Pop $0

  ; autostart for every account (incl. the guest session); --autostart keeps
  ; the logon launch quiet, while manual launches open the settings window
  WriteRegStr HKLM "${RUN_KEY}" "${APP_ID}" '"$INSTDIR\${APP_EXE}" --autostart'

  ; Start Menu shortcut (all users); opens the settings window
  CreateShortCut "$SMPROGRAMS\${APP_NAME}.lnk" "$INSTDIR\${APP_EXE}"

  ; uninstaller + Settings > Apps entry
  WriteUninstaller "$INSTDIR\uninstall.exe"
  WriteRegStr HKLM "${UNINST_KEY}" "DisplayName" "${APP_NAME}"
  WriteRegStr HKLM "${UNINST_KEY}" "DisplayVersion" "${APP_VERSION}"
  WriteRegStr HKLM "${UNINST_KEY}" "Publisher" "${APP_PUBLISHER}"
  WriteRegStr HKLM "${UNINST_KEY}" "InstallLocation" "$INSTDIR"
  WriteRegStr HKLM "${UNINST_KEY}" "DisplayIcon" "$INSTDIR\${APP_EXE}"
  WriteRegStr HKLM "${UNINST_KEY}" "UninstallString" '"$INSTDIR\uninstall.exe"'
  WriteRegStr HKLM "${UNINST_KEY}" "QuietUninstallString" '"$INSTDIR\uninstall.exe" /S'
  WriteRegDWORD HKLM "${UNINST_KEY}" "NoModify" 1
  WriteRegDWORD HKLM "${UNINST_KEY}" "NoRepair" 1
  ${GetSize} "$INSTDIR" "/S=0K" $0 $1 $2
  IntFmt $0 "0x%08X" $0
  WriteRegDWORD HKLM "${UNINST_KEY}" "EstimatedSize" "$0"
SectionEnd

; -----------------------------------------------------------------------------
Section "Uninstall"
  ; stop running instances in every session
  nsExec::Exec 'taskkill /F /IM "${APP_EXE}"'
  Pop $0
  Sleep 700

  ; autostart + Apps-list cleanup (HKLM, plus any per-user HKCU leftover)
  DeleteRegValue HKLM "${RUN_KEY}" "${APP_ID}"
  DeleteRegValue HKCU "${RUN_KEY}" "${APP_ID}"
  DeleteRegKey HKLM "${UNINST_KEY}"

  ; Start Menu shortcut (all users)
  SetShellVarContext all
  Delete "$SMPROGRAMS\${APP_NAME}.lnk"

  ; guarded delete: only remove a folder that really is a Playcast Companion install
  IfFileExists "$INSTDIR\${APP_EXE}" 0 SkipDelete
    RMDir /r "$INSTDIR"
  SkipDelete:

  ; current user's log folder (other profiles' logs are left as-is)
  SetShellVarContext current
  RMDir /r "$LOCALAPPDATA\${APP_ID}"
SectionEnd
