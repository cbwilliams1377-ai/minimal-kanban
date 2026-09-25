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
    LONGLONG sessionAccumulated = 0; // this run's paused-session time; reset on app close
    LONGLONG timerStart = 0;       // QPC timestamp when running (0 = stopped)
};

struct Settings {
    bool autoUpdate = false;
    bool checkOnStartup = true;
};

// These global variables hold the board and the pieces of UI state shared by message handlers.
static std::vector<Card> g_cards; // All cards currently loaded in memory.
static HFONT g_font = nullptr, g_boldFont = nullptr; // Fonts used while drawing text.
static int g_dragIndex = -1; // Index of the card being dragged; -1 means no drag is active.
static POINT g_dragPoint{}; // Current mouse position while dragging a card.
static POINT g_lastMouse{}; // Last known mouse position, used for hover-based keyboard actions.
static const wchar_t* MAIN_CLASS = L"MinimalKanbanWindow"; // Name of the main window class.
static const wchar_t* INPUT_CLASS = L"MinimalKanbanInput"; // Name of the add-card window class.
static const wchar_t* TIME_CLASS = L"MinimalKanbanTimeInput"; // Name of the timer entry window class.
static const wchar_t* TITLES[3] = { L"Todo", L"In-Progress", L"Complete" };
static const wchar_t* GITHUB_OWNER = L"cbwilliams1377-ai";
static const wchar_t* GITHUB_REPO = L"minimal-kanban";
static const wchar_t* APP_VERSION = L"0.1.1";
static LARGE_INTEGER g_qpcFreq{};        // QPC frequency, queried once at startup.
static UINT_PTR g_liveTimerID = 0;       // Win32 timer ID for live stopwatch updates (0 = not running).
static Settings g_settings;
static volatile LONG g_updateBusy = 0;
static volatile LONG g_updateCancelled = 0;

#define TIMER_LIVE 1                     // Timer ID for the live stopwatch tick.
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

// Get the total elapsed milliseconds for a card, folding the current run in.
// The current run is sessionAccumulated (paused stretches) plus the in-flight
// session when running. Used when persisting so time is never lost ("session
// closed/saved appends that time to the persisted total").
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

// Finalize all running card timers into their run counters. Kept for symmetry with
// the pause path in ToggleTimer (currently unused; saves go through TotalElapsedMs).
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
// Timers are NOT stopped here: TotalElapsedMs includes the whole current run
// (accumulated history + session pauses + any in-flight stretch), so the saved
// value is always the current total. (WM_DESTROY saves; on reload the restored
// board always shows stopped timers because sessionAccumulated is not persisted.)
static void SaveCards() {
    // trunc clears the previous file before writing the current board.
    std::ofstream f(std::filesystem::path(DataPath()), std::ios::binary | std::ios::trunc);
    if (!f) return;
    f << "{\n  \"cards\": [\n";
    for (size_t i = 0; i < g_cards.size(); ++i) {
        f << "    {\"column\": " << g_cards[i].column
          << ", \"text\": \"" << JsonEscape(Utf8(g_cards[i].text)) << "\""
          << ", \"blocked\": " << (g_cards[i].blocked ? "true" : "false")
          << ", \"timer_ms\": " << TotalElapsedMs(g_cards[i])
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

// Paint a rectangle with one solid color, then release the temporary brush.
static void Fill(HDC dc, const RECT& r, COLORREF color) {
    HBRUSH b = CreateSolidBrush(color);
    FillRect(dc, &r, b);
    DeleteObject(b);
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
    MSG msg;
    while (!state.done && GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (!IsDialogMessageW(dlg, &msg)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    }
    EnableWindow(owner, TRUE); SetForegroundWindow(owner); g_input = nullptr;
    if (state.accepted) out = state.text;
    return state.accepted;
}

// Ask for a card, add it to the Todo column, save it, and redraw the board.
static void AddCard(HWND hwnd) {
    std::wstring text;
    if (AskForCard(hwnd, text)) {
        g_cards.push_back({text, 0});
        SaveCards();
        InvalidateRect(hwnd, nullptr, FALSE);
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
    MSG msg;
    while (!state.done && GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (!IsDialogMessageW(dlg, &msg)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    }
    EnableWindow(hwnd, TRUE); SetForegroundWindow(hwnd);
    if (state.accepted) {
        LONGLONG parsed = ParseTimeString(state.text);
        if (parsed >= 0) {
            g_cards[cardIndex].timerAccumulated = parsed;
            g_cards[cardIndex].timerStart = 0; // stopped after manual entry
            g_cards[cardIndex].sessionAccumulated = 0; // explicit set lets the run countup start fresh
            SaveCards(); InvalidateRect(hwnd, nullptr, FALSE);
        }
    }
    g_timeInput = nullptr;
}

// Toggle the stopwatch for a card: start when stopped, pause when running.
// Pausing freezes the in-flight stretch into sessionAccumulated instead of the
// accumulated total, so the live countup keeps displaying the value (it only
// resets when the program closes and the card reloads).
static void ToggleTimer(HWND hwnd, int index) {
    if (g_cards[index].timerStart != 0) {
        g_cards[index].sessionAccumulated += (NowMs() - g_cards[index].timerStart);
        g_cards[index].timerStart = 0;
        if (!AnyTimerRunning()) StopLiveTimer(hwnd);
    } else {
        if (AnyTimerRunning()) return;
        g_cards[index].timerStart = NowMs();
        StartLiveTimer(hwnd);
    }
    SaveCards(); InvalidateRect(hwnd, nullptr, FALSE);
}

// Toggle the blocked flag for a card.
static void ToggleBlocked(HWND hwnd, int index) {
    g_cards[index].blocked = !g_cards[index].blocked;
    SaveCards(); InvalidateRect(hwnd, nullptr, FALSE);
}

// Return the index of the card under a client-space point, or -1 if none.
static int HoverIndex(const RECT& client, const POINT& p) {
    for (auto [index, r] : CardRects(client)) if (PtInRect(&r, p)) return index;
    return -1;
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
    case WM_KEYDOWN:
        // Ctrl+N is a keyboard shortcut for adding a card.
        if (wp == 'N' && (GetKeyState(VK_CONTROL) & 0x8000)) { AddCard(hwnd); return 0; }
        if (wp == 'R' && (GetKeyState(VK_CONTROL) & 0x8000)) { ReportBug(hwnd); return 0; }
        if (wp == 'U' && (GetKeyState(VK_CONTROL) & 0x8000)) { UpdateCheck(hwnd); return 0; }
        // Hover-based keyboard actions operate on the card under the last mouse position.
        if (wp == VK_SPACE || wp == VK_DELETE || wp == 'D' || wp == 'S' || wp == 'T' || wp == 'B') {
            RECT client{}; GetClientRect(hwnd, &client);
            int hover = HoverIndex(client, g_lastMouse);
            if (hover < 0) return 0;
            if (wp == VK_SPACE) {
                // Edit card text.
                std::wstring newText;
                if (AskForCard(hwnd, newText, g_cards[hover].text)) {
                    g_cards[hover].text = newText;
                    SaveCards(); InvalidateRect(hwnd, nullptr, FALSE);
                }
            } else if (wp == VK_DELETE || wp == 'D') {
                // Delete card.
                g_cards.erase(g_cards.begin() + hover);
                SaveCards(); InvalidateRect(hwnd, nullptr, FALSE);
            } else if (wp == 'S') {
                ToggleTimer(hwnd, hover);
            } else if (wp == 'T') {
                AskForTime(hwnd, hover);
            } else { // 'B'
                ToggleBlocked(hwnd, hover);
            }
            return 0;
        }
        break;
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
        if (PtInRect(&add, p)) AddCard(hwnd);
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
            MenuItemData globalItems[] = {
                { L"Report a Bug",       L"(ctrl+r)" },
                { L"Check for Updates",  L"(ctrl+u)" },
                { autoUpdateLabel.c_str(), autoUpdateHint.c_str() },
            };
            const int globalCmds[] = { CMD_REPORT, CMD_UPDATE, CMD_AUTO_UPDATE };
            HMENU menu = CreatePopupMenu();
            for (size_t i = 0; i < _countof(globalItems); ++i)
                AppendMenuW(menu, MF_OWNERDRAW, globalCmds[i], (LPCTSTR)&globalItems[i]);
            int cmd = TrackPopupMenuEx(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, screen.x, screen.y, hwnd, nullptr);
            DestroyMenu(menu);
            if (cmd == CMD_REPORT) ReportBug(hwnd);
            else if (cmd == CMD_UPDATE) UpdateCheck(hwnd);
            else if (cmd == CMD_AUTO_UPDATE) {
                g_settings.autoUpdate = !g_settings.autoUpdate;
                SaveSettings();
            }
            return 0;
        }
        POINT screen = p; ClientToScreen(hwnd, &screen);
        // Owner-drawn entries so the menu matches the dark theme.
        MenuItemData items[] = {
            { L"Edit",          L"(space)" },
            { L"Delete",        L"(d)"     },
            { L"Toggle Timer",  L"(s)"     },
            { L"Edit Timer",    L"(t)"     },
            { L"Blocked",       L"(b)"     },
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
                SaveCards(); InvalidateRect(hwnd, nullptr, FALSE);
            }
            break;
        }
        case CMD_DELETE:
            g_cards.erase(g_cards.begin() + target);
            SaveCards(); InvalidateRect(hwnd, nullptr, FALSE);
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
            SelectObject(dc, g_font);
            SIZE s{}; GetTextExtentPoint32W(dc, data->text, (int)wcslen(data->text), &s);
            SIZE hint{}; GetTextExtentPoint32W(dc, data->hint, (int)wcslen(data->hint), &hint);
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
        return 0;
    case WM_SIZE: InvalidateRect(hwnd, nullptr, FALSE); return 0;
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: {
        // Draw the entire board into an off-screen bitmap, then copy it to the window.
        // This double buffering prevents visible flicker during redraws.
        PAINTSTRUCT ps{}; HDC dc = BeginPaint(hwnd, &ps);
        RECT client{}; GetClientRect(hwnd, &client);
        HDC mem = CreateCompatibleDC(dc);
        HBITMAP bitmap = CreateCompatibleBitmap(dc, client.right, client.bottom);
        HGDIOBJ oldBitmap = SelectObject(mem, bitmap);
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
                SetTextColor(mem, RGB(190,190,190)); DrawTextW(mem, L"+ Add a card (ctrl+n)", -1, &add, DT_CENTER | DT_SINGLELINE | DT_VCENTER);
            }
        }
        // Draw cards that are not currently being dragged.
        auto rects = CardRects(client);
        HPEN penBlocked = CreatePen(PS_SOLID, 3, RGB(220, 50, 50));
        HPEN penTimer = CreatePen(PS_SOLID, 3, RGB(50, 180, 50));
        HPEN penOld = (HPEN)SelectObject(mem, GetStockObject(NULL_PEN));
        HBRUSH brOld = (HBRUSH)SelectObject(mem, GetStockObject(NULL_BRUSH));
        for (auto [index, r] : rects) {
            if (index == g_dragIndex) continue;
            // Card background.
            Fill(mem, r, RGB(50,50,50));
            // Draw outline: blocked (red) or timer running (green). Blocked takes priority visually.
            if (g_cards[index].blocked) {
                SelectObject(mem, penBlocked);
                Rectangle(mem, r.left, r.top, r.right, r.bottom);
            } else if (g_cards[index].timerStart != 0) {
                SelectObject(mem, penTimer);
                Rectangle(mem, r.left, r.top, r.right, r.bottom);
            }
            // Task text.
            SelectObject(mem, g_font); SetTextColor(mem, RGB(225,225,225));
            RECT text = r; text.left += 12; text.right -= 12; text.bottom = text.top + 30;
            DrawTextW(mem, g_cards[index].text.c_str(), -1, &text, DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
            // Timer row (below task text): total time left-justified in gray,
            // live countup right-justified in green with a "+". The countup shows
            // while running and stays frozen on the last value when paused; it only
            // resets when the program closes (sessionAccumulated is not persisted).
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
        DeleteObject(penBlocked); DeleteObject(penTimer);
        BitBlt(dc, 0, 0, client.right, client.bottom, mem, 0, 0, SRCCOPY);
        SelectObject(mem, oldBitmap); DeleteObject(bitmap); DeleteDC(mem); EndPaint(hwnd, &ps); return 0;
    }
    case WM_DESTROY:
        // Save before closing, release GDI font objects, and end the message loop.
        InterlockedExchange(&g_updateCancelled, 1);
        StopLiveTimer(hwnd);
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
    LoadCards(); // Restore the previous board before showing the window.
    LoadSettings();
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
