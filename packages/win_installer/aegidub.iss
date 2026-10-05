; Installer for aegidub (Windows x64)
;
; Build with tools\build-aegidub-installer.ps1, which downloads the Visual C++
; runtime and sets BUILD_ROOT and SOURCE_ROOT for this script.
;
; This is deliberately separate from the Aegisub installer scripts next to it:
; it has its own AppId so it never replaces or uninstalls an Aegisub
; installation on the same computer.

#define BUILD_ROOT GetEnv('BUILD_ROOT')
#define SOURCE_ROOT GetEnv('SOURCE_ROOT')
#define INSTALLER_DIR SOURCE_ROOT + "\packages\win_installer"
#define DEPS_DIR BUILD_ROOT + "\installer-deps"
#define CURRENT_YEAR GetDateTimeString('yyyy', '', '')

#include BUILD_ROOT + "\git_version.h"

; The MSVC toolset the program was built with; an older runtime won't do
#define VC_MAJOR 14
#define VC_MINOR 44

[Setup]
AppId={{A6884C1B-66C8-46A5-A6AA-90FEB79682FF}
AppName=aegidub
AppVerName=aegidub {#BUILD_GIT_VERSION_STRING}
AppVersion={#INSTALLER_VERSION}
AppPublisher=tuvshinorg
AppPublisherURL=https://github.com/tuvshinorg/aegidub
AppSupportURL=https://github.com/tuvshinorg/aegidub/issues
AppUpdatesURL=https://github.com/tuvshinorg/aegidub/releases
AppCopyright=2005-{#CURRENT_YEAR} The Aegisub Team and aegidub contributors
VersionInfoVersion={#INSTALLER_VERSION}
VersionInfoDescription=aegidub {#BUILD_GIT_VERSION_STRING} setup
DefaultDirName={autopf}\aegidub
DefaultGroupName=aegidub
DisableProgramGroupPage=yes
; The Visual C++ runtime installs system-wide
PrivilegesRequired=admin
ArchitecturesInstallIn64BitMode=x64compatible
ArchitecturesAllowed=x64compatible
MinVersion=10.0
LicenseFile={#INSTALLER_DIR}\license.txt
OutputDir={#BUILD_ROOT}
OutputBaseFilename=aegidub-{#BUILD_GIT_VERSION_STRING}-x64-setup
Compression=lzma2/ultra64
SolidCompression=yes
WizardStyle=modern
WizardImageFile={#INSTALLER_DIR}\welcome-large.bmp
WizardSmallImageFile={#INSTALLER_DIR}\aegisub-large.bmp
SetupIconFile={#INSTALLER_DIR}\portable\icon.ico
UninstallDisplayIcon={app}\aegidub.exe
UninstallDisplayName=aegidub

[Languages]
Name: "en"; MessagesFile: "compiler:Default.isl"

[CustomMessages]
en.InstallRuntime=Installing the Microsoft Visual C++ runtime...

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"

[Files]
Source: "{#BUILD_ROOT}\aegidub.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#INSTALLER_DIR}\license.txt"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#SOURCE_ROOT}\README.md"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#SOURCE_ROOT}\README.mn.md"; DestDir: "{app}"; Flags: ignoreversion

; Automation scripts that ship with the editor
Source: "{#SOURCE_ROOT}\automation\autoload\*"; DestDir: "{app}\automation\autoload"; Excludes: "meson.build"; Flags: ignoreversion recursesubdirs
Source: "{#SOURCE_ROOT}\automation\include\*"; DestDir: "{app}\automation\include"; Excludes: "meson.build"; Flags: ignoreversion recursesubdirs
Source: "{#SOURCE_ROOT}\automation\demos\*"; DestDir: "{app}\automation\demos"; Excludes: "meson.build"; Flags: ignoreversion recursesubdirs

; Spell checking dictionary, if it was downloaded
Source: "{#DEPS_DIR}\dictionaries\*"; DestDir: "{app}\dictionaries"; Flags: ignoreversion skipifsourcedoesntexist

; Only unpacked when the computer needs it
Source: "{#DEPS_DIR}\VC_redist\VC_redist.x64.exe"; DestDir: "{tmp}"; Flags: deleteafterinstall nocompression; Check: VCRuntimeNeeded

[Icons]
Name: "{autoprograms}\aegidub"; Filename: "{app}\aegidub.exe"; WorkingDir: "{app}"; Comment: "AI dubbing and subtitle editor"
Name: "{autodesktop}\aegidub"; Filename: "{app}\aegidub.exe"; WorkingDir: "{app}"; Tasks: desktopicon

[Registry]
; Lets "aegidub" be typed in the Run box
Root: HKLM; Subkey: "SOFTWARE\Microsoft\Windows\CurrentVersion\App Paths\aegidub.exe"; ValueType: string; ValueName: ""; ValueData: "{app}\aegidub.exe"; Flags: uninsdeletekey

[Run]
Filename: "{tmp}\VC_redist.x64.exe"; Parameters: "/install /quiet /norestart"; StatusMsg: "{cm:InstallRuntime}"; Flags: waituntilterminated; Check: VCRuntimeNeeded
Filename: "{app}\aegidub.exe"; Description: "{cm:LaunchProgram,aegidub}"; Flags: nowait postinstall skipifsilent

; Settings, API keys and projects in the user's folders are kept on uninstall

[Code]
// Is the x64 Visual C++ 2015-2022 runtime missing, or older than the one
// the program was built with?
function VCRuntimeNeeded: Boolean;
var
  Installed, Major, Minor: Cardinal;
  Key: String;
begin
  Key := 'SOFTWARE\Microsoft\VisualStudio\14.0\VC\Runtimes\x64';
  Result := True;
  if RegQueryDWordValue(HKLM64, Key, 'Installed', Installed) and (Installed = 1) and
     RegQueryDWordValue(HKLM64, Key, 'Major', Major) and
     RegQueryDWordValue(HKLM64, Key, 'Minor', Minor) then
  begin
    Result := (Major < {#VC_MAJOR}) or ((Major = {#VC_MAJOR}) and (Minor < {#VC_MINOR}));
  end;
end;
