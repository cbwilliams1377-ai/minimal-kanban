# Minimal Kanban — Agent Reference

## Overview

A minimal, self-contained Kanban board for Windows. Single-file C++ application (~750 lines) using raw Win32 API — no frameworks, no external dependencies. Dark-themed UI with three columns (Todo, In-Progress, Complete), drag-and-drop card movement, right-click context menus (Edit/Delete), per-card blocked flag and stopwatch timer, and persistence via a hand-rolled JSON file.

## Tech Stack

- **Language**: C++17
- **UI**: Raw Win32 API (GDI painting, window messages)
- **Compiler**: MinGW g++ (invoked via `build.bat`)
- **Dependencies**: None beyond Windows system libraries (`gdi32`, `user32`, `shell32`, `ole32`, `uuid`)
- **Data format**: Hand-rolled JSON (no JSON library)

## Build

Run `build.bat` from the workspace directory (Windows, MinGW), or `build.sh` on
Linux/macOS (cross-compiles the Windows .exe with mingw-w64):

```bat
ffmpeg -i check-square.png -vf scale=256:256 check-square.ico
windres minimal_kanban.rc minimal_kanban-res.o
g++ minimal_kanban.cpp minimal_kanban-res.o -o MinimalKanban.exe -std=c++17 -O2 -s -mwindows -static -static-libgcc -static-libstdc++ -municode -lole32 -lshell32 -lgdi32 -luser32 -luuid
```

Produces `MinimalKanban.exe` (statically linked, no runtime DLLs needed).

## File Structure

| File | Purpose |
|------|---------|
| `minimal_kanban.cpp` | Entire application source (single file) |
| `minimal_kanban.rc` | Resource file — embeds `check-square.ico` as icon (ID 101) |
| `minimal_kanban-res.o` | Compiled resource object (intermediate, from `windres`) |
| `build.bat` | Build script (Windows) |
| `build.sh` | Build script (Linux/macOS, cross-compiles with mingw-w64) |
| `check-square.ico` | Application icon (generated from PNG) |
| `check-square.png` | Source icon image |
| `check-square.svg` | Vector source of the icon |
| `MinimalKanban.exe` | Built executable (git-ignored) |
| `reports/` | BugBot report queue (`queue/`, `blocked/`, `done/`); git-ignored |
| `.bugbot/` | BugBot config, driver contract, launcher + runner scripts (see below) |

## Source Map (`minimal_kanban.cpp`)

### Data Structures & Globals (lines 14–50)

- `Card` — struct: `{ std::wstring text; int column; bool blocked; LONGLONG timerAccumulated; LONGLONG sessionAccumulated; LONGLONG timerStart; }` where column is 0 (Todo), 1 (In-Progress), or 2 (Complete). `timerAccumulated` is completed time from before the current program run; `sessionAccumulated` is this run's paused-session time (not persisted, resets on close); `timerStart` is a QPC timestamp when running (0 = stopped).
- `g_cards` — `vector<Card>`, the entire board state in memory.
- `g_font` / `g_boldFont` — Segoe UI 16pt normal/semibold, created at startup.
- `g_dragIndex` — index of card being dragged (-1 = none).
- `g_dragPoint` — mouse position during drag.
- `g_lastMouse` — last known mouse position, drives hover-based keyboard actions.
- `MAIN_CLASS` / `INPUT_CLASS` / `TIME_CLASS` — Win32 window class names.
- `TITLES[3]` — column header strings.
- `g_qpcFreq` — QPC frequency for stopwatch timing.
- `g_liveTimerID` — Win32 timer ID (100ms tick) driving live stopwatch updates.
- `CMD_EDIT`/`CMD_DELETE`/`CMD_TOGGLE_TIMER`/`CMD_EDIT_TIMER`/`CMD_BLOCKED` — context menu command IDs.
- `MenuItemData` — owner-drawn context menu item struct (label + keyboard hint).
- `TIMER_LIVE`, `CARD_HEIGHT` (60), `CARD_SPACING` (66) — constants.

### Data Path (lines 27–39)

- `DataPath()` — returns `%LOCALAPPDATA%\MinimalKanban\board.json`. Creates the directory if it doesn't exist. Falls back to `board.json` in CWD if SHGetKnownFolderPath fails.

### String Conversion (lines 42–58)

- `Utf8(wstring)` — UTF-16 → UTF-8 via `WideCharToMultiByte`.
- `Wide(string)` — UTF-8 → UTF-16 via `MultiByteToWideChar`.

### JSON Helpers (lines 130–228)

- `JsonEscape(string)` — escapes `\`, `"`, `\n`, `\r`, `\t` for JSON strings.
- `JsonUnescape(string)` — reverses the above.
- `SaveCards()` — writes all cards to `board.json`. Writes `TotalElapsedMs` per card (persisted total, folding in the whole current run: paused-session time + any in-flight stretch) and does **not** stop running timers in memory — so a live stopwatch survives mid-session saves. `column`, `text`, `blocked`, `timer_ms` per card.
- `LoadCards()` — line-by-line parser that reads the format produced by `SaveCards`. Expects one `{"column": N, "text": "...", "blocked": true/false, "timer_ms": N}` per line. `blocked` and `timer_ms` are optional (default false/0) for backward compatibility with older save files. Silently skips malformed lines.

### Timer Helpers (lines 74–139)

- `NowMs()` — current time in milliseconds via QPC.
- `SessionElapsedMs(Card&)` — milliseconds of the current running session (0 when stopped).
- `TotalElapsedMs(Card&)` — total elapsed ms (accumulated + this run's paused sessions + active session if running); used for persistence and edit-timer pre-population.
- `FormatTimer(ms)` — formats to `ss`, `m:ss`, or `h:mm:ss` depending on magnitude.
- `StartLiveTimer(HWND)` / `StopLiveTimer(HWND)` — manage the 100ms `WM_TIMER` tick.
- `StopAllTimers()` — finalizes all running card timers into `sessionAccumulated` (currently unused; saves go through `TotalElapsedMs`).
- `AnyTimerRunning()` — true if any card has a live timer.

### Card Action Helpers (lines ~540–558)

- `ToggleTimer(HWND, index)` — shared start/pause logic for the stopwatch (used by Shift+click, `S` key, and menu). Pausing freezes the in-flight stretch into `sessionAccumulated` (not `timerAccumulated`), so the live countup keeps its value.
- `ToggleBlocked(HWND, index)` — shared blocked-flag toggle (used by Ctrl+click, `B` key, and menu).
- `HoverIndex(client, point)` — returns the card index under a client-space point, or -1.

### GDI Drawing Helpers (lines 229–276)

- `Fill(HDC, RECT, COLORREF)` — fills a rectangle with a solid color brush.
- `ColumnRect(RECT client, int column)` — computes the screen rectangle for a column. Layout: 16px margin, 10px gap, 16px top offset.
- `AddRect(RECT col)` — the "Add a card" clickable area at the bottom of column 0.
- `SetDarkTitleBar(HWND)` — dynamically loads `dwmapi.dll` to enable dark title bar (DWMWA_USE_IMMERSIVE_DARK_MODE). Graceful fallback on older Windows.
- `CardRects(RECT client)` — returns all card screen rectangles (60px height, 66px spacing). Each card fits 10px inset from column edges.

### Add-Card Dialog (lines 278–395)

- `InputState` — tracks dialog state: edit control handle, done/accepted flags, result text, initial value for edit mode.
- `InputProc()` — window procedure for the add-card dialog. Handles WM_CREATE (creates edit + owner-drawn buttons), WM_DRAWITEM (dark-themed buttons), WM_COMMAND (Add/Cancel), dark theme painting.
- `AskForCard(HWND owner, wstring& out, wstring initial = L"")` — shows the dialog as a modal loop (disables owner window, pumps messages until dialog closes). Pre-populated with `initial` for edit mode. Returns true if user accepted.
- `AddCard(HWND)` — calls AskForCard, appends new card to column 0, saves, redraws.

### Timer Entry Dialog (lines 407–521)

- `TimeInputState` — tracks time dialog state.
- `TimeInputProc()` — window procedure for the "Set timer" dialog (uses its own class, `TIME_CLASS`).
- `ParseTimeString(wstring)` — parses `ss`, `m:ss`, or `h:mm:ss` into milliseconds; returns -1 on failure.
- `AskForTime(HWND, int cardIndex)` — shows the manual time entry dialog, pre-populated with current total time. On accept, sets the card's `timerAccumulated` and resets `sessionAccumulated` (so the run countup starts fresh); timer ends stopped.

### Main Window Procedure (lines ~560–790)

Handles all main board interactions:

- **WM_KEYDOWN** — Ctrl+N triggers AddCard. Hover-based actions on the card under `g_lastMouse`: **Space** (edit), **Delete / D** (delete), **S** (toggle stopwatch), **T** (edit timer), **B** (toggle blocked).
- **WM_LBUTTONDOWN** — on a card: **Shift+click** toggles stopwatch, **Ctrl+click** toggles blocked, plain click starts a drag. Click on the Add button adds a card.
- **WM_MOUSEMOVE** — tracks `g_lastMouse`; updates dragged-card ghost while a drag is active.
- **WM_LBUTTONUP** — drops card into whichever column the mouse is over.
- **WM_RBUTTONUP** — owner-drawn dark context menu on the card under the pointer: **Edit** (space), **Delete** (d), **Toggle Timer** (s), **Edit Timer** (t), **Blocked** (b). Wires to the same helpers as keyboard/mouse paths.
- **WM_MEASUREITEM** / **WM_DRAWITEM** — owner-drawn popup menu sizing/painting: black bg RGB(27,27,27), white label, gray right-aligned key hint, hover highlight RGB(70,70,70).
- **WM_CAPTURECHANGED** — cancels drag if capture lost.
- **WM_TIMER** — 100ms live tick: invalidates the window while any stopwatch is running; kills the timer when none are.
- **WM_SIZE** — triggers full redraw.
- **WM_ERASEBKGND** — returns 1 (all painting happens in WM_PAINT).
- **WM_PAINT** — double-buffered painting:
  - Background RGB(27,27,27), column backgrounds RGB(39,39,39), headers RGB(43,43,43).
  - Cards RGB(50,50,50). **Blocked** cards get a thick red (RGB(220,50,50)) 3px outline. **Running-timer** cards get a thick green (RGB(50,180,50)) 3px outline. Blocked takes drawing priority over green.
  - Task text in top ~30px of card. Timer row in the bottom ~30px: **total** time left-justified in gray (RGB(140,140,140)), plus the **live countup** right-justified in green (RGB(50,180,50)) with a `+` prefix. The countup shows while running and stays frozen on the last value when paused; it only resets when the program closes (closing/saving appends the run to the total).
- **WM_DESTROY** — stops live timer, saves (the whole current run is folded into the persisted total; it restores stopped with `sessionAccumulated` reset), frees fonts, posts quit.

### Entry Point (lines ~790–830)

- `wWinMain()` — initializes COM, queries QPC frequency, creates fonts, registers window classes (main, input, time input), loads cards, creates main window (920×520), enables dark title bar, runs message loop.

## Data Format

`%LOCALAPPDATA%\MinimalKanban\board.json`:

```json
{
  "cards": [
    {"column": 0, "text": "Buy groceries", "blocked": false, "timer_ms": 0},
    {"column": 1, "text": "Write docs", "blocked": false, "timer_ms": 90000},
    {"column": 2, "text": "Ship v1.0", "blocked": true, "timer_ms": 5435000}
  ]
}
```

- `column`: integer 0, 1, or 2
- `text`: UTF-8 string with JSON escaping
- `blocked`: true/false (optional, defaults false)
- `timer_ms`: accumulated stopwatch time in milliseconds (optional, defaults 0)

## Card Interactions

| Action | Mouse | Keyboard (hover over card) |
|--------|-------|----------------------------|
| Move card between columns | Drag (plain click) | — (mouse-only) |
| Toggle stopwatch start/stop | Shift+left-click | **S** |
| Toggle blocked flag (red outline) | Ctrl+left-click | **B** |
| Edit card text | Right-click → Edit | **Space** |
| Delete card | Right-click → Delete | **Delete** or **D** |
| Set stopwatch time manually | Right-click → Edit Timer | **T** |
| Add new card | Click "+ Add a card" | **Ctrl+N** |

Hover-based keyboard actions use the card under the last known mouse position; they no-op if the cursor isn't over a card.

## Conventions

- **Single file** — all code lives in `minimal_kanban.cpp`. No header files, no separate modules.
- **Global state** — prefixed with `g_` (e.g., `g_cards`, `g_font`).
- **No external libraries** — pure Win32 API only. Dynamically load optional DLLs (`dwmapi.dll`, `uxtheme.dll`) with manual `GetProcAddress` for backward compatibility.
- **UTF-16 internally, UTF-8 for files** — all UI strings are `std::wstring`. JSON files are UTF-8.
- **Dark theme** — hardcoded RGB values, no theming system. Popup menus are owner-drawn to match.
- **Double-buffered painting** — all rendering goes to an off-screen bitmap first to avoid flicker.
- **Forgiving persistence** — `LoadCards` skips malformed lines rather than crashing.
- **Static linking** — executable is self-contained, no DLL dependencies.
- **Shared action helpers** — stopwatch/blocked/edit/delete each have one implementation (mouse, keyboard, and menu all call the same helper) so behavior stays consistent.
- **Timers keep running during save** — `SaveCards` writes the live total without stopping in-memory timers. On app close the total is persisted and restored as stopped.
- **Extensible card properties** — the `Card` struct and `LoadCards`/`SaveCards` use defaults for missing fields, making it easy to add future properties.

## BugBot (report automation)

This repository also hosts a scheduled, agent-driven bug-fixing system. Reports are Markdown files in a
folder-based queue; the **BugBot** agent driver (`.bugbot/BUGBOT.md`) governs how a headless `opencode run`
processes them.

- `reports/queue/` — pending reports, one file each, named `YYYYMMDD_HHMMSS-title.md` (oldest-first).
  Status = location: nothing to parse.
- `reports/blocked/` — awaiting the owner: either an **answered question** (agent appended
  `## Questions for owner`, owner adds `## Owner response` and returns the file to `queue/`) or a
  **review-mode fix** awaiting approval before the owner commits + moves it to `done/`.
- `reports/done/` — completed reports, with the agent's `## Resolution` appended.
- `.bugbot/config.json` — `workspace`, `mode` (`review`|`auto`), `model`, `opencodePath`.
- `.bugbot/report.bat` + `report.ps1` — interactive capture launcher (Windows; writes a new report into `queue/`).
- `.bugbot/report.sh` — interactive capture launcher (Linux/macOS).
- `.bugbot/run_bugbot.bat` + `run_bugbot.ps1` — the daily runner (Windows): locks, picks the oldest report,
  invokes `opencode run --model <cfg> --auto` with `BUGBOT.md` as the prompt, appends a transcript to
  `bugbot.log`.
- `.bugbot/run_bugbot.sh` — the daily runner (Linux/macOS), same behavior.
- `.bugbot/install_schedule.bat` — registers/removes the daily Windows scheduled task (elevated prompt:
  `install_schedule.bat [time]` or `install_schedule.bat /delete`).
- `.bugbot/install_schedule.sh` — daily cron entry (Linux/macOS, per-user crontab, no root needed).

If you're invoked to work a bug report that lives under `reports/`, follow `.bugbot/BUGBOT.md`'s rules
exactly (build verification, one report per run, move/`## Resolution` bookkeeping, no guessing).
