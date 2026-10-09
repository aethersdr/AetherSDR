; AetherSDR Inno Setup Installer Script
; Version is passed via /DAPP_VERSION=x.y.z from the CI workflow

#ifndef APP_VERSION
  #define APP_VERSION "0.0.0"
#endif

[Setup]
AppName=AetherSDR
AppVersion={#APP_VERSION}
AppPublisher=AetherSDR Project
AppPublisherURL=https://github.com/ten9876/AetherSDR
AppSupportURL=https://github.com/ten9876/AetherSDR/issues
AppCopyright=Copyright (C) AetherSDR contributors
LicenseFile=..\..\LICENSE
DefaultDirName={autopf}\AetherSDR
DefaultGroupName=AetherSDR
UninstallDisplayIcon={app}\AetherSDR.exe
SetupIconFile=AetherSDR.ico
OutputBaseFilename=AetherSDR-v{#APP_VERSION}-Windows-x64-setup
OutputDir=..\..
Compression=lzma2/ultra64
SolidCompression=yes
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
WizardStyle=modern
WizardImageFile=wizard-image.bmp
WizardSmallImageFile=wizard-small-image.bmp
DisableWelcomePage=no
DisableProgramGroupPage=yes
PrivilegesRequired=lowest
PrivilegesRequiredOverridesAllowed=dialog

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Messages]
FinishedLabel=It's time to get on the air.%n%nSetup has finished installing [name] on your computer. The application may be launched by selecting the installed shortcuts.
FinishedLabelNoIcons=It's time to get on the air.%n%nSetup has finished installing [name] on your computer.

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"

[Files]
#ifdef VC_RUNTIME_DIR
Source: "..\..\deploy\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs; Excludes: "concrt140.dll,msvcp140*.dll,vccorlib140.dll,vcruntime140*.dll,vcomp140*.dll"
Source: "{#VC_RUNTIME_DIR}\*.dll"; DestDir: "{app}"; Flags: ignoreversion
#else
Source: "..\..\deploy\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs
#endif

[Icons]
Name: "{group}\AetherSDR"; Filename: "{app}\AetherSDR.exe"
Name: "{group}\Uninstall AetherSDR"; Filename: "{uninstallexe}"
Name: "{autodesktop}\AetherSDR"; Filename: "{app}\AetherSDR.exe"; Tasks: desktopicon

[Run]
; Keep the established per-user install scope. Elevate only this program-scoped
; firewall update, in one UAC operation; FirewallParams below builds it.
Filename: "{cmd}"; Parameters: "{code:FirewallParams}"; WorkingDir: "{sys}"; Verb: "runas"; StatusMsg: "Configuring Windows Firewall for AetherSDR..."; Flags: shellexec runhidden waituntilterminated
Filename: "{app}\AetherSDR.exe"; Description: "Launch AetherSDR"; Flags: nowait postinstall skipifsilent

[UninstallRun]
Filename: "{sys}\netsh.exe"; Parameters: "advfirewall firewall delete rule name=""AetherSDR D-STAR Waveform RX"""; Verb: "runas"; Flags: shellexec runhidden waituntilterminated; RunOnceId: "RemoveDStarWaveformFirewallRule"
Filename: "{cmd}"; Parameters: "/D /C ""netsh advfirewall firewall delete rule name=""AetherSDR (TCP-In)"" & netsh advfirewall firewall delete rule name=""AetherSDR (UDP-In)"""""; WorkingDir: "{sys}"; Verb: "runas"; Flags: shellexec runhidden waituntilterminated; RunOnceId: "RemoveAetherSdrFirewallRules"

[Code]
// The elevated firewall step, as one cmd.exe command line:
//  - AetherSDR.exe, always: delete its INBOUND rules first, which also removes
//    the block rule Windows leaves when its firewall prompt was dismissed (a
//    block beats an allow), then allow UDP on every network (radio discovery
//    and streams) and TCP on domain and private networks only. TCI, CAT and
//    the other TCP listeners are unauthenticated on all interfaces and TCI can
//    key the transmitter, so on a Public network incoming TCP stays blocked
//    unless the operator allows it in Windows Defender Firewall. Outbound
//    rules are left alone, so an administrator's outbound allows survive.
//  - aether-dv-waveform.exe, when the D-STAR helper is installed: its UDP rule.
// netsh is called by its full path; WorkingDir is {sys} as well.
function FirewallParams(Param: String): String;
var
  App, Netsh, Cmd: String;
begin
  App := ExpandConstant('{app}');
  Netsh := '"' + ExpandConstant('{sys}') + '\netsh.exe" advfirewall firewall ';
  Cmd := Netsh + 'delete rule name=all dir=in program="' + App + '\AetherSDR.exe" >NUL 2>&1'
    + ' & ' + Netsh + 'add rule name="AetherSDR (UDP-In)" dir=in action=allow program="' + App + '\AetherSDR.exe" enable=yes profile=any protocol=UDP'
    + ' && ' + Netsh + 'add rule name="AetherSDR (TCP-In)" dir=in action=allow program="' + App + '\AetherSDR.exe" enable=yes profile=domain,private protocol=TCP';
  if FileExists(App + '\aether-dv-waveform.exe') then
    Cmd := Netsh + 'delete rule name="AetherSDR D-STAR Waveform RX" >NUL 2>&1'
      + ' & ' + Netsh + 'add rule name="AetherSDR D-STAR Waveform RX" dir=in action=allow program="' + App + '\aether-dv-waveform.exe" enable=yes profile=any protocol=UDP'
      + ' & ' + Cmd;
  Result := '/D /C "' + Cmd + '"';
end;
