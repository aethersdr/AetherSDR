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
; firewall update, replacing any prior rules in one UAC operation. Deleting
; AetherSDR.exe's INBOUND rules first also removes the block rule Windows leaves
; behind when its firewall prompt was dismissed: a block rule beats an allow.
; Outbound rules are left alone, so an administrator's outbound allows on an
; outbound-blocking PC survive an install or update. UDP (radio discovery and
; streams) is allowed on every network; TCP (TCI, CAT and the other listeners,
; unauthenticated on all interfaces, and TCI can key the transmitter) only on
; domain and private networks, so on a Public network Windows still asks.
Filename: "{cmd}"; Parameters: "/D /C ""netsh advfirewall firewall delete rule name=""AetherSDR D-STAR Waveform RX"" >NUL 2>&1 & netsh advfirewall firewall add rule name=""AetherSDR D-STAR Waveform RX"" dir=in action=allow program=""{app}\aether-dv-waveform.exe"" enable=yes profile=any protocol=UDP & netsh advfirewall firewall delete rule name=all dir=in program=""{app}\AetherSDR.exe"" >NUL 2>&1 & netsh advfirewall firewall add rule name=""AetherSDR (TCP-In)"" dir=in action=allow program=""{app}\AetherSDR.exe"" enable=yes profile=domain,private protocol=TCP & netsh advfirewall firewall add rule name=""AetherSDR (UDP-In)"" dir=in action=allow program=""{app}\AetherSDR.exe"" enable=yes profile=any protocol=UDP"""; WorkingDir: "{sys}"; Verb: "runas"; StatusMsg: "Configuring Windows Firewall for AetherSDR..."; Flags: shellexec runhidden waituntilterminated; Check: FileExists(ExpandConstant('{app}\aether-dv-waveform.exe'))
Filename: "{app}\AetherSDR.exe"; Description: "Launch AetherSDR"; Flags: nowait postinstall skipifsilent

[UninstallRun]
Filename: "{sys}\netsh.exe"; Parameters: "advfirewall firewall delete rule name=""AetherSDR D-STAR Waveform RX"""; Verb: "runas"; Flags: shellexec runhidden waituntilterminated; RunOnceId: "RemoveDStarWaveformFirewallRule"
Filename: "{cmd}"; Parameters: "/D /C ""netsh advfirewall firewall delete rule name=""AetherSDR (TCP-In)"" & netsh advfirewall firewall delete rule name=""AetherSDR (UDP-In)"""""; WorkingDir: "{sys}"; Verb: "runas"; Flags: shellexec runhidden waituntilterminated; RunOnceId: "RemoveAetherSdrFirewallRules"
