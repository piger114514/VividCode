; Self-extracting wrapper for the existing transactional, per-user installer.
; No runtime downloads, new component UI, shortcuts or uninstall subsystem.
#ifndef PackageVersion
  #error PackageVersion is required; use package.ps1
#endif
#ifndef Payload
  #error Payload is required; use package.ps1
#endif
[Setup]
AppName=vividcode
AppVersion={#PackageVersion}
AppId=vividcode-package
PrivilegesRequired=lowest
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
DefaultDirName={code:ExistingHome}
UsePreviousAppDir=no
Uninstallable=no
DisableDirPage=no
DisableProgramGroupPage=yes
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
SetupLogging=yes

[Files]
Source: "{#Payload}\*"; DestDir: "{tmp}\vividcode-payload"; Excludes: "runtime.lock"; Flags: recursesubdirs createallsubdirs dontcopy

[Code]
function ExistingHome(Param: String): String;
begin
  if not RegQueryStringValue(HKCU, 'Software\vividcode', 'InstallHome', Result) then
    Result := ExpandConstant('{localappdata}\vividcode');
end;

procedure InitializeWizard;
var
  Home: String;
begin
  Home := ExistingHome('');
  if FileExists(AddBackslash(Home) + 'runtime\installation.json') then
    WizardForm.SelectDirLabel.Caption := 'vividcode is already installed at ' + Home +
      '. Keep this location to upgrade, or choose a new empty folder to move the program, settings and caches. The old installation will be removed after verification.';
end;

function PrepareToInstall(var NeedsRestart: Boolean): String;
var
  ExitCode: Integer;
  ErrorText: AnsiString;
begin
  Result := '';
  ExtractTemporaryFiles('{tmp}\vividcode-payload\*');
  if not Exec(ExpandConstant('{sys}\WindowsPowerShell\v1.0\powershell.exe'),
    '-NoProfile -NonInteractive -ExecutionPolicy Bypass -File "' +
    ExpandConstant('{tmp}\vividcode-payload\install-package.ps1') + '" -HomeDirectory "' + WizardDirValue + '"',
    '', SW_HIDE, ewWaitUntilTerminated, ExitCode) then
    Result := 'Could not start the vividcode installer.';
  if (Result = '') and (ExitCode <> 0) then begin
    Result := 'vividcode installation/migration failed. Close vividcode and retry.';
    if LoadStringFromFile(ExpandConstant('{tmp}\vividcode-payload\install-error.txt'), ErrorText) then
      Result := Result + #13#10 + UTF8Decode(ErrorText);
  end;
end;
