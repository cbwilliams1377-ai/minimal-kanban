#define _UNICODE
#define WIN32_LEAN_AND_MEAN
#define APP_ICON 101 // Numeric ID used to find the icon in minimal_kanban.rc.
#include <windows.h>
#include <windowsx.h>
#include <shlobj.h>
#include <shellapi.h>
#include <urlmon.h>
#include <winternl.h>
#include <algorithm>
#include <iterator>
#include <fstream>
#include <memory>
#include <filesystem>
#include <sstream>
#include <string>
#include <vector>

// One task on the board. column is 0 (Todo), 1 (In-Progress), or 2 (Complete).
struct Card {
    std::wstring text;
    int column;
    bool blocked = false;
    LONGLONG timerAccumulated = 0; // completed time from runs before this program run
    LONGLONG sessionAccumulated = 0; // paused countup time; persisted and cleared by a "Reset Time"
    LONGLONG timerStart = 0;       // QPC timestamp when running (0 = stopped)
};

struct Settings {
    bool autoUpdate = false;
    bool checkOnStartup = true;
};

// The board actions that hotkeys.json can rebind, in the order they are listed
// in the generated file. An action holds one or more bindings; a key press
// matches an action when modifiers and virtual key are all identical.
enum {
    HK_ADD = 0, HK_EDIT, HK_DELETE, HK_TOGGLE_TIMER, HK_EDIT_TIMER, HK_BLOCKED,
    HK_SELECT_UP, HK_SELECT_DOWN, HK_MOVE_LEFT, HK_MOVE_RIGHT,
    HK_REPORT, HK_REPORT_PROMPT, HK_UPDATE, HK_NEW_DAY, HK_COUNT
};

// One keyboard shortcut: modifier flags (MOD_*) plus a virtual-key code.
struct Hotkey { UINT mods = 0; UINT vk = 0; };

// These global variables hold the board and the pieces of UI state shared by message handlers.
static std::vector<Card> g_cards; // All cards currently loaded in memory.
static HFONT g_font = nullptr, g_boldFont = nullptr; // Fonts used while drawing text.
static int g_dragIndex = -1; // Index of the card being dragged; -1 means no drag is active.
static POINT g_dragPoint{}; // Current mouse position while dragging a card.
static POINT g_lastMouse{}; // Last known mouse position, used for hover-based keyboard actions.
static int g_selected = -1; // Index of the card the focus box is on; -1 means the box is hidden.
static LONGLONG g_focusAt = 0; // When the focus box last moved (QPC ms); 0 = no idle countdown pending.
static const wchar_t* MAIN_CLASS = L"MinimalKanbanWindow"; // Name of the main window class.
static const wchar_t* INPUT_CLASS = L"MinimalKanbanInput"; // Name of the add-card window class.
static const wchar_t* TIME_CLASS = L"MinimalKanbanTimeInput"; // Name of the timer entry window class.
static const wchar_t* PROMPT_CLASS = L"MinimalKanbanReportPrompt"; // Name of the report prompt window class.
static const wchar_t* CONFIRM_CLASS = L"MinimalKanbanConfirm"; // Name of the delete confirmation window class.
static const wchar_t* HELP_CLASS = L"MinimalKanbanHelp"; // Name of the F1 help window class.
static const wchar_t* TITLES[3] = { L"Todo", L"In-Progress", L"Complete" };
static const wchar_t* GITHUB_OWNER = L"cbwilliams1377-ai";
static const wchar_t* GITHUB_REPO = L"minimal-kanban";
static const wchar_t* APP_VERSION = L"0.1.11";
// Names of the rebindable actions, used both as hotkeys.json keys and when
// matching a pressed key back to its action. "new_day" keeps its original file
// key even though the action is now called "Reset Time", so hotkeys.json files
// written by earlier builds keep working untouched.
static const wchar_t* HK_NAMES[HK_COUNT] = {
    L"add", L"edit", L"delete", L"toggle_timer", L"edit_timer", L"blocked",
    L"select_up", L"select_down", L"move_left", L"move_right",
    L"report", L"report_prompt", L"update", L"new_day"
};
// The current binding set, loaded from hotkeys.json at startup or on "Reload Hotkeys".
static std::vector<Hotkey> g_hotkeys[HK_COUNT];
static LARGE_INTEGER g_qpcFreq{};        // QPC frequency, queried once at startup.
static UINT_PTR g_liveTimerID = 0;       // Win32 timer ID for live stopwatch updates (0 = not running).
static UINT_PTR g_focusTimerID = 0;      // Win32 timer ID for the focus box idle countdown (0 = not running).
static HWND g_helpHwnd = nullptr;        // Handle of the F1 help window (0 = closed).
// Cached double-buffer used by WM_PAINT. Allocating a full-window bitmap on
// every paint made the process footprint grow (the live stopwatch repaints ten
// times a second), so the buffer is kept and only rebuilt when the size changes.
static HDC g_backDC = nullptr;
static HBITMAP g_backBitmap = nullptr;
static int g_backW = 0, g_backH = 0;
static Settings g_settings;
static volatile LONG g_updateBusy = 0;
static volatile LONG g_updateCancelled = 0;

#define TIMER_LIVE 1                     // Timer ID for the live stopwatch tick.
#define TIMER_FOCUS 2                    // Timer ID for the focus box idle countdown tick.
#define FOCUS_IDLE_MS 10000              // Idle time before the focus box falls back to the running stopwatch.
#define CARD_HEIGHT 60                   // Height of each card in pixels.
#define CARD_SPACING 66                  // Vertical spacing between cards.
#define WM_APP_UPDATE_CHECK (WM_APP + 1)
#define WM_APP_UPDATE_RESULT (WM_APP + 2)

// Context menu command IDs.
#define CMD_EDIT 1
#define CMD_DELETE 2
#define CMD_TOGGLE_TIMER 3
#define CMD_EDIT_TIMER 4
#define CMD_BLOCKED 5
#define CMD_REPORT 6
#define CMD_UPDATE 7
#define CMD_AUTO_UPDATE 8
#define CMD_REPORT_PROMPT 9
#define CMD_RELOAD_HOTKEYS 10
#define CMD_NEW_DAY 11

// Size limits for the natural-language report prompt dialog.
#define PROMPT_CLIENT_W 460 // Dialog client width in pixels.
#define PROMPT_CLIENT_H 180 // Dialog client height in pixels.
#define PROMPT_LIMIT 4000  // Max characters accepted so the issue URL never gets unwieldy.

// Size of the delete confirmation dialog, and how much of the card text it shows.
#define CONFIRM_CLIENT_W 360 // Dialog client width in pixels.
#define CONFIRM_CLIENT_H 150 // Dialog client height in pixels.
#define CONFIRM_TEXT_MAX 120 // Max characters of the card text quoted in the prompt.

// Owner-drawn menu item: the label plus the keyboard-hint shown right-aligned.
struct MenuItemData { const wchar_t* text; const wchar_t* hint; };

// Return the app's install directory under Local AppData, creating it if needed.
static std::wstring AppLocalDir() {
    PWSTR raw = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_CREATE, nullptr, &raw))) {
        std::wstring dir(raw);
        CoTaskMemFree(raw);
        dir += L"\\MinimalKanban";
        CreateDirectoryW(dir.c_str(), nullptr);
        return dir;
    }
    return L".";
}

// Return the path where the board is saved.
static std::wstring DataPath() { return AppLocalDir() + L"\\board.json"; }

static std::wstring SettingsPath() { return AppLocalDir() + L"\\settings.json"; }

static std::wstring HotkeysPath() { return AppLocalDir() + L"\\hotkeys.json"; }

// Convert Windows' UTF-16 string type to UTF-8 for the JSON file.
static std::string Utf8(const std::wstring& s) {
    if (s.empty()) return {};
    // First ask how many bytes are needed, then perform the conversion.
    int n = WideCharToMultiByte(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0, nullptr, nullptr);
    std::string out(n, 0);
    WideCharToMultiByte(CP_UTF8, 0, s.data(), (int)s.size(), out.data(), n, nullptr, nullptr);
    return out;
}

// Convert UTF-8 read from the JSON file back to Windows' UTF-16 string type.
static std::wstring Wide(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring out(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), out.data(), n);
    return out;
}

// Return the current time in milliseconds using QPC.
static LONGLONG NowMs() {
    LARGE_INTEGER t; QueryPerformanceCounter(&t);
    return t.QuadPart * 1000 / g_qpcFreq.QuadPart;
}

// Get the milliseconds of the current running session for a card (0 when stopped).
static LONGLONG SessionElapsedMs(const Card& c) {
    if (c.timerStart == 0) return 0;
    return NowMs() - c.timerStart;
}

// Get the grand total elapsed milliseconds for a card (accumulated total plus
// the live countup). Used only to pre-fill the "Set timer" dialog; persistence
// writes timer_ms and session_ms separately so the countup survives saves.
static LONGLONG TotalElapsedMs(const Card& c) {
    return c.timerAccumulated + c.sessionAccumulated + SessionElapsedMs(c);
}

// Format milliseconds into a display string: ss, m:ss, or h:mm:ss.
static std::wstring FormatTimer(LONGLONG ms) {
    if (ms <= 0) return L"";
    LONGLONG totalSec = ms / 1000;
    LONGLONG h = totalSec / 3600;
    LONGLONG m = (totalSec % 3600) / 60;
    LONGLONG s = totalSec % 60;
    if (h > 0) {
        return std::to_wstring(h) + L":" + (m < 10 ? L"0" : L"") + std::to_wstring(m)
             + L":" + (s < 10 ? L"0" : L"") + std::to_wstring(s);
    }
    if (m > 0) {
        return std::to_wstring(m) + L":" + (s < 10 ? L"0" : L"") + std::to_wstring(s);
    }
    return std::to_wstring(s);
}

// Start the live 100ms timer if not already running.
static void StartLiveTimer(HWND hwnd) {
    if (g_liveTimerID == 0) g_liveTimerID = SetTimer(hwnd, TIMER_LIVE, 100, nullptr);
}

// Kill the live timer if running.
static void StopLiveTimer(HWND hwnd) {
    if (g_liveTimerID != 0) { KillTimer(hwnd, TIMER_LIVE); g_liveTimerID = 0; }
}

// Finalize all running card timers into their countups. Kept for symmetry with
// the pause path in ToggleTimer (currently unused).
static void StopAllTimers() {
    LONGLONG now = NowMs();
    for (auto& c : g_cards) {
        if (c.timerStart != 0) {
            c.sessionAccumulated += (now - c.timerStart);
            c.timerStart = 0;
        }
    }
}

// Check if any card has a running timer.
static bool AnyTimerRunning() {
    for (const auto& c : g_cards) if (c.timerStart != 0) return true;
    return false;
}

// Return the index of the card whose stopwatch is running, or -1 when none is.
static int RunningTimerIndex() {
    for (size_t i = 0; i < g_cards.size(); ++i) if (g_cards[i].timerStart != 0) return (int)i;
    return -1;
}

// Start the 1s focus countdown if not already running.
static void StartFocusTimer(HWND hwnd) {
    if (g_focusTimerID == 0) g_focusTimerID = SetTimer(hwnd, TIMER_FOCUS, 1000, nullptr);
}

// Kill the focus countdown if running.
static void StopFocusTimer(HWND hwnd) {
    if (g_focusTimerID != 0) { KillTimer(hwnd, TIMER_FOCUS); g_focusTimerID = 0; }
}

// Move the focus box onto a card (or hide it with -1), restart its idle
// countdown, and repaint. Every path that moves the box comes through here, so
// one rule holds everywhere: the box lands on whatever the user just acted on
// and quietly returns to the running stopwatch after FOCUS_IDLE_MS of idling.
static void FocusCard(HWND hwnd, int index) {
    if (index < 0 || index >= (int)g_cards.size()) index = -1;
    g_selected = index;
    g_focusAt = index >= 0 ? NowMs() : 0;
    if (g_focusAt != 0) StartFocusTimer(hwnd); else StopFocusTimer(hwnd);
    InvalidateRect(hwnd, nullptr, FALSE);
}

// The focus box's idle tick: once the box has sat untouched for FOCUS_IDLE_MS it
// returns to the card with the running stopwatch, or disappears when no
// stopwatch is running, so an idle box never draws the eye to a random card.
static void FocusIdleTimeout(HWND hwnd) {
    if (g_focusAt == 0 || NowMs() - g_focusAt < FOCUS_IDLE_MS) return;
    int running = RunningTimerIndex();
    g_focusAt = 0;
    StopFocusTimer(hwnd);
    if (g_selected == running) return; // The fallback lands where the box already was.
    g_selected = running;
    InvalidateRect(hwnd, nullptr, FALSE);
}

// Escape characters that have special meaning inside a JSON string.
static std::string JsonEscape(const std::string& s) {
    std::string out;
    for (unsigned char c : s) {
        if (c == '\\' || c == '"') { out += '\\'; out += char(c); }
        else if (c == '\n') out += "\\n";
        else if (c == '\r') out += "\\r";
        else if (c == '\t') out += "\\t";
        else out += char(c);
    }
    return out;
}

// Undo the escaping performed by JsonEscape when loading a card.
static std::string JsonUnescape(const std::string& s) {
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\\' && i + 1 < s.size()) {
            char c = s[++i];
            if (c == 'n') out += '\n';
            else if (c == 'r') out += '\r';
            else if (c == 't') out += '\t';
            else out += c;
        } else out += s[i];
    }
    return out;
}

static bool JsonBoolField(const std::string& json, const char* key, bool fallback) {
    std::string quoted = std::string("\"") + key + "\"";
    size_t p = json.find(quoted);
    if (p == std::string::npos) return fallback;
    size_t colon = json.find(':', p + quoted.size());
    if (colon == std::string::npos) return fallback;
    size_t value = colon + 1;
    while (value < json.size() && (json[value] == ' ' || json[value] == '\t'
        || json[value] == '\r' || json[value] == '\n')) ++value;
    if (json.compare(value, 4, "true") == 0) return true;
    if (json.compare(value, 5, "false") == 0) return false;
    return fallback;
}

static std::wstring JsonStringField(const std::string& json, const char* key) {
    std::string quoted = std::string("\"") + key + "\"";
    size_t p = json.find(quoted);
    if (p == std::string::npos) return {};
    size_t colon = json.find(':', p + quoted.size());
    if (colon == std::string::npos) return {};
    size_t q1 = json.find('"', colon + 1);
    if (q1 == std::string::npos) return {};
    size_t q2 = json.find('"', q1 + 1);
    if (q2 == std::string::npos) return {};
    return Wide(json.substr(q1 + 1, q2 - q1 - 1));
}

// Write every card to the board.json file.
// Timers are NOT stopped here: "timer_ms" is the accumulated total and
// "session_ms" is the live countup (paused stretches plus any in-flight run),
// so a running or paused stopwatch survives saves and restarts with its "+"
// display intact. (WM_DESTROY saves; on reload the timers restore stopped.)
// Only "Reset Time" ever folds a countup into a total.
static void SaveCards() {
    // trunc clears the previous file before writing the current board.
    std::ofstream f(std::filesystem::path(DataPath()), std::ios::binary | std::ios::trunc);
    if (!f) return;
    f << "{\n  \"cards\": [\n";
    for (size_t i = 0; i < g_cards.size(); ++i) {
        f << "    {\"column\": " << g_cards[i].column
          << ", \"text\": \"" << JsonEscape(Utf8(g_cards[i].text)) << "\""
          << ", \"blocked\": " << (g_cards[i].blocked ? "true" : "false")
          << ", \"timer_ms\": " << g_cards[i].timerAccumulated
          << ", \"session_ms\": " << (g_cards[i].sessionAccumulated + SessionElapsedMs(g_cards[i]))
          << "}";
        if (i + 1 != g_cards.size()) f << ',';
        f << '\n';
    }
    f << "  ]\n}\n";
}

// Read cards from board.json. This parser expects the simple format produced by SaveCards.
static void LoadCards() {
    std::ifstream f(std::filesystem::path(DataPath()), std::ios::binary);
    if (!f) return;
    std::string line;
    while (std::getline(f, line)) { // SaveCards writes one card per line.
        size_t cp = line.find("\"column\"");
        size_t tp = line.find("\"text\"");
        if (cp == std::string::npos || tp == std::string::npos) continue;
        size_t colon = line.find(':', cp), comma = line.find(',', colon);
        int column = 0;
        // Ignore malformed lines rather than allowing a damaged save file to crash the app.
        try { column = std::stoi(line.substr(colon + 1, comma - colon - 1)); } catch (...) { continue; }
        size_t q1 = line.find('"', line.find(':', tp) + 1);
        if (q1 == std::string::npos) continue;
        std::string encoded;
        bool escaped = false;
        size_t q2 = q1 + 1;
        // Find the closing quote, skipping quotes that are escaped with a backslash.
        for (; q2 < line.size(); ++q2) {
            char c = line[q2];
            if (!escaped && c == '"') break;
            encoded += c;
            if (!escaped && c == '\\') escaped = true; else escaped = false;
        }
        if (column < 0 || column > 2) continue;
        Card card{Wide(JsonUnescape(encoded)), column};
        // Parse optional "blocked" field (default false for backward compatibility).
        size_t bp = line.find("\"blocked\"");
        if (bp != std::string::npos) {
            size_t bcolon = line.find(':', bp);
            if (bcolon != std::string::npos && bcolon < line.size()) {
                // Skip whitespace after colon.
                size_t bval = bcolon + 1;
                while (bval < line.size() && (line[bval] == ' ' || line[bval] == '\t')) ++bval;
                if (bval < line.size() && line[bval] == 't') card.blocked = true;
            }
        }
        // Parse optional "timer_ms" field (default 0 for backward compatibility).
        size_t tp2 = line.find("\"timer_ms\"");
        if (tp2 != std::string::npos) {
            size_t tcolon = line.find(':', tp2);
            if (tcolon != std::string::npos) {
                size_t tcomma = line.find(',', tcolon);
                if (tcomma == std::string::npos) tcomma = line.find('}', tcolon);
                try { card.timerAccumulated = std::stoll(line.substr(tcolon + 1, tcomma - tcolon - 1)); } catch (...) {}
            }
        }
        // Parse optional "session_ms" field: the persisted countup (default 0 for
        // backward compatibility). Restored stopped; a live run is not resumed.
        size_t sp = line.find("\"session_ms\"");
        if (sp != std::string::npos) {
            size_t scolon = line.find(':', sp);
            if (scolon != std::string::npos) {
                size_t scomma = line.find(',', scolon);
                if (scomma == std::string::npos) scomma = line.find('}', scolon);
                try { card.sessionAccumulated = std::stoll(line.substr(scolon + 1, scomma - scolon - 1)); } catch (...) {}
            }
        }
        g_cards.push_back(card);
    }
}

static void LoadSettings() {
    g_settings = Settings();
    std::ifstream f(std::filesystem::path(SettingsPath()), std::ios::binary);
    if (!f) return;
    std::string json((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    g_settings.autoUpdate = JsonStringField(json, "update_mode") == L"auto";
    g_settings.checkOnStartup = JsonBoolField(json, "check_on_startup", true);
}

static void SaveSettings() {
    std::ofstream f(std::filesystem::path(SettingsPath()), std::ios::binary | std::ios::trunc);
    if (!f) return;
    f << "{\n  \"update_mode\": \"" << (g_settings.autoUpdate ? "auto" : "ask") << "\""
      << ",\n  \"check_on_startup\": " << (g_settings.checkOnStartup ? "true" : "false") << "\n}\n";
}

// Hotkeys: an editable config file that rebinds the board's keyboard shortcuts.

// Restore the shipped shortcut set. delete keeps both Del and D so the keys the
// app has always documented keep working until the user changes them.
static void ResetHotkeysToDefaults() {
    g_hotkeys[HK_ADD]           = { { MOD_CONTROL, 'N' } };
    g_hotkeys[HK_EDIT]          = { { 0, VK_SPACE } };
    g_hotkeys[HK_DELETE]        = { { 0, VK_DELETE }, { 0, 'D' } };
    g_hotkeys[HK_TOGGLE_TIMER]  = { { 0, 'S' } };
    g_hotkeys[HK_EDIT_TIMER]    = { { 0, 'T' } };
    g_hotkeys[HK_BLOCKED]       = { { 0, 'B' } };
    g_hotkeys[HK_SELECT_UP]     = { { 0, VK_UP } };
    g_hotkeys[HK_SELECT_DOWN]   = { { 0, VK_DOWN } };
    g_hotkeys[HK_MOVE_LEFT]     = { { 0, VK_LEFT } };
    g_hotkeys[HK_MOVE_RIGHT]    = { { 0, VK_RIGHT } };
    g_hotkeys[HK_REPORT]        = { { MOD_CONTROL, 'R' } };
    g_hotkeys[HK_REPORT_PROMPT] = { { MOD_CONTROL | MOD_SHIFT, 'R' } };
    g_hotkeys[HK_UPDATE]        = { { MOD_CONTROL, 'U' } };
    g_hotkeys[HK_NEW_DAY]       = { { MOD_CONTROL, 'Y' } };
}

// Turn one key name (as written in hotkeys.json) into a virtual-key code.
// Returns 0 when the name is not a key this build can bind.
static UINT HotkeyKeyFromName(const std::wstring& upper) {
    static const struct { const wchar_t* name; UINT vk; } named[] = {
        { L"SPACE", VK_SPACE }, { L"ENTER", VK_RETURN }, { L"RETURN", VK_RETURN },
        { L"TAB", VK_TAB }, { L"ESC", VK_ESCAPE }, { L"ESCAPE", VK_ESCAPE },
        { L"BACK", VK_BACK }, { L"BACKSPACE", VK_BACK }, { L"DEL", VK_DELETE },
        { L"DELETE", VK_DELETE }, { L"INS", VK_INSERT }, { L"INSERT", VK_INSERT },
        { L"UP", VK_UP }, { L"DOWN", VK_DOWN }, { L"LEFT", VK_LEFT }, { L"RIGHT", VK_RIGHT },
        { L"HOME", VK_HOME }, { L"END", VK_END }, { L"PGUP", VK_PRIOR }, { L"PGDN", VK_NEXT },
        { L"PRTSC", VK_SNAPSHOT }, { L"PAUSE", VK_PAUSE },
    };
    for (const auto& n : named) if (upper == n.name) return n.vk;
    if (upper.size() > 1 && upper[0] == L'F' && std::all_of(upper.begin() + 1, upper.end(),
        [](wchar_t c) { return c >= L'0' && c <= L'9'; })) {
        int n = _wtoi(upper.c_str());
        if (n >= 1 && n <= 12) return (UINT)(VK_F1 + n - 1);
    }
    if (upper.size() == 1) {
        wchar_t ch = upper[0];
        if (ch >= L'A' && ch <= L'Z') return (UINT)ch;
        if (ch >= L'0' && ch <= L'9') return (UINT)ch;
        SHORT scan = VkKeyScanW(ch); // Other printable characters (e.g. ",", "/").
        if (scan != -1) return (UINT)(scan & 0xFF);
    }
    return 0;
}

// Render a virtual-key code the way hotkeys.json spells it (the inverse of
// HotkeyKeyFromName; used when writing the file and for on-screen hints).
static std::wstring HotkeyKeyName(UINT vk) {
    if (vk >= 'A' && vk <= 'Z') return std::wstring(1, (wchar_t)vk);
    if (vk >= '0' && vk <= '9') return std::wstring(1, (wchar_t)vk);
    static const struct { UINT vk; const wchar_t* name; } named[] = {
        { VK_SPACE, L"Space" }, { VK_RETURN, L"Enter" }, { VK_TAB, L"Tab" },
        { VK_ESCAPE, L"Esc" }, { VK_BACK, L"Back" }, { VK_DELETE, L"Del" },
        { VK_INSERT, L"Ins" }, { VK_UP, L"Up" }, { VK_DOWN, L"Down" },
        { VK_LEFT, L"Left" }, { VK_RIGHT, L"Right" }, { VK_HOME, L"Home" },
        { VK_END, L"End" }, { VK_PRIOR, L"PgUp" }, { VK_NEXT, L"PgDn" },
        { VK_SNAPSHOT, L"PrtSc" }, { VK_PAUSE, L"Pause" },
    };
    for (const auto& n : named) if (n.vk == vk) return n.name;
    if (vk >= VK_F1 && vk <= VK_F12) return L"F" + std::to_wstring((int)(vk - VK_F1 + 1));
    wchar_t ch = (wchar_t)MapVirtualKeyW(vk, MAPVK_VK_TO_CHAR); // Any other printable key.
    return ch ? std::wstring(1, ch) : L"?";
}

// Render one binding with "+"-joined modifiers, e.g. "Ctrl+Shift+R".
static std::wstring HotkeyText(const Hotkey& h) {
    std::wstring out;
    if (h.mods & MOD_CONTROL) out += L"Ctrl+";
    if (h.mods & MOD_SHIFT) out += L"Shift+";
    if (h.mods & MOD_ALT) out += L"Alt+";
    if (h.mods & MOD_WIN) out += L"Win+";
    return out + HotkeyKeyName(h.vk);
}

// Parse a "Ctrl+Shift+R" style binding; modifiers and key names are case-insensitive.
static bool HotkeyFromText(const std::wstring& text, Hotkey& out) {
    Hotkey h;
    size_t start = 0;
    while (start < text.size()) {
        size_t plus = text.find(L'+', start);
        std::wstring part = text.substr(start, plus == std::wstring::npos ? std::wstring::npos : plus - start);
        std::wstring upper = part;
        std::transform(upper.begin(), upper.end(), upper.begin(), [](wchar_t c) { return (wchar_t)towupper(c); });
        if (plus == std::wstring::npos) {
            if (h.vk) return false;                       // Two key parts, no "+" left.
            h.vk = HotkeyKeyFromName(upper);
            if (!h.vk) return false;                      // Unknown key name.
            break;
        }
        if (upper == L"CTRL" || upper == L"CONTROL") h.mods |= MOD_CONTROL;
        else if (upper == L"SHIFT") h.mods |= MOD_SHIFT;
        else if (upper == L"ALT") h.mods |= MOD_ALT;
        else if (upper == L"WIN" || upper == L"CMD") h.mods |= MOD_WIN;
        else return false;
        start = plus + 1;
    }
    if (start >= text.size()) return false;               // Trailing "+".
    out = h;
    return true;
}

// Extract the JSON value that follows a "key": match and hand back the raw value:
// strings are unquoted, arrays keep their brackets so the caller can split them.
static std::string JsonValueField(const std::string& json, const char* key) {
    std::string quoted = std::string("\"") + key + "\"";
    size_t p = json.find(quoted);
    if (p == std::string::npos) return {};
    size_t at = p + quoted.size(); // Skip whitespace, then require a real key (a colon).
    while (at < json.size() && (json[at] == ' ' || json[at] == '\t' || json[at] == '\r' || json[at] == '\n')) ++at;
    if (at >= json.size() || json[at] != ':') return {};
    ++at;
    while (at < json.size() && (json[at] == ' ' || json[at] == '\t' || json[at] == '\r' || json[at] == '\n')) ++at;
    if (at >= json.size()) return {};
    if (json[at] == '"') {
        size_t q2 = json.find('"', at + 1);
        return q2 == std::string::npos ? std::string() : json.substr(at + 1, q2 - at - 1);
    }
    if (json[at] == '[') {
        size_t close = json.find(']', at);
        return close == std::string::npos ? std::string() : json.substr(at + 1, close - at - 1);
    }
    return {};
}

// Write the current binding set to hotkeys.json. Only called to create the file
// on first run so there is always something to edit; a file the user already has
// is never rewritten by loading or reloading.
static void SaveHotkeys() {
    std::ofstream f(std::filesystem::path(HotkeysPath()), std::ios::binary | std::ios::trunc);
    if (!f) return;
    f << "{\n";
    for (int a = 0; a < HK_COUNT; ++a) {
        f << "  \"" << Utf8(HK_NAMES[a]) << "\": ";
        const std::vector<Hotkey>& keys = g_hotkeys[a];
        if (keys.size() == 1) {
            f << '"' << Utf8(HotkeyText(keys[0])) << '"';
        } else {
            f << '[';
            for (size_t i = 0; i < keys.size(); ++i) {
                if (i) f << ", ";
                f << '"' << Utf8(HotkeyText(keys[i])) << '"';
            }
            f << ']';
        }
        f << (a + 1 == HK_COUNT ? "\n" : ",\n");
    }
    f << "}\n";
}

// Read hotkeys.json over the built-in defaults. A missing file writes the
// defaults so there is always a file to edit; a malformed, unknown, or
// duplicate entry is ignored and that action keeps its default (never an error).
static void LoadHotkeys() {
    ResetHotkeysToDefaults();
    std::ifstream f(std::filesystem::path(HotkeysPath()), std::ios::binary);
    if (!f) { SaveHotkeys(); return; }
    std::string json((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    for (int a = 0; a < HK_COUNT; ++a) {
        std::string value = JsonValueField(json, Utf8(HK_NAMES[a]).c_str());
        if (value.empty()) continue;
        // Split array members on commas; a single binding is one member.
        std::vector<Hotkey> parsed;
        size_t start = 0;
        while (start <= value.size()) {
            size_t comma = value.find(',', start);
            std::string member = value.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
            size_t q1 = member.find('"'), q2 = member.find('"', q1 + 1);
            Hotkey h;
            if (q1 != std::string::npos && q2 != std::string::npos
                && HotkeyFromText(Wide(member.substr(q1 + 1, q2 - q1 - 1)), h)) parsed.push_back(h);
            if (comma == std::string::npos) break;
            start = comma + 1;
        }
        if (!parsed.empty()) g_hotkeys[a] = parsed;
    }
}

// The shortcut currently held down as modifier flags plus a virtual-key code.
static Hotkey HotkeyFromKeysDown(WPARAM wp) {
    Hotkey h;
    h.vk = (UINT)wp;
    if (GetKeyState(VK_CONTROL) & 0x8000) h.mods |= MOD_CONTROL;
    if (GetKeyState(VK_SHIFT) & 0x8000) h.mods |= MOD_SHIFT;
    if (GetKeyState(VK_MENU) & 0x8000) h.mods |= MOD_ALT;
    if ((GetKeyState(VK_LWIN) | GetKeyState(VK_RWIN)) & 0x8000) h.mods |= MOD_WIN;
    return h;
}

// The action bound to the key being pressed, or -1. Modifiers must match
// exactly so that, say, Ctrl+Shift+R and Ctrl+R stay distinguishable.
static int HotkeyAction(WPARAM wp) {
    Hotkey pressed = HotkeyFromKeysDown(wp);
    for (int a = 0; a < HK_COUNT; ++a)
        for (const Hotkey& h : g_hotkeys[a])
            if (h.vk == pressed.vk && h.mods == pressed.mods) return a;
    return -1;
}

// The "(ctrl+shift+r)" style hint used in menus and buttons for an action.
// Empty when the action has no binding (only possible via an empty list).
static std::wstring HotkeyHint(int action) {
    if (action < 0 || action >= HK_COUNT || g_hotkeys[action].empty()) return {};
    std::wstring text = HotkeyText(g_hotkeys[action].front());
    std::transform(text.begin(), text.end(), text.begin(), [](wchar_t c) { return (wchar_t)towlower(c); });
    return L"(" + text + L")";
}

// Paint a rectangle with one solid color, then release the temporary brush.
static void Fill(HDC dc, const RECT& r, COLORREF color) {
    HBRUSH b = CreateSolidBrush(color);
    FillRect(dc, &r, b);
    DeleteObject(b);
}

// Free the cached double-buffer. Called when the window is destroyed, and
// whenever the buffer has to be rebuilt for a new client size. The DC is
// deleted first so the bitmap is no longer selected into it and can be freed.
static void ReleaseBackBuffer() {
    if (g_backDC) { DeleteDC(g_backDC); g_backDC = nullptr; }
    if (g_backBitmap) { DeleteObject(g_backBitmap); g_backBitmap = nullptr; }
    g_backW = 0; g_backH = 0;
}

// Return the cached double-buffer sized w x h, creating it on first use and
// recreating it only when the client area changes. Reusing one buffer across
// paints is what keeps the process footprint flat: the board repaints ten times
// a second while a stopwatch runs, and a fresh full-window bitmap per repaint
// left the working set growing without bound. A minimized window reports a
// zero-size client, which has nothing to draw, so it gets no buffer.
static HDC BackBuffer(HDC dc, int w, int h) {
    if (w <= 0 || h <= 0) return nullptr;
    if (g_backDC && (g_backW != w || g_backH != h)) ReleaseBackBuffer();
    if (!g_backDC) {
        g_backDC = CreateCompatibleDC(dc);
        if (!g_backDC) return nullptr;
        g_backBitmap = CreateCompatibleBitmap(dc, w, h);
        if (!g_backBitmap) { ReleaseBackBuffer(); return nullptr; }
        SelectObject(g_backDC, g_backBitmap); // The stock 1x1 mono bitmap goes with the DC.
        g_backW = w; g_backH = h;
    }
    return g_backDC;
}

// Calculate the screen rectangle occupied by one of the three columns.
static RECT ColumnRect(const RECT& client, int column) {
    const int margin = 16, gap = 10, top = 16, bottom = 16;
    int available = std::max(300, static_cast<int>(client.right - client.left) - margin * 2 - gap * 2);
    int width = available / 3;
    int left = margin + column * (width + gap);
    int right = (column == 2) ? client.right - margin : left + width;
    return {left, top, right, client.bottom - bottom};
}

// Calculate the clickable "Add a card" area at the bottom of the first column.
static RECT AddRect(const RECT& col) { return {col.left + 10, col.bottom - 40, col.right - 10, col.bottom - 10}; }

// Calculate the clickable "Reset Time" area at the bottom of the In-Progress column.
static RECT ResetRect(const RECT& col) { return {col.left + 10, col.bottom - 40, col.right - 10, col.bottom - 10}; }

// Ask Windows to use a dark title bar when that feature is available.
static void SetDarkTitleBar(HWND hwnd) {
    HMODULE dwm = LoadLibraryW(L"dwmapi.dll");
    if (!dwm) return;
    // This function is loaded dynamically so the program can still run on older Windows versions.
    using DwmSetWindowAttributeFn = HRESULT(WINAPI*)(HWND, DWORD, LPCVOID, DWORD);
    auto setAttribute = reinterpret_cast<DwmSetWindowAttributeFn>(GetProcAddress(dwm, "DwmSetWindowAttribute"));
    if (setAttribute) {
        BOOL enabled = TRUE;
        setAttribute(hwnd, 20, &enabled, sizeof(enabled));
    }
    FreeLibrary(dwm);
}

// F1 help window. The help text is generated at open time from the *configured*
// hotkey bindings (via HotkeyText), so after editing hotkeys.json and choosing
// "Reload Hotkeys" the page always shows the current shortcuts. Nothing is
// written to disk; the app keeps working offline.

// Describe one keyboard action with its bindings, e.g. "Delete card: Del, D".
static std::wstring HelpBindingLine(const wchar_t* label, int action) {
    std::wstring out(label);
    out += L": ";
    for (size_t i = 0; i < g_hotkeys[action].size(); ++i) {
        if (i) out += L", ";
        out += HotkeyText(g_hotkeys[action][i]);
    }
    return out;
}

// Assemble the entire help page: columns, card visuals, mouse interactions, the
// keyboard actions (each with its live bindings) and the dialog shortcuts.
static std::wstring BuildHelpText() {
    std::wstring t;
    t += L"Minimal Kanban - Help (v" + std::wstring(APP_VERSION) + L")\r\n";
    t += L"\r\n";
    t += L"The board has three columns:\r\n";
    t += L"  Todo - tasks not started yet\r\n";
    t += L"  In-Progress - tasks being worked on now\r\n";
    t += L"  Complete - finished tasks\r\n";
    t += L"\r\n";
    t += L"Cards:\r\n";
    t += L"  Task text - the card's description\r\n";
    t += L"  Gray time - the total stopwatch time\r\n";
    t += L"  Green +time - the current stopwatch countup (running or paused; survives restarts, cleared by Reset Time)\r\n";
    t += L"    A stopwatch only counts up on a card in the Todo column, and moving a card out of Todo pauses it\r\n";
    t += L"  Red outline - the card is blocked\r\n";
    t += L"  Green outline - the stopwatch is running on this card\r\n";
    t += L"  Black outline - the focus box: the card the arrow keys picked, or the card you last acted on\r\n";
    t += L"    After 10 idle seconds it returns to the card with the running stopwatch, or goes away when none runs\r\n";
    t += L"\r\n";
    t += L"Mouse:\r\n";
    t += L"  Click \"+ Add a card\" - add a task to the Todo column\r\n";
    t += L"  Click \"Reset Time\" at the bottom of In-Progress - push every countup into its card's total\r\n";
    t += L"  Drag a card - move it to another column (leaving Todo pauses its stopwatch)\r\n";
    t += L"  Shift+click a card - start/stop its stopwatch (Todo cards only)\r\n";
    t += L"  Ctrl+click a card - toggle its blocked flag\r\n";
    t += L"  Right-click a card - edit, delete or set its timer\r\n";
    t += L"  Right-click empty board space - reports, updates, reload hotkeys\r\n";
    t += L"\r\n";
    t += L"Keyboard (F1 stays fixed and is not rebindable):\r\n";
    t += L"  F1 - show this help page\r\n";
    t += HelpBindingLine(L"  Add a new card", HK_ADD) + L"\r\n";
    t += HelpBindingLine(L"  Edit card text", HK_EDIT) + L"\r\n";
    t += HelpBindingLine(L"  Delete card (asks to confirm)", HK_DELETE) + L"\r\n";
    t += HelpBindingLine(L"  Toggle stopwatch (Todo cards only)", HK_TOGGLE_TIMER) + L"\r\n";
    t += HelpBindingLine(L"  Set stopwatch time manually", HK_EDIT_TIMER) + L"\r\n";
    t += HelpBindingLine(L"  Toggle blocked flag", HK_BLOCKED) + L"\r\n";
    t += HelpBindingLine(L"  Select previous card", HK_SELECT_UP) + L"\r\n";
    t += HelpBindingLine(L"  Select next card", HK_SELECT_DOWN) + L"\r\n";
    t += HelpBindingLine(L"  Move card to the previous column", HK_MOVE_LEFT) + L"\r\n";
    t += HelpBindingLine(L"  Move card to the next column", HK_MOVE_RIGHT) + L"\r\n";
    t += HelpBindingLine(L"  File a bug report", HK_REPORT) + L"\r\n";
    t += HelpBindingLine(L"  File a report in your own words", HK_REPORT_PROMPT) + L"\r\n";
    t += HelpBindingLine(L"  Check for updates", HK_UPDATE) + L"\r\n";
    t += HelpBindingLine(L"  Reset Time: fold every countup into its total", HK_NEW_DAY) + L"\r\n";
    t += L"\r\n";
    t += L"Keyboard card actions apply to the card under the mouse, falling back\r\n";
    t += L"to the focused card when the pointer is not over one. Acting on a card\r\n";
    t += L"(stopwatch, blocked, edit) moves the focus box onto it, and so does adding\r\n";
    t += L"a card.\r\n";
    t += L"\r\n";
    t += L"Dialogs:\r\n";
    t += L"  Enter / Esc - accept or dismiss the add-card and set-timer dialogs\r\n";
    t += L"  Ctrl+Backspace - delete the previous word in a text box\r\n";
    t += L"  Ctrl+Enter - submit the \"Report with your own words\" dialog\r\n";
    t += L"  Deleting a card always asks first: Enter deletes, Esc or Backspace keeps it\r\n";
    t += L"\r\n";
    t += L"The shortcuts above are configured in hotkeys.json next to the app.\r\n";
    t += L"Edit that file and choose \"Reload Hotkeys\" (right-click empty board\r\n";
    t += L"space) to apply the changes without restarting. This page always shows\r\n";
    t += L"the configured shortcuts.\r\n";
    return t;
}

// Window procedure for the help window: a dark, scrollable, read-only text view.
static LRESULT CALLBACK HelpProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE: {
        // A read-only multiline EDIT control fills the client area and scrolls.
        CREATESTRUCT* cs = (CREATESTRUCT*)lp;
        RECT client{}; GetClientRect(hwnd, &client);
        HWND edit = CreateWindowW(L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_BORDER | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL,
            0, 0, client.right, client.bottom, hwnd, (HMENU)100, GetModuleHandleW(nullptr), nullptr);
        SendMessageW(edit, WM_SETFONT, (WPARAM)g_font, TRUE);
        if (cs->lpCreateParams) SetWindowTextW(edit, ((std::wstring*)cs->lpCreateParams)->c_str());
        SetFocus(edit);
        return 0;
    }
    case WM_ERASEBKGND: { RECT client{}; GetClientRect(hwnd, &client); Fill((HDC)wp, client, RGB(32,32,32)); return 1; }
    case WM_CTLCOLORSTATIC: case WM_CTLCOLOREDIT: {
        HDC dc = (HDC)wp; SetTextColor(dc, RGB(225,225,225)); SetBkColor(dc, RGB(32,32,32));
        static HBRUSH brush = CreateSolidBrush(RGB(32,32,32)); return (LRESULT)brush;
    }
    case WM_SIZE: {
        HWND edit = GetDlgItem(hwnd, 100);
        if (edit) MoveWindow(edit, 0, 0, LOWORD(lp), HIWORD(lp), TRUE);
        return 0;
    }
    case WM_KEYDOWN:
        if (wp == VK_ESCAPE) { DestroyWindow(hwnd); return 0; }
        break;
    case WM_CLOSE: DestroyWindow(hwnd); return 0;
    case WM_DESTROY: g_helpHwnd = nullptr; return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// Open (or refresh) the F1 help window. Rebuilding the text each time keeps the
// page truthful after hotkeys.json is edited and reloaded.
static void ShowHelp(HWND owner) {
    std::wstring text = BuildHelpText();
    if (g_helpHwnd && IsWindow(g_helpHwnd)) {
        HWND edit = GetDlgItem(g_helpHwnd, 100);
        if (edit) SetWindowTextW(edit, text.c_str());
        SetForegroundWindow(g_helpHwnd);
        return;
    }
    RECT pr{}; GetWindowRect(owner, &pr);
    int x = pr.left + (pr.right - pr.left - 560) / 2;
    int y = pr.top + (pr.bottom - pr.top - 600) / 2;
    g_helpHwnd = CreateWindowExW(WS_EX_APPWINDOW, HELP_CLASS, L"Minimal Kanban - Help",
        WS_OVERLAPPEDWINDOW | WS_VISIBLE, x, y, 560, 600, owner, nullptr, GetModuleHandleW(nullptr), &text);
    if (g_helpHwnd) SetDarkTitleBar(g_helpHwnd);
}

// Return the rectangles of all cards that fit inside their columns.
// Each pair contains the card's index in g_cards and its screen rectangle.
static std::vector<std::pair<int, RECT>> CardRects(const RECT& client) {
    std::vector<std::pair<int, RECT>> result;
    int y[3] = {62, 62, 62};
    for (int i = 0; i < (int)g_cards.size(); ++i) {
        int c = g_cards[i].column;
        RECT col = ColumnRect(client, c);
        RECT r{col.left + 10, y[c], col.right - 10, y[c] + CARD_HEIGHT};
        if (r.bottom < col.bottom - 48) result.push_back({i, r});
        y[c] += CARD_SPACING;
    }
    return result;
}

// Read the whole contents of an edit control.
static std::wstring EditText(HWND edit) {
    int n = GetWindowTextLengthW(edit);
    std::wstring text(n + 1, L'\0');
    GetWindowTextW(edit, text.data(), n + 1);
    text.resize(n);
    return text;
}

// Characters that separate words when walking backwards from the caret.
static bool IsWordSeparator(wchar_t ch) {
    return ch == L' ' || ch == L'\t' || ch == L'\r' || ch == L'\n';
}

// Delete the word in front of the caret (Ctrl+Backspace). With no selection the
// word plus the whitespace before it is removed and the caret lands where the
// word started; with a selection the selection itself is deleted. The dialogs call
// this from the modal loop because Windows never hands Ctrl+Backspace to their
// edit boxes.
static void DeleteWordBack(HWND edit) {
    if (!edit) return;
    DWORD from = 0, to = 0;
    SendMessageW(edit, EM_GETSEL, (WPARAM)&from, (LPARAM)&to);
    std::wstring text = EditText(edit);
    size_t start = std::min<size_t>(from, to), end = std::max<size_t>(from, to);
    start = std::min(start, text.size());
    end = std::min(end, text.size());
    if (start == end) {
        // No selection: step back over any whitespace, then over the word itself.
        while (start > 0 && IsWordSeparator(text[start - 1])) --start;
        while (start > 0 && !IsWordSeparator(text[start - 1])) --start;
    }
    if (start == end) return;
    SendMessageW(edit, EM_SETSEL, (WPARAM)start, (LPARAM)end);
    SendMessageW(edit, EM_REPLACESEL, TRUE, (LPARAM)L"");
}

// Shared modal loop for the app's dialogs. Besides pumping messages until the
// dialog closes, it supplies the shortcuts the dialog plumbing swallows:
// Ctrl+Backspace deletes the previous word in the text box, dialogs that have no
// text box at all (the delete confirmation) treat a plain Backspace as "no", and
// dialogs that ask for it (the report prompt) get Ctrl+Enter as a shortcut for
// the Submit button.
static void PumpDialog(HWND dlg, HWND edit, bool* done, bool ctrlEnterSubmits) {
    MSG msg;
    while (!*done && GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (msg.message == WM_KEYDOWN && msg.wParam == VK_BACK && !edit) {
            // No text box to edit, so Backspace is the second "no" key next to Esc.
            // Handled here because a focused child control would swallow the key
            // before it reached the dialog's own window procedure.
            SendMessageW(dlg, WM_COMMAND, MAKEWPARAM(IDCANCEL, BN_CLICKED), (LPARAM)GetDlgItem(dlg, IDCANCEL));
            continue;
        }
        if (msg.message == WM_KEYDOWN && (GetKeyState(VK_CONTROL) & 0x8000)) {
            if (msg.wParam == VK_BACK && edit && GetFocus() == edit) {
                DeleteWordBack(edit);
                continue;
            }
            if (ctrlEnterSubmits && msg.wParam == VK_RETURN) {
                SendMessageW(dlg, WM_COMMAND, MAKEWPARAM(IDOK, BN_CLICKED), (LPARAM)GetDlgItem(dlg, IDOK));
                continue;
            }
        }
        if (!IsDialogMessageW(dlg, &msg)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    }
}

// State used while the add-card window is open.
// The main window waits until done becomes true, then checks accepted and text.
struct InputState { HWND edit = nullptr; bool done = false; bool accepted = false; std::wstring text; std::wstring initial; };
static InputState* g_input = nullptr;

// Window procedure for the small add-card window.
// Windows calls this function whenever that window receives an event/message.
static LRESULT CALLBACK InputProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE: {
        // Create the text box and the two buttons as child controls.
        g_input->edit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
            16, 20, 328, 28, hwnd, (HMENU)100, GetModuleHandleW(nullptr), nullptr);
        HWND addButton = CreateWindowW(L"BUTTON", L"Add", WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON | BS_OWNERDRAW,
            188, 62, 75, 28, hwnd, (HMENU)IDOK, GetModuleHandleW(nullptr), nullptr);
        HWND cancelButton = CreateWindowW(L"BUTTON", L"Cancel", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
            269, 62, 75, 28, hwnd, (HMENU)IDCANCEL, GetModuleHandleW(nullptr), nullptr);
        // Remove the theme decoration from the buttons so their colors match the dark UI.
        HMODULE uxtheme = LoadLibraryW(L"uxtheme.dll");
        if (uxtheme) {
            using SetWindowThemeFn = HRESULT(WINAPI*)(HWND, LPCWSTR, LPCWSTR);
            auto setWindowTheme = reinterpret_cast<SetWindowThemeFn>(GetProcAddress(uxtheme, "SetWindowTheme"));
            if (setWindowTheme) {
                setWindowTheme(addButton, L"", L"");
                setWindowTheme(cancelButton, L"", L"");
            }
            FreeLibrary(uxtheme);
        }
        SendMessageW(g_input->edit, WM_SETFONT, (WPARAM)g_font, TRUE);
        for (HWND child = GetWindow(hwnd, GW_CHILD); child; child = GetWindow(child, GW_HWNDNEXT))
            SendMessageW(child, WM_SETFONT, (WPARAM)g_font, TRUE);
        if (!g_input->initial.empty()) SetWindowTextW(g_input->edit, g_input->initial.c_str());
        SetFocus(g_input->edit); // Put the keyboard cursor in the text box immediately.
        return 0;
    }
    case WM_ERASEBKGND: {
        // Paint the dialog background ourselves to prevent flicker and use the dark color.
        RECT client{}; GetClientRect(hwnd, &client);
        Fill((HDC)wp, client, RGB(32, 32, 32));
        return 1;
    }
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLOREDIT: {
        HDC dc = (HDC)wp;
        SetTextColor(dc, RGB(230, 230, 230));
        SetBkColor(dc, RGB(32, 32, 32));
        static HBRUSH brush = CreateSolidBrush(RGB(32, 32, 32));
        return (LRESULT)brush;
    }
    case WM_CTLCOLORBTN: {
        HDC dc = (HDC)wp;
        SetTextColor(dc, RGB(230, 230, 230));
        SetBkColor(dc, RGB(55, 55, 55));
        static HBRUSH brush = CreateSolidBrush(RGB(55, 55, 55));
        return (LRESULT)brush;
    }
    case WM_DRAWITEM: {
        // Owner-draw the Add/Cancel buttons to match the dark card background.
        DRAWITEMSTRUCT* ds = (DRAWITEMSTRUCT*)lp;
        if (ds->CtlType == ODT_BUTTON) {
            HBRUSH br = CreateSolidBrush(RGB(50, 50, 50));
            FillRect(ds->hDC, &ds->rcItem, br);
            DeleteObject(br);
            SetTextColor(ds->hDC, RGB(230, 230, 230));
            SetBkMode(ds->hDC, TRANSPARENT);
            wchar_t buf[64];
            GetWindowTextW(ds->hwndItem, buf, 64);
            DrawTextW(ds->hDC, buf, -1, &ds->rcItem, DT_CENTER | DT_SINGLELINE | DT_VCENTER);
            if (ds->itemState & ODS_FOCUS) {
                RECT r = ds->rcItem; InflateRect(&r, -3, -3);
                DrawFocusRect(ds->hDC, &r);
            }
            return TRUE;
        }
        break;
    }
    case WM_COMMAND:
        if (LOWORD(wp) == IDOK) {
            // IDOK means the Add button was pressed.
            int n = GetWindowTextLengthW(g_input->edit);
            std::wstring text(n + 1, L'\0');
            GetWindowTextW(g_input->edit, text.data(), n + 1);
            text.resize(n);
            if (!text.empty()) { g_input->text = text; g_input->accepted = true; }
            DestroyWindow(hwnd);
            return 0;
        }
        if (LOWORD(wp) == IDCANCEL) { DestroyWindow(hwnd); return 0; }
        break;
    case WM_CLOSE: DestroyWindow(hwnd); return 0;
    case WM_DESTROY: g_input->done = true; return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// Show the add-card window and wait until the user accepts or cancels it.
// If initial is non-empty, the dialog is pre-populated (edit mode).
static bool AskForCard(HWND owner, std::wstring& out, const std::wstring& initial = L"") {
    InputState state;
    state.initial = initial;
    g_input = &state;
    EnableWindow(owner, FALSE); // Make the main window temporarily non-interactive.
    RECT pr{}; GetWindowRect(owner, &pr);
    int x = pr.left + (pr.right - pr.left - 376) / 2;
    int y = pr.top + (pr.bottom - pr.top - 140) / 2;
    HWND dlg = CreateWindowExW(WS_EX_DLGMODALFRAME | WS_EX_TOPMOST, INPUT_CLASS, L"Add a card",
        WS_CAPTION | WS_SYSMENU | WS_VISIBLE, x, y, 376, 140, owner, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!dlg) { EnableWindow(owner, TRUE); g_input = nullptr; return false; }
    SetDarkTitleBar(dlg);
    // This is a small modal message loop: process dialog messages until it closes.
    PumpDialog(dlg, state.edit, &state.done, false);
    EnableWindow(owner, TRUE); SetForegroundWindow(owner); g_input = nullptr;
    if (state.accepted) out = state.text;
    return state.accepted;
}

// Ask for a card, add it to the Todo column, save it, and redraw the board.
// A newly created card takes the focus box, so it is where the board points
// right after it appears.
static void AddCard(HWND hwnd) {
    std::wstring text;
    if (AskForCard(hwnd, text)) {
        g_cards.push_back({text, 0});
        SaveCards();
        FocusCard(hwnd, (int)g_cards.size() - 1);
    }
}

// State for the manual time entry dialog.
struct TimeInputState { HWND edit = nullptr; bool done = false; bool accepted = false; std::wstring text; };
static TimeInputState* g_timeInput = nullptr;

// Window procedure for the time entry dialog.
static LRESULT CALLBACK TimeInputProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE: {
        g_timeInput->edit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
            16, 20, 328, 28, hwnd, (HMENU)100, GetModuleHandleW(nullptr), nullptr);
        HWND okButton = CreateWindowW(L"BUTTON", L"OK", WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON | BS_OWNERDRAW,
            188, 62, 75, 28, hwnd, (HMENU)IDOK, GetModuleHandleW(nullptr), nullptr);
        HWND cancelButton = CreateWindowW(L"BUTTON", L"Cancel", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
            269, 62, 75, 28, hwnd, (HMENU)IDCANCEL, GetModuleHandleW(nullptr), nullptr);
        HMODULE uxtheme = LoadLibraryW(L"uxtheme.dll");
        if (uxtheme) {
            using SetWindowThemeFn = HRESULT(WINAPI*)(HWND, LPCWSTR, LPCWSTR);
            auto setWindowTheme = reinterpret_cast<SetWindowThemeFn>(GetProcAddress(uxtheme, "SetWindowTheme"));
            if (setWindowTheme) { setWindowTheme(okButton, L"", L""); setWindowTheme(cancelButton, L"", L""); }
            FreeLibrary(uxtheme);
        }
        SendMessageW(g_timeInput->edit, WM_SETFONT, (WPARAM)g_font, TRUE);
        for (HWND child = GetWindow(hwnd, GW_CHILD); child; child = GetWindow(child, GW_HWNDNEXT))
            SendMessageW(child, WM_SETFONT, (WPARAM)g_font, TRUE);
        if (!g_timeInput->text.empty()) SetWindowTextW(g_timeInput->edit, g_timeInput->text.c_str());
        SetFocus(g_timeInput->edit);
        return 0;
    }
    case WM_ERASEBKGND: { RECT client{}; GetClientRect(hwnd, &client); Fill((HDC)wp, client, RGB(32,32,32)); return 1; }
    case WM_CTLCOLORSTATIC: case WM_CTLCOLOREDIT: {
        HDC dc = (HDC)wp; SetTextColor(dc, RGB(230,230,230)); SetBkColor(dc, RGB(32,32,32));
        static HBRUSH brush = CreateSolidBrush(RGB(32,32,32)); return (LRESULT)brush;
    }
    case WM_CTLCOLORBTN: {
        HDC dc = (HDC)wp; SetTextColor(dc, RGB(230,230,230)); SetBkColor(dc, RGB(50,50,50));
        static HBRUSH brush = CreateSolidBrush(RGB(50,50,50)); return (LRESULT)brush;
    }
    case WM_DRAWITEM: {
        DRAWITEMSTRUCT* ds = (DRAWITEMSTRUCT*)lp;
        if (ds->CtlType == ODT_BUTTON) {
            HBRUSH br = CreateSolidBrush(RGB(50,50,50));
            FillRect(ds->hDC, &ds->rcItem, br); DeleteObject(br);
            SetTextColor(ds->hDC, RGB(230,230,230)); SetBkMode(ds->hDC, TRANSPARENT);
            wchar_t buf[64]; GetWindowTextW(ds->hwndItem, buf, 64);
            DrawTextW(ds->hDC, buf, -1, &ds->rcItem, DT_CENTER | DT_SINGLELINE | DT_VCENTER);
            if (ds->itemState & ODS_FOCUS) { RECT r = ds->rcItem; InflateRect(&r, -3, -3); DrawFocusRect(ds->hDC, &r); }
            return TRUE;
        }
        break;
    }
    case WM_COMMAND:
        if (LOWORD(wp) == IDOK) {
            int n = GetWindowTextLengthW(g_timeInput->edit);
            std::wstring text(n + 1, L'\0');
            GetWindowTextW(g_timeInput->edit, text.data(), n + 1);
            text.resize(n);
            if (!text.empty()) { g_timeInput->text = text; g_timeInput->accepted = true; }
            DestroyWindow(hwnd); return 0;
        }
        if (LOWORD(wp) == IDCANCEL) { DestroyWindow(hwnd); return 0; }
        break;
    case WM_CLOSE: DestroyWindow(hwnd); return 0;
    case WM_DESTROY: g_timeInput->done = true; return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// Parse a time string like "45", "1:03", "1:30:15" into milliseconds.
// Returns -1 on parse failure.
static LONGLONG ParseTimeString(const std::wstring& s) {
    std::vector<LONGLONG> parts;
    std::wstring current;
    for (wchar_t ch : s) {
        if (ch == L':') { if (current.empty()) return -1; parts.push_back(std::stoll(current)); current.clear(); }
        else if (ch >= L'0' && ch <= L'9') current += ch;
        else return -1;
    }
    if (current.empty()) return -1;
    parts.push_back(std::stoll(current));
    if (parts.size() == 1) return parts[0] * 1000;
    if (parts.size() == 2) return (parts[0] * 60 + parts[1]) * 1000;
    if (parts.size() == 3) return (parts[0] * 3600 + parts[1] * 60 + parts[2]) * 1000;
    return -1;
}

// Show the manual time entry dialog for a card.
static void AskForTime(HWND hwnd, int cardIndex) {
    TimeInputState state;
    // Pre-populate with current total time if any.
    LONGLONG ms = TotalElapsedMs(g_cards[cardIndex]);
    if (ms > 0) state.text = FormatTimer(ms);
    g_timeInput = &state;
    EnableWindow(hwnd, FALSE);
    RECT pr{}; GetWindowRect(hwnd, &pr);
    int x = pr.left + (pr.right - pr.left - 376) / 2;
    int y = pr.top + (pr.bottom - pr.top - 140) / 2;
    HWND dlg = CreateWindowExW(WS_EX_DLGMODALFRAME | WS_EX_TOPMOST, TIME_CLASS, L"Set timer (ss / m:ss / h:mm:ss)",
        WS_CAPTION | WS_SYSMENU | WS_VISIBLE, x, y, 376, 140, hwnd, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!dlg) { EnableWindow(hwnd, TRUE); g_timeInput = nullptr; return; }
    SetDarkTitleBar(dlg);
    PumpDialog(dlg, state.edit, &state.done, false);
    EnableWindow(hwnd, TRUE); SetForegroundWindow(hwnd);
    if (state.accepted) {
        LONGLONG parsed = ParseTimeString(state.text);
        if (parsed >= 0) {
            g_cards[cardIndex].timerAccumulated = parsed;
            g_cards[cardIndex].timerStart = 0; // stopped after manual entry
            g_cards[cardIndex].sessionAccumulated = 0; // explicit set lets the run countup start fresh
            SaveCards(); FocusCard(hwnd, cardIndex);
        }
    }
    g_timeInput = nullptr;
}

// Remove leading and trailing spaces and tabs from a string.
static std::wstring Trim(const std::wstring& s) {
    size_t b = s.find_first_not_of(L" \t");
    if (b == std::wstring::npos) return {};
    size_t e = s.find_last_not_of(L" \t");
    return s.substr(b, e - b + 1);
}

// State for the natural-language report prompt dialog.
struct PromptState { HWND edit = nullptr; bool done = false; bool accepted = false; std::wstring text; };
static PromptState* g_prompt = nullptr;

// Window procedure for the report prompt dialog: a dark multiline box with Submit/Cancel.
static LRESULT CALLBACK PromptInputProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE: {
        HWND hint = CreateWindowW(L"STATIC", L"Describe what you want changed in your own words.",
            WS_CHILD | WS_VISIBLE, 16, 10, 428, 20, hwnd, (HMENU)101, GetModuleHandleW(nullptr), nullptr);
        g_prompt->edit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN,
            16, 34, 428, 106, hwnd, (HMENU)100, GetModuleHandleW(nullptr), nullptr);
        // Submit is deliberately not the default button: the edit box has ES_WANTRETURN, so a
        // default button would make Enter both insert a newline and submit the dialog.
        HWND submitButton = CreateWindowW(L"BUTTON", L"Submit", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
            288, 150, 75, 28, hwnd, (HMENU)IDOK, GetModuleHandleW(nullptr), nullptr);
        HWND cancelButton = CreateWindowW(L"BUTTON", L"Cancel", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
            369, 150, 75, 28, hwnd, (HMENU)IDCANCEL, GetModuleHandleW(nullptr), nullptr);
        HMODULE uxtheme = LoadLibraryW(L"uxtheme.dll");
        if (uxtheme) {
            using SetWindowThemeFn = HRESULT(WINAPI*)(HWND, LPCWSTR, LPCWSTR);
            auto setWindowTheme = reinterpret_cast<SetWindowThemeFn>(GetProcAddress(uxtheme, "SetWindowTheme"));
            if (setWindowTheme) { setWindowTheme(submitButton, L"", L""); setWindowTheme(cancelButton, L"", L""); }
            FreeLibrary(uxtheme);
        }
        for (HWND child = hint; child; child = GetWindow(child, GW_HWNDNEXT))
            SendMessageW(child, WM_SETFONT, (WPARAM)g_font, TRUE);
        SendMessageW(g_prompt->edit, EM_SETLIMITTEXT, PROMPT_LIMIT, 0);
        SetFocus(g_prompt->edit);
        return 0;
    }
    case WM_ERASEBKGND: { RECT client{}; GetClientRect(hwnd, &client); Fill((HDC)wp, client, RGB(32,32,32)); return 1; }
    case WM_CTLCOLORSTATIC: case WM_CTLCOLOREDIT: {
        HDC dc = (HDC)wp; SetTextColor(dc, RGB(230,230,230)); SetBkColor(dc, RGB(32,32,32));
        static HBRUSH brush = CreateSolidBrush(RGB(32,32,32)); return (LRESULT)brush;
    }
    case WM_CTLCOLORBTN: {
        HDC dc = (HDC)wp; SetTextColor(dc, RGB(230,230,230)); SetBkColor(dc, RGB(50,50,50));
        static HBRUSH brush = CreateSolidBrush(RGB(50,50,50)); return (LRESULT)brush;
    }
    case WM_DRAWITEM: {
        DRAWITEMSTRUCT* ds = (DRAWITEMSTRUCT*)lp;
        if (ds->CtlType == ODT_BUTTON) {
            HBRUSH br = CreateSolidBrush(RGB(50,50,50));
            FillRect(ds->hDC, &ds->rcItem, br); DeleteObject(br);
            SetTextColor(ds->hDC, RGB(230,230,230)); SetBkMode(ds->hDC, TRANSPARENT);
            wchar_t buf[64]; GetWindowTextW(ds->hwndItem, buf, 64);
            DrawTextW(ds->hDC, buf, -1, &ds->rcItem, DT_CENTER | DT_SINGLELINE | DT_VCENTER);
            if (ds->itemState & ODS_FOCUS) { RECT r = ds->rcItem; InflateRect(&r, -3, -3); DrawFocusRect(ds->hDC, &r); }
            return TRUE;
        }
        break;
    }
    case WM_COMMAND:
        if (LOWORD(wp) == IDOK) {
            int n = GetWindowTextLengthW(g_prompt->edit);
            std::wstring text(n + 1, L'\0');
            GetWindowTextW(g_prompt->edit, text.data(), n + 1);
            text.resize(n);
            // Ignore a blank prompt so Submit never opens an empty issue.
            if (!Trim(text).empty()) { g_prompt->text = text; g_prompt->accepted = true; }
            DestroyWindow(hwnd); return 0;
        }
        if (LOWORD(wp) == IDCANCEL) { DestroyWindow(hwnd); return 0; }
        break;
    case WM_CLOSE: DestroyWindow(hwnd); return 0;
    case WM_DESTROY: g_prompt->done = true; return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// Show the report prompt dialog and wait until the user submits or cancels it.
static bool AskForReportPrompt(HWND owner, std::wstring& out) {
    PromptState state;
    g_prompt = &state;
    const DWORD style = WS_CAPTION | WS_SYSMENU;
    const DWORD exStyle = WS_EX_DLGMODALFRAME | WS_EX_TOPMOST;
    // Size the window from the desired client area so the controls always fit.
    RECT frame{0, 0, PROMPT_CLIENT_W, PROMPT_CLIENT_H};
    AdjustWindowRectEx(&frame, style, FALSE, exStyle);
    int w = frame.right - frame.left, h = frame.bottom - frame.top;
    RECT pr{}; GetWindowRect(owner, &pr);
    int x = pr.left + (pr.right - pr.left - w) / 2;
    int y = pr.top + (pr.bottom - pr.top - h) / 2;
    EnableWindow(owner, FALSE); // Make the main window temporarily non-interactive.
    HWND dlg = CreateWindowExW(exStyle, PROMPT_CLASS, L"Report with your own words",
        style | WS_VISIBLE, x, y, w, h, owner, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!dlg) { EnableWindow(owner, TRUE); g_prompt = nullptr; return false; }
    SetDarkTitleBar(dlg);
    // Modal message loop: process dialog messages until it closes.
    // Ctrl+Enter submits, since Enter alone has to insert a newline in the text box.
    PumpDialog(dlg, state.edit, &state.done, true);
    EnableWindow(owner, TRUE); SetForegroundWindow(owner); g_prompt = nullptr;
    if (state.accepted) out = state.text;
    return state.accepted;
}

// State for the delete confirmation dialog.
struct ConfirmState { HWND edit = nullptr; bool done = false; bool accepted = false; std::wstring question; std::wstring detail; };
static ConfirmState* g_confirm = nullptr;

// Window procedure for the delete confirmation dialog: a question, the card text it
// is about to drop, and Delete/Cancel. Delete is the default button, so Enter
// confirms; Esc and Backspace both cancel.
static LRESULT CALLBACK ConfirmProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE: {
        CreateWindowW(L"STATIC", g_confirm->question.c_str(),
            WS_CHILD | WS_VISIBLE | SS_LEFT, 16, 12, 328, 20, hwnd, (HMENU)101, GetModuleHandleW(nullptr), nullptr);
        CreateWindowW(L"STATIC", g_confirm->detail.c_str(),
            WS_CHILD | WS_VISIBLE | SS_LEFT, 16, 38, 328, 60, hwnd, (HMENU)102, GetModuleHandleW(nullptr), nullptr);
        HWND deleteButton = CreateWindowW(L"BUTTON", L"Delete", WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON | BS_OWNERDRAW,
            188, 112, 75, 28, hwnd, (HMENU)IDOK, GetModuleHandleW(nullptr), nullptr);
        HWND cancelButton = CreateWindowW(L"BUTTON", L"Cancel", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
            269, 112, 75, 28, hwnd, (HMENU)IDCANCEL, GetModuleHandleW(nullptr), nullptr);
        HMODULE uxtheme = LoadLibraryW(L"uxtheme.dll");
        if (uxtheme) {
            using SetWindowThemeFn = HRESULT(WINAPI*)(HWND, LPCWSTR, LPCWSTR);
            auto setWindowTheme = reinterpret_cast<SetWindowThemeFn>(GetProcAddress(uxtheme, "SetWindowTheme"));
            if (setWindowTheme) { setWindowTheme(deleteButton, L"", L""); setWindowTheme(cancelButton, L"", L""); }
            FreeLibrary(uxtheme);
        }
        for (HWND child = GetWindow(hwnd, GW_CHILD); child; child = GetWindow(child, GW_HWNDNEXT))
            SendMessageW(child, WM_SETFONT, (WPARAM)g_font, TRUE);
        SetFocus(deleteButton);
        return 0;
    }
    case WM_ERASEBKGND: { RECT client{}; GetClientRect(hwnd, &client); Fill((HDC)wp, client, RGB(32,32,32)); return 1; }
    case WM_CTLCOLORSTATIC: case WM_CTLCOLOREDIT: {
        HDC dc = (HDC)wp; SetTextColor(dc, RGB(230,230,230)); SetBkColor(dc, RGB(32,32,32));
        static HBRUSH brush = CreateSolidBrush(RGB(32,32,32)); return (LRESULT)brush;
    }
    case WM_CTLCOLORBTN: {
        HDC dc = (HDC)wp; SetTextColor(dc, RGB(230,230,230)); SetBkColor(dc, RGB(50,50,50));
        static HBRUSH brush = CreateSolidBrush(RGB(50,50,50)); return (LRESULT)brush;
    }
    case WM_DRAWITEM: {
        DRAWITEMSTRUCT* ds = (DRAWITEMSTRUCT*)lp;
        if (ds->CtlType == ODT_BUTTON) {
            // The Delete button is tinted red so the destructive action stands out.
            bool isDelete = ds->itemID == IDOK;
            HBRUSH br = CreateSolidBrush(isDelete ? RGB(70, 32, 32) : RGB(50, 50, 50));
            FillRect(ds->hDC, &ds->rcItem, br); DeleteObject(br);
            SetTextColor(ds->hDC, isDelete ? RGB(255, 190, 190) : RGB(230, 230, 230));
            SetBkMode(ds->hDC, TRANSPARENT);
            wchar_t buf[64];
            GetWindowTextW(ds->hwndItem, buf, 64);
            DrawTextW(ds->hDC, buf, -1, &ds->rcItem, DT_CENTER | DT_SINGLELINE | DT_VCENTER);
            if (ds->itemState & ODS_FOCUS) { RECT r = ds->rcItem; InflateRect(&r, -3, -3); DrawFocusRect(ds->hDC, &r); }
            return TRUE;
        }
        break;
    }
    case WM_COMMAND:
        if (LOWORD(wp) == IDOK) { g_confirm->accepted = true; DestroyWindow(hwnd); return 0; }
        if (LOWORD(wp) == IDCANCEL) { DestroyWindow(hwnd); return 0; }
        break;
    case WM_CLOSE: DestroyWindow(hwnd); return 0; }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// Ask the user to confirm a destructive action. Returns true only when the user
// picks Delete (the default button, so Enter confirms; Esc and Backspace cancel).
static bool AskConfirm(HWND owner, const std::wstring& question, const std::wstring& detail) {
    ConfirmState state;
    state.question = question;
    state.detail = detail;
    g_confirm = &state;
    const DWORD style = WS_CAPTION | WS_SYSMENU;
    const DWORD exStyle = WS_EX_DLGMODALFRAME | WS_EX_TOPMOST;
    // Size the window from the desired client area so the controls always fit.
    RECT frame{0, 0, CONFIRM_CLIENT_W, CONFIRM_CLIENT_H};
    AdjustWindowRectEx(&frame, style, FALSE, exStyle);
    int w = frame.right - frame.left, h = frame.bottom - frame.top;
    RECT pr{}; GetWindowRect(owner, &pr);
    int x = pr.left + (pr.right - pr.left - w) / 2;
    int y = pr.top + (pr.bottom - pr.top - h) / 2;
    EnableWindow(owner, FALSE); // Make the main window temporarily non-interactive.
    HWND dlg = CreateWindowExW(exStyle, CONFIRM_CLASS, L"Please confirm",
        style | WS_VISIBLE, x, y, w, h, owner, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!dlg) { EnableWindow(owner, TRUE); g_confirm = nullptr; return false; }
    SetDarkTitleBar(dlg);
    PumpDialog(dlg, state.edit, &state.done, false);
    EnableWindow(owner, TRUE); SetForegroundWindow(owner); g_confirm = nullptr;
    return state.accepted;
}

// Pause a card's stopwatch, freezing its in-flight stretch into the countup so
// the "+" display keeps the value it had. Every path that ends a run comes
// through here: the pause toggle, switching the run to another card, and a card
// leaving the Todo column (the only column a stopwatch may count up in).
static void PauseCardTimer(int index) {
    if (g_cards[index].timerStart == 0) return;
    g_cards[index].sessionAccumulated += (NowMs() - g_cards[index].timerStart);
    g_cards[index].timerStart = 0;
}

// Toggle the stopwatch for a card: start when stopped, pause when running.
// A stopwatch only counts up on a card in the Todo column, so starting one
// anywhere else is refused with a short explanation. Starting while another card
// is running switches the run over: the previous card is paused first (freezing
// its in-flight stretch into sessionAccumulated), so exactly one stopwatch runs
// at a time and it is the one the user just toggled.
// Pausing freezes the in-flight stretch into sessionAccumulated instead of the
// accumulated total, so the live countup keeps displaying the value (it is only
// cleared by a "Reset Time").
static void ToggleTimer(HWND hwnd, int index) {
    if (g_cards[index].timerStart != 0) {
        PauseCardTimer(index);
        if (!AnyTimerRunning()) StopLiveTimer(hwnd);
    } else {
        if (g_cards[index].column != 0) {
            MessageBoxW(hwnd,
                L"Stopwatches only count up on cards in the Todo column.\n"
                L"Move this card back to Todo to run its stopwatch.",
                L"Stopwatch", MB_OK | MB_ICONINFORMATION);
            return;
        }
        for (size_t i = 0; i < g_cards.size(); ++i)
            if (static_cast<int>(i) != index && g_cards[i].timerStart != 0) PauseCardTimer((int)i);
        g_cards[index].timerStart = NowMs();
        StartLiveTimer(hwnd);
    }
    SaveCards(); FocusCard(hwnd, index); // Acting on a card pulls the focus box onto it.
}

// Toggle the blocked flag for a card.
static void ToggleBlocked(HWND hwnd, int index) {
    g_cards[index].blocked = !g_cards[index].blocked;
    SaveCards(); FocusCard(hwnd, index); // Acting on a card pulls the focus box onto it.
}

// The total countup waiting to be folded into the cards' totals, i.e. what
// "Reset Time" is about to push (running stretches included).
static LONGLONG PendingCountupMs() {
    LONGLONG total = 0;
    for (const auto& c : g_cards) total += c.sessionAccumulated + SessionElapsedMs(c);
    return total;
}

// "Reset Time": the one and only action that pushes time. It folds every card's
// countup (running or paused, including countups carried over from earlier
// launches) into its accumulated total and stops the running stopwatch; the user
// restarts anything they want running afterwards. Nothing else folds on its own,
// so the countups simply carry across a close and reopen until the user asks.
static void ResetTime(HWND hwnd) {
    LONGLONG now = NowMs();
    bool reset = false;
    for (auto& c : g_cards) {
        if (c.sessionAccumulated == 0 && c.timerStart == 0) continue;
        c.timerAccumulated += c.sessionAccumulated + (c.timerStart != 0 ? now - c.timerStart : 0);
        c.sessionAccumulated = 0;
        c.timerStart = 0;
        reset = true;
    }
    SaveCards();
    StopLiveTimer(hwnd);
    if (reset) InvalidateRect(hwnd, nullptr, FALSE);
}

// Return the index of the card under a client-space point, or -1 if none.
static int HoverIndex(const RECT& client, const POINT& p) {
    for (auto [index, r] : CardRects(client)) if (PtInRect(&r, p)) return index;
    return -1;
}

// The card indices in the order they appear on screen: column by column, top to bottom.
static std::vector<int> VisibleOrder() {
    std::vector<int> order;
    for (int c = 0; c < 3; ++c)
        for (size_t i = 0; i < g_cards.size(); ++i)
            if (g_cards[i].column == c) order.push_back((int)i);
    return order;
}

// Move the focus box one card up (-1) or down (+1) in on-screen order.
// With nothing focused yet, stepping down starts at the top card and stepping
// up starts at the bottom one; the box stops at the ends instead of wrapping.
static void SelectStep(HWND hwnd, int delta) {
    std::vector<int> order = VisibleOrder();
    if (order.empty()) { FocusCard(hwnd, -1); return; }
    int pos = -1;
    for (size_t i = 0; i < order.size(); ++i) if (order[i] == g_selected) { pos = (int)i; break; }
    if (pos < 0) pos = delta > 0 ? -1 : (int)order.size();
    int next = pos + delta;
    if (next < 0 || next >= (int)order.size()) return; // Already at the first/last card.
    FocusCard(hwnd, order[next]);
}

// Move the selected card one column left (-1) or right (+1); it stays selected afterwards.
// Only the Todo column runs stopwatches, so a card that leaves it also has its
// stopwatch paused (its countup is kept for the next "Reset Time").
static void MoveSelectedColumn(HWND hwnd, int delta) {
    if (g_selected < 0 || g_selected >= (int)g_cards.size()) return;
    int column = g_cards[g_selected].column + delta;
    if (column < 0 || column > 2) return; // Already in the first/last column.
    g_cards[g_selected].column = column;
    if (column != 0) PauseCardTimer(g_selected);
    if (!AnyTimerRunning()) StopLiveTimer(hwnd);
    SaveCards(); FocusCard(hwnd, g_selected);
}

// The card a keyboard action applies to: the one under the mouse if there is one,
// otherwise the card picked with the arrow keys.
static int ActionIndex(HWND hwnd) {
    RECT client{}; GetClientRect(hwnd, &client);
    int hover = HoverIndex(client, g_lastMouse);
    return hover >= 0 ? hover : g_selected;
}

// Keep the keyboard selection on the same card after one is erased from the vector.
static void FixSelectionAfterErase(HWND hwnd, int erased) {
    if (g_selected == erased) g_selected = -1;
    else if (g_selected > erased) --g_selected;
    FocusCard(hwnd, g_selected); // Hides the box when the focused card is the one that went away.
}

// Delete a card once the user has confirmed it. Every delete path (the hotkey and
// the context menu) comes through here, so none of them can drop a card silently.
// The confirmation quotes the card text so the user can see what is going away.
static void DeleteCard(HWND hwnd, int index) {
    if (index < 0 || index >= static_cast<int>(g_cards.size())) return;
    std::wstring detail = Trim(g_cards[index].text);
    if (detail.size() > CONFIRM_TEXT_MAX) { detail.resize(CONFIRM_TEXT_MAX); detail += L"..."; }
    if (!AskConfirm(hwnd, L"Delete this card?", detail)) return;
    g_cards.erase(g_cards.begin() + index);
    FixSelectionAfterErase(hwnd, index);
    SaveCards(); InvalidateRect(hwnd, nullptr, FALSE);
}

// Percent-encode a string for use in a URL query string (UTF-8 aware).
static std::string UrlEncode(const std::wstring& s) {
    std::string u8 = Utf8(s);
    std::string out;
    static const char* hex = "0123456789ABCDEF";
    for (unsigned char c : u8) {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')
            || c == '-' || c == '_' || c == '.' || c == '~') out += (char)c;
        else { out += '%'; out += hex[c >> 4]; out += hex[c & 15]; }
    }
    return out;
}

// Build the "Windows 10.0.22631" style string via RtlGetVersion (works on all Windows 10/11).
static std::wstring OsVersion() {
    std::wstring ver = L"unknown";
    HMODULE ntdll = LoadLibraryW(L"ntdll.dll");
    if (!ntdll) return ver;
    using RtlGetVersionFn = LONG(WINAPI*)(PRTL_OSVERSIONINFOW);
    auto rtl = reinterpret_cast<RtlGetVersionFn>(GetProcAddress(ntdll, "RtlGetVersion"));
    if (rtl) {
        RTL_OSVERSIONINFOW info{};
        info.dwOSVersionInfoSize = sizeof(info);
        if (rtl(&info) == 0)
            ver = L"Windows " + std::to_wstring(info.dwMajorVersion) + L"." + std::to_wstring(info.dwMinorVersion)
                + L"." + std::to_wstring(info.dwBuildNumber);
    }
    FreeLibrary(ntdll);
    return ver;
}

static std::wstring GitHubRepoUrl(const wchar_t* suffix) {
    return std::wstring(L"https://github.com/") + GITHUB_OWNER + L"/" + GITHUB_REPO + suffix;
}

static std::wstring GitHubApiUrl(const wchar_t* suffix) {
    return std::wstring(L"https://api.github.com/repos/") + GITHUB_OWNER + L"/" + GITHUB_REPO + suffix;
}

// Open the browser to a pre-filled "new issue" page on the project's repo.
static void ReportBug(HWND hwnd) {
    std::wstring title = L"Bug report (MinimalKanban v" + std::wstring(APP_VERSION) + L")";
    std::wstring body;
    body += L"OS: " + OsVersion() + L"\n";
    body += L"App version / build: v" + std::wstring(APP_VERSION) + L"\n";
    body += L"Board file: " + DataPath() + L"\n\n";
    body += L"## What happened\n\n\n";
    body += L"## Steps to reproduce\n\n1. \n\n";
    body += L"## Expected behavior\n\n\n";
    body += L"## Notes\n\n";
    std::wstring url = GitHubRepoUrl(L"/issues/new?title=") + Wide(UrlEncode(title)) + L"&body=" + Wide(UrlEncode(body));
    ShellExecuteW(hwnd, L"open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

// Ask for the report in the user's own words, then open a pre-filled issue carrying that text.
// The title is the first line of the prompt so the issue list stays readable; the body is verbatim.
static void ReportBugWithPrompt(HWND hwnd) {
    std::wstring prompt;
    if (!AskForReportPrompt(hwnd, prompt)) return;
    std::wstring firstLine = prompt.substr(0, prompt.find_first_of(L"\r\n"));
    std::wstring title = Trim(firstLine);
    if (title.size() > 64) title.resize(64);
    if (title.empty()) title = L"Bug report from app";
    std::wstring url = GitHubRepoUrl(L"/issues/new?title=") + Wide(UrlEncode(title)) + L"&body=" + Wide(UrlEncode(prompt));
    ShellExecuteW(hwnd, L"open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

// Extract tag_name from a GitHub releases/latest JSON payload.
static std::wstring ReadTagName(const std::string& json) {
    const std::string key = "\"tag_name\"";
    size_t p = json.find(key);
    if (p == std::string::npos) return {};
    size_t q1 = json.find('"', p + key.size());
    if (q1 == std::string::npos) return {};
    size_t q2 = json.find('"', q1 + 1);
    if (q2 == std::string::npos) return {};
    return Wide(json.substr(q1 + 1, q2 - q1 - 1));
}

// Compare dotted numeric versions. Returns <0 (a older), 0 (equal), >0 (a newer).
static int VersionCmp(const std::wstring& a, const std::wstring& b) {
    size_t i = 0, j = 0;
    while (i < a.size() || j < b.size()) {
        size_t e1 = a.find(L'.', i), e2 = b.find(L'.', j);
        if (e1 == std::wstring::npos) e1 = a.size();
        if (e2 == std::wstring::npos) e2 = b.size();
        long long v1 = 0, v2 = 0;
        try { if (i < a.size()) v1 = std::stoll(a.substr(i, e1 - i)); } catch (...) {}
        try { if (j < b.size()) v2 = std::stoll(b.substr(j, e2 - j)); } catch (...) {}
        if (v1 != v2) return v1 < v2 ? -1 : 1;
        i = e1 + 1; j = e2 + 1;
    }
    return 0;
}

static void InstallUpdate(HWND hwnd, const std::wstring& newPath);

struct UpdateCheckRequest {
    HWND hwnd = nullptr;
    bool autoUpdate = false;
};

struct UpdateResult {
    bool reached = false;
    bool newer = false;
    bool downloaded = false;
    std::wstring tag;
    std::wstring newPath;
};

static DWORD WINAPI UpdateCheckWorker(LPVOID param) {
    std::unique_ptr<UpdateCheckRequest> request(static_cast<UpdateCheckRequest*>(param));
    HRESULT coResult = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    std::unique_ptr<UpdateResult> result(new UpdateResult());
    std::wstring dir = AppLocalDir();
    std::wstring infoPath = dir + L"\\update_info.json";
    DeleteFileW(infoPath.c_str());
    if (SUCCEEDED(URLDownloadToFileW(nullptr, GitHubApiUrl(L"/releases/latest").c_str(), infoPath.c_str(), 0, nullptr))) {
        std::ifstream f(std::filesystem::path(infoPath), std::ios::binary);
        if (f) {
            std::string json((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
            std::wstring tag = ReadTagName(json);
            if (!tag.empty()) {
                result->reached = true;
                result->tag = tag;
                std::wstring latest = tag;
                if (!latest.empty() && (latest[0] == L'v' || latest[0] == L'V')) latest = latest.substr(1);
                result->newer = VersionCmp(latest, APP_VERSION) > 0;
                result->newPath = dir + L"\\MinimalKanban.new.exe";
                if (result->newer && request->autoUpdate) {
                    DeleteFileW(result->newPath.c_str());
                    result->downloaded = SUCCEEDED(URLDownloadToFileW(nullptr,
                        GitHubRepoUrl(L"/releases/latest/download/MinimalKanban.exe").c_str(),
                        result->newPath.c_str(), 0, nullptr));
                }
            }
        }
    }
    if (SUCCEEDED(coResult)) CoUninitialize();
    if (InterlockedCompareExchange(&g_updateCancelled, 0, 0) != 0) {
        InterlockedExchange(&g_updateBusy, 0);
        return 0;
    }
    if (!PostMessageW(request->hwnd, WM_APP_UPDATE_RESULT, 0, (LPARAM)result.get())) {
        InterlockedExchange(&g_updateBusy, 0);
        return 0;
    }
    result.release();
    return 0;
}

static void StartStartupUpdateCheck(HWND hwnd) {
    if (!g_settings.checkOnStartup) return;
    if (InterlockedCompareExchange(&g_updateBusy, 1, 0) != 0) return;
    UpdateCheckRequest* request = new UpdateCheckRequest{hwnd, g_settings.autoUpdate};
    HANDLE thread = CreateThread(nullptr, 0, UpdateCheckWorker, request, 0, nullptr);
    if (!thread) {
        delete request;
        InterlockedExchange(&g_updateBusy, 0);
        return;
    }
    CloseHandle(thread);
}

static void HandleUpdateResult(HWND hwnd, UpdateResult* result) {
    std::unique_ptr<UpdateResult> keep(result);
    InterlockedExchange(&g_updateBusy, 0);
    if (!result || !result->reached || !result->newer) return;
    if (g_settings.autoUpdate) {
        if (!result->downloaded) {
            DeleteFileW(result->newPath.c_str());
            if (FAILED(URLDownloadToFileW(nullptr, GitHubRepoUrl(L"/releases/latest/download/MinimalKanban.exe").c_str(),
                result->newPath.c_str(), 0, nullptr))) {
                MessageBoxW(hwnd, L"The automatic update could not be downloaded.", L"Check for Updates", MB_OK | MB_ICONWARNING);
                return;
            }
        }
        InstallUpdate(hwnd, result->newPath);
        return;
    }
    int answer = MessageBoxW(hwnd,
        (L"A new version (" + result->tag + L") is available.\n\nDownload and install it now?").c_str(),
        L"Check for Updates", MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON1);
    if (answer != IDYES) return;
    DeleteFileW(result->newPath.c_str());
    if (FAILED(URLDownloadToFileW(nullptr, GitHubRepoUrl(L"/releases/latest/download/MinimalKanban.exe").c_str(),
        result->newPath.c_str(), 0, nullptr))) {
        MessageBoxW(hwnd, L"The update failed to download.", L"Check for Updates", MB_OK | MB_ICONWARNING);
        return;
    }
    InstallUpdate(hwnd, result->newPath);
}

// Swap in the freshly downloaded exe after this process exits, then relaunch from LocalAppData.
static void InstallUpdate(HWND hwnd, const std::wstring& newPath) {
    SaveCards();
    std::wstring dest = AppLocalDir() + L"\\MinimalKanban.exe";
    std::wstring cmd = L"cmd.exe /c ping -n 4 127.0.0.1 >nul & move /y \""
        + newPath + L"\" \"" + dest + L"\" & start \"\" \"" + dest + L"\"";
    STARTUPINFOW si{ sizeof(si) };
    si.dwFlags = STARTF_USESHOWWINDOW; si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi{};
    if (CreateProcessW(nullptr, &cmd[0], nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
        MessageBoxW(hwnd, L"Update downloaded. The app will restart automatically in a few seconds.",
            L"Check for Updates", MB_OK | MB_ICONINFORMATION);
        PostQuitMessage(0);
    } else {
        MessageBoxW(hwnd, L"The update downloaded, but it could not be installed.",
            L"Check for Updates", MB_OK | MB_ICONWARNING);
    }
}

// Check the latest GitHub release, download it if newer, install and restart.
static void UpdateCheck(HWND hwnd) {
    if (InterlockedCompareExchange(&g_updateBusy, 1, 0) != 0) return;
    if (wcscmp(GITHUB_OWNER, L"your-github-username") == 0) {
        MessageBoxW(hwnd, L"Set the GitHub owner for this build in minimal_kanban.cpp, then rebuild.",
            L"Check for Updates", MB_OK | MB_ICONWARNING);
        InterlockedExchange(&g_updateBusy, 0);
        return;
    }
    std::wstring dir = AppLocalDir();
    std::wstring infoPath = dir + L"\\update_info.json";
    std::wstring newPath = dir + L"\\MinimalKanban.new.exe";
    DeleteFileW(infoPath.c_str());
    if (FAILED(URLDownloadToFileW(nullptr, GitHubApiUrl(L"/releases/latest").c_str(), infoPath.c_str(), 0, nullptr))) {
        MessageBoxW(hwnd, L"Could not reach GitHub to check for updates.", L"Check for Updates", MB_OK | MB_ICONWARNING);
        InterlockedExchange(&g_updateBusy, 0);
        return;
    }
    std::ifstream f(std::filesystem::path(infoPath), std::ios::binary);
    if (!f) {
        MessageBoxW(hwnd, L"Could not read the release info.", L"Check for Updates", MB_OK | MB_ICONWARNING);
        InterlockedExchange(&g_updateBusy, 0);
        return;
    }
    std::string json((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    std::wstring tag = ReadTagName(json);
    if (tag.empty()) {
        MessageBoxW(hwnd, L"Could not read the release info.", L"Check for Updates", MB_OK | MB_ICONWARNING);
        InterlockedExchange(&g_updateBusy, 0);
        return;
    }
    std::wstring latest = tag;
    if (!latest.empty() && (latest[0] == L'v' || latest[0] == L'V')) latest = latest.substr(1);
    if (VersionCmp(latest, APP_VERSION) <= 0) {
        MessageBoxW(hwnd, (L"You're up to date (v" + std::wstring(APP_VERSION) + L").").c_str(),
            L"Check for Updates", MB_OK | MB_ICONINFORMATION);
        InterlockedExchange(&g_updateBusy, 0);
        return;
    }
    if (!g_settings.autoUpdate) {
        int res = MessageBoxW(hwnd,
            (L"A new version (" + tag + L") is available.\n\nDownload and install it now?").c_str(),
            L"Check for Updates", MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON1);
        if (res != IDYES) {
            InterlockedExchange(&g_updateBusy, 0);
            return;
        }
    }
    DeleteFileW(newPath.c_str());
    if (FAILED(URLDownloadToFileW(nullptr, GitHubRepoUrl(L"/releases/latest/download/MinimalKanban.exe").c_str(), newPath.c_str(), 0, nullptr))) {
        MessageBoxW(hwnd, L"The update failed to download.", L"Check for Updates", MB_OK | MB_ICONWARNING);
        InterlockedExchange(&g_updateBusy, 0);
        return;
    }
    InstallUpdate(hwnd, newPath);
    InterlockedExchange(&g_updateBusy, 0);
}

// Window procedure for the main board window.
static LRESULT CALLBACK MainProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
case WM_KEYDOWN: {
        // F1 is a fixed key (never rebindable): it opens the help page, showing
        // the currently configured shortcuts. Handled before the configurable
        // dispatch so it always wins.
        if (wp == VK_F1) { ShowHelp(hwnd); return 0; }
        // Every keyboard action is routed through the bindings loaded from
        // hotkeys.json, so rebinding one action changes the whole board at once.
        // The arrow-key selection, the report/update actions and the card
        // actions below are all reached through that single dispatch.
        int action = HotkeyAction(wp);
        if (action < 0) break;
        if (action == HK_ADD) { AddCard(hwnd); return 0; }
        if (action == HK_SELECT_UP || action == HK_SELECT_DOWN) { SelectStep(hwnd, action == HK_SELECT_DOWN ? 1 : -1); return 0; }
        if (action == HK_MOVE_LEFT || action == HK_MOVE_RIGHT) { MoveSelectedColumn(hwnd, action == HK_MOVE_RIGHT ? 1 : -1); return 0; }
        if (action == HK_REPORT) { ReportBug(hwnd); return 0; }
        if (action == HK_REPORT_PROMPT) { ReportBugWithPrompt(hwnd); return 0; }
        if (action == HK_UPDATE) { UpdateCheck(hwnd); return 0; }
        if (action == HK_NEW_DAY) { ResetTime(hwnd); return 0; }
        // The remaining actions apply to the card under the last mouse position,
        // or to the keyboard-selected card when the mouse is not over one.
        int hover = ActionIndex(hwnd);
        if (hover < 0) return 0;
        if (action == HK_EDIT) {
            // Edit card text.
            std::wstring newText;
            if (AskForCard(hwnd, newText, g_cards[hover].text)) {
                g_cards[hover].text = newText;
                SaveCards(); FocusCard(hwnd, hover);
            }
        } else if (action == HK_DELETE) {
            // Delete card, after the confirmation dialog.
            DeleteCard(hwnd, hover);
        } else if (action == HK_TOGGLE_TIMER) {
            ToggleTimer(hwnd, hover);
        } else if (action == HK_EDIT_TIMER) {
            AskForTime(hwnd, hover);
        } else { // HK_BLOCKED
            ToggleBlocked(hwnd, hover);
        }
        return 0;
    }
    case WM_LBUTTONDOWN: {
        // A press on a card either toggles a property (with a modifier) or starts a drag.
        POINT p{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        g_lastMouse = p;
        RECT client{}; GetClientRect(hwnd, &client);
        int hit = HoverIndex(client, p);
        if (hit >= 0) {
            if (GetAsyncKeyState(VK_SHIFT) & 0x8000) {
                // Shift+click toggles the stopwatch.
                ToggleTimer(hwnd, hit);
            } else if (GetAsyncKeyState(VK_CONTROL) & 0x8000) {
                // Ctrl+click toggles the blocked flag.
                ToggleBlocked(hwnd, hit);
            } else {
                // Plain click starts dragging the card.
                g_dragIndex = hit; g_dragPoint = p; SetCapture(hwnd);
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        }
        RECT add = AddRect(ColumnRect(client, 0));
        if (PtInRect(&add, p)) { AddCard(hwnd); return 0; }
        RECT reset = ResetRect(ColumnRect(client, 1));
        if (PtInRect(&reset, p)) { ResetTime(hwnd); return 0; }
        return 0;
    }
    case WM_MOUSEMOVE: {
        POINT p{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        g_lastMouse = p;
        // While the left button is held, redraw the dragged card at the new position.
        if (g_dragIndex >= 0 && (wp & MK_LBUTTON)) { g_dragPoint = p; InvalidateRect(hwnd, nullptr, FALSE); }
        return 0;
    }
    case WM_LBUTTONUP:
        // Dropping a card inside a column changes its column and saves the board.
        if (g_dragIndex >= 0) {
            RECT client{}; GetClientRect(hwnd, &client);
            POINT p{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            for (int c = 0; c < 3; ++c) { RECT r = ColumnRect(client, c); if (PtInRect(&r, p)) g_cards[g_dragIndex].column = c; }
            // Only the Todo column runs stopwatches, so dropping a card anywhere
            // else pauses it and keeps its countup for the next "Reset Time".
            if (g_cards[g_dragIndex].column != 0) PauseCardTimer(g_dragIndex);
            if (!AnyTimerRunning()) StopLiveTimer(hwnd);
            g_dragIndex = -1; ReleaseCapture(); SaveCards(); InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    case WM_RBUTTONUP: {
        // Right-click context menu on a card: Edit, Delete, Toggle Timer, Edit Timer, Blocked.
        POINT p{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        g_lastMouse = p;
        RECT client{}; GetClientRect(hwnd, &client);
        int target = HoverIndex(client, p);
        if (target < 0) {
            // Global menu when the click is on empty board space.
            POINT screen = p; ClientToScreen(hwnd, &screen);
            std::wstring autoUpdateLabel = L"Automatic updates: ";
            autoUpdateLabel += g_settings.autoUpdate ? L"auto" : L"ask";
            std::wstring autoUpdateHint = L"(click to change)";
            std::wstring reportHint = HotkeyHint(HK_REPORT);
            std::wstring reportPromptHint = HotkeyHint(HK_REPORT_PROMPT);
            std::wstring updateHint = HotkeyHint(HK_UPDATE);
            std::wstring newDayHint = HotkeyHint(HK_NEW_DAY);
            MenuItemData globalItems[] = {
                { L"Report a Bug",           reportHint.c_str()       },
                { L"Report with Prompt...",  reportPromptHint.c_str() },
                { L"Check for Updates",      updateHint.c_str()       },
                { autoUpdateLabel.c_str(),   autoUpdateHint.c_str()   },
                { L"Reset Time (push countups to totals)", newDayHint.c_str() },
                { L"Reload Hotkeys",         L"(re-reads the file)"   },
            };
            const int globalCmds[] = { CMD_REPORT, CMD_REPORT_PROMPT, CMD_UPDATE, CMD_AUTO_UPDATE, CMD_NEW_DAY, CMD_RELOAD_HOTKEYS };
            HMENU menu = CreatePopupMenu();
            for (size_t i = 0; i < _countof(globalItems); ++i)
                AppendMenuW(menu, MF_OWNERDRAW, globalCmds[i], (LPCTSTR)&globalItems[i]);
            int cmd = TrackPopupMenuEx(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, screen.x, screen.y, hwnd, nullptr);
            DestroyMenu(menu);
            if (cmd == CMD_REPORT) ReportBug(hwnd);
            else if (cmd == CMD_REPORT_PROMPT) ReportBugWithPrompt(hwnd);
            else if (cmd == CMD_UPDATE) UpdateCheck(hwnd);
            else if (cmd == CMD_AUTO_UPDATE) {
                g_settings.autoUpdate = !g_settings.autoUpdate;
                SaveSettings();
            }
            else if (cmd == CMD_NEW_DAY) ResetTime(hwnd);
            else if (cmd == CMD_RELOAD_HOTKEYS) {
                LoadHotkeys(); InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        }
        POINT screen = p; ClientToScreen(hwnd, &screen);
        // Owner-drawn entries so the menu matches the dark theme. The hints show
        // the configured bindings, so a remap stays truthful in the menus too.
        std::wstring editHint = HotkeyHint(HK_EDIT);
        std::wstring deleteHint = HotkeyHint(HK_DELETE);
        std::wstring toggleHint = HotkeyHint(HK_TOGGLE_TIMER);
        std::wstring editTimerHint = HotkeyHint(HK_EDIT_TIMER);
        std::wstring blockedHint = HotkeyHint(HK_BLOCKED);
        MenuItemData items[] = {
            { L"Edit",          editHint.c_str()    },
            { L"Delete",        deleteHint.c_str()  },
            { L"Toggle Timer",  toggleHint.c_str()  },
            { L"Edit Timer",    editTimerHint.c_str() },
            { L"Blocked",       blockedHint.c_str() },
        };
        const int cmdIds[] = { CMD_EDIT, CMD_DELETE, CMD_TOGGLE_TIMER, CMD_EDIT_TIMER, CMD_BLOCKED };
        HMENU menu = CreatePopupMenu();
        for (int i = 0; i < 5; ++i)
            AppendMenuW(menu, MF_OWNERDRAW, cmdIds[i], (LPCTSTR)&items[i]);
        int cmd = TrackPopupMenuEx(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, screen.x, screen.y, hwnd, nullptr);
        DestroyMenu(menu);
        switch (cmd) {
        case CMD_EDIT: {
            std::wstring newText;
            if (AskForCard(hwnd, newText, g_cards[target].text)) {
                g_cards[target].text = newText;
                SaveCards(); FocusCard(hwnd, target);
            }
            break;
        }
        case CMD_DELETE:
            DeleteCard(hwnd, target);
            break;
        case CMD_TOGGLE_TIMER:
            ToggleTimer(hwnd, target);
            break;
        case CMD_EDIT_TIMER:
            AskForTime(hwnd, target);
            break;
        case CMD_BLOCKED:
            ToggleBlocked(hwnd, target);
            break;
        }
        return 0;
    }
    case WM_MEASUREITEM: {
        // Owner-drawn menu item sizing.
        MEASUREITEMSTRUCT* mi = (MEASUREITEMSTRUCT*)lp;
        if (mi->CtlType == ODT_MENU) {
            MenuItemData* data = (MenuItemData*)mi->itemData;
            HDC dc = GetDC(hwnd);
            HGDIOBJ oldFont = SelectObject(dc, g_font);
            SIZE s{}; GetTextExtentPoint32W(dc, data->text, (int)wcslen(data->text), &s);
            SIZE hint{}; GetTextExtentPoint32W(dc, data->hint, (int)wcslen(data->hint), &hint);
            SelectObject(dc, oldFont); // Put the window DC's own font back before releasing it.
            ReleaseDC(hwnd, dc);
            // 12px left padding + 16px gap between label and hint + 12px right padding.
            mi->itemWidth = s.cx + hint.cx + 40;
            mi->itemHeight = 24;
            return TRUE;
        }
        break;
    }
    case WM_DRAWITEM: {
        // Owner-drawn menu item painting: black background, white text, gray hint.
        DRAWITEMSTRUCT* ds = (DRAWITEMSTRUCT*)lp;
        if (ds->CtlType == ODT_MENU && ds->itemData) {
            MenuItemData* data = (MenuItemData*)ds->itemData;
            bool selected = (ds->itemState & ODS_SELECTED) != 0;
            HBRUSH br = CreateSolidBrush(selected ? RGB(70, 70, 70) : RGB(27, 27, 27));
            FillRect(ds->hDC, &ds->rcItem, br);
            DeleteObject(br);
            SetBkMode(ds->hDC, TRANSPARENT);
            SelectObject(ds->hDC, g_font);
            // Lay out the label and right-aligned hint from measured widths.
            SIZE s{}, hint{};
            GetTextExtentPoint32W(ds->hDC, data->text, (int)wcslen(data->text), &s);
            GetTextExtentPoint32W(ds->hDC, data->hint, (int)wcslen(data->hint), &hint);
            int hintRight = ds->rcItem.right - 12;
            int hintLeft = hintRight - hint.cx;
            RECT label = ds->rcItem;
            label.left += 12; label.right = hintLeft - 16;
            SetTextColor(ds->hDC, RGB(255, 255, 255));
            DrawTextW(ds->hDC, data->text, -1, &label, DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
            RECT hintRect = { hintLeft, ds->rcItem.top, hintRight, ds->rcItem.bottom };
            SetTextColor(ds->hDC, RGB(140, 140, 140));
            DrawTextW(ds->hDC, data->hint, -1, &hintRect, DT_LEFT | DT_SINGLELINE | DT_VCENTER);
            return TRUE;
        }
        break;
    }
    case WM_CAPTURECHANGED:
        // Cancel the drag if Windows takes mouse capture away from this window.
        if (g_dragIndex >= 0) { g_dragIndex = -1; InvalidateRect(hwnd, nullptr, FALSE); }
        return 0;
    case WM_APP_UPDATE_CHECK:
        StartStartupUpdateCheck(hwnd);
        return 0;
    case WM_APP_UPDATE_RESULT:
        HandleUpdateResult(hwnd, (UpdateResult*)lp);
        return 0;
    case WM_TIMER:
        // Live timer tick: redraw to update running stopwatch displays.
        if (wp == TIMER_LIVE) {
            if (AnyTimerRunning()) InvalidateRect(hwnd, nullptr, FALSE);
            else StopLiveTimer(hwnd);
        }
        // Focus box tick: hand the box back once it has been idle long enough.
        if (wp == TIMER_FOCUS) FocusIdleTimeout(hwnd);
        return 0;
    case WM_SIZE: InvalidateRect(hwnd, nullptr, FALSE); return 0;
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: {
        // Draw the entire board into the cached off-screen buffer, then copy it to
        // the window. This double buffering prevents visible flicker during redraws.
        PAINTSTRUCT ps{}; HDC dc = BeginPaint(hwnd, &ps);
        RECT client{}; GetClientRect(hwnd, &client);
        HDC mem = BackBuffer(dc, client.right, client.bottom);
        if (!mem) { EndPaint(hwnd, &ps); return 1; }
        SetBkMode(mem, TRANSPARENT);
        Fill(mem, client, RGB(27,27,27));
        // Draw the three columns, their headers, card counts, and the Add button.
        for (int c = 0; c < 3; ++c) {
            RECT col = ColumnRect(client, c);
            Fill(mem, col, RGB(39,39,39));
            RECT header{col.left, col.top, col.right, col.top + 38}; Fill(mem, header, RGB(43,43,43));
            SelectObject(mem, g_boldFont); SetTextColor(mem, RGB(220,220,220));
            RECT title{col.left + 16, col.top + 10, col.right - 50, col.top + 32};
            DrawTextW(mem, TITLES[c], -1, &title, DT_LEFT | DT_SINGLELINE | DT_VCENTER);
            int count = 0; for (const auto& card : g_cards) if (card.column == c) ++count;
            std::wstring countText = std::to_wstring(count);
            SelectObject(mem, g_font); SetTextColor(mem, RGB(180,180,180));
            RECT countRect{col.right - 42, col.top + 10, col.right - 14, col.top + 32};
            DrawTextW(mem, countText.c_str(), -1, &countRect, DT_RIGHT | DT_SINGLELINE | DT_VCENTER);
            if (c == 0) {
                RECT add = AddRect(col); Fill(mem, add, RGB(42,42,42));
                SetTextColor(mem, RGB(190,190,190));
                std::wstring addLabel = L"+ Add a card " + HotkeyHint(HK_ADD);
                DrawTextW(mem, addLabel.c_str(), -1, &add, DT_CENTER | DT_SINGLELINE | DT_VCENTER);
            }
            if (c == 1) {
                // "Reset Time": the button that pushes the countups into the totals,
                // labeled with the amount it is about to fold in.
                RECT reset = ResetRect(col); Fill(mem, reset, RGB(42,42,42));
                std::wstring resetLabel = L"Reset Time " + HotkeyHint(HK_NEW_DAY);
                LONGLONG pending = PendingCountupMs();
                if (pending >= 1000) resetLabel += L"  +" + FormatTimer(pending) + L" to totals";
                SetTextColor(mem, RGB(190,190,190));
                DrawTextW(mem, resetLabel.c_str(), -1, &reset, DT_CENTER | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
            }
        }
        // Draw cards that are not currently being dragged.
        auto rects = CardRects(client);
        HPEN penBlocked = CreatePen(PS_SOLID, 3, RGB(220, 50, 50));
        HPEN penTimer = CreatePen(PS_SOLID, 3, RGB(50, 180, 50));
        HPEN penSelected = CreatePen(PS_SOLID, 3, RGB(0, 0, 0));
        HPEN penOld = (HPEN)SelectObject(mem, GetStockObject(NULL_PEN));
        HBRUSH brOld = (HBRUSH)SelectObject(mem, GetStockObject(NULL_BRUSH));
        for (auto [index, r] : rects) {
            if (index == g_dragIndex) continue;
            // Card background.
            Fill(mem, r, RGB(50,50,50));
            // Draw outline: blocked (red) or timer running (green); the focus box
            // is black and only shows when neither of those applies.
            if (g_cards[index].blocked) {
                SelectObject(mem, penBlocked);
                Rectangle(mem, r.left, r.top, r.right, r.bottom);
            } else if (g_cards[index].timerStart != 0) {
                SelectObject(mem, penTimer);
                Rectangle(mem, r.left, r.top, r.right, r.bottom);
            } else if (index == g_selected) {
                SelectObject(mem, penSelected);
                Rectangle(mem, r.left, r.top, r.right, r.bottom);
            }
            // Task text.
            SelectObject(mem, g_font); SetTextColor(mem, RGB(225,225,225));
            RECT text = r; text.left += 12; text.right -= 12; text.bottom = text.top + 30;
            DrawTextW(mem, g_cards[index].text.c_str(), -1, &text, DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
            // Timer row (below task text): total time left-justified in gray,
            // live countup right-justified in green with a "+". The countup shows
            // while running and stays frozen on the last value when paused; it is
            // persisted and only cleared by a "Reset Time".
            RECT lower = r; lower.left += 12; lower.right -= 12; lower.top = r.top + 30; lower.bottom = r.bottom;
            if (g_cards[index].timerAccumulated > 0) {
                SelectObject(mem, g_font); SetTextColor(mem, RGB(140, 140, 140));
                DrawTextW(mem, FormatTimer(g_cards[index].timerAccumulated).c_str(), -1, &lower, DT_LEFT | DT_SINGLELINE | DT_VCENTER);
            }
            if (g_cards[index].sessionAccumulated > 0 || g_cards[index].timerStart != 0) {
                std::wstring sessionText = L"+" + FormatTimer(g_cards[index].sessionAccumulated + SessionElapsedMs(g_cards[index]));
                SelectObject(mem, g_font); SetTextColor(mem, RGB(50, 180, 50));
                DrawTextW(mem, sessionText.c_str(), -1, &lower, DT_RIGHT | DT_SINGLELINE | DT_VCENTER);
            }
        }
        // Draw a copy of the dragged card under the mouse pointer.
        if (g_dragIndex >= 0) {
            RECT r{g_dragPoint.x - 100, g_dragPoint.y - 30, g_dragPoint.x + 100, g_dragPoint.y + 30};
            Fill(mem, r, RGB(66,66,66)); SelectObject(mem, g_font); SetTextColor(mem, RGB(245,245,245));
            RECT text = r; text.left += 12; text.right -= 12;
            DrawTextW(mem, g_cards[g_dragIndex].text.c_str(), -1, &text, DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
        }
        SelectObject(mem, penOld); SelectObject(mem, brOld);
        DeleteObject(penBlocked); DeleteObject(penTimer); DeleteObject(penSelected);
        BitBlt(dc, 0, 0, client.right, client.bottom, mem, 0, 0, SRCCOPY);
        EndPaint(hwnd, &ps); return 0;
    }
    case WM_DESTROY:
        // Save before closing, release GDI objects, and end the message loop.
        InterlockedExchange(&g_updateCancelled, 1);
        StopLiveTimer(hwnd);
        StopFocusTimer(hwnd);
        ReleaseBackBuffer();
        SaveCards(); if (g_font) DeleteObject(g_font); if (g_boldFont) DeleteObject(g_boldFont); PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// Application entry point. wWinMain is the Unicode/GUI equivalent of main().
int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show) {
    // Initialize COM because some Windows shell APIs used by the program require it.
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    QueryPerformanceFrequency(&g_qpcFreq); // Initialize QPC frequency for stopwatch timing.
    // Create the normal and semibold fonts used by the board.
    g_font = CreateFontW(-16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    g_boldFont = CreateFontW(-16, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    // Register the main window class: this tells Windows how to create and draw board windows.
    WNDCLASSEXW mainClass{sizeof(mainClass)};
    mainClass.lpfnWndProc = MainProc; mainClass.hInstance = instance; mainClass.lpszClassName = MAIN_CLASS;
    mainClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    mainClass.hIcon = static_cast<HICON>(LoadImageW(instance, MAKEINTRESOURCEW(APP_ICON), IMAGE_ICON, 32, 32, LR_DEFAULTCOLOR));
    mainClass.hIconSm = static_cast<HICON>(LoadImageW(instance, MAKEINTRESOURCEW(APP_ICON), IMAGE_ICON, 16, 16, LR_DEFAULTCOLOR));
    mainClass.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1); RegisterClassExW(&mainClass);
    // Register a second class for the add-card window.
    WNDCLASSEXW inputClass{sizeof(inputClass)};
    inputClass.lpfnWndProc = InputProc; inputClass.hInstance = instance; inputClass.lpszClassName = INPUT_CLASS;
    inputClass.hCursor = LoadCursorW(nullptr, IDC_ARROW); inputClass.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    RegisterClassExW(&inputClass);
    // Register the time input dialog using the same class name so it shares InputProc's theme handling.
    // TimeInputProc handles its own WM_DRAWITEM for owner-drawn buttons.
    WNDCLASSEXW timeClass{sizeof(timeClass)};
    timeClass.lpfnWndProc = TimeInputProc; timeClass.hInstance = instance; timeClass.lpszClassName = L"MinimalKanbanTimeInput";
    timeClass.hCursor = LoadCursorW(nullptr, IDC_ARROW); timeClass.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    RegisterClassExW(&timeClass);
    // Register the natural-language report prompt dialog class.
    WNDCLASSEXW promptClass{sizeof(promptClass)};
    promptClass.lpfnWndProc = PromptInputProc; promptClass.hInstance = instance; promptClass.lpszClassName = PROMPT_CLASS;
    promptClass.hCursor = LoadCursorW(nullptr, IDC_ARROW); promptClass.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    RegisterClassExW(&promptClass);
    // Register the delete confirmation dialog class.
    WNDCLASSEXW confirmClass{sizeof(confirmClass)};
    confirmClass.lpfnWndProc = ConfirmProc; confirmClass.hInstance = instance; confirmClass.lpszClassName = CONFIRM_CLASS;
    confirmClass.hCursor = LoadCursorW(nullptr, IDC_ARROW); confirmClass.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    RegisterClassExW(&confirmClass);
    // Register the F1 help window class.
    WNDCLASSEXW helpClass{sizeof(helpClass)};
    helpClass.lpfnWndProc = HelpProc; helpClass.hInstance = instance; helpClass.lpszClassName = HELP_CLASS;
    helpClass.hCursor = LoadCursorW(nullptr, IDC_ARROW); helpClass.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    RegisterClassExW(&helpClass);
    LoadCards(); // Restore the previous board before showing the window.
    LoadSettings();
    LoadHotkeys();
    // Create the actual main window using the class registered above.
    HWND hwnd = CreateWindowExW(0, MAIN_CLASS, L"Minimal Kanban", WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, 920, 520, nullptr, nullptr, instance, nullptr);
    if (!hwnd) return 1;
    SetDarkTitleBar(hwnd);
    ShowWindow(hwnd, show); // Make the window visible.
    UpdateWindow(hwnd); // Ask Windows to send WM_PAINT immediately.
    PostMessageW(hwnd, WM_APP_UPDATE_CHECK, 0, 0);
    // The main message loop dispatches mouse, keyboard, paint, and close events.
    MSG msg; while (GetMessageW(&msg, nullptr, 0, 0) > 0) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    CoUninitialize(); return (int)msg.wParam;
}
