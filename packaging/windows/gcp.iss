#ifndef BundleDir
  #error BundleDir is required
#endif
#ifndef OutputDirPath
  #error OutputDirPath is required
#endif
#ifndef ReleaseVersion
  #error ReleaseVersion is required
#endif
#ifndef NumericVersion
  #error NumericVersion is required
#endif

[Setup]
AppId={{F6CA3831-848C-4148-831E-804B1F6DC06B}
AppName=Gaming Communication Platform Alpha
AppVersion={#ReleaseVersion}
VersionInfoVersion={#NumericVersion}
AppPublisher=GCP Project
AppPublisherURL=https://github.com/justinangeloperez327/gaming-communication-platform
DefaultDirName={localappdata}\Programs\GCP-Alpha
DefaultGroupName=Gaming Communication Platform Alpha
PrivilegesRequired=lowest
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0
OutputDir={#OutputDirPath}
OutputBaseFilename=GCP-{#ReleaseVersion}-windows-x64-setup
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
UninstallDisplayIcon={app}\gcp-desktop.exe
CloseApplications=yes
RestartApplications=no
SetupMutex=GCPAlphaInstaller
InfoBeforeFile=preview-notice.txt

[Tasks]
Name: "desktopicon"; Description: "Create a desktop shortcut"; Flags: unchecked

[Files]
Source: "{#BundleDir}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{group}\Gaming Communication Platform Alpha"; Filename: "{app}\gcp-desktop.exe"; WorkingDir: "{app}"
Name: "{autodesktop}\Gaming Communication Platform Alpha"; Filename: "{app}\gcp-desktop.exe"; WorkingDir: "{app}"; Tasks: desktopicon
