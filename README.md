# Overlook

A clock for the Windows 11 desktop in the style of the macOS lock screen. It sits on your wallpaper, below the desktop icons and every window, and takes its colour from the wallpaper behind it.

![The clock on a dark gradient wallpaper: date, time, a weather line and a now-playing line](docs/screenshot.png)

Overlook is new. If you try it, please [tell me how it went](#feedback).

## Features

- **Lock-screen look**: the date over a large time, centred on each monitor.
- **Matches your wallpaper**: light or dark text chosen automatically, tinted toward the wallpaper's colour, and updated when the wallpaper or slideshow changes.
- **Smooth minute changes**: digits that stay slide into place and new ones fade in, at your display's refresh rate.
- **Weather**: temperature and high/low, plus extras only when they matter (feels-like, wind, rain, UV, air quality, sunrise or sunset). No account or API key.
- **Now playing**: the artist and track from Spotify and other music apps, dimmed while paused.
- **Several monitors**: a clock on each, with its own settings, sharp at any display scaling.
- **OLED care**: an optional slow pixel shift and night dimming.
- **Light**: one small exe with no installer. It sleeps between minutes and pauses while the screen is off or locked.

| The minute changes | The wallpaper changes |
|---|---|
| ![14:19 turns into 14:20: the 14: slides into place while 19 fades into 20](docs/demo-minute.gif) | ![The wallpaper fades from dark to light and the clock fades from light to dark text with it](docs/demo-wallpaper.gif) |

## Install

1. Download `Overlook.zip` from [Releases](../../releases) and unzip it to a folder you'll keep, e.g. `%LOCALAPPDATA%\Overlook`.
2. Run `Overlook.exe`. It isn't code-signed yet, so Windows SmartScreen may warn the first time: choose **More info**, then **Run anyway**.
3. To start it with Windows, tick **Start with Windows** in its tray menu.

Needs Windows 11 (x64). To remove it, choose **Exit** in the tray menu and delete the folder.

## Settings

Right-click the tray icon and choose **Edit settings**. Changes apply as soon as you save the file. The ones you'll most likely want:

| Setting | What it does | Default |
|---|---|---|
| `Monitor` | Which monitors get a clock: `all`, `primary`, `top`, `2`, … | `all` |
| `VerticalPosition` | Height on screen, % from the top | `16.5` |
| `TimeSize`, `DateSize` | Text sizes | `145`, `32` |
| `Opacity`, `Tint` | Text opacity, and how much it takes the wallpaper's colour | `80`, `35` |
| `Hour24`, `DateFormat` | 12 or 24-hour time, date layout | `1`, `dddd, MMMM d` |
| `WeatherCity`, `WeatherUnits` | Your city (empty = no weather), `C` or `F` | empty, `C` |
| `NowPlaying`, `NowPlayingApps` | The now-playing line, and which apps it listens to | on, music apps |
| `PixelShift`, `NightDim` | OLED care | off |

Any setting can be different on one monitor:

```ini
[Monitor primary]
VerticalPosition=30
ShowWeather=0
```

Every setting is explained in the file itself and in [docs/DETAILS.md](docs/DETAILS.md).

## Feedback

Overlook has only been tested on a few setups so far, so reports from yours are very welcome:

- **Something looks wrong?** [Open an issue](../../issues/new) with a screenshot, your Windows version and your display scaling.
- **Missing something?** Feature requests are welcome too, from small tweaks to new lines of information.
- **Want to help?** Pull requests are welcome. For bigger changes, open an issue first so we can agree on the approach.

## How it works

- It's a window inside the Windows desktop itself, between the wallpaper and the icons. So it never covers your apps, and it shows when you press Win+D.
- Text is drawn with Direct2D and DirectWrite, using the Inter typeface built into the exe. The minute animation runs in the Windows compositor, so the app does no work while it plays.
- The text colour comes from the part of the wallpaper behind the clock.
- Each line is centred by its visible outline rather than by font measurements. A built-in self-test checks every minute of the day and every date of a year.
- Weather comes from [Open-Meteo](https://open-meteo.com) every 30 minutes. Only your city name (once) and its coordinates are sent.
- Now playing listens to Windows' media controls, the same source as the volume flyout, with no polling.

More detail: [docs/DETAILS.md](docs/DETAILS.md).

## Build

Run `build.cmd`. It needs Visual Studio 2022 or later (any edition, or the Build Tools) with **Desktop development with C++**, and finds it by itself. The output is `bin\Overlook.exe`. GitHub Actions builds every push and attaches a zip to each `v*` release.

## Credits and licence

- Code: MIT licence, see [LICENSE](LICENSE).
- [Inter](https://rsms.me/inter/) typeface by Rasmus Andersson, under the SIL Open Font License 1.1 ([fonts/OFL.txt](fonts/OFL.txt)).
- [Weather data by Open-Meteo.com](https://open-meteo.com/), licensed under [CC BY 4.0](https://creativecommons.org/licenses/by/4.0/). Their free API is for non-commercial use.
- Inspired by the macOS lock screen; not affiliated with Apple.
