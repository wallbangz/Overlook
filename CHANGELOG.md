# Changelog

What changed in each version of Overlook. Each release on GitHub shows its section of this file.

## [Unreleased]

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

[Unreleased]: https://github.com/wallbangz/Overlook/compare/v0.2.1...HEAD
[0.2.1]: https://github.com/wallbangz/Overlook/compare/v0.2.0...v0.2.1
[0.2.0]: https://github.com/wallbangz/Overlook/compare/v0.1.0...v0.2.0
[0.1.0]: https://github.com/wallbangz/Overlook/releases/tag/v0.1.0
