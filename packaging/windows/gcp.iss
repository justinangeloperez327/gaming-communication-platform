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
; Embed the verified list for a read-only preflight, before any payload is replaced.
Source: "{#BundleDir}\GCP-PAYLOAD-PATHS.txt"; Flags: dontcopy

[Icons]
Name: "{group}\Gaming Communication Platform Alpha"; Filename: "{app}\gcp-desktop.exe"; WorkingDir: "{app}"
Name: "{autodesktop}\Gaming Communication Platform Alpha"; Filename: "{app}\gcp-desktop.exe"; WorkingDir: "{app}"; Tasks: desktopicon

[Code]
const
  UninstallKey = 'Software\Microsoft\Windows\CurrentVersion\Uninstall\{F6CA3831-848C-4148-831E-804B1F6DC06B}_is1';

function NumericRelease(Value: String; var Version: Int64): Boolean;
var
  I, Dots, PartLength: Integer;
begin
  Result := False;
  I := Pos('-', Value);
  if I > 0 then Value := Copy(Value, 1, I - 1);
  if (Length(Value) = 0) or (Length(Value) > 23) then Exit;
  Dots := 0; PartLength := 0;
  for I := 1 to Length(Value) do begin
    if Value[I] = '.' then begin
      if PartLength = 0 then Exit;
      Dots := Dots + 1; PartLength := 0;
    end else begin
      if (Value[I] < '0') or (Value[I] > '9') then Exit;
      PartLength := PartLength + 1;
    end;
  end;
  if (Dots <> 2) or (PartLength = 0) then Exit;
  Result := StrToVersion(Value, Version);
end;

function SafePayloadPath(Value: String): Boolean;
var I: Integer;
begin
  Result := False;
  if (Value = '') or (Value[1] = '/') or (Pos('\', Value) > 0) or
     (Pos(':', Value) > 0) or (Pos('*', Value) > 0) or (Pos('?', Value) > 0) or
     (Pos('//', Value) > 0) or (Pos('/../', '/' + Value + '/') > 0) or
     (Pos('/./', '/' + Value + '/') > 0) then Exit;
  for I := 1 to Length(Value) do if Ord(Value[I]) < 32 then Exit;
  Result := True;
end;

function SmallTextFile(Name: String; var Lines: TArrayOfString): Boolean;
var Info: TFindRec;
begin
  Result := False;
  if not FindFirst(Name, Info) then Exit;
  try
    if (Info.SizeHigh <> 0) or (Info.SizeLow > 8388608) or ((Info.Attributes and $400) <> 0) then Exit;
  finally
    FindClose(Info);
  end;
  Result := LoadStringsFromFile(Name, Lines);
  if GetArrayLength(Lines) > 40000 then Result := False;
end;

function PrepareToInstall(var NeedsRestart: Boolean): String;
var
  OldLabel, OldDirectory, Value, OldPath, Why: String;
  OldVersion, NewVersion: Int64;
  OldLines, NewLines: TArrayOfString;
  NewPaths, Seen: TStringList;
  I, Count: Integer;
begin
  Result := '';
  if not RegQueryStringValue(HKCU, UninstallKey, 'DisplayVersion', OldLabel) then begin
    if FileExists(ExpandConstant('{app}\gcp-desktop.exe')) then
      Result := 'An unmanaged GCP application exists here. Choose a new folder or uninstall it first.';
    Exit;
  end;
  if not NumericRelease(OldLabel, OldVersion) or not NumericRelease('{#NumericVersion}', NewVersion) then begin
    Result := 'The installed version cannot be compared safely. Uninstall it before installing this build.'; Exit;
  end;
  if ComparePackedVersion(OldVersion, NewVersion) > 0 then begin
    Result := 'GCP downgrades require an explicit uninstall first. Saved preferences are preserved.'; Exit;
  end;
  if not RegQueryStringValue(HKCU, UninstallKey, 'InstallLocation', OldDirectory) or
     (CompareText(AddBackslash(OldDirectory), AddBackslash(ExpandConstant('{app}'))) <> 0) then begin
    Result := 'Use the existing GCP install folder, or uninstall before changing its location.'; Exit;
  end;
  Why := 'The previous payload cannot be upgraded safely. Uninstall first, then install this build. Saved preferences are preserved.';
  if not SmallTextFile(ExpandConstant('{app}\SHA256-MANIFEST.json'), OldLines) then begin Result := Why; Exit; end;
  ExtractTemporaryFile('GCP-PAYLOAD-PATHS.txt');
  if not SmallTextFile(ExpandConstant('{tmp}\GCP-PAYLOAD-PATHS.txt'), NewLines) then begin Result := Why; Exit; end;
  NewPaths := TStringList.Create; Seen := TStringList.Create;
  try
    NewPaths.Sorted := True; Seen.Sorted := True;
    for I := 0 to GetArrayLength(NewLines) - 1 do begin
      if not SafePayloadPath(NewLines[I]) then begin Result := Why; Exit; end;
      NewPaths.Add(NewLines[I]);
    end;
    Count := 0;
    for I := 0 to GetArrayLength(OldLines) - 1 do begin
      Value := Trim(OldLines[I]);
      if Pos('"path":', Value) = 1 then begin
        Value := Trim(Copy(Value, 8, Length(Value)));
        if (Length(Value) > 0) and (Value[Length(Value)] = ',') then Delete(Value, Length(Value), 1);
        if (Length(Value) < 3) or (Value[1] <> '"') or (Value[Length(Value)] <> '"') then begin Result := Why; Exit; end;
        OldPath := Copy(Value, 2, Length(Value) - 2);
        if not SafePayloadPath(OldPath) or (Seen.IndexOf(OldPath) >= 0) or (NewPaths.IndexOf(OldPath) < 0) then begin
          Result := Why; Exit;
        end;
        Seen.Add(OldPath); Count := Count + 1;
      end;
    end;
    if Count < 4 then Result := Why;
  finally
    Seen.Free; NewPaths.Free;
  end;
end;
