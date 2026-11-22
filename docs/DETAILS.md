# Overlook: settings and internals

The [README](../README.md) covers installing and the main settings. This page has the rest: every setting group in detail, the command-line tools and how the clock works.

## Settings in detail

- Run `Overlook.exe`. On first run it writes `Overlook.ini` next to the exe; edits are applied as soon as the file is saved.
- Tray icon menu: Hide clocks, one checkbox per monitor (when there are several), Edit settings, Reload settings, Open folder, Start with Windows, Exit. If the icon is hidden in the overflow, drag it onto the taskbar. The menu saves its choices in `[Clock]`: `HideClocks=1`, and `HiddenMonitors=` (monitors listed by name, or `\\.\DISPLAYn` when two share a name). The per-monitor checkboxes appear whenever there are several monitors or one is hidden, so a hidden clock can always be brought back. A clock switched off with `Show=0` in a `[Monitor ...]` section is listed greyed out, because that setting lives in the .ini.
- **Monitors**: `Monitor=all`, one monitor (`top`, `primary`, `2`, `\\.\DISPLAY1`, part of its name), or a list such as `top,primary`. Every monitor uses the `[Clock]` settings. A `[Monitor <which>]` section overrides any of them for that monitor, for example:

  ```ini
  [Monitor primary]
  VerticalPosition=30
  ShowWeather=0
  ```

  These keys are app-wide and only read from `[Clock]`: `Monitor`, `WeatherCity`, `WeatherUnits`, `RainHours`, `NowPlayingApps`, `StartWithWindows`, `TrayIcon`, `HideClocks`, `HiddenMonitors`, `Log`. If several sections match a monitor, later ones win.
- **OLED care** (per monitor, off by default; set it in the OLED's `[Monitor ...]` section):
  - `PixelShift=1` moves the clock along a slow loop within `PixelShiftRange` pixels (default 4). It takes one step of at most 1 px every 10 minutes, and the step glides as part of the minute transition.
  - `NightDim=1` lowers the opacity to `NightOpacity` (default 45) between sunset and sunrise. The times come from the weather data, or from `NightStart`–`NightEnd` (20:00–07:00) when the weather line is off.
- **Now playing apps** (`NowPlayingApps`, app-wide): the media apps the now-playing line listens to, by part of the app's id (case and spaces ignored). The default covers Spotify, Apple Music, iTunes, TIDAL, Deezer, Amazon Music, Qobuz, YouTube Music, Pandora, foobar2000, MusicBee, AIMP, Winamp, Plexamp and Cider. Browsers and video players are left out, so a YouTube video doesn't show up; add a browser (e.g. `chrome`, `msedge`, `firefox`) to include it, or set `all`. When several apps have a session, one that is playing wins over a paused one.

## Command-line tools

- `Overlook.exe --weather-test [folder]` checks the weather line. It renders the line for made-up readings (every symbol by day and night, negative temperatures, °F, every extra, 12-hour time, polar night, stale or wrong-unit readings) and for a few real cities in other time zones. The results go to `weather-test-120.png`, `weather-test-192.png` and `weather-test.txt`. It doesn't touch a running clock or the weather cache.
  - `worst-case-120.png` / `-192.png` draw the whole clock with every weather extra and a long track title at three width limits.
  - `centring.txt` checks centring: every minute in 24-hour, 24-hour without a leading zero, and 12-hour AM/PM formats; every date of a year; and every weather and now-playing line above, at both DPIs. Anything more than 0.6 px off the centre line is marked UNEXPECTED.
  - `centring.txt` also checks the colon with `CentreOn=colon` in every minute, and reports how far the whole time then leans.
  - `time-centring-120.png` / `-192.png` compare `OpticalCentring` 0, 50 and 100 and `CentreOn=colon` on hard times.
- `Overlook.exe --preview [folder]` draws each monitor's clock over its wallpaper as `preview-N-now.png`, and as `preview-N-proposed.png` with the sizes 15% larger and the date's capitals at 9.5% of the screen height. `preview.txt` lists the settings each image used. `--wallpaper <image>` draws over that image instead (how `docs/screenshot.png` was made). It doesn't touch a running clock.
- `Overlook.exe --demo <folder> <dark image> <light image>` renders the README animations (a minute change, a wallpaper change, the weather through a day) as PNG frames (30 fps, half size) with the clock's own drawing and transition, over two background images, with default settings, made-up weather and a sample track. It writes `minute-NNN.png`, `wallpaper-NNN.png`, `weather-NNN.png` and `demo-still.png` (the screenshot). The GIFs were made with ffmpeg:

  ```
  ffmpeg -framerate 30 -i minute-%03d.png -vf "crop=760:240:260:30,split[a][b];[a]palettegen[p];[b][p]paletteuse=dither=sierra2_4a" -loop 0 demo-minute.gif
  ```

- `Overlook.exe --exit` closes a running instance. Launching it again while running reloads its settings.
- Keep `OFL.txt` (the Inter font licence) next to the exe.

## How it works

- **Fonts**: Inter Display Bold (time) and Medium (date), from the rsms/inter v4.1 release, are embedded as RCDATA. They are loaded privately with `IDWriteFactory5::CreateInMemoryFontFileLoader` → `IDWriteFontSetBuilder1` → a custom font collection, so nothing is installed system-wide.
- **Desktop layer**: on Windows 11 24H2+, Progman is the desktop window and has `WS_EX_NOREDIRECTIONBITMAP`. Each clock is a child of Progman, placed between `SHELLDLL_DefView` (icons) and the wallpaper `WorkerW`, which Progman creates when sent message `0x052C`. There is one clock window per monitor, each covering its monitor, stacked directly below the icons. Because it's part of the desktop, Win+D shows it. When Explorer restarts, the window is destroyed with Progman. It re-attaches on `TaskbarCreated`, and a once-a-minute health check also re-attaches it if needed. Older Windows builds use the classic WorkerW layout.
- **Rendering**: Direct2D/DirectWrite with grayscale anti-aliasing, presented through DirectComposition with a premultiplied-alpha surface. A layered window (`UpdateLayeredWindow`) can't be used here: Progman has no GDI redirection surface, so layered or GDI children inside it are never composited. The D3D device is WARP (software). It needs no GPU memory or device-loss handling, and drawing one frame a minute costs almost nothing.
- **Timing**: one coalescing-free timer set just past each minute boundary. It also redraws on resume, time changes, display or DPI changes, wallpaper changes and saved settings.
- **Pausing**: while every display is off (`GUID_CONSOLE_DISPLAY_STATE`) or the session is locked (`WTSRegisterSessionNotification`), nothing is drawn and the minute timer stops. Weather fetches due in that time wait too. When the screen comes back, the clocks are redrawn at once and any due fetch runs.
- **Centring**: every line is centred by its ink, the outline actually drawn, on the same vertical line.
  - Centring by the font's advance boxes would leave lines off by the uneven space beside glyphs. A trailing `1` moved the time up to 2.7 DIP, and a leading icon or trailing `°` moved the weather line.
  - After layout, each line is drawn on its own at the frame's DPI, measured, and moved so its ink is centred. Lines still start on whole pixels so a minute's unchanged digits land on the same pixels, which leaves at most half a pixel.
  - The time keeps Inter's proportional digits, so its colon moves a little with the digits. `TimeFeatures=tnum` gives equal-width digits instead, but Inter's tabular `1` has a foot.
  - `OpticalCentring` (0–100, default 0) moves the time toward its visual weight. A thin `1` or `7` makes some times look heavier on one side: 13:23 by about 4 DIP, 17:38 by about 12 DIP. 0 keeps the outline centred, in line with the other lines.
  - `CentreOn=colon` (per monitor; default `ink`) puts the time's colon on the centre line instead, and every other line stays centred on it. The colon never moves, and at a minute change only the changed digits slide. With proportional digits a lopsided time then sits off-centre: 04:11 leans about 44 px at 125% scaling (2560×1440) and 69 px at 200% (3840×2160). A 12-hour time with AM/PM leans far more, because the suffix sits on one side. `OpticalCentring` is then ignored.
- **Transition** (`TransitionMs`, default 500, 0 = off): when the minute changes, time characters that stay the same (e.g. `03:4` in 03:42 → 03:43) slide to their new centred position, and only the changed digits crossfade.
  - Each piece is pre-rendered once, with its shadow, and handed to DWM as a DirectComposition visual. Its offset and opacity are DirectComposition animations (smoothstep), so the compositor runs the transition in sync with each display refresh at the monitor's own rate (60 Hz, 240 Hz, …), with no per-frame work in this process.
  - The fade-out curve is shaped with the text opacity, `(1 − s) / (1 − a·s)`, so pixels covered by both fading layers blend linearly instead of dipping mid-fade.
  - Each text line starts on a whole pixel, so a sliding run lands pixel-identical to the next static frame.
  - The clock window covers its whole monitor and has no redirection bitmap, so it never moves during a transition.
  - A transition costs about 80 ms of CPU per minute for one clock (about 120 ms for two), all of it up front.
- **Weather** (`WeatherCity`, off while empty): a third line under the time, e.g. `[cloud] 12°  feels 9°  ↑15° ↓7°  [wind] 32 km/h NW  [umbrella] 60% at 15:00  [sunset] 17:21`.
  - Extras, each with its own toggle, appear only when they matter:
    - "feels" appears when the apparent temperature is `FeelsLikeDiff`° (default 2) or more away from the actual temperature.
    - Wind appears from `WindThreshold` (default 20 km/h, or mph with `WeatherUnits=F`), with the direction it blows from.
    - The next sunrise or sunset appears when it is less than `SunHours` away (default 3).
    - `UV 7` appears from `UVThreshold` (default 6).
    - Air quality appears from "poor": `Air poor`, `Air very poor` and `Air extremely poor` on the European index, or the US categories with `WeatherUnits=F`. It comes from Open-Meteo's air-quality API, a second request made with each fetch; if it fails, the weather still updates.
  - On a busy day the line is kept to `WeatherMaxWidth`% of the time's width (default 150, measured against the time with every digit as "0", so it doesn't change from minute to minute). Beyond that, extras give way in this order: sunrise/sunset, wind, UV, air quality, feels-like, rain. The temperature and high/low always stay. At 150, rain and feels-like fit together.
  - Sunrise, sunset and the rain hour are stored as UTC instants, using the location's UTC offset from Open-Meteo. They're shown in the PC's own time zone, so a city in another zone still gives correct times and night dimming.
  - A reading in other units than `WeatherUnits` (just after switching) is not shown until the new one arrives.
  - Sunrise and sunset use icons the app draws itself, a half sun on the horizon with an up or down arrow. They're centred on the cap height like the other symbols, and their horizontal strokes sit on whole pixels.
  - Data comes from [Open-Meteo](https://open-meteo.com) (free, no account).
  - The city is geocoded once and its coordinates cached in `Overlook.weather.ini`, which also keeps the last reading for after a restart.
  - Forecasts are fetched every 30 minutes with WinHTTP on a worker thread. They're also fetched 5 s after resume, and retried at 1, 2, 5, 10, then every 15 minutes while offline.
  - The line is hidden once the last reading is more than 3 hours old.
  - The icons are drawn by the app (`icons.cpp`) in one style: filled shapes and round strokes, with a small gap where one shape sits in front of another.
    - On a clear night the moon shows its current phase (one of 8), worked out locally and mirrored south of the equator.
    - Each weather code has its own icon. Mainly clear is a big sun with a small cloud; partly cloudy is a big cloud with the sun peeking out; overcast is two clouds. Fog, drizzle, rain, showers, snow and thunder each have their own mark under a cloud, and night uses the moon.
    - Wind and the rain umbrella are drawn the same way.
    - Each icon is a single unioned shape filled in the clock's tinted colour, centred on the cap height. It has the same padding on both sides, so the gaps around icons are always even.
  - The rain chance appears only when it reaches `RainThreshold`% within `RainHours`.
  - The line sits below the date and time block and doesn't affect its vertical centre. When it changes, it crossfades like the date.
  - Privacy: only the city name (once) and its coordinates are sent to open-meteo.com.
- **Now playing** (`NowPlaying`, default on): a line under the weather, e.g. `[note] Artist — Title`, from Windows' media controls, for the apps in `NowPlayingApps` (music apps by default). Titles lose the extras that crowd the line, such as `(feat. …)`, `(with …)`, `[From …]` and remaster notes. Versions that change the music, such as `(Live)`, `(Remix)` or `(Acoustic)`, stay. What's left is cut with "…" at `WeatherMaxWidth`.
  - A background thread in its own WinRT apartment subscribes to the media-session events (session, track and playback changes), with no polling. Each change crossfades the line straight away, whatever triggered it: a song ending, skip, play/pause from the keyboard, app or headset.
  - When playback pauses, the line fades to `NowPlayingPausedOpacity`% (default 50) and stays for `NowPlayingLinger` seconds (default 60), then fades out. Playing again within that time brings it back to full strength.
- `SecondaryOpacity` (default 100) sets the weather and now-playing lines' opacity as a % of `Opacity`. Below 100 the time stands out more, the way macOS keeps everything but the time dimmer.
- The tray menu has **Refresh weather**, which fetches at once.
- **Staying above the wallpaper**: changing the wallpaper makes Explorer recreate the wallpaper `WorkerW` on top of the clock. An out-of-context WinEvent hook on the desktop window's thread moves the clock back immediately. It also starts the colour fade early.
- **Auto-contrast**: reads the current wallpaper for that monitor via `IDesktopWallpaper`, mapping it with the fill, fit, stretch, tile, center or span mode. It averages perceived lightness (L\*) over the clock area and switches to `DarkColor` above `BrightThreshold`.
  - The area is the date and time block at the width of the time with every digit as "0", so the digits (or `LeadingZero`) can't change the result.
  - Once a clock has chosen light or dark text, it switches only when the wallpaper is 4 L\* past the threshold, so a wallpaper right at the threshold doesn't make it flip. A new wallpaper decides afresh.
- **Tint** (`Tint`, default 35, 0 = neutral): like the macOS lock screen, the text takes on the hue of the wallpaper behind it. The average wallpaper colour over the clock area is converted to OKLab. The text keeps its lightness, gets the wallpaper's hue, and gets a chroma scaled by `Tint` and by how colourful the wallpaper is, so grey wallpapers leave it neutral. Lightness is lowered only as much as needed to keep the colour displayable. For example, white over a dark purple night sky becomes lavender `#E2DFFE`.
- **Wallpaper changes**: when Windows starts a wallpaper change (its new wallpaper `WorkerW` appears), the clock waits until Windows reports the new image, usually within half a second, and fades to its new colour so that it finishes with Windows' own crossfade, about 1.2 s after the change began. Explorer's `%APPDATA%\Microsoft\Windows\Themes` folder is also watched, because slideshow changes aren't always broadcast. A repeated notification for the same change doesn't restart or cut the fade. Colour or tint edits in the .ini fade too, at `TransitionMs`. (A one-frame blurred flash at the end of a wallpaper change comes from Windows itself; it happens with the clock closed too.)
- **DPI**: the process is PerMonitorV2. Sizes are in DIPs and scaled by each monitor's own effective DPI (e.g. 125% and 200% side by side). Text is laid out with baselines snapped to whole pixels.
- **Several monitors**: each clock has its own window, DirectComposition target, settings, wallpaper sample and transition. They share one renderer and one weather reading. Adding or removing a monitor, or changing `Monitor=`, rebuilds the set of windows.
