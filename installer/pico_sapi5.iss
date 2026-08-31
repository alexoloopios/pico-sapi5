; Installer for the Pico SAPI 5 voices.
;
; Both engine DLLs are installed into one directory beside a single copy of the
; lingware, and each is registered for applications of its own bitness. The
; registration writes to HKEY_LOCAL_MACHINE, which is the only place SAPI looks
; for voice tokens, so the installer requires administrative rights.

#define AppName "Pico SAPI5"
#define AppVersion "1.0.0"
#define AppPublisher "Pico SAPI5"
#define AppURL "https://github.com/alexoloopios/pico-sapi5"

[Setup]
AppId={{8F769291-D6D4-47AB-A9BB-5341ACB89225}
AppName={#AppName}
AppVersion={#AppVersion}
AppVerName={#AppName} {#AppVersion}
AppPublisher={#AppPublisher}
AppSupportURL={#AppURL}
DefaultDirName={autopf}\{#AppName}
DefaultGroupName={#AppName}
DisableProgramGroupPage=yes
OutputDir=..\dist
OutputBaseFilename=PicoSAPI5-{#AppVersion}-setup
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern
; The voice tokens live under HKEY_LOCAL_MACHINE, so there is no meaningful
; non-elevated installation to offer.
PrivilegesRequired=admin
; Install into the native Program Files and register the 64-bit engine where a
; 64-bit Windows can use it. The 32-bit engine is installed either way.
ArchitecturesInstallIn64BitMode=x64compatible
UninstallDisplayName={#AppName}
LicenseFile=..\LICENSE
InfoBeforeFile=before_install.txt
SetupLogging=yes

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Files]
; The 64-bit engine, for 64-bit applications. Only on a 64-bit Windows.
Source: "..\dist\PicoSAPI5.dll"; DestDir: "{app}"; \
    Flags: ignoreversion regserver 64bit restartreplace uninsrestartdelete; \
    Check: Is64BitInstallMode

; The 32-bit engine, for 32-bit applications. Registered in the 32-bit view, so
; on a 64-bit Windows its class lands under WOW6432Node where those
; applications look for it.
Source: "..\dist\PicoSAPI5_x86.dll"; DestDir: "{app}"; \
    Flags: ignoreversion regserver 32bit restartreplace uninsrestartdelete

; The lingware, shared by both engines.
Source: "..\dist\lang\*"; DestDir: "{app}\lang"; Flags: ignoreversion

; Tools for checking an installation after the fact.
Source: "..\dist\pico_speak.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\dist\pico_render.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\dist\pico_sapitest.exe"; DestDir: "{app}"; Flags: ignoreversion

Source: "..\README.md"; DestDir: "{app}"; Flags: ignoreversion isreadme
Source: "..\CHANGELOG.md"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\LICENSE"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\NOTICE"; DestDir: "{app}"; Flags: ignoreversion

[Icons]
Name: "{group}\Test the Pico voices"; Filename: "{app}\pico_speak.exe"; \
    Parameters: "--all-pico"; Comment: "Speaks a sample with each installed Pico voice"
Name: "{group}\Read me"; Filename: "{app}\README.md"
Name: "{group}\Uninstall {#AppName}"; Filename: "{uninstallexe}"

[Run]
; Offered rather than run silently: it speaks out loud, which is not something
; to do to someone without asking.
Filename: "{app}\pico_speak.exe"; Parameters: "--voice ""Pico American English"" ""The Pico voices are installed."""; \
    Description: "Speak a test phrase now"; Flags: postinstall nowait skipifsilent unchecked

[Messages]
FinishedLabel=Setup has installed the six Pico voices.%n%nApplications that were already running, including screen readers, generally need to be restarted before a newly installed voice appears in their voice list.
