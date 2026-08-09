!include "MUI2.nsh"

Name "Motion CLI"
OutFile "..\build\MotionCLI-Installer.exe"
InstallDir "$PROGRAMFILES\Motion CLI"
RequestExecutionLevel admin

!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH

!insertmacro MUI_UNPAGE_WELCOME
!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES
!insertmacro MUI_UNPAGE_FINISH

!insertmacro MUI_LANGUAGE "English"

Section "Install"
  SetOutPath "$INSTDIR"
  File "..\build\Release\motioncli.exe"
  CreateShortcut "$SMPROGRAMS\Motion CLI.lnk" "$INSTDIR\motioncli.exe"
  CreateShortcut "$DESKTOP\Motion CLI.lnk" "$INSTDIR\motioncli.exe"
  
  WriteUninstaller "$INSTDIR\uninstall.exe"
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\Motion CLI" "DisplayName" "Motion CLI"
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\Motion CLI" "UninstallString" '"$INSTDIR\uninstall.exe"'
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\Motion CLI" "Publisher" "tenzo"
SectionEnd

Section "Uninstall"
  Delete "$INSTDIR\motioncli.exe"
  Delete "$INSTDIR\uninstall.exe"
  Delete "$SMPROGRAMS\Motion CLI.lnk"
  Delete "$DESKTOP\Motion CLI.lnk"
  RMDir "$INSTDIR"
  DeleteRegKey HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\Motion CLI"
SectionEnd
