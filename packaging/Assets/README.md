# Placeholder Store assets

These tile/logo images are **programmatically generated placeholders**
(solid brand-color background + "LP" text), not final designed assets.
They exist so `packaging/Package.appxmanifest` references valid files and
the packaging script produces an installable MSIX for local testing.

Before a real Store submission, replace all of these with properly
designed artwork (see the [Microsoft tile/icon asset guidelines](https://learn.microsoft.com/windows/apps/design/style/iconography/app-icon-construction)):

| File                    | Size    | Notes                                   |
|-------------------------|---------|------------------------------------------|
| `Square44x44Logo.png`   | 44x44   | App list icon, transparent background   |
| `Square150x150Logo.png` | 150x150 | Medium tile, transparent background     |
| `Wide310x150Logo.png`   | 310x150 | Wide tile, transparent background       |
| `StoreLogo.png`         | 50x50   | Store listing logo, opaque background   |
| `SplashScreen.png`      | 620x300 | Launch splash screen                    |
