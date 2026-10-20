; Apocky's Labyrinth installer (Inno Setup 6). Built by tools\apx\package.ps1:
;   ISCC /DAppVersion=0.1.2 /DSrcDir=<staging folder> /O<dist> ApockysLabyrinth.iss
; Per-user install (no admin), Start menu and optional desktop shortcut, uninstaller in Windows Apps.
; The same AppId means a newer Setup upgrades the existing install in place; saves are never touched
; (they are created at runtime, so Setup neither installs nor removes them).
; Barony's art is NOT included: Setup finds the player's own Steam Barony and records its path.

#ifndef AppVersion
  #define AppVersion "0.0.0"
#endif
#ifndef SrcDir
  #error SrcDir must point at the package staging folder
#endif

[Setup]
AppId={{6B8F3C2A-7E14-4C5B-9A1D-AF0C4B5E2D31}
AppName=Apocky's Labyrinth
AppVersion={#AppVersion}
AppVerName=Apocky's Labyrinth {#AppVersion}
AppPublisher=Apocky
AppPublisherURL=https://apocky.com/labyrinth/
AppSupportURL=https://github.com/Apocky/barony-apx
AppUpdatesURL=https://github.com/Apocky/barony-apx/releases
DefaultDirName={localappdata}\Programs\ApockysLabyrinth
DefaultGroupName=Apocky's Labyrinth
DisableProgramGroupPage=yes
PrivilegesRequired=lowest
OutputBaseFilename=ApockysLabyrinth-{#AppVersion}-Setup
SetupIconFile={#SrcDir}\barony.ico
UninstallDisplayIcon={app}\barony.exe
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern
LicenseFile={#SrcDir}\LICENSE.txt
CloseApplications=yes
ArchitecturesInstallIn64BitMode=x64compatible
ArchitecturesAllowed=x64compatible

[Tasks]
Name: "desktopicon"; Description: "Create a &desktop shortcut"; GroupDescription: "Shortcuts:"

[Files]
Source: "{#SrcDir}\*"; DestDir: "{app}"; Excludes: "barony.ico"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "{#SrcDir}\barony.ico"; DestDir: "{app}"; Flags: ignoreversion

[Icons]
Name: "{group}\Apocky's Labyrinth"; Filename: "{sys}\WindowsPowerShell\v1.0\powershell.exe"; Parameters: "-NoProfile -ExecutionPolicy Bypass -WindowStyle Minimized -File ""{app}\launcher.ps1"""; WorkingDir: "{app}"; IconFilename: "{app}\barony.ico"
Name: "{group}\Uninstall Apocky's Labyrinth"; Filename: "{uninstallexe}"
Name: "{autodesktop}\Apocky's Labyrinth"; Filename: "{sys}\WindowsPowerShell\v1.0\powershell.exe"; Parameters: "-NoProfile -ExecutionPolicy Bypass -WindowStyle Minimized -File ""{app}\launcher.ps1"""; WorkingDir: "{app}"; IconFilename: "{app}\barony.ico"; Tasks: desktopicon

[Run]
Filename: "{sys}\WindowsPowerShell\v1.0\powershell.exe"; Parameters: "-NoProfile -ExecutionPolicy Bypass -WindowStyle Minimized -File ""{app}\launcher.ps1"" --no-update"; WorkingDir: "{app}"; Description: "Play Apocky's Labyrinth now"; Flags: postinstall nowait skipifsilent

[UninstallDelete]
; files the game and launcher create at runtime; saves (savegames\) are kept on purpose
Type: files; Name: "{app}\launcher.ps1.new"
Type: files; Name: "{app}\barony-path.txt"
Type: files; Name: "{app}\installed-by-setup.txt"
Type: files; Name: "{app}\apxgen_floor*"
Type: files; Name: "{app}\log.txt"
Type: filesandordirs; Name: "{app}\logfiles"
Type: filesandordirs; Name: "{app}\apx-out"

[Code]
var
  BaronyPage: TInputDirWizardPage;

function IsBaronyDir(const D: String): Boolean;
begin
  Result := DirExists(AddBackslash(D) + 'models') and DirExists(AddBackslash(D) + 'books');
end;

{ Steam library folders from libraryfolders.vdf: every "path" "..." value. }
function FindInVdf(const SteamRoot: String): String;
var
  Lines: TArrayOfString;
  I, P: Integer;
  S, Lib: String;
begin
  Result := '';
  if not LoadStringsFromFile(AddBackslash(SteamRoot) + 'steamapps\libraryfolders.vdf', Lines) then Exit;
  for I := 0 to GetArrayLength(Lines) - 1 do
  begin
    S := Trim(Lines[I]);
    if Pos('"path"', S) = 1 then
    begin
      Delete(S, 1, 6);
      S := Trim(S);
      P := Pos('"', S); Delete(S, 1, P);
      P := Pos('"', S);
      Lib := Copy(S, 1, P - 1);
      StringChangeEx(Lib, '\\', '\', True);
      if IsBaronyDir(AddBackslash(Lib) + 'steamapps\common\Barony') then
      begin
        Result := AddBackslash(Lib) + 'steamapps\common\Barony';
        Exit;
      end;
    end;
  end;
end;

function FindBarony(): String;
var
  Steam: String;
begin
  Result := '';
  if RegQueryStringValue(HKCU, 'Software\Valve\Steam', 'SteamPath', Steam) then
  begin
    StringChangeEx(Steam, '/', '\', True);
    if IsBaronyDir(AddBackslash(Steam) + 'steamapps\common\Barony') then begin Result := AddBackslash(Steam) + 'steamapps\common\Barony'; Exit; end;
    Result := FindInVdf(Steam);
    if Result <> '' then Exit;
  end;
  if IsBaronyDir(ExpandConstant('{commonpf32}\Steam\steamapps\common\Barony')) then
    Result := ExpandConstant('{commonpf32}\Steam\steamapps\common\Barony');
end;

procedure InitializeWizard();
var
  Found: String;
begin
  BaronyPage := CreateInputDirPage(wpSelectDir, 'Your copy of Barony',
    'Apocky''s Labyrinth uses the art, sounds and music of your own Steam copy of Barony.',
    'Setup looked for Barony in your Steam libraries. Check the folder below (it contains "models" and "books"), then click Next.',
    False, '');
  BaronyPage.Add('Barony folder:');
  Found := FindBarony();
  BaronyPage.Values[0] := Found;
end;

function NextButtonClick(CurPageID: Integer): Boolean;
begin
  Result := True;
  if (CurPageID = BaronyPage.ID) and not IsBaronyDir(BaronyPage.Values[0]) then
  begin
    MsgBox('That folder does not look like Barony (no "models" and "books" folders). Install Barony from Steam, or choose its folder.', mbError, MB_OK);
    Result := False;
  end;
end;

procedure CurStepChanged(CurStep: TSetupStep);
begin
  if CurStep = ssPostInstall then
  begin
    if IsBaronyDir(BaronyPage.Values[0]) then { a silent upgrade with no Barony found keeps the old path }
      SaveStringToFile(ExpandConstant('{app}\barony-path.txt'), BaronyPage.Values[0], False);
    SaveStringToFile(ExpandConstant('{app}\installed-by-setup.txt'), '{#AppVersion}', False);
  end;
end;
