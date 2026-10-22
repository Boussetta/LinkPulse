# LinkPulse v0.2.0 Release Checklist

## Maintainer Build

Run on a native Windows machine with Visual Studio Build Tools and Inno Setup installed:

```powershell
powershell -ExecutionPolicy Bypass -File .\packaging\build-installer.ps1
```

Confirm that CMake configuration, the MSVC Release build, all tests (ctest), and Inno Setup compilation succeed.

For moving the repository between WSL and native Windows, including MSVC debugger setup and
runtime diagnostics, see [development-handoff.md](development-handoff.md).

## Code Signing

The in-app updater only runs installers with a valid Authenticode signature; for an
unsigned installer it opens the GitHub release page instead. Releases must therefore be
signed. `packaging\sign-file.ps1` signs `linkpulse-tray.exe` before Inno Setup runs and
`LinkPulseSetup.exe` afterwards, using SHA-256 and an RFC 3161 timestamp. Both the release
workflow and `build-installer.ps1` call it. Without a certificate it warns and leaves the
files unsigned.

To enable signing in GitHub Actions:

1. Obtain an Authenticode code-signing certificate (OV or EV) as a `.pfx` file.
2. Encode it: `[Convert]::ToBase64String([IO.File]::ReadAllBytes("cert.pfx")) | Set-Clipboard`.
3. In the repository's **Settings → Secrets and variables → Actions**, add the secrets
   `WINDOWS_SIGNING_CERT_PFX_BASE64` (the encoded file) and `WINDOWS_SIGNING_CERT_PASSWORD`.
4. Optionally add the repository **variable** `LINKPULSE_UPDATE_SIGNER` with the
   certificate's exact subject display name. Release builds then reject update installers
   signed by anyone else. Local builds can pass `-DLINKPULSE_UPDATE_SIGNER="..."` to CMake.

For local builds, set the same two environment variables before running
`build-installer.ps1`.

A release run without the secrets succeeds but shows an "Unsigned release" warning in the
workflow summary.

EV certificates on hardware tokens cannot be exported to a `.pfx`. In that case, use
[Azure Trusted Signing](https://learn.microsoft.com/azure/trusted-signing/): replace the
two signing steps with the `azure/trusted-signing-action` and keep the same files.

## Clean-Machine Validation

Copy `build\installer\LinkPulseSetup.exe` to a Windows machine without Visual Studio, CMake, MinGW, or Inno Setup installed. Verify:

- Installer runs and installs without administrator privileges.
- Start Menu shortcut launches `linkpulse-tray.exe` without a console window.
- Tray graph, tooltip, pause/resume, units, and Start with Windows work.
- Reboot starts LinkPulse when autostart is enabled.
- `%LOCALAPPDATA%\LinkPulse\LinkPulse.log` is created.
- Existing configuration survives an update installation.
- An update notification appears when a newer GitHub Release exists.
- `Get-AuthenticodeSignature build\installer\LinkPulseSetup.exe` reports `Valid`.
- Clicking the notification body downloads and launches the installer without opening a browser.
- Right-clicking **Download update** uses the same internal downloader.
- Uninstall removes the application, shortcuts, protocol registration, COM registration, and autostart entry.
- Native network validation shows the local computer, gateway, and currently visible devices in the network map.
- A quiet Wi-Fi device may be absent from the Windows ARP/NDP cache; gateway-adapter validation is required for complete client inventory.

## Publish

After the clean-machine check passes, publish from `main`:

```powershell
git checkout main
git pull
git tag v0.2.0
git push origin v0.2.0
```

The GitHub Actions `Release` workflow builds the versioned MSVC installer, runs tests, and attaches `LinkPulseSetup.exe` to the GitHub Release.

## Screenshots

Capture the following for release notes and future Store preparation:

- Installed tray graph in light theme
- Installed tray graph in dark theme
- Native update notification with its **Download update** action
- Installer welcome or license page
- Start Menu shortcut and installed app
