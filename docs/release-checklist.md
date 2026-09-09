# LinkPulse v0.1.3 Release Checklist

## Maintainer Build

Run on a native Windows machine with Visual Studio Build Tools and Inno Setup installed:

```powershell
powershell -ExecutionPolicy Bypass -File .\packaging\build-installer.ps1
```

Confirm that CMake configuration, the MSVC Release build, all tests (ctest), and Inno Setup compilation succeed.

For moving the repository between WSL and native Windows, including MSVC debugger setup and
runtime diagnostics, see [development-handoff.md](development-handoff.md).

## Clean-Machine Validation

Copy `build\installer\LinkPulseSetup.exe` to a Windows machine without Visual Studio, CMake, MinGW, or Inno Setup installed. Verify:

- Installer runs and installs without administrator privileges.
- Start Menu shortcut launches `linkpulse-tray.exe` without a console window.
- Tray graph, tooltip, pause/resume, units, and Start with Windows work.
- Reboot starts LinkPulse when autostart is enabled.
- `%LOCALAPPDATA%\LinkPulse\LinkPulse.log` is created.
- Existing configuration survives an update installation.
- An update notification appears when a newer GitHub Release exists.
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
git tag v0.1.3
git push origin v0.1.3
```

The GitHub Actions `Release` workflow builds the versioned MSVC installer, runs tests, and attaches `LinkPulseSetup.exe` to the GitHub Release.

## Screenshots

Capture the following for release notes and future Store preparation:

- Installed tray graph in light theme
- Installed tray graph in dark theme
- Native update notification with its **Download update** action
- Installer welcome or license page
- Start Menu shortcut and installed app
