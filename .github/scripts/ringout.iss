; Ring Out - Ver 1.0 : Windows installer (Inno Setup)
;
; Built by .github/scripts/package-windows.ps1 on the CI runner.
;
; PrivilegesRequired=lowest is deliberate and load-bearing. Ring Out recompiles
; the game from the user's own disc INTO ITS OWN FOLDER -- setup.ps1 writes
; game/, work/ and bin/g<ID>_recomp.dll there, and the runtime writes userdata/.
; A machine-wide install under Program Files is read-only for a normal user, so
; first-run setup would fail. "lowest" resolves {autopf} to the per-user
; Programs folder, which is writable.

#define AppName    "Ring Out"
; Overridden by package-windows.ps1 with /DAppVersion=<VERSION>. The value
; here is only a fallback for compiling this script by hand -- the packaged
; installer takes its version from the repo's VERSION file, so the filename
; and the Add/Remove Programs entry cannot disagree with what was built.
#ifndef AppVersion
  #define AppVersion "0.0-handbuilt"
#endif
#define AppPublisher "Jack Poison"

[Setup]
AppId={{7C1D9E2A-4F63-4A11-9E5B-3D8A2C6F1B04}
AppName={#AppName}
AppVersion={#AppVersion}
AppVerName={#AppName} {#AppVersion}
AppPublisher={#AppPublisher}
DefaultDirName={autopf}\Ring Out
DefaultGroupName={#AppName}
DisableProgramGroupPage=yes
PrivilegesRequired=lowest
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
OutputDir=.
OutputBaseFilename=RingOut-{#AppVersion}-windows-x64-setup
; ISCC.exe is a 32-bit process. lzma2/ultra64 is a 64 MB dictionary, and with
; solid compression over a ~450 MB payload of thousands of small files it runs
; out of address space: "Error in ringout.iss: Out of memory. Compile aborted."
; max is a 32 MB dictionary, which fits and costs very little ratio on a payload
; that is mostly already-compressed executables.
Compression=lzma2/max
SolidCompression=yes
LZMANumBlockThreads=2
WizardStyle=modern
; The bundled toolchain is thousands of small files; without this the progress
; bar sits at 0% for a long time while Inno enumerates them.
SetupLogging=yes
UninstallDisplayName={#AppName} {#AppVersion}
UninstallDisplayIcon={app}\RingOut.exe
; Extracted disc + build output need considerably more room than the install.
ExtraDiskSpaceRequired=1600000000

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "Create a &desktop shortcut"; GroupDescription: "Additional shortcuts:"

[Files]
; StageDir comes from package-windows.ps1 (/DStageDir), for the same reason
; AppVersion does: the staging directory is named from VERSION, so a literal
; "RingOut-1.0" here silently stops matching the moment the version moves.
; That is exactly what happened -- ISCC failed with "No files found matching
; ...\RingOut-1.0\*" after the stage became RingOut-1.5.2.
#ifndef StageDir
  #define StageDir "RingOut-" + AppVersion
#endif
Source: "{#StageDir}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{group}\{#AppName}";           Filename: "{app}\RingOut.exe"; WorkingDir: "{app}"
Name: "{group}\Add guest fighters";   Filename: "{app}\RingOut.exe"; Parameters: "--add-guests"; WorkingDir: "{app}"
Name: "{group}\Uninstall {#AppName}"; Filename: "{uninstallexe}"
Name: "{autodesktop}\{#AppName}";     Filename: "{app}\RingOut.exe"; WorkingDir: "{app}"; Tasks: desktopicon

[Run]
Filename: "{app}\RingOut.exe"; Description: "Set up my game disc now"; WorkingDir: "{app}"; Flags: postinstall nowait skipifsilent unchecked

[UninstallDelete]
; Everything below is produced on the user's machine after install, so Inno does
; not track it and would otherwise leave gigabytes behind.
Type: filesandordirs; Name: "{app}\game"
Type: filesandordirs; Name: "{app}\work"
Type: filesandordirs; Name: "{app}\userdata"
Type: files;          Name: "{app}\bin\g*_recomp.dll"

[Messages]
WelcomeLabel2=This will install {#AppName} {#AppVersion} on your computer.%n%nNo game data is included. After installing, you supply a GameCube disc image you already own, and it is extracted and recompiled on this machine -- that step takes several minutes and needs about 1.5 GB of free space.
