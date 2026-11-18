; Inno Setup script for Darker Reality (docs/port/LAUNCHER.md, "Releases"). Run packaging/windows/build-windows.sh,
; which sets every name below from packaging/package.env and stages the files; never iscc on its own.
;
; The installer does not ask for the disc: the launcher asks at the first start, builds the game into
; %LOCALAPPDATA%\darker-reality\builds and keeps settings and saves in %APPDATA%\darker-reality
; (port/platform/data_dir.c, which moves them there from the old name, holyland), so the install folder stays
; read-only, an uninstall leaves saves alone, and reinstalling the same release does not build again.
#define AppVersion GetEnv("DARKER_REALITY_VERSION")
#define FileName   GetEnv("DARKER_REALITY_NAME")
#define AppName    GetEnv("DARKER_REALITY_DISPLAY_NAME")
#define Exe        GetEnv("DARKER_REALITY_APP_ID") + ".exe"
#define AppGuid    GetEnv("DARKER_REALITY_INNO_APP_ID")
#define Stage      GetEnv("DARKER_REALITY_STAGE")
#define Out        GetEnv("DARKER_REALITY_OUT")
#define Privileges GetEnv("DARKER_REALITY_INNO_PRIVILEGES")
#if AppVersion == "" || AppName == "" || FileName == "" || AppGuid == "" || Stage == "" || Out == ""
  #error Run packaging/windows/build-windows.sh rather than iscc directly.
#endif
#if Privileges == ""
  #define Privileges "lowest"
#endif

[Setup]
AppId={{{#AppGuid}}
AppName={#AppName}
AppVersion={#AppVersion}
AppPublisher=Voicedrew11
PrivilegesRequired={#Privileges}
DefaultDirName={autopf}\{#AppName}
DefaultGroupName={#AppName}
UninstallDisplayIcon={app}\{#Exe}
OutputDir={#Out}
OutputBaseFilename={#FileName}-{#AppVersion}-win-x64-setup
Compression=lzma2/max
SolidCompression=yes
; LZMA2 in blocks on 4 threads (the release workflow's runner has 4): a little larger, minutes faster
LZMANumBlockThreads=4
LZMAUseSeparateProcess=yes
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
WizardStyle=modern
LicenseFile={#Stage}\LICENSE.txt
DisableProgramGroupPage=yes

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: desktopicon; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked

; An upgrade installs over the old folder and Inno removes nothing the new release dropped: the old release's
; source and tools go first (never the player's files, which are not here)
[InstallDelete]
Type: filesandordirs; Name: "{app}\content"

[Files]
Source: "{#Stage}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{group}\{#AppName}"; Filename: "{app}\{#Exe}"
Name: "{group}\{#AppName} (disc, rebuild, updates)"; Filename: "{app}\{#Exe}"; Parameters: "--launcher"
Name: "{autodesktop}\{#AppName}"; Filename: "{app}\{#Exe}"; Tasks: desktopicon

[Run]
Filename: "{app}\{#Exe}"; Description: "{cm:LaunchProgram,{#AppName}}"; Flags: nowait postinstall skipifsilent
