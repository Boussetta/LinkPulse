; Inno Setup script for LinkPulse.
; Build linkpulse-tray.exe first (cmake --build --preset msvc-release), then
; compile this with Inno Setup's ISCC.exe:
;   & "C:\Program Files (x86)\Inno Setup 6\ISCC.exe" packaging\LinkPulse.iss
; Produces build\installer\LinkPulseSetup.exe.

#define MyAppName "LinkPulse"
#ifndef MyAppVersion
  #define MyAppVersion "0.1.0"
#endif
#define MyAppPublisher "LinkPulse"
#define MyAppURL "https://github.com/Boussetta/LinkPulse"
#define MyAppExeName "linkpulse-tray.exe"

[Setup]
AppId={{9F6B9F7A-2C7E-4C0A-8A7B-6E1B8B7B9E01}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppPublisher={#MyAppPublisher}
AppPublisherURL={#MyAppURL}
AppSupportURL={#MyAppURL}
AppUpdatesURL={#MyAppURL}
; Per-user install: no admin/UAC prompt needed, matches the app's existing
; per-user config (%APPDATA%) and autostart (HKCU) storage.
DefaultDirName={localappdata}\Programs\LinkPulse
DefaultGroupName=LinkPulse
DisableProgramGroupPage=yes
PrivilegesRequired=lowest
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
LicenseFile=..\LICENSE
OutputDir=..\build\installer
OutputBaseFilename=LinkPulseSetup
Compression=lzma
SolidCompression=yes
WizardStyle=modern

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "Create a &desktop shortcut"; GroupDescription: "Additional shortcuts:"; Flags: unchecked

[Files]
Source: "..\out\windows\{#MyAppExeName}"; DestDir: "{app}"; Flags: ignoreversion

[Icons]
Name: "{group}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"
Name: "{group}\Uninstall {#MyAppName}"; Filename: "{uninstallexe}"
Name: "{autodesktop}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"; Tasks: desktopicon

[Registry]
; The app itself toggles this key for "Start with Windows" (see
; src/platform/win32/autostart_win32.c) -- the installer never creates it,
; but cleans it up on uninstall if the user had enabled it, so no dangling
; entry points at a deleted exe.
Root: HKCU; Subkey: "Software\Microsoft\Windows\CurrentVersion\Run"; ValueType: none; ValueName: "LinkPulse"; Flags: uninsdeletevalue dontcreatekey

[Run]
Filename: "{app}\{#MyAppExeName}"; Description: "Launch {#MyAppName}"; Flags: postinstall nowait skipifsilent unchecked
