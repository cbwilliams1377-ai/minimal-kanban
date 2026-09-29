# Minimal Kanban — Agent Reference

## Overview

A minimal, self-contained Kanban board for Windows. Single-file C++ application (~750 lines) using raw Win32 API — no frameworks, no external dependencies. Dark-themed UI with three columns (Todo, In-Progress, Complete), drag-and-drop and keyboard card movement, right-click context menus (Edit/Delete), per-card blocked flag and stopwatch timer, and persistence via a hand-rolled JSON file.

## Tech Stack

- **Language**: C++17
- **UI**: Raw Win32 API (GDI painting, window messages)
- **Compiler**: MinGW g++ (invoked via `build.bat`)
- **Dependencies**: None beyond Windows system libraries (`gdi32`, `user32`, `shell32`, `ole32`, `uuid`, `urlmon`)
- **Data format**: Hand-rolled JSON (no JSON library)

## Build

Run `build.bat` from the workspace directory (Windows, MinGW), or `build.sh` on
Linux/macOS (cross-compiles the Windows .exe with mingw-w64):

```bat
ffmpeg -i check-square.png -vf scale=256:256 check-square.ico
windres minimal_kanban.rc minimal_kanban-res.o
g++ minimal_kanban.cpp minimal_kanban-res.o -o MinimalKanban.exe -std=c++17 -O2 -s -mwindows -static -static-libgcc -static-libstdc++ -municode -lurlmon -lole32 -lshell32 -lgdi32 -luser32 -luuid
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
| `server/` | BugBot Docker container: `Dockerfile`, `entrypoint.sh`, `run-daily.sh`, `sync_issues.sh`, compose + env template |

## Source Map (`minimal_kanban.cpp`)

### Data Structures & Globals (lines 14–50)

- `Card` — struct: `{ std::wstring text; int column; bool blocked; std::wstring blockerNote; LONGLONG timerAccumulated; LONGLONG sessionAccumulated; LONGLONG timerStart; }` where column is 0 (Todo), 1 (In-Progress), or 2 (Complete). `blockerNote` is the short blocker description asked for when a card is blocked (drawn in red on the card, dropped again when it is unblocked). `timerAccumulated` is the total lifetime time (only a "Reset Time" folds a countup into it); `sessionAccumulated` is the persisted countup (`session_ms`), cleared by a "Reset Time"; `timerStart` is a QPC timestamp when running (0 = stopped). Only cards in the Todo column (0) may run a stopwatch.
- `g_cards` — `vector<Card>`, the entire board state in memory.
- `g_font` / `g_boldFont` — Segoe UI 16pt normal/semibold, created at startup.
- `g_dragIndex` — index of card being dragged (-1 = none).
- `g_dragPoint` — mouse position during drag.
- `g_dragOrigin` / `g_dragMoved` — where the current drag started, and whether the pointer has travelled past `DRAG_THRESHOLD`; a drag that really moved keeps the following double-click from also toggling a stopwatch.
- `g_lastMouse` — last known mouse position, drives hover-based keyboard actions.
- `g_selected` — index of the card the focus box is on (-1 = the box is hidden).
- `g_focusAt` — when the focus box last moved (QPC ms); 0 = no idle countdown pending.
- `MAIN_CLASS` / `INPUT_CLASS` / `TIME_CLASS` / `PROMPT_CLASS` / `CONFIRM_CLASS` — Win32 window class names.
- `TITLES[3]` — column header strings.
- `g_qpcFreq` — QPC frequency for stopwatch timing.
- `g_liveTimerID` — Win32 timer ID (100ms tick) driving live stopwatch updates.
- `CMD_EDIT`/`CMD_DELETE`/`CMD_TOGGLE_TIMER`/`CMD_EDIT_TIMER`/`CMD_BLOCKED` — context menu command IDs (`CMD_REPORT`/`CMD_REPORT_PROMPT`/`CMD_UPDATE`/`CMD_AUTO_UPDATE`/`CMD_RELOAD_HOTKEYS` — global menu command IDs).
- `MenuItemData` — owner-drawn context menu item struct (label + keyboard hint).
- `HK_ADD`...`HK_NEW_DAY` (`HK_COUNT`-sized) — enum of the board actions that `hotkeys.json` can rebind; `HK_NAMES[HK_COUNT]` maps each action to its file key. `Hotkey{mods, vk}` is one shortcut (MOD_* flags + virtual-key). `g_hotkeys[HK_COUNT]` holds the current binding list per action (vector: an action can have several keys, e.g. delete = Del + D).
- `TIMER_LIVE`, `CARD_HEIGHT` (60), `CARD_SPACING` (66), `DRAG_THRESHOLD` (4), `BLOCKER_NOTE_MAX` (60) — constants. `PROMPT_CLIENT_W` (460), `PROMPT_CLIENT_H` (180), `PROMPT_LIMIT` (4000) — report prompt dialog size/text limits. `CONFIRM_CLIENT_W` (360), `CONFIRM_CLIENT_H` (150), `CONFIRM_TEXT_MAX` (120) — delete confirmation dialog size and how much card text it quotes.

### Data Path (lines 27–39)

- `AppLocalDir()` — returns `%LOCALAPPDATA%\MinimalKanban` (created if missing). `DataPath()`/`SettingsPath()`/`HotkeysPath()` return the `board.json`/`settings.json`/`hotkeys.json` paths under it. Falls back to `.` if SHGetKnownFolderPath fails.

### String Conversion (lines 42–58)

- `Utf8(wstring)` — UTF-16 → UTF-8 via `WideCharToMultiByte`.
- `Wide(string)` — UTF-8 → UTF-16 via `MultiByteToWideChar`.

### JSON Helpers (lines 130–228)

- `JsonEscape(string)` — escapes `\`, `"`, `\n`, `\r`, `\t` for JSON strings.
- `JsonUnescape(string)` — reverses the above.
- `JsonEscapedField(line, key, out)` — reads one quoted, escaped string field out of a saved card line (used for `text` and `blocker_note`), skipping quotes a backslash has escaped; returns false when the key is absent or malformed.
- `SaveCards()` — writes all cards to `board.json`. Writes `timer_ms` (persisted total) and `session_ms` (live countup: paused-session time + any in-flight stretch) per card and does **not** stop running timers in memory — so a live stopwatch survives mid-session saves and restarts. `column`, `text`, `blocked`, `blocker_note`, `timer_ms`, `session_ms` per card.
- `LoadCards()` — line-by-line parser that reads the format produced by `SaveCards`. Expects one `{"column": N, "text": "...", "blocked": true/false, "blocker_note": "...", "timer_ms": N, "session_ms": N}` per line. `blocked`, `blocker_note`, `timer_ms` and `session_ms` are optional (default false/""/0) for backward compatibility with older save files. Silently skips malformed lines.

### Hotkeys (Hotkey internals, ~lines 350–520)

- `ResetHotkeysToDefaults()` — restores the shipped shortcut set into `g_hotkeys` (delete has both Del and D; **Left/Right** only walk the focus box between columns, so moving a card waits for **Ctrl+Arrow**).
- `HotkeyKeyFromName(wstring)` / `HotkeyKeyName(vk)` — name ↔ virtual-key tables for every bindable key (letters, digits, F1–F12, named keys, other printable chars); unknown/unbindable names return 0 / `"?"`.
- `HotkeyText(Hotkey)` — canonical `"Ctrl+Shift+R"` spelling (modifiers in Ctrl, Shift, Alt, Win order) used for writing the file and hints.
- `HotkeyFromText(wstring, Hotkey&)` — parses one `"Ctrl+Shift+R"` binding back into mods + vk; case-insensitive; rejects empty parts, duplicate key parts, and unknown modifiers/keys.
- `SaveHotkeys()` / `LoadHotkeys()` — write/read `hotkeys.json`. `LoadHotkeys` starts from the defaults and replaces an action's bindings only when the file yields at least one valid entry; `SaveHotkeys` runs only when the file is missing (first run), so a user-edited file is never rewritten by a load or reload.
- `JsonValueField(json, key)` — small helper that reads a string or array value for a given key out of the hotkeys file.
- `HotkeyFromKeysDown(wp)` / `HotkeyAction(wp)` — build the pressed shortcut and match it against `g_hotkeys` (exact modifier match, so Ctrl+R and Ctrl+Shift+R stay distinct); returns -1 when nothing matches.
- `HotkeyHint(action)` — the `"(ctrl+shift+r)"` display hint for menus/buttons from the first binding of an action, empty when unbound.

### Timer Helpers (lines 74–139)

- `NowMs()` — current time in milliseconds via QPC.
- `SessionElapsedMs(Card&)` — milliseconds of the current running session (0 when stopped).
- `TotalElapsedMs(Card&)` — grand total elapsed ms (accumulated total + the countup); used only for edit-timer pre-population.
- `FormatTimer(ms)` — formats to `ss`, `m:ss`, or `h:mm:ss` depending on magnitude.
- `StartLiveTimer(HWND)` / `StopLiveTimer(HWND)` — manage the 100ms `WM_TIMER` tick.
- `NowMs()` — current time in milliseconds via QPC.
- `StopAllTimers()` — finalizes all running card timers into `sessionAccumulated` (currently unused).
- `AnyTimerRunning()` — true if any card has a live timer.
- `RunningTimerIndex()` — index of the card whose stopwatch is running, or -1 when none is.

### Focus Box Helpers

- `StartFocusTimer(HWND)` / `StopFocusTimer(HWND)` — manage the 1s `WM_TIMER` (`TIMER_FOCUS`) idle countdown.
- `FocusCard(HWND, index)` — the single way the focus box moves: sets `g_selected` (index, or -1 to hide it), stamps `g_focusAt`, arms/clears the countdown, and repaints. Every path that focuses a card (arrow keys, add card, toggle stopwatch, toggle blocked, edit, edit timer, delete fixup) goes through it, so acting on a card always pulls the box onto it and restarts the countdown.
- `FocusIdleTimeout(HWND)` — the `WM_TIMER` tick: after `FOCUS_IDLE_MS` (10s) with no focus activity the box falls back to the card with the running stopwatch, or disappears when no stopwatch runs, and the countdown stops until the box next moves.

### Card Action Helpers (lines ~540–558)

- `ToggleTimer(HWND, index)` — shared start/pause logic for the stopwatch (used by Shift+click, double-click, `S` key, and menu). Pausing freezes the in-flight stretch into `sessionAccumulated` (not `timerAccumulated`), so the live countup keeps its value. Starting while another card is running **switches the run over**: the previous card is paused the same way, so the stopwatch always moves to the card the user just toggled (only one runs at a time). Only cards in the **Todo** column may count up; starting elsewhere shows a message box and changes nothing.
- `ToggleBlocked(HWND, index)` — shared blocked-flag toggle (used by Ctrl+click, `B` key, and menu). Blocking a card first asks for a short description of the blocker through the add-card dialog (capped at `BLOCKER_NOTE_MAX`); submitting it blank still blocks the card without a note, cancelling changes nothing, and unblocking drops the description with the flag.
- `PauseCardTimer(int index)` — ends a card's run, freezing its in-flight stretch into `sessionAccumulated`. The single place a run stops other than by choice: the pause toggle, switching the run to another card, and a card leaving the Todo column (drag-drop and Ctrl+Arrow).
- `PendingCountupMs()` — total countup waiting to be pushed (all cards, running stretches included); the amount the "Reset Time" button displays.
- `ResetTime(HWND)` — the "Reset Time" action, the *only* thing that pushes time: folds every card's countup (running or paused, including ones carried over from earlier launches) into its `timerAccumulated`, stops all running stopwatches, and saves. Triggered by the In-Progress column button, the `new_day` hotkey (**Ctrl+Y**), and the global context menu. Nothing folds automatically at launch, so countups survive a close and reopen untouched. Not undoable.
- `HoverIndex(client, point)` — returns the card index under a client-space point, or -1.
- `VisibleOrder()` — card indices in on-screen order (column by column, top to bottom).
- `SelectStep(HWND, delta)` — moves the focus box one card up (-1) / down (+1); starts at the top card stepping down and the bottom one stepping up, and stops at the ends instead of wrapping.
- `SelectColumn(HWND, delta)` — moves the focus box into the neighbouring column (-1) / +1) and onto the card nearest the row it is on; with nothing focused, Left lands on Todo's first card and Right on Complete's first. Only the box moves, never a card.
- `MoveSelectedColumn(HWND, delta)` — moves the selected card one column left (-1) / right (+1); it keeps the focus box, and a card that leaves Todo has its stopwatch paused.
- `ActionIndex(HWND)` — the card a keyboard action applies to: the one under the mouse if there is one, otherwise the focused card.
- `FixSelectionAfterErase(HWND, int)` — keeps `g_selected` on the same card after a deletion shifts the vector, hiding the box when the focused card is the one that went away.
- `DeleteCard(HWND, index)` — shared delete path (used by the `Delete`/`D` key and the context menu): asks for confirmation first, then erases, fixes the selection, saves and redraws. See the Delete Confirmation Dialog below.

### GDI Drawing Helpers (lines 229–276)

- `Fill(HDC, RECT, COLORREF)` — fills a rectangle with a solid color brush.
- `ColumnRect(RECT client, int column)` — computes the screen rectangle for a column. Layout: 16px margin, 10px gap, 16px top offset.
- `AddRect(RECT col)` — the "Add a card" clickable area at the bottom of column 0.
- `ResetRect(RECT col)` — the "Reset Time" clickable area at the bottom of column 1 (In-Progress).
- `SetDarkTitleBar(HWND)` — dynamically loads `dwmapi.dll` to enable dark title bar (DWMWA_USE_IMMERSIVE_DARK_MODE). Graceful fallback on older Windows.
- `CardRects(RECT client)` — returns all card screen rectangles (60px height, 66px spacing). Each card fits 10px inset from column edges.

### Shared Dialog Helpers (lines ~375–428)

- `EditText(HWND edit)` — reads an edit control's contents into a `std::wstring`.
- `IsWordSeparator(wchar_t)` — whitespace test used when walking back a word.
- `DeleteWordBack(HWND edit)` — implements **Ctrl+Backspace** (delete the word, and any whitespace before it, in front of the caret) by selecting the range and clearing it. Windows' edit control never receives that combination through `IsDialogMessage`, so the dialogs run it themselves.
- `PumpDialog(HWND dlg, HWND edit, bool* done, bool ctrlEnterSubmits)` — the one modal message loop all four dialogs use. It forwards the **Ctrl+Backspace** above when the text box has focus, turns a plain **Backspace** into the dialog's IDCANCEL when the dialog has no text box at all (the delete confirmation), and, when `ctrlEnterSubmits` is set, turns **Ctrl+Enter** into the dialog's IDOK (used by the report prompt's Submit).

### Add-Card Dialog (lines 278–395)

- `InputState` — tracks dialog state: edit control handle, done/accepted flags, result text, initial value for edit mode, `allowEmpty` (take a blank box as an answer), `limit` (max characters), the accept-button label, and an optional hint line.
- `InputProc()` — window procedure for the shared text dialog (add/edit card **and** the blocker description). Handles WM_CREATE (optional hint label, edit + owner-drawn buttons, `EM_SETLIMITTEXT`), WM_DRAWITEM (dark-themed buttons), WM_COMMAND (accept/Cancel), dark theme painting.
- `AskForText(HWND owner, wstring& out, const wchar_t* title, const wchar_t* okLabel, wstring initial = L"", bool allowEmpty = false, int limit = 0, const wchar_t* hint = nullptr)` — the shared single-line text dialog, shown as a modal loop (disables owner window, pumps messages until the dialog closes). Pre-populated with `initial` for edit mode. Returns true if the user accepted.
- `AskForCard(HWND owner, wstring& out, wstring initial = L"")` — `AskForText` with the add-card title/button. Returns true if user accepted.
- `AddCard(HWND)` — calls AskForCard, appends new card to column 0, saves, redraws.

### Timer Entry Dialog (lines 407–521)

- `TimeInputState` — tracks time dialog state.
- `TimeInputProc()` — window procedure for the "Set timer" dialog (uses its own class, `TIME_CLASS`).
- `ParseTimeString(wstring)` — parses `ss`, `m:ss`, or `h:mm:ss` into milliseconds; returns -1 on failure.
- `AskForTime(HWND, int cardIndex)` — shows the manual time entry dialog, pre-populated with current total time. On accept, sets the card's `timerAccumulated` and resets `sessionAccumulated` (so the run countup starts fresh); timer ends stopped.

### Report Prompt Dialog

- `PromptState` — tracks the free-form report dialog state.
- `PromptInputProc()` — window procedure for the "Report with your own words" dialog (its own class, `PROMPT_CLASS`). Mirrors the other dialogs: WM_CREATE builds a hint label, a `ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN` edit capped at `PROMPT_LIMIT` (4000) chars, and owner-drawn **Submit**/**Cancel** buttons; WM_DRAWITEM paints them. Submit is deliberately *not* `BS_DEFPUSHBUTTON` so Enter inserts a newline instead of submitting — **Ctrl+Enter** is the submit shortcut instead (handled by `PumpDialog`).
- `AskForReportPrompt(HWND owner, wstring& out)` — modal loop (owner disabled, `AdjustWindowRectEx`-sized to a 460x180 client area); returns true only when the user submits non-blank text.
- `Trim(wstring)` — shared leading/trailing whitespace trim used for the report title, the prompt blank check, and the blocker description.

### Delete Confirmation Dialog

- `ConfirmState` — tracks the confirmation dialog state: done/accepted flags plus the question and the card text it quotes.
- `ConfirmProc()` — window procedure for the confirmation dialog (its own class, `CONFIRM_CLASS`). WM_CREATE builds a question label, a wrapped label quoting the card, and owner-drawn **Delete**/**Cancel** buttons; WM_DRAWITEM paints them, tinting Delete red. **Delete** is `BS_DEFPUSHBUTTON`, so **Enter** confirms; **Esc** (via `IsDialogMessage`) and **Backspace** (via `PumpDialog`) both cancel. There is no text box.
- `AskConfirm(HWND owner, wstring question, wstring detail)` — modal loop (owner disabled, `AdjustWindowRectEx`-sized to a 360x150 client area); returns true only when the user picks Delete.
- `DeleteCard(HWND, int index)` — the single delete path: asks first (quoting up to `CONFIRM_TEXT_MAX` characters of the card), then erases the card, fixes the keyboard selection, saves and redraws. Both the **Delete**/**D** hotkey and the context menu's **Delete** call it, so neither can drop a card without confirming.

### F1 Help Page

- `HelpBindingLine(label, action)` — renders one keyboard action and its bindings, e.g. `Delete card: Del, D`.
- `BuildHelpText()` — assembles the whole help page (columns, card visuals, mouse interactions, every configured shortcut via `HotkeyText`, dialog shortcuts). Generated at F1 time from the *configured* bindings, so a remap or `Reload Hotkeys` is always reflected; nothing is written to disk and the app stays offline. Text lives in the exe, so it can never drift from the binary after a self-update.
- `HelpProc()` — the help window's procedure (`HELP_CLASS`): a dark, read-only, scrollable multiline EDIT filling the client; `Esc`/`Close` dismiss, `WM_SIZE` re-lays the text. `g_helpHwnd` guards against duplicates; pressing F1 again refreshes the open page with the current bindings.
- `ShowHelp(HWND)` — opens (or refreshes) the 560×600 help window centered over the board, dark-title-bared. Launched only by **F1**, which is fixed and deliberately not one of the rebindable `HK_*` actions.

### Main Window Procedure (lines ~560–790)

Handles all main board interactions:

- **WM_KEYDOWN** — **F1** (a fixed, non-rebindable key) opens the in-app help page listing the columns, the card interactions, and every *configured* shortcut. Every other keyboard action routes through `HotkeyAction`, i.e. the bindings loaded from `hotkeys.json` (defaults: **Ctrl+N** add, **Ctrl+R** structured bug report, **Ctrl+Shift+R** free-form prompt, **Ctrl+U** update check, **Ctrl+Y** Reset Time, **Up/Down** select a card with the focus box, **Left/Right** move the focus box to the adjacent column, **Ctrl+Left/Ctrl+Right** move the selected card, and for the card under `g_lastMouse` or the selected card: **Space** edit, **Delete / D** delete, **S** toggle stopwatch, **T** edit timer, **B** toggle blocked). Rebinding an action in the file changes the whole board at once.
- **WM_LBUTTONDOWN** — on a card: **Shift+click** toggles stopwatch, **Ctrl+click** toggles blocked, plain click starts a drag. Click on the Add button adds a card; click on the "Reset Time" button in In-Progress pushes every countup into its total.
- **WM_LBUTTONDBLCLK** — toggles the stopwatch on the card under the pointer (the mouse equivalent of `S`). The main window class carries `CS_DBLCLKS` so Windows sends this message, and it arrives instead of the second `WM_LBUTTONDOWN`, i.e. after the drag the first press started has already been dropped. `g_dragMoved` (set once the pointer travelled past `DRAG_THRESHOLD`) makes a real drag-and-drop swallow the double-click, and a double-click with Shift/Ctrl held does nothing since the first press already acted.
- **WM_MOUSEMOVE** — tracks `g_lastMouse`; updates dragged-card ghost while a drag is active, and flags `g_dragMoved` once the pointer has travelled past `DRAG_THRESHOLD`.
- **WM_LBUTTONUP** — drops card into whichever column the mouse is over, pausing its stopwatch if it left the Todo column.
- **WM_RBUTTONUP** — owner-drawn dark context menu on the card under the pointer: **Edit**, **Delete**, **Toggle Timer**, **Edit Timer**, **Blocked** (the right-aligned key hints come from the configured bindings). Right-click on empty board space shows the global menu: **Report a Bug**, **Report with Prompt...**, **Check for Updates**, the automatic-update mode toggle, **Reset Time (push countups to totals)**, and **Reload Hotkeys** (re-reads `hotkeys.json` without restarting). Wires to the same helpers as keyboard/mouse paths.
- **WM_MEASUREITEM** / **WM_DRAWITEM** — owner-drawn popup menu sizing/painting: black bg RGB(27,27,27), white label, gray right-aligned key hint, hover highlight RGB(70,70,70).
- **WM_CAPTURECHANGED** — cancels drag if capture lost.
- **WM_TIMER** — 100ms live tick: invalidates the window while any stopwatch is running; kills the timer when none are. `TIMER_FOCUS` is the 1s focus-idle tick (`FocusIdleTimeout`).
- **WM_SIZE** — triggers full redraw.
- **WM_ERASEBKGND** — returns 1 (all painting happens in WM_PAINT).
- **WM_PAINT** — double-buffered painting:
  - Background RGB(27,27,27), column backgrounds RGB(39,39,39), headers RGB(43,43,43).
  - Column buttons: "+ Add a card (ctrl+n)" in Todo and "Reset Time (ctrl+y)" (plus the total it will push, when any) in In-Progress, both on RGB(42,42,42).
  - Cards RGB(50,50,50). **Blocked** cards get a thick red (RGB(220,50,50)) 3px outline. **Running-timer** cards get a thick green (RGB(50,180,50)) 3px outline. The **focused** card gets a black (RGB(0,0,0)) 3px outline, so the box reads as a subtle marker instead of a highlight. Blocked and running take drawing priority over the focus outline.
  - Task text in top ~30px of card, with a **blocked** card's blocker description right-justified in red (RGB(220,50,50)) on the same row (the note is measured first and never takes more than half the row, so the task text keeps its space; longer notes are ellipsized). Timer row in the bottom ~30px: **total** time left-justified in gray (RGB(140,140,140)), plus the **live countup** right-justified in green (RGB(50,180,50)) with a `+` prefix. The countup shows while running and stays frozen on the last value when paused; it is persisted (`session_ms`) and only cleared by a "Reset Time".
- **WM_DESTROY** — stops the live and focus timers, saves (the countup is persisted in `session_ms`, so it restores stopped with the `+` display intact), frees fonts, posts quit.

### Entry Point (lines ~790–830)

- `wWinMain()` — initializes COM, queries QPC frequency, creates fonts, registers window classes (main — with `CS_DBLCLKS` so the board receives double-clicks — input, time input, report prompt, delete confirmation, help), loads cards, settings and hotkeys, creates main window (920×520), enables dark title bar, runs message loop. No time is folded at launch: countups are only pushed by "Reset Time".

### Networking & Self-Updates (GitHub integration)

All network use is on-demand — there are no threads, timers, or persistent connections hanging around.
`GITHUB_OWNER`, `GITHUB_REPO`, and `APP_VERSION` are compile-time constants near the top of the file.

- `UrlEncode(wstring)` — UTF-8-aware percent-encoding for URL query strings.
- `OsVersion()` — real Windows version via dynamically-loaded `RtlGetVersion` (works on Win10/11).
- `ReportBug(HWND)` — `ShellExecuteW` opens a pre-filled GitHub **new issue** URL (title/body come from a compact pure-`WideCharToMultiByte` encoder; user's browser does GitHub auth).
- `ReportBugWithPrompt(HWND)` — same, but the body is the user's own wording captured by the report prompt dialog: title = first line trimmed and capped at 64 chars (fallback `Bug report from app`), body = the full prompt verbatim.
- `UpdateCheck(HWND)` — hits `api.github.com/.../releases/latest` via `URLDownloadToFileW` (writes to disk, not RAM), compares `tag_name` to the compiled `APP_VERSION` with `VersionCmp` (dotted-numeric), prompts, then downloads the release asset in place.
- `InstallUpdate(HWND, path)` — spawns a detached, hidden `cmd` that waits ~3s for the process to exit, `move`s the new exe over the running one in `%LOCALAPPDATA%`, and relaunches.
- Entry points: **right-click empty board space** shows a global owner-drawn menu (**Report a Bug** / **Report with Prompt...** / **Check for Updates** / update-mode toggle); **Ctrl+R**, **Ctrl+Shift+R** and **Ctrl+U** do the same. `CMD_REPORT`/`CMD_REPORT_PROMPT`/`CMD_UPDATE` are the menu IDs; the global menu reuses the card menu's `WM_MEASUREITEM`/`WM_DRAWITEM` painting.

## Data Format

`%LOCALAPPDATA%\MinimalKanban\board.json`:

```json
{
  "cards": [
    {"column": 0, "text": "Buy groceries", "blocked": false, "blocker_note": "", "timer_ms": 0, "session_ms": 0},
    {"column": 1, "text": "Write docs", "blocked": false, "blocker_note": "", "timer_ms": 90000, "session_ms": 12500},
    {"column": 2, "text": "Ship v1.0", "blocked": true, "blocker_note": "waiting on the vendor", "timer_ms": 5435000, "session_ms": 0}
  ]
}
```

- `column`: integer 0, 1, or 2
- `text`: UTF-8 string with JSON escaping
- `blocked`: true/false (optional, defaults false)
- `blocker_note`: the short blocker description shown in red on a blocked card (optional, defaults "")
- `timer_ms`: accumulated stopwatch time in milliseconds (optional, defaults 0); grows when "Reset Time" pushes a countup
- `session_ms`: the live countup ("+" display) in milliseconds (optional, defaults 0); cleared by a "Reset Time"

`%LOCALAPPDATA%\MinimalKanban\hotkeys.json` (written once with the defaults on first run, then
never rewritten by the app — edit it and reload via the right-click menu or restart):

```json
{
  "add": "Ctrl+N",
  "edit": "Space",
  "delete": ["Del", "D"],
  "toggle_timer": "S",
  "edit_timer": "T",
  "blocked": "B",
  "select_up": "Up",
  "select_down": "Down",
  "select_left": "Left",
  "select_right": "Right",
  "move_left": "Ctrl+Left",
  "move_right": "Ctrl+Right",
  "report": "Ctrl+R",
  "report_prompt": "Ctrl+Shift+R",
  "update": "Ctrl+U",
  "new_day": "Ctrl+Y"
}
```

- Each key is an action name; the value is one binding `"Ctrl+Shift+R"` or a list `["Del", "D"]`.
  Modifiers are `Ctrl`/`Shift`/`Alt`/`Win` joined with `+`; the key part is a letter, digit, F1–F12,
  or a named key (Space, Del, Back, Enter, Esc, Tab, arrows, Home, End, PgUp, PgDn, PrtSc, Pause,
  Ins). Names are case-insensitive.
- Actions are rebound **exactly**: a press must match modifiers *and* key. `LoadHotkeys` starts from
  the defaults and swaps in an action's bindings only when the file yields at least one valid entry;
  malformed lines, unknown key names, and duplicate entries are silently ignored (default kept), and
  a missing file just writes the defaults.
- `Reload Hotkeys` (right-click empty board space) re-reads the file without restarting; the menu and
  button hints always reflect the currently configured bindings.
- `new_day` is the historical key name for the action now labelled "Reset Time"; it is kept as-is so
  `hotkeys.json` files written by earlier builds keep working without edits.

## Card Interactions

| Action | Mouse | Keyboard |
|--------|-------|----------|
| Select a card | Hover the pointer over it | **Up** / **Down** (focus box) |
| Move the focus box to the adjacent column (only the box moves) | Hover the pointer over it | **Left** / **Right** |
| Move card between columns (leaving Todo pauses the stopwatch) | Drag (plain click) | **Ctrl+Left** / **Ctrl+Right** on the selected card |
| Toggle stopwatch start/stop on a Todo card (starts a running stopwatch on the other card) | Shift+left-click or double-click | **S** |
| Toggle blocked flag (red outline; blocking asks for a description, shown in red on the card) | Ctrl+left-click | **B** |
| Edit card text | Right-click → Edit | **Space** |
| Delete card (asks for confirmation first) | Right-click → Delete | **Delete** or **D** |
| Set stopwatch time manually | Right-click → Edit Timer | **T** |
| Add new card | Click "+ Add a card" | **Ctrl+N** |
| File a bug report (opens browser) | Right-click empty space → Report a Bug | **Ctrl+R** |
| File a report in your own words (opens browser) | Right-click empty space → Report with Prompt... | **Ctrl+Shift+R** |
| Check for updates (self-updates) | Right-click empty space → Check for Updates | **Ctrl+U** |
| Reset Time: fold every countup into its total and stop the stopwatch | Right-click empty space → Reset Time (push countups to totals), or click "Reset Time" at the bottom of In-Progress | **Ctrl+Y** |

Keyboard actions target the card under the last known mouse position, or the card picked with the arrow keys when the cursor isn't over one; they no-op when neither applies. The focused card is outlined in black, and the red (blocked) and green (running) outlines take visual priority.

The **focus box** is a black 3px outline and follows the user: it lands on the card the arrow keys picked, on a newly created card, and on any card an action ran against (stopwatch, blocked, edit, edit timer). After `FOCUS_IDLE_MS` (10s) without focus activity it falls back to the card with the running stopwatch, or disappears when no stopwatch is running, so an untouched box never sits drawing attention to a random card.

Every keyboard shortcut above is a **default** editable in `hotkeys.json` (see Data Format); the keys
shown in the menus and on the "+ Add a card" and "Reset Time" buttons always reflect the current bindings.

Stopwatches only count up in the **Todo** column: starting one elsewhere (Shift+click, double-click,
**S**, or the card menu) says so and changes nothing, and a card dragged or keyed out of Todo has its
stopwatch paused with the countup kept for the next "Reset Time".

## Dialog Interactions

All four dialogs (add/edit card, blocker description, set timer, report prompt, delete confirmation)
share one modal loop, so they share these shortcuts:

| Action | Keyboard |
|--------|----------|
| Delete the previous word in the text box | **Ctrl+Backspace** |
| Submit the "Report with your own words" dialog (Enter inserts a newline there) | **Ctrl+Enter** |
| Accept / dismiss the add-card, blocker and set-timer dialogs | **Enter** / **Esc** |
| Confirm / dismiss the delete confirmation (it has no text box) | **Enter** / **Esc** or **Backspace** |

## Conventions

- **Single file** — all code lives in `minimal_kanban.cpp`. No header files, no separate modules.
- **Global state** — prefixed with `g_` (e.g., `g_cards`, `g_font`).
- **No external libraries** — pure Win32 API only (plus Windows system DLLs `urlmon` for the update check; this is a Microsoft OS library, not a third-party dependency). Dynamically load optional DLLs (`dwmapi.dll`, `uxtheme.dll`, `ntdll.dll`) with manual `GetProcAddress` for backward compatibility.
- **UTF-16 internally, UTF-8 for files** — all UI strings are `std::wstring`. JSON files are UTF-8.
- **Dark theme** — hardcoded RGB values, no theming system. Popup menus are owner-drawn to match.
- **Double-buffered painting** — all rendering goes to an off-screen bitmap first to avoid flicker.
- **Forgiving persistence** — `LoadCards` skips malformed lines rather than crashing.
- **Static linking** — executable is self-contained, no DLL dependencies.
- **Shared action helpers** — stopwatch/blocked/edit/delete each have one implementation (mouse, keyboard, and menu all call the same helper) so behavior stays consistent.
- **Timers keep running during save** — `SaveCards` writes `timer_ms` and `session_ms` without stopping in-memory timers, so a running or paused stopwatch survives saves and app closes with its countup intact. Timers restore stopped; running ones stay running only in memory until the next restart.
- **Extensible card properties** — the `Card` struct and `LoadCards`/`SaveCards` use defaults for missing fields, making it easy to add future properties.
- **Configurable hotkeys** — the board's keyboard shortcuts come from `hotkeys.json`, and every part of the UI that shows a shortcut (context menus, "+ Add a card") renders the configured binding. Key matching is exact on modifiers so distinct chords stay distinct; the file is treated as user data and never rewritten after first run.

## BugBot (report automation)

This repository also hosts a scheduled, agent-driven bug-fixing system. Reports are Markdown files in a
folder-based queue; the **BugBot** agent driver (`.bugbot/BUGBOT.md`) governs how a headless `opencode run`
processes them.

The bug pipeline (GitHub-integrated):

1. A bug is reported as a **GitHub issue** (in-app "Report a Bug" opens a pre-filled issue; web/phone work too).
2. The **BugBot server container** (`server/` — Docker Compose, cron) runs daily:
   - `entrypoint.sh` checks out/clones the repo into a named volume (`/workspace`).
   - `sync_issues.sh` turns open GitHub issues into `reports/queue/*.md`, and re-queues
     `blocked/` reports whose issue has new owner comments (appended as `## Owner response`).
   - `run-daily.sh` invokes `run_bugbot.sh` (the agent, auto mode), then, when the agent changed source:
     bumps `APP_VERSION`, cross-compiles with `bash build.sh`, pushes the configured branch, uploads
     `MinimalKanban.exe` to a draft release targeting the exact commit, publishes it, and closes only
     the issue linked to that successfully published transaction. Failures retain persistent state
     for retry; interrupted agents require inspection.
   - The in-app **Check for Updates** downloads the newest release asset and self-installs to `%LOCALAPPDATA%`.
3. `report.bat`/`report.ps1`/`report.sh` remain as local-only capture launchers (no GitHub dependency).

`reports/` layout:

- `reports/queue/` — pending reports, one file each, named `YYYYMMDD_HHMMSS-title.md` (oldest-first).
  Status = location: nothing to parse.
- `reports/blocked/` — awaiting the owner: either an **answered question** (agent appended
  `## Questions for owner`, owner adds `## Owner response` and returns the file to `queue/`) or a
  **review-mode fix** awaiting approval before the owner commits + moves it to `done/`.
- `reports/done/` — completed reports, with the agent's `## Resolution` appended.
- Automatic reports carry `- #<issue>` in the filename so `server/sync_issues.sh` can match an
  issue to its report and dedupe / close issues; local reports just use a description slug.

Server-side files:

- `server/Dockerfile` — Debian image with mingw-w64 cross toolchain, ffmpeg, `gh`, and opencode.
- `server/entrypoint.sh` — clone/pull on start, `gh auth`, cron schedule, optional start-run.
- `server/run-daily.sh` — pull → sync issues → run agent → bump version → build → push → release → close.
- `server/sync_issues.sh` — GitHub issues ↔ `reports/` bridge (dedupe + owner-response resume).
- `server/docker-compose.yml` + `server/.env.example` — copy `.env.example` to `.env` and fill in
  `GITHUB_OWNER`, `GITHUB_REPO`, `GITHUB_TOKEN` (fine-grained PAT: Contents R/W, Issues R/W).

If you're invoked to work a bug report that lives under `reports/`, follow `.bugbot/BUGBOT.md`'s rules
exactly (build verification, one report per run, move/`## Resolution` bookkeeping, no guessing).

### Debian deployment and verification

For a fresh Debian deployment, start with `server/OPENCODE-HANDOFF.md`.
Follow `MinimalKanban-Bug-Pipeline-Setup.md` for the current server procedure.
`GITHUB_BRANCH` defaults to `master` and is used explicitly for clone/pull/push.
`RUN_ON_START=0` and `SCHEDULE_ENABLED=0` keep initial setup idle. Credentials use
`GH_TOKEN`; OpenCode auth persists in `/root/.local/share/opencode`. Debian cron
receives exported container variables through a root-only runtime file.

The entire pipeline holds a lock. `.bugbot/server-state/pending.json` associates
a report with its release commit/tag; a failed release retries before new work.
Only fresh comments from `BUGBOT_OWNER` resume blocked reports; questions are
posted back to GitHub. Issue deduplication uses exact filename issue numbers.

Run `python3 server/tests/test_pipeline.py -v` for offline integration checks of
success, failure/retry, report matching and owner replies. These use mocked APIs
and temporary Git repositories, never real tickets or releases.
