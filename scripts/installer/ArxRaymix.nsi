; Arx Raymix Windows installer (NSIS 3, Modern UI 2).
;
; Built by scripts/build-installer.ps1 from the folder scripts/package.ps1 stages. Defines:
;   VERSION          tag, e.g. v1.4
;   VERSION_NUMERIC  four numbers for the file version, e.g. 1.4.0.0
;   STAGE            dist\ArxRaymix-<tag>, the unpacked player package
;   DIRS_NSH         generated RMDir lines for the package's folders, deepest first
;   ICON             the .ico for the setup and the uninstaller
;   OUTFILE          the setup .exe to write
;
; Installs per user (no administrator prompt) under %LOCALAPPDATA%\Programs\ArxRaymix by
; default, so ArxRaymix.exe can update the folder later. The uninstaller removes the files
; listed in package.txt (kept current by the launcher's updates) and what the launcher
; creates, never cfg.ini, saves or the Arx Fatalis folder.
;
; SPDX-License-Identifier: GPL-3.0-or-later

Unicode true
SetCompressor /SOLID lzma
RequestExecutionLevel user
ManifestDPIAware true

!include "MUI2.nsh"
!include "LogicLib.nsh"
!include "StrFunc.nsh"
${UnStrRep}
${UnStrStr}

!define APP_NAME "Arx Raymix"
!define APP_KEY "ArxRaymix"
!define UNINSTALL_KEY "Software\Microsoft\Windows\CurrentVersion\Uninstall\${APP_KEY}"
!define SETTINGS_KEY "Software\${APP_KEY}"
!define HOMEPAGE "https://github.com/gabriel7silva/ArxRaymix"

Name "${APP_NAME}"
OutFile "${OUTFILE}"
InstallDir "$LOCALAPPDATA\Programs\${APP_KEY}"
InstallDirRegKey HKCU "${SETTINGS_KEY}" "InstallDir"
BrandingText "${APP_NAME} ${VERSION}"

; A complete version block is one of the few things an unsigned installer can offer a
; reputation scanner. Missing CompanyName, InternalName or OriginalFilename scores against
; the file, and this installer has no certificate to outweigh that yet.
VIProductVersion "${VERSION_NUMERIC}"
VIAddVersionKey "ProductName" "${APP_NAME}"
VIAddVersionKey "ProductVersion" "${VERSION}"
VIAddVersionKey "FileVersion" "${VERSION_NUMERIC}"
VIAddVersionKey "FileDescription" "${APP_NAME} ${VERSION} Setup"
VIAddVersionKey "CompanyName" "${APP_NAME} Team"
VIAddVersionKey "InternalName" "${APP_KEY}Setup"
VIAddVersionKey "OriginalFilename" "${APP_KEY}-${VERSION}-setup.exe"
VIAddVersionKey "Comments" "Open source under the GPLv3. Source: ${HOMEPAGE}"
VIAddVersionKey "LegalCopyright" "GPLv3. Arx Fatalis (c) Arkane Studios / ZeniMax Media"

!define MUI_ICON "${ICON}"
!define MUI_UNICON "${ICON}"
!define MUI_ABORTWARNING

!define MUI_WELCOMEPAGE_TITLE "$(WelcomeTitle)"
!define MUI_WELCOMEPAGE_TEXT "$(WelcomeText)"
!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_LICENSE "${STAGE}\LICENSE"
!insertmacro MUI_PAGE_COMPONENTS
!define MUI_PAGE_CUSTOMFUNCTION_LEAVE CheckFolderBeforeInstall
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!define MUI_FINISHPAGE_RUN "$INSTDIR\ArxRaymix.exe"
!define MUI_FINISHPAGE_RUN_TEXT "$(RunText)"
!define MUI_FINISHPAGE_LINK "$(LinkText)"
!define MUI_FINISHPAGE_LINK_LOCATION "${HOMEPAGE}"
!insertmacro MUI_PAGE_FINISH

!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES

; The installer follows the Windows display language; anything else gets English.
!insertmacro MUI_LANGUAGE "English"
!insertmacro MUI_LANGUAGE "PortugueseBR"

LangString WelcomeTitle ${LANG_ENGLISH} "Welcome to ${APP_NAME} ${VERSION}"
LangString WelcomeTitle ${LANG_PORTUGUESEBR} "Bem-vindo ao ${APP_NAME} ${VERSION}"
LangString WelcomeText ${LANG_ENGLISH} "Arx Fatalis on the Arx Libertatis engine, with Direct3D 12, DXR ray tracing, NVIDIA DLSS and Frame Generation.$\r$\n$\r$\nYou need your own copy of Arx Fatalis from Steam or GOG, installed. This setup does not change it.$\r$\n$\r$\nClick Next to continue."
LangString WelcomeText ${LANG_PORTUGUESEBR} "Arx Fatalis no motor Arx Libertatis, com Direct3D 12, ray tracing DXR, NVIDIA DLSS e Frame Generation.$\r$\n$\r$\nVocê precisa ter o Arx Fatalis da Steam ou da GOG instalado. Esta instalação não o altera.$\r$\n$\r$\nClique em Avançar para continuar."
LangString RunText ${LANG_ENGLISH} "Start ${APP_NAME}"
LangString RunText ${LANG_PORTUGUESEBR} "Abrir o ${APP_NAME}"
LangString LinkText ${LANG_ENGLISH} "Source code and help on GitHub"
LangString LinkText ${LANG_PORTUGUESEBR} "Código-fonte e ajuda no GitHub"
LangString SecGame ${LANG_ENGLISH} "${APP_NAME} (required)"
LangString SecGame ${LANG_PORTUGUESEBR} "${APP_NAME} (obrigatório)"
LangString SecStart ${LANG_ENGLISH} "Start menu shortcuts"
LangString SecStart ${LANG_PORTUGUESEBR} "Atalhos no Menu Iniciar"
LangString SecDesktop ${LANG_ENGLISH} "Desktop shortcut"
LangString SecDesktop ${LANG_PORTUGUESEBR} "Atalho na Área de Trabalho"
LangString DescGame ${LANG_ENGLISH} "The game, the launcher with automatic updates, and the NVIDIA DLSS files."
LangString DescGame ${LANG_PORTUGUESEBR} "O jogo, o launcher com atualização automática e os arquivos do NVIDIA DLSS."
LangString DescStart ${LANG_ENGLISH} "Arx Raymix, Arx Raymix (DirectX 9) and Uninstall in the Start menu."
LangString DescStart ${LANG_PORTUGUESEBR} "Arx Raymix, Arx Raymix (DirectX 9) e Desinstalar no Menu Iniciar."
LangString DescDesktop ${LANG_ENGLISH} "An Arx Raymix shortcut on the desktop."
LangString DescDesktop ${LANG_PORTUGUESEBR} "Um atalho do Arx Raymix na Área de Trabalho."
LangString ShortcutDx9 ${LANG_ENGLISH} "${APP_NAME} (DirectX 9)"
LangString ShortcutDx9 ${LANG_PORTUGUESEBR} "${APP_NAME} (DirectX 9)"
LangString ShortcutUninstall ${LANG_ENGLISH} "Uninstall ${APP_NAME}"
LangString ShortcutUninstall ${LANG_PORTUGUESEBR} "Desinstalar o ${APP_NAME}"
LangString GameFolderWarning ${LANG_ENGLISH} "This folder contains data.pak, so it looks like the Arx Fatalis folder. Install Arx Raymix in a folder of its own."
LangString GameFolderWarning ${LANG_PORTUGUESEBR} "Esta pasta tem o data.pak, então parece ser a pasta do Arx Fatalis. Instale o Arx Raymix numa pasta própria."
LangString RunningWarning ${LANG_ENGLISH} "Arx Raymix is running. Close the game, then click Retry."
LangString RunningWarning ${LANG_PORTUGUESEBR} "O Arx Raymix está aberto. Feche o jogo e clique em Repetir."
LangString FolderNotEmpty ${LANG_ENGLISH} "This folder already has other files. Choose an empty folder, or the folder of an earlier Arx Raymix install."
LangString FolderNotEmpty ${LANG_PORTUGUESEBR} "Esta pasta já tem outros arquivos. Escolha uma pasta vazia ou a pasta de uma instalação anterior do Arx Raymix."
LangString UnKeepNote ${LANG_ENGLISH} "Your saves and settings (cfg.ini) are kept in Saved Games\Arx Libertatis."
LangString UnKeepNote ${LANG_PORTUGUESEBR} "Seus saves e configurações (cfg.ini) ficam guardados em Jogos Salvos\Arx Libertatis."

Function .onVerifyInstDir
	; Refuse the Arx Fatalis folder: the mod must not mix its files with the game's.
	${If} ${FileExists} "$INSTDIR\data.pak"
		Abort
	${EndIf}
FunctionEnd

Function CheckFolderBeforeInstall
	; Refuse a folder that already has files and is not an earlier Arx Raymix install: the
	; uninstaller removes the folders the launcher creates, and must only find ours there.
	${If} ${FileExists} "$INSTDIR\*"
	${AndIfNot} ${FileExists} "$INSTDIR\package.txt"
		FindFirst $0 $1 "$INSTDIR\*"
		${DoWhile} $1 != ""
			${If} $1 != "."
			${AndIf} $1 != ".."
				FindClose $0
				MessageBox MB_OK|MB_ICONEXCLAMATION "$(FolderNotEmpty)"
				Abort
			${EndIf}
			FindNext $0 $1
		${Loop}
		FindClose $0
	${EndIf}
FunctionEnd

Function .onInit
	; A per-user install never needs elevation; keep the default even for admin accounts.
	SetShellVarContext current
FunctionEnd

!macro WaitForGame
	; arx.exe and ArxRaymix.exe cannot be replaced while they run.
	${Do}
		${IfNot} ${FileExists} "$INSTDIR\arx.exe"
			${ExitDo}
		${EndIf}
		ClearErrors
		FileOpen $0 "$INSTDIR\arx.exe" a
		${IfNot} ${Errors}
			FileClose $0
			${ExitDo}
		${EndIf}
		; Retry jumps over Abort and loops back to the check.
		MessageBox MB_RETRYCANCEL|MB_ICONEXCLAMATION "$(RunningWarning)" /SD IDCANCEL IDRETRY +2
		Abort
	${Loop}
!macroend

Section "$(SecGame)" SEC_GAME
	SectionIn RO
	SetOutPath "$INSTDIR"
	!insertmacro WaitForGame
	File /r "${STAGE}\*"
	WriteUninstaller "$INSTDIR\Uninstall.exe"

	WriteRegStr HKCU "${SETTINGS_KEY}" "InstallDir" "$INSTDIR"
	WriteRegStr HKCU "${UNINSTALL_KEY}" "DisplayName" "${APP_NAME}"
	WriteRegStr HKCU "${UNINSTALL_KEY}" "DisplayVersion" "${VERSION}"
	WriteRegStr HKCU "${UNINSTALL_KEY}" "Publisher" "gabriel7silva"
	WriteRegStr HKCU "${UNINSTALL_KEY}" "DisplayIcon" "$INSTDIR\arx.exe,0"
	WriteRegStr HKCU "${UNINSTALL_KEY}" "InstallLocation" "$INSTDIR"
	WriteRegStr HKCU "${UNINSTALL_KEY}" "URLInfoAbout" "${HOMEPAGE}"
	WriteRegStr HKCU "${UNINSTALL_KEY}" "HelpLink" "${HOMEPAGE}/issues"
	WriteRegStr HKCU "${UNINSTALL_KEY}" "UninstallString" '"$INSTDIR\Uninstall.exe"'
	WriteRegStr HKCU "${UNINSTALL_KEY}" "QuietUninstallString" '"$INSTDIR\Uninstall.exe" /S'
	WriteRegDWORD HKCU "${UNINSTALL_KEY}" "NoModify" 1
	WriteRegDWORD HKCU "${UNINSTALL_KEY}" "NoRepair" 1
	SectionGetSize ${SEC_GAME} $0
	WriteRegDWORD HKCU "${UNINSTALL_KEY}" "EstimatedSize" $0
SectionEnd

Section "$(SecStart)" SEC_START
	CreateDirectory "$SMPROGRAMS\${APP_NAME}"
	CreateShortcut "$SMPROGRAMS\${APP_NAME}\${APP_NAME}.lnk" "$INSTDIR\ArxRaymix.exe" "" "$INSTDIR\arx.exe" 0
	CreateShortcut "$SMPROGRAMS\${APP_NAME}\$(ShortcutDx9).lnk" "$INSTDIR\Arx Raymix - DirectX 9.bat" "" "$INSTDIR\arx.exe" 0
	CreateShortcut "$SMPROGRAMS\${APP_NAME}\$(ShortcutUninstall).lnk" "$INSTDIR\Uninstall.exe"
SectionEnd

Section "$(SecDesktop)" SEC_DESKTOP
	CreateShortcut "$DESKTOP\${APP_NAME}.lnk" "$INSTDIR\ArxRaymix.exe" "" "$INSTDIR\arx.exe" 0
SectionEnd

!insertmacro MUI_FUNCTION_DESCRIPTION_BEGIN
	!insertmacro MUI_DESCRIPTION_TEXT ${SEC_GAME} "$(DescGame)"
	!insertmacro MUI_DESCRIPTION_TEXT ${SEC_START} "$(DescStart)"
	!insertmacro MUI_DESCRIPTION_TEXT ${SEC_DESKTOP} "$(DescDesktop)"
!insertmacro MUI_FUNCTION_DESCRIPTION_END

; ---------------------------------------------------------------------------------------------
; Uninstall

Function un.onInit
	SetShellVarContext current
FunctionEnd

; Removes $1's parent folders inside the install folder, deepest first, while they are empty.
Function un.RemoveParents
	${Do}
		StrLen $5 $1
		IntOp $6 $5 - 1
		${Do}
			${If} $6 < 1
				${ExitDo}
			${EndIf}
			StrCpy $7 $1 1 $6
			${If} $7 == "\"
				${ExitDo}
			${EndIf}
			IntOp $6 $6 - 1
		${Loop}
		${If} $6 < 1
			${ExitDo}
		${EndIf}
		StrCpy $1 $1 $6
		RMDir "$INSTDIR\$1"
	${Loop}
FunctionEnd

; Deletes every file package.txt lists (line 1 is the version), or with $R0 = 1 removes the
; folders they were in once empty: an update can add folders the setup did not create. Lines
; that could point outside the install folder, or are not plain file names, are skipped.
Function un.RemovePackageFiles
	ClearErrors
	FileOpen $0 "$INSTDIR\package.txt" r
	${If} ${Errors}
		Return
	${EndIf}
	FileRead $0 $1 ; version
	${Do}
		ClearErrors
		FileRead $0 $1
		${If} ${Errors}
			${ExitDo}
		${EndIf}
		${UnStrRep} $1 $1 "$\r" ""
		${UnStrRep} $1 $1 "$\n" ""
		${UnStrRep} $1 $1 "/" "\"
		StrCpy $2 $1 1
		${If} $1 == ""
		${OrIf} $2 == "#"
		${OrIf} $2 == "\"
			${Continue}
		${EndIf}
		${UnStrStr} $3 $1 ".."
		${UnStrStr} $4 $1 ":"
		${UnStrStr} $8 $1 "*"
		${UnStrStr} $9 $1 "?"
		${If} $3 == ""
		${AndIf} $4 == ""
		${AndIf} $8 == ""
		${AndIf} $9 == ""
			${If} $R0 == 1
				Call un.RemoveParents
			${Else}
				Delete "$INSTDIR\$1"
			${EndIf}
		${EndIf}
	${Loop}
	FileClose $0
FunctionEnd

Section "Uninstall"
	!insertmacro WaitForGame

	StrCpy $R0 0
	Call un.RemovePackageFiles
	StrCpy $R0 1
	Call un.RemovePackageFiles
	Delete "$INSTDIR\package.txt"
	Delete "$INSTDIR\launcher.log"
	Delete "$INSTDIR\launcher.ini"
	Delete "$INSTDIR\ArxRaymix.exe.old"
	; Created by the launcher: the previous version and a pending download.
	RMDir /r "$INSTDIR\.arxraymix-backup"
	RMDir /r "$INSTDIR\.arxraymix-update"
	; The 1.4 / 1.5 launcher used plain names; removed only when they hold what it put there.
	${If} ${FileExists} "$INSTDIR\backup\package.txt"
		RMDir /r "$INSTDIR\backup"
	${EndIf}
	${If} ${FileExists} "$INSTDIR\update\staging\*"
	${OrIf} ${FileExists} "$INSTDIR\update\rolled-back\*"
		RMDir /r "$INSTDIR\update"
	${EndIf}
	!include "${DIRS_NSH}"
	Delete "$INSTDIR\Uninstall.exe"
	RMDir "$INSTDIR"

	Delete "$SMPROGRAMS\${APP_NAME}\${APP_NAME}.lnk"
	Delete "$SMPROGRAMS\${APP_NAME}\$(ShortcutDx9).lnk"
	Delete "$SMPROGRAMS\${APP_NAME}\$(ShortcutUninstall).lnk"
	RMDir "$SMPROGRAMS\${APP_NAME}"
	Delete "$DESKTOP\${APP_NAME}.lnk"

	DeleteRegKey HKCU "${UNINSTALL_KEY}"
	DeleteRegKey HKCU "${SETTINGS_KEY}"

	DetailPrint "$(UnKeepNote)"
SectionEnd
