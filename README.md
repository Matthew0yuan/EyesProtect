# EyesProtect

A tiny Windows tray app for the **20-20-20 rule**: after 20 minutes of screen time, look at something far away (the trees outside) for a few seconds.

- **Tiny**: one ~22 KB exe with no runtime or installer, about 2 MB of memory, and close to 0% CPU.
- **Knows when you're actually at the screen**: idle time, locking the PC and sleep all count as breaks.
- **Stays out of your way**: a small pixel-art widget shows the minutes left, and the break card never takes keyboard focus.

## How it works

1. **Counting screen time.** About once a minute the app asks Windows when the last keyboard, mouse, touch or pen input happened (`GetLastInputInfo`). It doesn't install keyboard hooks and never sees what you type.
   - Input within the last 5 minutes: you're using the screen, so the minute counts. Reading without touching anything still counts.
   - No input for 5 minutes or more: you stepped away. That's a natural break, so the counter resets.
   - A fullscreen video or game in front: the minute still counts, even without input.
   - Locking the PC or putting it to sleep: the counter stops, and starts a fresh 20 minutes when you come back.
2. **The break.** At 20 minutes a card appears in the bottom-right corner: *Look out at the trees*.
   - The 10-second countdown only runs while your hands are off the keyboard and mouse. Touching anything sends it back to 10.
   - A chime plays when it's done, so you know when to look back.
   - **Skip** ends the break early.
   - The card waits while an exclusive-fullscreen game or presentation is running.
3. **The widget.** A small pixel-art widget: a pixel tree, the minutes left in a 3×5 pixel font, and a game-style health bar that drains as the minutes pass.
   - In the last 3 minutes the tree's leaves turn autumn orange and the bar turns red.
   - It shows `REST` during a break and `HOLD` while paused (the tree turns gray).
   - It hides itself over fullscreen apps and comes back as soon as you leave them.

## Using it

Run `EyesProtect.exe`. A tree icon appears in the tray. Windows 11 may put it in the `^` overflow; drag it onto the taskbar to keep it visible.

| Action | Widget | Tray icon |
|---|---|---|
| See time left | always shown | hover |
| Take a break now | double-click | double-click |
| Menu | right-click | right-click |
| Move | drag (position is remembered) | — |

The menu has **Take a break now**, **Pause for 1 hour / Resume**, **Show widget**, **Start with Windows** and **Exit**.

## Settings (optional)

Create `EyesProtect.ini` next to the exe. The app never creates this file itself.

```ini
[EyesProtect]
WorkMinutes=20     ; screen time before a break  (1-240)
BreakSeconds=10    ; hands-off seconds per break (3-300)
IdleMinutes=5      ; no input this long = a break (1-60)
```

`EyesProtect.exe /test` uses 30 s of work and a 15 s idle reset, so you can try the full cycle quickly.

## Building

You need Visual Studio 2019 or later with the **Desktop development with C++** workload (MSVC + Windows SDK). Then run:

```bat
build.cmd
```

The script finds MSVC with `vswhere`, then builds `EyesProtect.c` without the C runtime (`/NODEFAULTLIB`, custom entry point), optimized for size. The exe only depends on DLLs that ship with Windows.

## Why it's light

- Plain C on the Win32 API in a single file. There's no framework or C runtime, and only one thread.
- That thread sleeps in `GetMessage` and wakes about once a minute. The tick uses a coalescable timer, so Windows can batch it with other wakeups.
- While the PC is locked or asleep it doesn't wake at all.
- Lock/unlock and sleep/resume arrive as Windows events, so the app never checks for them in a loop.
- The break card exists only during a break. The widget is redrawn only when its content changes, and it reacts to foreground-window changes through an event hook instead of a timer.

## Privacy

- There are no keyboard or mouse hooks, no camera, and no network access.
- The only data it reads is the *time* of the last input.
- It writes only these registry values:
  - `HKCU\Software\EyesProtect`: widget position and whether the widget is shown.
  - `HKCU\Software\Microsoft\Windows\CurrentVersion\Run\EyesProtect`: only if you turn on **Start with Windows**.

## Uninstall

1. Untick **Start with Windows**.
2. Choose **Exit**.
3. Delete the exe.
4. Optionally delete `HKCU\Software\EyesProtect`.

## Known limitations

- A video that isn't fullscreen, watched for 5+ minutes without touching anything, counts as a break.
- Gamepad input isn't seen by Windows' idle timer. A non-fullscreen game played only with a controller looks like idle time.

## License

MIT, see [LICENSE](LICENSE).
