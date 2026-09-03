!include "MUI2.nsh"
!include "LogicLib.nsh"

Name "Motion CLI"
OutFile "..\build\MotionCLI-Installer.exe"
InstallDir "$LOCALAPPDATA\Programs\MotionCLI"
RequestExecutionLevel user

!define MUI_ICON "..\resources\app.ico"
!define MUI_UNICON "..\resources\app.ico"

!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!define MUI_FINISHPAGE_RUN "$INSTDIR\motioncli.exe"
!define MUI_FINISHPAGE_RUN_TEXT "Launch Motion CLI now"
!insertmacro MUI_PAGE_FINISH

!insertmacro MUI_UNPAGE_WELCOME
!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES
!insertmacro MUI_UNPAGE_FINISH

!insertmacro MUI_LANGUAGE "English"

Section "Install"
  SetOutPath "$INSTDIR"

  DetailPrint "Fetching latest release from version.json..."
  nsExec::ExecToStack 'powershell -NoProfile -ExecutionPolicy Bypass -Command "$v = Invoke-RestMethod -Uri \"https://raw.githubusercontent.com/RealTenzo/motioncli/main/version.json\"; Write-Output $v.download_url"'
  Pop $0 ; status
  Pop $1 ; url output

  ; Clean URL string
  StrCpy $1 $1 -2

  ${If} $1 == ""
    StrCpy $1 "https://github.com/RealTenzo/motioncli/releases/latest/download/motioncli_portable.exe"
  ${EndIf}

  DetailPrint "Downloading latest Motion CLI..."
  nsExec::ExecToLog 'powershell -NoProfile -ExecutionPolicy Bypass -Command "[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12; Invoke-WebRequest -Uri \"$1\" -OutFile \"$INSTDIR\motioncli.exe\""'

  CreateShortcut "$SMPROGRAMS\Motion CLI.lnk" "$INSTDIR\motioncli.exe"
  CreateShortcut "$DESKTOP\Motion CLI.lnk" "$INSTDIR\motioncli.exe"

  ; Add install dir to User PATH
  ReadRegStr $2 HKCU "Environment" "Path"
  ${If} $2 == ""
    WriteRegStr HKCU "Environment" "Path" "$INSTDIR"
  ${Else}
    ; Check if path already exists
    Push $2
    Push "$INSTDIR"
    ; Add if not present
    WriteRegStr HKCU "Environment" "Path" "$2;$INSTDIR"
  ${EndIf}
  SendMessage 0xffff 0x001A 0 "STR:Environment" /TIMEOUT=1000

  WriteUninstaller "$INSTDIR\uninstall.exe"
  WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\Motion CLI" "DisplayName" "Motion CLI"
  WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\Motion CLI" "DisplayIcon" "$INSTDIR\motioncli.exe,0"
  WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\Motion CLI" "UninstallString" '"$INSTDIR\uninstall.exe"'
  WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\Motion CLI" "Publisher" "tenzo"
SectionEnd

Section "Uninstall"
  Delete "$INSTDIR\motioncli.exe"
  Delete "$INSTDIR\uninstall.exe"
  Delete "$SMPROGRAMS\Motion CLI.lnk"
  Delete "$DESKTOP\Motion CLI.lnk"
  RMDir "$INSTDIR"
  DeleteRegKey HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\Motion CLI"
SectionEnd
