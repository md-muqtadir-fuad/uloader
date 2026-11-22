; U-loader setup program (Inno Setup 6).  Build it with build-installer.bat,
; which builds U-loader, collects the helper programs into bin\ and runs ISCC.
;
; I agree -> components -> options -> Install, for the current user only
; (no administrator rights), with yt-dlp, FFmpeg and Deno as optional parts.

#define AppName      "U-loader"
#define AppVersion   "1.3.0"
#define AppPublisher "Md. Muqtadir Fuad"
#define AppURL       "https://github.com/md-muqtadir-fuad/uloader"

#define HaveYtdlp    FileExists(AddBackslash(SourcePath) + "bin\yt-dlp.exe")
#define HaveFfmpeg   FileExists(AddBackslash(SourcePath) + "bin\ffmpeg.exe")
#define HaveDeno     FileExists(AddBackslash(SourcePath) + "bin\deno.exe")

[Setup]
; never change AppId: it ties updates and the uninstaller to this program
AppId={{653FF13A-3EB4-4A0E-8CED-420BF4B7A2FD}
AppName={#AppName}
AppVersion={#AppVersion}
AppVerName={#AppName} {#AppVersion}
AppPublisher={#AppPublisher}
AppPublisherURL={#AppURL}
AppSupportURL={#AppURL}/issues
AppUpdatesURL={#AppURL}/releases
DefaultDirName={localappdata}\Programs\{#AppName}
DisableProgramGroupPage=yes
PrivilegesRequired=lowest
LicenseFile=out\license.txt
OutputDir=out
OutputBaseFilename=U-loader-Setup-{#AppVersion}
SetupIconFile=..\src\gui.ico
UninstallDisplayIcon={app}\uloader-gui.exe
UninstallDisplayName={#AppName}
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
Compression=lzma2/ultra64
SolidCompression=yes
WizardStyle=modern
; the window holds this mutex: setup and uninstall ask to close U-loader first
AppMutex=ULoaderWindowSingleInstance
CloseApplications=yes
VersionInfoVersion={#AppVersion}.0

[Types]
Name: "full";   Description: "Everything (recommended)"
Name: "custom"; Description: "Choose what to install"; Flags: iscustom

[Components]
Name: "main";   Description: "U-loader (window, command line and browser extension)"; Types: full custom; Flags: fixed
#if HaveYtdlp
Name: "ytdlp";  Description: "yt-dlp - videos from YouTube, Facebook, TikTok and 1800+ sites"; Types: full
#endif
#if HaveFfmpeg
Name: "ffmpeg"; Description: "FFmpeg - joins video and audio (1080p and higher), converts streams to MP4"; Types: full
#endif
#if HaveDeno
Name: "deno";   Description: "Deno - lets yt-dlp read YouTube in full quality"; Types: full
#endif

[Tasks]
Name: "browser";     Description: "Connect U-loader to Chrome, Edge and Brave"
Name: "desktopicon"; Description: "Create a desktop icon"; Flags: unchecked
Name: "autostart";   Description: "Start U-loader with Windows (in the notification area)"; Flags: unchecked

[Files]
Source: "..\uloader-gui.exe";         DestDir: "{app}"; Components: main; Flags: ignoreversion
Source: "..\uloader.exe";             DestDir: "{app}"; Components: main; Flags: ignoreversion
Source: "..\extension\*";             DestDir: "{app}\extension"; Components: main; Flags: ignoreversion recursesubdirs
Source: "..\LICENSE";                 DestDir: "{app}"; DestName: "LICENSE.txt"; Components: main
Source: "..\THIRD_PARTY_NOTICES.md";  DestDir: "{app}"; Components: main
Source: "..\README.md";               DestDir: "{app}"; Components: main
Source: "..\CHANGELOG.md";            DestDir: "{app}"; Components: main
#if HaveYtdlp
Source: "bin\yt-dlp.exe";             DestDir: "{app}"; Components: ytdlp; Flags: ignoreversion
#endif
#if HaveFfmpeg
Source: "bin\ffmpeg.exe";             DestDir: "{app}"; Components: ffmpeg; Flags: ignoreversion
Source: "bin\ffmpeg-build.txt";       DestDir: "{app}"; Components: ffmpeg; Flags: ignoreversion skipifsourcedoesntexist
#endif
#if HaveDeno
Source: "bin\deno.exe";               DestDir: "{app}"; Components: deno; Flags: ignoreversion
#endif

[Icons]
Name: "{autoprograms}\{#AppName}"; Filename: "{app}\uloader-gui.exe"
Name: "{autodesktop}\{#AppName}";  Filename: "{app}\uloader-gui.exe"; Tasks: desktopicon

[Registry]
Root: HKCU; Subkey: "Software\Microsoft\Windows\CurrentVersion\Run"; ValueType: string; ValueName: "{#AppName}"; \
    ValueData: """{app}\uloader-gui.exe"" --tray"; Tasks: autostart; Flags: uninsdeletevalue

[Run]
; native messaging registration for the browser extension (per user)
Filename: "{app}\uloader.exe"; Parameters: "--install"; Tasks: browser; Flags: runhidden; \
    StatusMsg: "Connecting U-loader to your browsers..."
; U-loader then shows how to add its browser extension (it cannot be added by a program)
Filename: "{app}\uloader-gui.exe"; Description: "Start U-loader and add it to your browser"; Flags: postinstall nowait skipifsilent
; a silent update started by U-loader itself (/RESTARTAPP) starts it again, in the tray with /TRAY
Filename: "{app}\uloader-gui.exe"; Flags: nowait; Check: RestartApp(False)
Filename: "{app}\uloader-gui.exe"; Parameters: "--tray"; Flags: nowait; Check: RestartApp(True)

[UninstallRun]
Filename: "{app}\uloader.exe"; Parameters: "--uninstall"; Flags: runhidden; RunOnceId: "DisconnectBrowsers"

[UninstallDelete]
; written by "uloader --install"; the downloads and %APPDATA%\U-loader are kept
Type: files; Name: "{app}\uloader-host.json"

[Code]
{ U-loader runs the setup of an update with /VERYSILENT /RESTARTAPP [/TRAY]
  and closes; the setup then starts it again the way it was. }
function HasParam(const Name: String): Boolean;
var
  I: Integer;
begin
  Result := False;
  for I := 1 to ParamCount do
    if CompareText(ParamStr(I), Name) = 0 then
      Result := True;
end;

function RestartApp(InTray: Boolean): Boolean;
begin
  Result := HasParam('/RESTARTAPP') and (HasParam('/TRAY') = InTray);
end;
