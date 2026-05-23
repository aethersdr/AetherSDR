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
Source: "..\..\deploy\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs; Excludes: "concrt140.dll,msvcp140*.dll,vccorlib140.dll,vcruntime140*.dll"
Source: "{#VC_RUNTIME_DIR}\*.dll"; DestDir: "{app}"; Flags: ignoreversion
#else
Source: "..\..\deploy\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs
#endif

[Icons]
Name: "{group}\AetherSDR"; Filename: "{app}\AetherSDR.exe"
Name: "{group}\Uninstall AetherSDR"; Filename: "{uninstallexe}"
Name: "{autodesktop}\AetherSDR"; Filename: "{app}\AetherSDR.exe"; Tasks: desktopicon

[Run]
Filename: "{app}\AetherSDR.exe"; Description: "Launch AetherSDR"; Flags: nowait postinstall skipifsilent
