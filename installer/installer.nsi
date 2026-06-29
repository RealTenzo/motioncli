; Motion CLI Installer
; Requires NSIS 3.0+ https://nsis.sourceforge.io

!include "MUI2.nsh"
!include "FileFunc.nsh"
!include "LogicLib.nsh"

Name "Motion CLI"
OutFile "motioncli-setup.exe"
InstallDir "$LOCALAPPDATA\MotionCLI"
RequestExecutionLevel user

Var StartMenuGroup

!define MUI_ICON "..\resources\app.ico"
!define MUI_UNICON "..\resources\app.ico"
!define MUI_ABORTWARNING
!define MUI_WELCOMEPAGE_TITLE "Motion CLI Setup"
!define MUI_WELCOMEPAGE_TEXT "This wizard will install Motion CLI on your computer.$\r$\n$\r$\nMotion CLI is a super-lightweight live wallpaper engine for Windows.$\r$\nNo Electron. No browser. Just Media Foundation.$\r$\n$\r$\nClick Next to continue."

!define MUI_FINISHPAGE_RUN "$INSTDIR\motioncli.exe"
!define MUI_FINISHPAGE_RUN_TEXT "Launch Motion CLI"
!define MUI_FINISHPAGE_LINK "Visit GitHub"
!define MUI_FINISHPAGE_LINK_LOCATION "https://github.com/tenzo/motioncli"

!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_LICENSE "..\LICENSE"
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH

!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES

!insertmacro MUI_LANGUAGE "English"

Section "Motion CLI" SecMain
    SetOutPath "$INSTDIR"

    File "..\build\Release\motioncli.exe"
    File "..\LICENSE"
    File "..\motion_logo.png"

    WriteRegStr HKCU "Software\MotionCLI" "InstallDir" "$INSTDIR"
    WriteRegStr HKCU "Software\MotionCLI" "Version" "1.0.0"

    ${GetSize} "$INSTDIR" "/S=0K" $0 $1 $2
    IntFmt $0 "0x%08X" $0

    WriteUninstaller "$INSTDIR\uninstall.exe"

    CreateDirectory "$SMPROGRAMS\$StartMenuGroup"
    CreateShortCut "$SMPROGRAMS\$StartMenuGroup\Motion CLI.lnk" "$INSTDIR\motioncli.exe"
    CreateShortCut "$SMPROGRAMS\$StartMenuGroup\Uninstall Motion CLI.lnk" "$INSTDIR\uninstall.exe"

    WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\MotionCLI" \
        "DisplayName" "Motion CLI"
    WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\MotionCLI" \
        "UninstallString" '"$INSTDIR\uninstall.exe"'
    WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\MotionCLI" \
        "InstallLocation" "$INSTDIR"
    WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\MotionCLI" \
        "DisplayVersion" "1.0.0"
    WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\MotionCLI" \
        "Publisher" "tenzo"
    WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\MotionCLI" \
        "URLInfoAbout" "https://github.com/tenzo/motioncli"
    WriteRegDWORD HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\MotionCLI" \
        "NoModify" 1
    WriteRegDWORD HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\MotionCLI" \
        "NoRepair" 1
SectionEnd

Section "Start on Login" SecAutostart
    WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Run" \
        "MotionCLI" '"$INSTDIR\motioncli.exe" --startup'
SectionEnd

Function .onInit
    StrCpy $StartMenuGroup "Motion CLI"
FunctionEnd

Section "Uninstall"
    DeleteRegValue HKCU "Software\Microsoft\Windows\CurrentVersion\Run" "MotionCLI"
    DeleteRegKey HKCU "Software\MotionCLI"
    DeleteRegKey HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\MotionCLI"

    Delete "$INSTDIR\motioncli.exe"
    Delete "$INSTDIR\LICENSE"
    Delete "$INSTDIR\motion_logo.png"
    Delete "$INSTDIR\uninstall.exe"
    RMDir "$INSTDIR"

    Delete "$SMPROGRAMS\$StartMenuGroup\Motion CLI.lnk"
    Delete "$SMPROGRAMS\$StartMenuGroup\Uninstall Motion CLI.lnk"
    RMDir "$SMPROGRAMS\$StartMenuGroup"
SectionEnd
