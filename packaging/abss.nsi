; ABSS Windows installer (NSIS 3, Modern UI 2). Built by scripts\package.ps1:
;   makensis /DDISTDIR=<dist\ABSS> /DOUTFILE=<setup.exe> /DVERSION=<x.y.z> packaging\abss.nsi
; Every component is selected by default; the only question is the compilers' licence consent.
; Fully unattended (IT deployment):  ABSS-Setup-<v>.exe /S /ACCEPT_LICENSES  [/D=C:\path\to\ABSS]
; The installer asks for administrator rights once (Windows prompt). The optional "User-subroutine compilers"
; component asks for explicit consent to the vendors' licences, then detects/downloads/installs Microsoft C++ Build
; Tools and Intel Fortran inside this already-approved session (no further prompts).
Unicode true
Target amd64-unicode
!include "MUI2.nsh"
!include "LogicLib.nsh"
!include "x64.nsh"
!include "Sections.nsh"
!include "FileFunc.nsh"

!ifndef VERSION
  !define VERSION "0.1.0"
!endif
!ifndef DISTDIR
  !error "Pass /DDISTDIR=<path to dist\ABSS>"
!endif
!ifndef OUTFILE
  !define OUTFILE "ABSS-Setup-${VERSION}.exe"
!endif
!define FULLNAME "Abaqus Black-Box Sensitivity Studio"
!define UNINSTKEY "Software\Microsoft\Windows\CurrentVersion\Uninstall\ABSS"

Name "${FULLNAME} ${VERSION}"
OutFile "${OUTFILE}"
InstallDir "$PROGRAMFILES64\ABSS"
InstallDirRegKey HKLM "Software\ABSS" "InstallDir"
RequestExecutionLevel admin
SetCompressor /SOLID lzma
ShowInstDetails show
ShowUninstDetails show
BrandingText "ABSS ${VERSION}"
VIProductVersion "${VERSION}.0"
VIAddVersionKey "ProductName" "${FULLNAME}"
VIAddVersionKey "FileDescription" "${FULLNAME} setup"
VIAddVersionKey "FileVersion" "${VERSION}"
VIAddVersionKey "ProductVersion" "${VERSION}"
VIAddVersionKey "LegalCopyright" "ABSS contributors"

!define MUI_ABORTWARNING
!define MUI_WELCOMEPAGE_TITLE "Welcome to ${FULLNAME} ${VERSION}"
!define MUI_WELCOMEPAGE_TEXT "ABSS computes how every result of an Abaqus/Standard analysis, at every accepted increment, changes when a model parameter changes, and lets you explore those sensitivities in 2D/3D.$\r$\n$\r$\nEverything runs on this computer; nothing is uploaded.$\r$\n$\r$\nEverything is installed for you: the application, the examples, and the compilers for Abaqus user subroutines and compiled vendor materials (you are asked once to accept the Microsoft and Intel licences). Parts already on this computer are detected and skipped.$\r$\n$\r$\nClick Next to continue."
!define MUI_COMPONENTSPAGE_SMALLDESC
!define MUI_FINISHPAGE_RUN "$INSTDIR\abss.exe"
!define MUI_FINISHPAGE_RUN_TEXT "Start ABSS now"
!define MUI_FINISHPAGE_SHOWREADME "$INSTDIR\README.md"
!define MUI_FINISHPAGE_SHOWREADME_TEXT "Open the README"
!define MUI_FINISHPAGE_SHOWREADME_NOTCHECKED

!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_COMPONENTS
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH
!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES
!insertmacro MUI_LANGUAGE "English"

Var ToolchainPresent

Section "ABSS application" SecApp
  SectionIn RO
  SetOutPath "$INSTDIR"
  File /r /x "examples" /x "docs" "${DISTDIR}\*.*"
  SetOutPath "$INSTDIR\tools"
  File "toolchain.ps1"
  SetOutPath "$INSTDIR"
  WriteUninstaller "$INSTDIR\Uninstall ABSS.exe"
  WriteRegStr HKLM "Software\ABSS" "InstallDir" "$INSTDIR"
  WriteRegStr HKLM "${UNINSTKEY}" "DisplayName" "${FULLNAME}"
  WriteRegStr HKLM "${UNINSTKEY}" "DisplayVersion" "${VERSION}"
  WriteRegStr HKLM "${UNINSTKEY}" "Publisher" "ABSS contributors"
  WriteRegStr HKLM "${UNINSTKEY}" "DisplayIcon" "$INSTDIR\abss.exe"
  WriteRegStr HKLM "${UNINSTKEY}" "InstallLocation" "$INSTDIR"
  WriteRegStr HKLM "${UNINSTKEY}" "UninstallString" '"$INSTDIR\Uninstall ABSS.exe"'
  WriteRegDWORD HKLM "${UNINSTKEY}" "NoModify" 1
  WriteRegDWORD HKLM "${UNINSTKEY}" "NoRepair" 1
  CreateDirectory "$SMPROGRAMS\ABSS"
  CreateShortcut "$SMPROGRAMS\ABSS\ABSS.lnk" "$INSTDIR\abss.exe"
  CreateShortcut "$SMPROGRAMS\ABSS\Uninstall ABSS.lnk" "$INSTDIR\Uninstall ABSS.exe"
SectionEnd

Section "Examples and documentation" SecDocs
  SetOutPath "$INSTDIR\examples"
  File /r "${DISTDIR}\examples\*.*"
  SetOutPath "$INSTDIR\docs"
  File /r "${DISTDIR}\docs\*.*"
  CreateShortcut "$SMPROGRAMS\ABSS\Examples.lnk" "$INSTDIR\examples"
  CreateShortcut "$SMPROGRAMS\ABSS\Documentation.lnk" "$INSTDIR\docs"
SectionEnd

Section "Desktop shortcut" SecDesktop
  CreateShortcut "$DESKTOP\ABSS.lnk" "$INSTDIR\abss.exe"
SectionEnd

Var AcceptLicences
Section "User-subroutine compilers (UMAT/UEL)" SecTool
  ; Unattended installs (/S) accept the vendor licences only when /ACCEPT_LICENSES is given on the command line.
  StrCmp $AcceptLicences "1" tool_go
  MessageBox MB_YESNO|MB_ICONQUESTION "ABSS will now download and install, if they are missing:$\r$\n$\r$\n  - Microsoft Visual Studio 2022 Build Tools (C++), about 2-4 GB$\r$\n  - Intel oneAPI HPC Toolkit 2025.1 (Fortran compiler), about 2.8 GB download$\r$\n$\r$\nThese are installed under their own licences:$\r$\n  Microsoft: https://visualstudio.microsoft.com/license-terms/$\r$\n  Intel: https://www.intel.com/content/www/us/en/developer/articles/license/end-user-license-agreement.html$\r$\n$\r$\nDo you accept these licences and want to install them now? (This can take 30 minutes or more.)" /SD IDNO IDYES tool_go
    DetailPrint "User-subroutine compilers: skipped (licences not accepted)."
    Goto tool_done
  tool_go:
  DetailPrint "Checking and installing the user-subroutine toolchain..."
  nsExec::ExecToLog 'powershell.exe -NoProfile -ExecutionPolicy Bypass -File "$INSTDIR\tools\toolchain.ps1" -Install'
  Pop $0
  ${If} $0 != 0
    MessageBox MB_OK|MB_ICONEXCLAMATION "The user-subroutine compilers could not be installed completely (see the details list).$\r$\nABSS itself is installed and works; prebuilt standardU.dll libraries can still be used.$\r$\nYou can retry later from: $INSTDIR\tools\toolchain.ps1 -Install (run as administrator)."
  ${EndIf}
  tool_done:
SectionEnd

Section "-Report" SecReport
  DetailPrint "Checking this computer for Abaqus and compilers..."
  nsExec::ExecToLog 'powershell.exe -NoProfile -ExecutionPolicy Bypass -File "$INSTDIR\tools\toolchain.ps1"'
  Pop $0
SectionEnd

!insertmacro MUI_FUNCTION_DESCRIPTION_BEGIN
  !insertmacro MUI_DESCRIPTION_TEXT ${SecApp} "The ABSS desktop application and command-line tool (required)."
  !insertmacro MUI_DESCRIPTION_TEXT ${SecDocs} "Example Abaqus models (truss, J2 plasticity, UMAT, UEL) and the science/validation documentation."
  !insertmacro MUI_DESCRIPTION_TEXT ${SecDesktop} "Put an ABSS shortcut on the desktop."
  !insertmacro MUI_DESCRIPTION_TEXT ${SecTool} "Microsoft C++ Build Tools + Intel Fortran: build Abaqus UMAT/UEL subroutines from source or .obj files, and the material bridge for compiled vendor materials (including Linux .so libraries, which then run natively). Already-installed parts are detected and skipped."
!insertmacro MUI_FUNCTION_DESCRIPTION_END

Function .onInit
  ${IfNot} ${RunningX64}
    MessageBox MB_OK|MB_ICONSTOP "ABSS requires 64-bit Windows."
    Abort
  ${EndIf}
  SetRegView 64
  ${GetParameters} $R0
  ClearErrors
  ${GetOptions} $R0 "/ACCEPT_LICENSES" $R1
  StrCpy $AcceptLicences "0"
  IfErrors +2 0
    StrCpy $AcceptLicences "1"
  ; Mark the toolchain component when it is already present.
  StrCpy $ToolchainPresent "0"
  IfFileExists "$PROGRAMFILES32\Intel\oneAPI\setvars.bat" 0 +3
    IfFileExists "$PROGRAMFILES32\Microsoft Visual Studio\Installer\vswhere.exe" 0 +2
      StrCpy $ToolchainPresent "1"
  ${If} $ToolchainPresent == "1"
    SectionSetText ${SecTool} "User-subroutine compilers (already installed)"
  ${EndIf}
FunctionEnd

Section "Uninstall"
  SetRegView 64
  Delete "$DESKTOP\ABSS.lnk"
  RMDir /r "$SMPROGRAMS\ABSS"
  RMDir /r "$INSTDIR"
  DeleteRegKey HKLM "${UNINSTKEY}"
  DeleteRegKey HKLM "Software\ABSS"
  ; The compilers (if installed) are separate products: remove them from Windows Settings > Apps if no longer needed.
SectionEnd
