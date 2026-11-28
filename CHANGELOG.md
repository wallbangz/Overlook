# Changelog

What changed in each version of Overlook. Each release on GitHub shows its section of this file.

## [Unreleased]

### Added
- **Run over Remote Desktop** (`WeatherEffectsOverRemote`, off by default; Settings › Weather): weather effects keep playing in a Remote Desktop session instead of pausing.

## [0.4.0-beta.2] - 2026-11-26

A beta with the 0.3.2 fix; the weather effects are as in beta.1.

### Fixed
- **Settings weren't kept when Overlook was in a read-only folder** such as `C:\Program Files`. When the exe's folder can't be written, the settings, log and caches now go to `%APPDATA%\Overlook`, and **Open folder** and **Open log folder** open it (from 0.3.2; #2, thanks @tjsuominen).

## [0.3.2] - 2026-11-26

### Fixed
- **Settings weren't kept when Overlook was in a read-only folder** such as `C:\Program Files`: no `Overlook.ini` was created, the settings window couldn't save, and **Edit settings file** did nothing. Overlook still keeps its files beside the exe when it can. When that folder can't be written, it now uses `%APPDATA%\Overlook`, copying an `Overlook.ini` found beside the exe there once. **Open folder** opens whichever folder is in use. (#2, thanks @tjsuominen)

## [0.4.0-beta.1] - 2026-11-25

A beta for trying the weather effects; settings and the ini work as in 0.3.1.

### Added
- **Weather effects** (`WeatherEffects`, off by default; Settings › Weather): soft clouds, fog, rain, freezing rain and sleet, thunderstorms with lightning glows, and snow drift over the wallpaper, below the clock and icons. They follow the current weather and wind, are fainter and bluer at night, and are animated by Windows, so Overlook uses almost no CPU while they run. Choose the real weather or one effect, the opacity, and whether lightning shows. They need a weather city.
- Effects pause behind maximised or full-screen windows, on battery (unless **Run on battery** is on), in Energy Saver, with Windows animations turned off, in remote desktop sessions, and while the screen is off or locked.
- **Troubleshooting** on the About page: a diagnostic log, with place names, wallpaper names and track titles masked unless **Include private details** is on.

### Changed
- The log carries the full date on each line, starts with the version, Windows build and monitors, and rolls over to `Overlook.log.1` at 1 MB.

### Testing the beta
- **Ctrl+Shift+D** in the settings window opens a hidden **Debug** page: simulate any weather, change the wind and the amount, preview the night look, keep effects running when they would pause, and **Copy report** for an issue. A **DEBUG MODE** label shows in the settings window and under each clock while it is on; Ctrl+Shift+D again turns it off.
- To hear about later betas, add `UpdateChannel=beta` under `[Clock]` in `Overlook.ini`.

## [0.3.1] - 2026-11-25

### Added
- **Update check** (`CheckForUpdates`, on by default; Settings › About). Once a day, Overlook asks GitHub whether a newer release is out. If one is, the tray menu shows **Update available**, a one-time notification says so, and the About page offers **Get the latest release**, which downloads `Overlook.zip` in your browser, and **What's new**. It never downloads or installs anything by itself. `UpdateChannel=beta` also counts pre-releases.

## [0.3.0] - 2026-11-23

### Added
- **Adapt to busy wallpapers** (`AdaptiveLegibility`, on by default; Settings › Look). On a busy wallpaper, such as bright sky through dark leaves, the clock strengthens its shadow and brings the weather and now-playing lines up to full opacity, as much as that wallpaper needs. If the text still clashes, it picks whichever of light or dark text reads better. Calm wallpapers look exactly as before.

## [0.2.1] - 2026-11-22

### Fixed
- Settings window, high contrast themes: the selected page and the selected item in a drop-down list are drawn in the theme's highlight colours, so their text is readable. Hovered pages get an outline, and the colour swatch's border follows the theme.

## [0.2.0] - 2026-11-22

### Added
- **Settings window**: tray menu › Settings…, or `Overlook.exe --settings`. Pages for Clock, Look, Weather, Now playing, Monitors and About, styled like Windows 11 Settings. It follows light, dark and high contrast, stays sharp at any display scaling, works from the keyboard and with screen readers.
- Each change is saved to `Overlook.ini` and applied at once. Comments and settings the window doesn't show are kept, and **Undo changes** puts the file back as it was when the window opened.
- **Settings for** edits one monitor's own settings, with **Reset** for each one. Under "All monitors", a setting a monitor keeps for itself says so, with **Use for all**.
- About page with the version, links to the source code, releases and issue forms, and the credits.
- Issue forms for bug reports and feature requests.

### Changed
- The exe's version information now matches the release.

## [0.1.0] - 2026-11-22

First public release.

### Added
- The date over a large time, centred on each monitor, in the style of the macOS lock screen. It sits on the desktop below the icons and every window.
- Light or dark text chosen from the wallpaper, tinted toward its colour. It fades along with Windows' own wallpaper crossfade.
- Smooth minute changes, run by the Windows compositor at the display's refresh rate.
- Weather line from Open-Meteo, no account or API key. Extras appear only when they matter: feels-like, wind, the hour rain is likely, UV, air quality, sunrise or sunset, and the moon's phase at night.
- Now playing from music apps, dimmed while paused.
- A clock on each monitor, with per-monitor settings.
- OLED care: slow pixel shift and night dimming.
- Pauses while the screen is off or locked; tray menu to hide the clocks or pick monitors; Start with Windows.

[Unreleased]: https://github.com/wallbangz/Overlook/compare/v0.4.0-beta.2...HEAD
[0.4.0-beta.2]: https://github.com/wallbangz/Overlook/compare/v0.4.0-beta.1...v0.4.0-beta.2
[0.4.0-beta.1]: https://github.com/wallbangz/Overlook/compare/v0.3.1...v0.4.0-beta.1
[0.3.2]: https://github.com/wallbangz/Overlook/compare/v0.3.1...v0.3.2
[0.3.1]: https://github.com/wallbangz/Overlook/compare/v0.3.0...v0.3.1
[0.3.0]: https://github.com/wallbangz/Overlook/compare/v0.2.1...v0.3.0
[0.2.1]: https://github.com/wallbangz/Overlook/compare/v0.2.0...v0.2.1
[0.2.0]: https://github.com/wallbangz/Overlook/compare/v0.1.0...v0.2.0
[0.1.0]: https://github.com/wallbangz/Overlook/releases/tag/v0.1.0
