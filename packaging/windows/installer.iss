; Inno Setup script for Darker Reality, the release of holyland (docs/port/LAUNCHER.md, "Releases"). Run packaging/windows/build-windows.sh,
; which sets every name below from packaging/package.env and stages the files; never iscc on its own.
;
; The installer does not ask for the disc: the launcher asks at the first start, builds the game into
; %LOCALAPPDATA%\holyland\builds and keeps settings and saves in %APPDATA%\holyland (the project's
; folder name, kept from before the release had a name of its own), so the install folder stays
; read-only, an uninstall leaves saves alone, and reinstalling the same release does not build again.
#define AppVersion GetEnv("HOLYLAND_VERSION")
#define FileName   GetEnv("HOLYLAND_NAME")
#define AppName    GetEnv("HOLYLAND_DISPLAY_NAME")
#define Exe        GetEnv("HOLYLAND_APP_ID") + ".exe"
#define AppGuid    GetEnv("HOLYLAND_INNO_APP_ID")
#define Stage      GetEnv("HOLYLAND_STAGE")
#define Out        GetEnv("HOLYLAND_OUT")
#define Privileges GetEnv("HOLYLAND_INNO_PRIVILEGES")
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
