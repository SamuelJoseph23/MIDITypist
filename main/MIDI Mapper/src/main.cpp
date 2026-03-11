/**
 * MIDITypist — Main Entry Point
 * Handles WebView2 init, Window Procedure, Message Dispatch, and App Lifecycle.
 * Module functions are defined in:
 *   bridge.cpp, input_simulation.cpp, config.cpp,
 *   tray.cpp, midi_engine.cpp, app_context.cpp
 */

#include "globals.h"
#include "bridge.h"
#include "input_simulation.h"
#include "config.h"
#include "tray.h"
#include "midi_engine.h"
#include "app_context.h"

// ══════════════════════════════════════════
//  Global State (Definitions)
// ══════════════════════════════════════════

HINSTANCE g_hInst;
HWND g_hwndMain = nullptr;
wil::com_ptr<ICoreWebView2> g_webview;
wil::com_ptr<ICoreWebView2Controller> g_controller;

std::unique_ptr<RtMidiIn> g_midiIn;
std::vector<std::string> g_ports;
bool g_connected = false;
int g_lastConnectedPort = -1;
std::string g_lastConnectedPortName;

std::vector<Mapping> g_mappings;
std::recursive_mutex g_mappingsMutex;
bool g_learning = false;
std::mutex g_learnMutex;
ULONGLONG g_learnStartTime = 0;
Mapping g_learn_pending = { -1, -1, {}, -1, 0, 1, 0, 0, -1, "", "", "", "", 0, true };
bool g_verboseLogging = false;

// ── Per-App Profile ──
std::map<std::wstring, std::wstring> g_appProfileBindings;
std::wstring g_currentApp;
std::wstring g_currentWindowTitle;
HWINEVENTHOOK g_hWinEventHook = nullptr;

std::map<int, KeyState> g_keyStates;
std::mutex g_gestureMutex;

// ── Tray Icon ──
NOTIFYICONDATA g_nid = {};
bool g_minimizedToTray = false;

// ── Persistent Config ──
std::wstring g_configPath;
std::wstring g_lastProfilePath;

// ── Piano Roll State ──
int g_pianoVelocity[PIANO_TOTAL_KEYS] = { 0 };
std::atomic<bool> g_pianoPhysicalDown[PIANO_TOTAL_KEYS] = {};
int g_pianoCC[128] = { 0 };
bool g_sustainActive = false;
std::set<int> g_sustainedVKs;
std::mutex g_sustainMutex;

// ── Auto Reconnect & App Switching ──
bool g_autoReconnect = true;
bool g_appSwitchingEnabled = true;
bool g_velocityZonesEnabled = true;
bool g_minimizeToTrayEnabled = true;
std::string g_aiApiKey;
std::string g_aiGlobalPrompt = "You are a desktop automation assistant. Perform the following task briefly: {prompt}";

// ── Chord Collector ──
std::vector<int> g_chordBuffer;
std::mutex g_chordMutex;

// ── UI Bridge Queue (Thread Safe) ──
std::queue<json> g_uiMessageQueue;
std::mutex g_uiMessageMutex;

// ── Profile Slots for MIDI switching ──
std::vector<std::wstring> g_profileSlots;

// ── CC Hold State ──
std::map<int, bool> g_ccHoldActive;

// ── Hook State ──
HHOOK g_hKeyboardHook = NULL;

// ── Performance Flags ──
std::atomic<bool> g_chordsEnabled{false};
std::set<int> g_gestureNotes;
std::mutex g_gestureNotesMutex;
ULONGLONG g_lastUiPushTime = 0;
const ULONGLONG UI_THROTTLE_MS = 16; // ~60fps
const std::string APP_VERSION = "1.1";

// ── HUD Overlay (Feature 4) ──
HWND g_hwndHud = NULL;
wil::com_ptr<ICoreWebView2> g_hudWebview;
wil::com_ptr<ICoreWebView2Controller> g_hudController;

// ── Forward Declaration ──
LRESULT CALLBACK LowLevelKeyboardProc(int nCode, WPARAM wParam, LPARAM lParam);

// ══════════════════════════════════════════
//  HandleWebMessage (JS → C++ Dispatch)
// ══════════════════════════════════════════

void HandleWebMessage(const std::string& messageStr) {
    json msg;
    try { msg = json::parse(messageStr); } catch (...) { return; }

    std::string action = msg.value("action", "");

    if (action == "init") {
        ScanMidiPorts();
        SendMappingsToUI();
        // Send config to UI
        PostToWebView({
            {"type", "config"},
            {"config", {
                {"auto_reconnect", g_autoReconnect},
                {"app_switching", g_appSwitchingEnabled},
                {"velocity_zones", g_velocityZonesEnabled},
                {"minimize_to_tray", g_minimizeToTrayEnabled},
                {"ai_api_key", g_aiApiKey},
                {"ai_global_prompt", g_aiGlobalPrompt}
            }}
        });
        // Auto-connect last port
        if (!g_lastConnectedPortName.empty() && g_autoReconnect) {
            for (int i = 0; i < (int)g_ports.size(); ++i) {
                if (g_ports[i] == g_lastConnectedPortName) {
                    ConnectMidi(i);
                    PostToWebView({ {"type", "ports"}, {"ports", g_ports}, {"selected", i} });
                    break;
                }
            }
        }
        // Auto-load last profile
        if (!g_lastProfilePath.empty()) {
            LoadMappings(g_lastProfilePath);
        }
        // Send workspace bindings to UI (Feature 2)
        {
            json bindings = json::object();
            for (auto& [exe, profile] : g_appProfileBindings)
                bindings[WideToUtf8(exe)] = WideToUtf8(profile);
            PostToWebView({ {"type", "workspace_bindings"}, {"bindings", bindings} });
        }
    }
    else if (action == "toggle_connect") {
        if (g_connected) {
            DisconnectMidi();
        } else {
            int port = msg.value("port", 0);
            ScanMidiPorts();
            ConnectMidi(port);
        }
    }
    else if (action == "select_port") {
        // No-op, but keep for future MIDI Out support
    }
    else if (action == "start_learn") {
        std::lock_guard<std::mutex> lock(g_learnMutex);
        g_learning = true;
        g_learn_pending = { -1, -1, {}, -1, 0, 1, 0, 0, -1, "", "", "", "", 0, true };
        g_learnStartTime = GetTickCount64();
        PostToWebView({ {"type", "learn_phase"}, {"phase", 1}, {"text", "Play a MIDI note or move a CC..."} });
        SendLog("Learning mode: waiting for MIDI input...");
    }
    else if (action == "cancel_learn") {
        std::lock_guard<std::mutex> lock(g_learnMutex);
        g_learning = false;
        PostToWebView({ {"type", "learn_done"} });
        SendLog("Learning cancelled.");
        if (g_hKeyboardHook) {
            UnhookWindowsHookEx(g_hKeyboardHook);
            g_hKeyboardHook = NULL;
        }
    }
    else if (action == "learn_key") {
        // Phase 2: Install a low-level keyboard hook to capture the next keypress
        std::lock_guard<std::mutex> lock(g_learnMutex);
        if (!g_learning || g_learn_pending.midi_type == -1) {
            SendLog("Learn key: Not in valid learn state.", "error");
            return;
        }
        PostToWebView({ {"type", "learn_phase"}, {"phase", 2}, {"text", "Press a key to assign..."} });
        SendLog("Waiting for keyboard input...");

        // Install hook
        g_hKeyboardHook = SetWindowsHookEx(WH_KEYBOARD_LL, LowLevelKeyboardProc, GetModuleHandle(NULL), 0);
    }
    else if (action == "add_mapping") {
        {
            std::lock_guard<std::recursive_mutex> lock(g_mappingsMutex);
            g_mappings.push_back({ 0, 60, {}, 0x41, 0, 1, 0, 0, -1, "", "", "", "", 0, true });
        }
        SendMappingsToUI();
        SendLog("Added new mapping.");
    }
    else if (action == "delete_mapping") {
        int idx = msg.value("index", -1);
        {
            std::lock_guard<std::recursive_mutex> lock(g_mappingsMutex);
            if (idx >= 0 && idx < (int)g_mappings.size()) {
                g_mappings.erase(g_mappings.begin() + idx);
            }
        }
        SendMappingsToUI();
    }
    else if (action == "toggle_mapping") {
        int idx = msg.value("index", -1);
        {
            std::lock_guard<std::recursive_mutex> lock(g_mappingsMutex);
            if (idx >= 0 && idx < (int)g_mappings.size()) {
                g_mappings[idx].enabled = !g_mappings[idx].enabled;
            }
        }
        SendMappingsToUI();
    }
    else if (action == "update_mapping") {
        int idx = msg.value("index", -1);
        {
            std::lock_guard<std::recursive_mutex> lock(g_mappingsMutex);
            if (idx >= 0 && idx < (int)g_mappings.size()) {
                auto& m = g_mappings[idx];
                if (msg.contains("midi_type")) m.midi_type = msg["midi_type"].get<int>();
                if (msg.contains("key_vk")) m.key_vk = msg["key_vk"].get<int>();
                if (msg.contains("gesture_id")) m.gesture_id = msg["gesture_id"].get<int>();
                if (msg.contains("macro_text")) m.macro_text = msg["macro_text"].get<std::string>();
                if (msg.contains("ai_prompt")) m.ai_prompt = msg["ai_prompt"].get<std::string>();
                if (msg.contains("app_pattern")) m.app_pattern = msg["app_pattern"].get<std::string>();
                if (msg.contains("title_pattern")) m.title_pattern = msg["title_pattern"].get<std::string>();
                if (msg.contains("midi_chord") && msg["midi_chord"].is_array()) {
                    m.midi_chord = msg["midi_chord"].get<std::vector<int>>();
                }
            }
        }
        SendMappingsToUI();
    }
    else if (action == "update_mappings") {
        // Full mapping list update (from JS undo/reorder)
        if (msg.contains("mappings") && msg["mappings"].is_array()) {
            std::lock_guard<std::recursive_mutex> lock(g_mappingsMutex);
            g_mappings.clear();
            for (const auto& it : msg["mappings"]) {
                Mapping m = {};
                m.midi_type = it.value("midi_type", 0);
                m.midi_num = it.value("midi_num", 0);
                m.key_vk = it.value("key_vk", 0);
                m.modifiers = it.value("modifiers", 0);
                m.vel_min = it.value("vel_min", 1);
                m.vel_zone = it.value("vel_zone", 0);
                m.cc_action = it.value("cc_action", 0);
                m.profile_switch = it.value("profile_switch", -1);
                m.macro_text = it.value("macro_text", "");
                m.ai_prompt = it.value("ai_prompt", "");
                m.title_pattern = it.value("title_pattern", "");
                m.app_pattern = it.value("app_pattern", "");
                m.gesture_id = it.value("gesture_id", 0);
                m.enabled = it.value("enabled", true);
                if (it.contains("midi_chord") && it["midi_chord"].is_array())
                    m.midi_chord = it["midi_chord"].get<std::vector<int>>();
                g_mappings.push_back(m);
            }
        }
        SendMappingsToUI();
    }
    else if (action == "clear_mappings") {
        {
            std::lock_guard<std::recursive_mutex> lock(g_mappingsMutex);
            g_mappings.clear();
        }
        SendMappingsToUI();
        SendLog("All mappings cleared.");
    }
    else if (action == "save_profile") {
        wchar_t filepath[MAX_PATH] = { 0 };
        OPENFILENAMEW ofn = {};
        ofn.lStructSize = sizeof(ofn);
        ofn.hwndOwner = g_hwndMain;
        ofn.lpstrFilter = L"JSON Files\0*.json\0All Files\0*.*\0";
        ofn.lpstrFile = filepath;
        ofn.nMaxFile = MAX_PATH;
        ofn.Flags = OFN_OVERWRITEPROMPT;
        ofn.lpstrDefExt = L"json";
        if (GetSaveFileName(&ofn)) {
            SaveMappings(filepath);
            g_lastProfilePath = filepath;
            SaveConfig();
            SendLog("Profile saved: " + WideToUtf8(g_lastProfilePath));
        }
    }
    else if (action == "load_profile") {
        wchar_t filepath[MAX_PATH] = { 0 };
        OPENFILENAMEW ofn = {};
        ofn.lStructSize = sizeof(ofn);
        ofn.hwndOwner = g_hwndMain;
        ofn.lpstrFilter = L"JSON Files\0*.json\0All Files\0*.*\0";
        ofn.lpstrFile = filepath;
        ofn.nMaxFile = MAX_PATH;
        ofn.Flags = OFN_FILEMUSTEXIST;
        if (GetOpenFileName(&ofn)) {
            LoadMappings(filepath);
            SaveConfig();
            SendLog("Profile loaded: " + WideToUtf8(g_lastProfilePath));
        }
    }
    else if (action == "update_config") {
        if (msg.contains("auto_reconnect")) g_autoReconnect = msg["auto_reconnect"].get<bool>();
        if (msg.contains("app_switching")) {
            bool newState = msg["app_switching"].get<bool>();
            if (newState && !g_appSwitchingEnabled) StartAppMonitoring();
            else if (!newState && g_appSwitchingEnabled) StopAppMonitoring();
            g_appSwitchingEnabled = newState;
        }
        if (msg.contains("velocity_zones")) g_velocityZonesEnabled = msg["velocity_zones"].get<bool>();
        if (msg.contains("minimize_to_tray")) g_minimizeToTrayEnabled = msg["minimize_to_tray"].get<bool>();
        if (msg.contains("ai_api_key")) g_aiApiKey = msg["ai_api_key"].get<std::string>();
        if (msg.contains("ai_global_prompt")) g_aiGlobalPrompt = msg["ai_global_prompt"].get<std::string>();
        SaveConfig();
    }
    else if (action == "simulate_text") {
        if (msg.contains("text")) SimulateText(msg["text"].get<std::string>());
    }
    else if (action == "drag_window") {
        ReleaseCapture();
        SendMessage(g_hwndMain, WM_NCLBUTTONDOWN, HTCAPTION, 0);
    }
    else if (action == "minimize_window") {
        if (g_minimizeToTrayEnabled) MinimizeToTray(g_hwndMain);
        else ShowWindow(g_hwndMain, SW_MINIMIZE);
    }
    else if (action == "maximize_window") {
        if (IsZoomed(g_hwndMain)) ShowWindow(g_hwndMain, SW_RESTORE);
        else ShowWindow(g_hwndMain, SW_MAXIMIZE);
    }
    else if (action == "close_window") {
        PostMessage(g_hwndMain, WM_CLOSE, 0, 0);
    }
    // Feature 9: Execute a single delayed keypress from JS macro engine
    else if (action == "execute_delayed_key") {
        int vk = msg.value("vk", 0);
        int mods = msg.value("modifiers", 0);
        if (vk > 0) SimulateKeyCombo(vk, mods);
    }
    // Feature 2: Workspace Bindings
    else if (action == "set_workspace_binding") {
        std::string app = msg.value("app", "");
        if (!app.empty()) {
            // Open file dialog for profile selection
            wchar_t filepath[MAX_PATH] = { 0 };
            OPENFILENAMEW ofn = {};
            ofn.lStructSize = sizeof(ofn);
            ofn.hwndOwner = g_hwndMain;
            ofn.lpstrFilter = L"JSON Files\0*.json\0All Files\0*.*\0";
            ofn.lpstrFile = filepath;
            ofn.nMaxFile = MAX_PATH;
            ofn.Flags = OFN_FILEMUSTEXIST;
            if (GetOpenFileName(&ofn)) {
                g_appProfileBindings[Utf8ToWide(app)] = filepath;
                SaveConfig();
                SendLog("Workspace bound: " + app + " -> " + WideToUtf8(std::wstring(filepath)));
                // Refresh UI
                json bindings = json::object();
                for (auto& [exe, profile] : g_appProfileBindings)
                    bindings[WideToUtf8(exe)] = WideToUtf8(profile);
                PostToWebView({ {"type", "workspace_bindings"}, {"bindings", bindings} });
            }
        }
    }
    else if (action == "remove_workspace_binding") {
        std::string app = msg.value("app", "");
        if (!app.empty()) {
            g_appProfileBindings.erase(Utf8ToWide(app));
            SaveConfig();
            SendLog("Workspace removed: " + app);
            json bindings = json::object();
            for (auto& [exe, profile] : g_appProfileBindings)
                bindings[WideToUtf8(exe)] = WideToUtf8(profile);
            PostToWebView({ {"type", "workspace_bindings"}, {"bindings", bindings} });
        }
    }
    else if (action == "get_workspace_bindings") {
        json bindings = json::object();
        for (auto& [exe, profile] : g_appProfileBindings)
            bindings[WideToUtf8(exe)] = WideToUtf8(profile);
        PostToWebView({ {"type", "workspace_bindings"}, {"bindings", bindings} });
    }
}

// ══════════════════════════════════════════
//  Window Procedure
// ══════════════════════════════════════════

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE:
        break;

    case WM_GETMINMAXINFO: {
        // Enforce minimum window size + correct maximize behavior
        LPMINMAXINFO mmi = (LPMINMAXINFO)lParam;
        int dpi = GetDpiForWindow(hwnd);
        float scale = dpi / 96.0f;
        mmi->ptMinTrackSize.x = (int)(750 * scale);
        mmi->ptMinTrackSize.y = (int)(450 * scale);
        // Respect the taskbar
        HMONITOR hmon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi = {};
        mi.cbSize = sizeof(mi);
        if (GetMonitorInfo(hmon, &mi)) {
            mmi->ptMaxPosition.x = mi.rcWork.left - mi.rcMonitor.left;
            mmi->ptMaxPosition.y = mi.rcWork.top - mi.rcMonitor.top;
            mmi->ptMaxSize.x = mi.rcWork.right - mi.rcWork.left;
            mmi->ptMaxSize.y = mi.rcWork.bottom - mi.rcWork.top;
        }
        return 0;
    }

    case WM_SIZE:
        if (g_controller) {
            RECT bounds;
            GetClientRect(hwnd, &bounds);
            g_controller->put_Bounds(bounds);
        }
        break;
    
    case WM_TIMER:
        // Gesture timers (one per note)
        if (wParam >= GESTURE_TIMER_ID && wParam < GESTURE_TIMER_ID + 128) {
            int noteNum = (int)(wParam - GESTURE_TIMER_ID);
            KillTimer(hwnd, wParam);
            
            std::lock_guard<std::mutex> lock(g_gestureMutex);
            auto& ks = g_keyStates[noteNum];
            int taps = ks.tapCount;
            ks.tapCount = 0;
            ks.processingGesture = false;
            
            if (taps >= 2) {
                ResolveGesture(noteNum, 1); // Double Tap
            } else if (taps == 1) {
                ResolveGesture(noteNum, 0); // Single Tap
            }
        }
        else if (wParam == RECONNECT_TIMER_ID) {
            TryAutoReconnect();
        }
        else if (wParam == PIANO_DECAY_TIMER) {
            json changes = json::array();
            for (int i = 0; i < PIANO_TOTAL_KEYS; i++) {
                if (g_pianoVelocity[i] > 0) {
                    g_pianoVelocity[i] -= 8;
                    if (g_pianoVelocity[i] < 0) g_pianoVelocity[i] = 0;
                    changes.push_back({{"k", i}, {"v", g_pianoVelocity[i]}});
                }
            }
            if (!changes.empty()) PostToWebView({ {"type", "piano_decay"}, {"keys", changes} });
        }
        else if (wParam == CHORD_TIMER_ID) {
            KillTimer(hwnd, CHORD_TIMER_ID);
            std::vector<int> chord;
            {
                std::lock_guard<std::mutex> lock(g_chordMutex);
                chord = g_chordBuffer;
                g_chordBuffer.clear();
            }
            if (!chord.empty()) ProcessChord(chord);
        }
        break;

    case WM_CHORD_SIGNAL: {
        // Reset and start chord timer each time a new note arrives
        KillTimer(hwnd, CHORD_TIMER_ID);
        SetTimer(hwnd, CHORD_TIMER_ID, CHORD_THRESHOLD_MS, NULL);
        break;
    }

    case WM_UI_BRIDGE_SIGNAL: {
        // Drain queued messages from MIDI thread
        while (true) {
            json msg;
            {
                std::lock_guard<std::mutex> lock(g_uiMessageMutex);
                if (g_uiMessageQueue.empty()) break;
                msg = g_uiMessageQueue.front();
                g_uiMessageQueue.pop();
            }
            if (g_webview) {
                std::string s = msg.dump();
                std::wstring ws = Utf8ToWide(s);
                g_webview->PostWebMessageAsJson(ws.c_str());
            }
        }
        break;
    }

    case WM_LEARN_MIDI_SIGNAL: {
        // Process learning capture from MIDI callback thread
        int learnedType = (int)wParam;
        int learnedNum = (int)lParam;

        // Grace period to prevent capturing accidental events at the start
        ULONGLONG elapsed = GetTickCount64() - g_learnStartTime;
        if (elapsed < 200) break; // Ignore events in the first 200ms

        std::lock_guard<std::mutex> lock(g_learnMutex);
        if (!g_learning || g_learn_pending.midi_type != -1) break;

        g_learn_pending.midi_type = learnedType;
        g_learn_pending.midi_num = learnedNum;

        std::string name = (learnedType == 0) ? "Note " + std::to_string(learnedNum) : "CC " + std::to_string(learnedNum);
        PostToWebView({ {"type", "learn_phase"}, {"phase", 2}, {"text", "Got " + name + ". Press a key to assign..."} });
        SendLog("Captured MIDI: " + name + ". Waiting for key...");

        // Install keyboard hook for phase 2
        g_hKeyboardHook = SetWindowsHookEx(WH_KEYBOARD_LL, LowLevelKeyboardProc, GetModuleHandle(NULL), 0);
        break;
    }

    case WM_USER + 100: {
        // Profile slot switch (from ProcessMIDIEvent)
        int slot = (int)wParam;
        if (slot >= 0 && slot < (int)g_profileSlots.size()) {
            if (!g_profileSlots[slot].empty()) {
                LoadMappings(g_profileSlots[slot]);
                SendLog("Switched to profile slot #" + std::to_string(slot));
            }
        }
        break;
    }

    case WM_TRAYICON:
        if (lParam == WM_LBUTTONDBLCLK) {
            RestoreFromTray(hwnd);
        }
        else if (lParam == WM_RBUTTONDOWN) {
            POINT pt;
            GetCursorPos(&pt);
            HMENU hMenu = CreatePopupMenu();
            AppendMenu(hMenu, MF_STRING, ID_TRAY_SHOW, L"Show");
            AppendMenu(hMenu, MF_STRING, ID_TRAY_EXIT, L"Exit");
            SetForegroundWindow(hwnd);
            TrackPopupMenu(hMenu, TPM_BOTTOMALIGN | TPM_LEFTALIGN, pt.x, pt.y, 0, hwnd, NULL);
            DestroyMenu(hMenu);
        }
        break;

    case WM_COMMAND:
        if (LOWORD(wParam) == ID_TRAY_SHOW) RestoreFromTray(hwnd);
        else if (LOWORD(wParam) == ID_TRAY_EXIT) {
            RemoveTrayIcon();
            DestroyWindow(hwnd);
        }
        break;

    case WM_CLOSE:
        if (g_minimizeToTrayEnabled) {
            MinimizeToTray(hwnd);
            return 0;
        }
        DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY: {
        // Graceful shutdown: release all held keys
        for (int i = 0; i < 128; i++) {
            if (g_pianoPhysicalDown[i].load(std::memory_order_relaxed)) {
                // Release any key that might be held
                std::lock_guard<std::recursive_mutex> lock(g_mappingsMutex);
                for (const auto& m : g_mappings) {
                    if (m.midi_type == 0 && m.midi_num == i) {
                        SendKeyInput(m.key_vk, false, m.modifiers);
                    }
                }
            }
        }
        // Release sustained keys
        {
            std::lock_guard<std::mutex> lock(g_sustainMutex);
            for (int vk : g_sustainedVKs) SendKeyInput(vk, false);
            g_sustainedVKs.clear();
        }

        // Save state
        SaveConfig();
        if (!g_lastProfilePath.empty()) SaveMappings(g_lastProfilePath);

        // Cleanup hooks
        if (g_hKeyboardHook) { UnhookWindowsHookEx(g_hKeyboardHook); g_hKeyboardHook = NULL; }
        StopAppMonitoring();
        RemoveTrayIcon();
        DisconnectMidi();

        g_controller = nullptr;
        g_webview = nullptr;
        PostQuitMessage(0);
        break;
    }

    case WM_NCHITTEST: {
        // Custom hit-testing for borderless window resizing
        POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        ScreenToClient(hwnd, &pt);
        RECT rc;
        GetClientRect(hwnd, &rc);
        int border = 8; // resize border width

        LRESULT hit = HTCLIENT;
        if (pt.y < border && pt.x < border) hit = HTTOPLEFT;
        else if (pt.y < border && pt.x >= rc.right - border) hit = HTTOPRIGHT;
        else if (pt.y >= rc.bottom - border && pt.x < border) hit = HTBOTTOMLEFT;
        else if (pt.y >= rc.bottom - border && pt.x >= rc.right - border) hit = HTBOTTOMRIGHT;
        else if (pt.y < border) hit = HTTOP;
        else if (pt.y >= rc.bottom - border) hit = HTBOTTOM;
        else if (pt.x < border) hit = HTLEFT;
        else if (pt.x >= rc.right - border) hit = HTRIGHT;

        if (hit != HTCLIENT) return hit;
        break;
    }

    default:
        break;
    }
    return DefWindowProc(hwnd, msg, wParam, lParam);
}

// ══════════════════════════════════════════
//  WebView2 Initialization
// ══════════════════════════════════════════

void InitWebView2(HWND hwnd) {
    // Find UI directory
    wchar_t exePath[MAX_PATH];
    GetModuleFileNameW(NULL, exePath, MAX_PATH);
    std::wstring exeDir(exePath);
    exeDir = exeDir.substr(0, exeDir.find_last_of(L"\\") + 1);

    std::wstring uiPath;
    std::vector<std::wstring> potentialPaths = {
        exeDir + L"ui",                           // Installed layout
        exeDir + L"..\\..\\MIDI Mapper\\src\\ui",  // Dev build from VS output
        exeDir + L"..\\src\\ui",                    // Alt dev layout
        exeDir                                      // Fallback
    };
    for (auto& p : potentialPaths) {
        DWORD attr = GetFileAttributesW(p.c_str());
        if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY)) {
            uiPath = p;
            break;
        }
    }
    if (uiPath.empty()) uiPath = exeDir;

    CreateCoreWebView2EnvironmentWithOptions(nullptr, nullptr, nullptr,
        Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
            [hwnd, uiPath](HRESULT result, ICoreWebView2Environment* env) -> HRESULT {
                if (FAILED(result)) return result;
                env->CreateCoreWebView2Controller(hwnd,
                    Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                        [hwnd, uiPath](HRESULT result, ICoreWebView2Controller* controller) -> HRESULT {
                            if (FAILED(result)) return result;
                            g_controller = controller;
                            g_controller->get_CoreWebView2(&g_webview);

                            // Settings
                            wil::com_ptr<ICoreWebView2Settings> settings;
                            g_webview->get_Settings(&settings);
                            settings->put_IsScriptEnabled(TRUE);
                            settings->put_IsWebMessageEnabled(TRUE);
                            settings->put_AreDevToolsEnabled(TRUE);

                            // Virtual Host Mapping
                            wil::com_ptr<ICoreWebView2_3> webView3;
                            g_webview.query_to(&webView3);
                            if (webView3) {
                                webView3->SetVirtualHostNameToFolderMapping(
                                    L"app.miditypist", uiPath.c_str(),
                                    COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_ALLOW);
                            }

                            // Transparent background
                            wil::com_ptr<ICoreWebView2Controller2> controller2;
                            g_controller.query_to(&controller2);
                            if (controller2) {
                                COREWEBVIEW2_COLOR transparent = { 0, 0, 0, 0 };
                                controller2->put_DefaultBackgroundColor(transparent);
                            }

                            // Size to window
                            RECT bounds;
                            GetClientRect(hwnd, &bounds);
                            g_controller->put_Bounds(bounds);

                            // Handle messages from JS
                            g_webview->add_WebMessageReceived(
                                Callback<ICoreWebView2WebMessageReceivedEventHandler>(
                                    [](ICoreWebView2* sender, ICoreWebView2WebMessageReceivedEventArgs* args) -> HRESULT {
                                        wil::unique_cotaskmem_string messageRaw;
                                        args->TryGetWebMessageAsString(&messageRaw);
                                        if (messageRaw) {
                                            HandleWebMessage(WideToUtf8(messageRaw.get()));
                                        }
                                        return S_OK;
                                    }).Get(), nullptr);

                            // Navigate
                            g_webview->Navigate(L"https://app.miditypist/index.html");

                            return S_OK;
                        }).Get());
                return S_OK;
            }).Get());
}

// ══════════════════════════════════════════
//  HUD Overlay (Feature 4)
// ══════════════════════════════════════════

void PostToHud(const json& msg) {
    if (!g_hudWebview) return;
    std::wstring jsonStr = Utf8ToWide(msg.dump());
    g_hudWebview->PostWebMessageAsJson(jsonStr.c_str());
}

void InitHudOverlay(HWND parent) {
    // Get primary monitor dimensions
    int screenW = GetSystemMetrics(SM_CXSCREEN);
    int screenH = GetSystemMetrics(SM_CYSCREEN);
    int hudW = 400, hudH = 80;
    int hudX = (screenW - hudW) / 2;
    int hudY = screenH - hudH - 48;

    // Register a separate window class for the HUD
    WNDCLASSEX wcHud = { sizeof(WNDCLASSEX) };
    wcHud.lpfnWndProc = DefWindowProc;
    wcHud.hInstance = g_hInst;
    wcHud.lpszClassName = L"MIDITypistHUD";
    RegisterClassEx(&wcHud);

    // Create a layered, transparent, click-through, always-on-top popup
    g_hwndHud = CreateWindowEx(
        WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW,
        L"MIDITypistHUD", L"HUD",
        WS_POPUP,
        hudX, hudY, hudW, hudH,
        nullptr, nullptr, g_hInst, nullptr);

    if (!g_hwndHud) return;

    // Make window fully transparent via layered alpha
    SetLayeredWindowAttributes(g_hwndHud, 0, 255, LWA_ALPHA);
    ShowWindow(g_hwndHud, SW_SHOWNOACTIVATE);

    // Find UI directory (same logic as main WebView)
    wchar_t exePath[MAX_PATH];
    GetModuleFileNameW(NULL, exePath, MAX_PATH);
    std::wstring exeDir(exePath);
    exeDir = exeDir.substr(0, exeDir.find_last_of(L"\\") + 1);

    std::wstring uiPath;
    std::vector<std::wstring> potentialPaths = {
        exeDir + L"ui",
        exeDir + L"..\\..\\MIDI Mapper\\src\\ui",
        exeDir + L"..\\src\\ui",
        exeDir
    };
    for (auto& p : potentialPaths) {
        DWORD attr = GetFileAttributesW(p.c_str());
        if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY)) {
            uiPath = p;
            break;
        }
    }
    if (uiPath.empty()) uiPath = exeDir;

    CreateCoreWebView2EnvironmentWithOptions(nullptr, nullptr, nullptr,
        Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
            [uiPath](HRESULT result, ICoreWebView2Environment* env) -> HRESULT {
                if (FAILED(result)) return result;
                env->CreateCoreWebView2Controller(g_hwndHud,
                    Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                        [uiPath](HRESULT result, ICoreWebView2Controller* controller) -> HRESULT {
                            if (FAILED(result)) return result;
                            g_hudController = controller;
                            g_hudController->get_CoreWebView2(&g_hudWebview);

                            // Settings
                            wil::com_ptr<ICoreWebView2Settings> settings;
                            g_hudWebview->get_Settings(&settings);
                            settings->put_IsScriptEnabled(TRUE);
                            settings->put_IsWebMessageEnabled(TRUE);
                            settings->put_AreDevToolsEnabled(FALSE);
                            settings->put_AreDefaultContextMenusEnabled(FALSE);

                            // Virtual host mapping
                            wil::com_ptr<ICoreWebView2_3> webView3;
                            g_hudWebview.query_to(&webView3);
                            if (webView3) {
                                webView3->SetVirtualHostNameToFolderMapping(
                                    L"hud.miditypist", uiPath.c_str(),
                                    COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_ALLOW);
                            }

                            // Transparent background
                            wil::com_ptr<ICoreWebView2Controller2> controller2;
                            g_hudController.query_to(&controller2);
                            if (controller2) {
                                COREWEBVIEW2_COLOR transparent = { 0, 0, 0, 0 };
                                controller2->put_DefaultBackgroundColor(transparent);
                            }

                            // Size to HUD window
                            RECT bounds;
                            GetClientRect(g_hwndHud, &bounds);
                            g_hudController->put_Bounds(bounds);

                            // Navigate to HUD page
                            g_hudWebview->Navigate(L"https://hud.miditypist/hud.html");

                            return S_OK;
                        }).Get());
                return S_OK;
            }).Get());
}

// ══════════════════════════════════════════
//  Entry Point
// ══════════════════════════════════════════

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR, int nCmdShow) {
    g_hInst = hInstance;
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    // Load persistent config
    g_configPath = GetConfigDir() + L"midityper_config.json";
    LoadConfig();

    WNDCLASSEX wc = {};
    wc.cbSize = sizeof(WNDCLASSEX);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = L"MIDITypistClass";
    wc.hIcon = LoadIcon(hInstance, MAKEINTRESOURCE(IDI_APP_ICON));
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    RegisterClassEx(&wc);

    g_hwndMain = CreateWindowEx(
        WS_EX_APPWINDOW,
        L"MIDITypistClass", L"MIDITypist",
        WS_POPUP | WS_THICKFRAME | WS_MINIMIZEBOX | WS_MAXIMIZEBOX,
        CW_USEDEFAULT, CW_USEDEFAULT, 1100, 700,
        nullptr, nullptr, hInstance, nullptr);

    // DWM: Dark title bar + Mica Effect
    BOOL darkMode = TRUE;
    DwmSetWindowAttribute(g_hwndMain, DWMWA_USE_IMMERSIVE_DARK_MODE, &darkMode, sizeof(darkMode));
    int backdropType = 2; // DWMSBT_MAINWINDOW (Mica)
    DwmSetWindowAttribute(g_hwndMain, 38, &backdropType, sizeof(backdropType)); // DWMWA_SYSTEMBACKDROP_TYPE
    MARGINS margins = { -1, -1, -1, -1 };
    DwmExtendFrameIntoClientArea(g_hwndMain, &margins);

    ShowWindow(g_hwndMain, nCmdShow);
    UpdateWindow(g_hwndMain);

    InitWebView2(g_hwndMain);
    AddTrayIcon(g_hwndMain);

    // Start app-monitoring & timers
    if (g_appSwitchingEnabled) StartAppMonitoring();
    SetTimer(g_hwndMain, RECONNECT_TIMER_ID, RECONNECT_INTERVAL, NULL);
    SetTimer(g_hwndMain, PIANO_DECAY_TIMER, PIANO_DECAY_MS, NULL);

    MSG msgLoop;
    while (GetMessage(&msgLoop, nullptr, 0, 0)) {
        TranslateMessage(&msgLoop);
        DispatchMessage(&msgLoop);
    }

    CoUninitialize();
    return (int)msgLoop.wParam;
}

// ══════════════════════════════════════════
//  Low-Level Keyboard Hook (Learning Mode)
// ══════════════════════════════════════════

LRESULT CALLBACK LowLevelKeyboardProc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode == HC_ACTION && wParam == WM_KEYDOWN) {
        KBDLLHOOKSTRUCT* pKey = (KBDLLHOOKSTRUCT*)lParam;
        std::lock_guard<std::mutex> lock(g_learnMutex);
        if (g_learning && g_learn_pending.midi_type != -1) {
            int vk = pKey->vkCode;
            // Ignore modifiers alone
            if (vk == VK_CONTROL || vk == VK_SHIFT || vk == VK_MENU ||
                vk == VK_LCONTROL || vk == VK_RCONTROL ||
                vk == VK_LSHIFT || vk == VK_RSHIFT ||
                vk == VK_LMENU || vk == VK_RMENU) {
                return CallNextHookEx(NULL, nCode, wParam, lParam);
            }

            int mods = 0;
            if (GetAsyncKeyState(VK_CONTROL) & 0x8000) mods |= 1;
            if (GetAsyncKeyState(VK_SHIFT) & 0x8000) mods |= 2;
            if (GetAsyncKeyState(VK_MENU) & 0x8000) mods |= 4;

            g_learn_pending.key_vk = vk;
            g_learn_pending.modifiers = mods;

            // Add the new mapping
            {
                std::lock_guard<std::recursive_mutex> mlock(g_mappingsMutex);
                g_mappings.push_back(g_learn_pending);
            }
            SendMappingsToUI();
            SendLog("Mapping created: " + WideToUtf8(GetModifierString(mods) + GetKeyName(vk)));

            // Reset
            g_learning = false;
            g_learn_pending = { -1, -1, {}, -1, 0, 1, 0, 0, -1, "", "", "", "", 0, true };
            PostToWebView({ {"type", "learn_done"} });

            // Unhook
            PostMessage(g_hwndMain, WM_USER + 50, 0, 0); // Safe to unhook outside callback
            return 1; // Block the key from reaching the OS
        }
    }

    // Handle the unhook message
    if (nCode < 0) return CallNextHookEx(NULL, nCode, wParam, lParam);
    return CallNextHookEx(NULL, nCode, wParam, lParam);
}
