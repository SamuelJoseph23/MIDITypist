#pragma once

// ══════════════════════════════════════════
//  MIDITypist — Shared Definitions & State
// ══════════════════════════════════════════

// ── System Includes ──
#include <windows.h>
#include <windowsx.h>
#include <CommCtrl.h>
#include <Psapi.h>
#include <shellapi.h>
#include <dwmapi.h>
#include <shlwapi.h>
#include <vector>
#include <string>
#include <sstream>
#include <fstream>
#include <map>
#include <set>
#include <mutex>
#include <atomic>
#include <memory>
#include <algorithm>
#include <functional>
#include <queue>
#include <thread>
#include "RtMidi.h"

// ── WebView2 ──
#include <wrl.h>
#include <wil/com.h>
#pragma warning(push)
#pragma warning(disable: 26819)
#pragma warning(disable: 26495)
#pragma warning(disable: 28182)
#include "json.hpp"
#include <wil/result.h>
#include <wil/resource.h>
#include "WebView2.h"
#pragma warning(pop)

// ── Linker Dependencies ──
#pragma comment(lib, "Comctl32.lib")
#pragma comment(lib, "Psapi.lib")
#pragma comment(lib, "Shell32.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "Shlwapi.lib")

using namespace Microsoft::WRL;
using json = nlohmann::json;

// ── DWM Defines ──
#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
#define DWMWA_USE_IMMERSIVE_DARK_MODE 20
#endif
#ifndef DWMWA_MICA_EFFECT
#define DWMWA_MICA_EFFECT 1029
#endif

// ── Timers & Tray ──
#define RECONNECT_TIMER_ID 502
#define RECONNECT_INTERVAL 3000
#define WM_TRAYICON (WM_USER + 1)
#define IDI_APP_ICON 101
#define ID_TRAY_SHOW 4001
#define ID_TRAY_EXIT 4002

// ── Piano Roll ──
#define PIANO_TOTAL_KEYS 128
#define PIANO_DECAY_TIMER 503
#define PIANO_DECAY_MS 50
#define CHORD_TIMER_ID 504
#define CHORD_THRESHOLD_MS 60 // Window to group notes into a chord
#define WM_CHORD_SIGNAL (WM_USER + 201)
#define WM_LEARN_MIDI_SIGNAL (WM_USER + 202)
#define WM_UI_BRIDGE_SIGNAL (WM_USER + 203)
#define GESTURE_TIMER_ID 505
#define GESTURE_WINDOW_MS 300
#define LONG_HOLD_MS 800

// ── Mapping struct ──
struct Mapping {
    int midi_type;      // 0=Note, 1=CC, 2=Chord, 3=LayerKey, 4=Macro, 5=AI
    int midi_num;       // for Note/CC/LayerKey/Macro/AI
    std::vector<int> midi_chord; // for Chord
    int key_vk;
    int modifiers;      // bitmask: 1=Ctrl, 2=Shift, 4=Alt
    int vel_min;
    int vel_zone;       // 0=any, 1=soft(1-63), 2=hard(64-127)
    int cc_action;      // 0=keypress, 1=mouse_x, 2=mouse_y, 3=scroll, 4=hold_key
    int profile_switch; // -1=normal, 0+=profile slot index
    std::string macro_text; // for Macro
    std::string ai_prompt;  // for AI
    std::string title_pattern; // for Context Filter
    std::string app_pattern;   // for Process Filter (e.g. chrome.exe)
    int gesture_id;     // 0=Single/Any, 1=Double Tap, 2=Long Hold
    bool enabled = true; // Enable/disable toggle
};

// ── Gesture State ──
struct KeyState {
    ULONGLONG lastPressTime = 0;
    int tapCount = 0;
    bool processingGesture = false;
    bool holdTriggered = false;
};

// ══════════════════════════════════════════
//  Global State (extern declarations)
// ══════════════════════════════════════════

extern HINSTANCE g_hInst;
extern HWND g_hwndMain;
extern wil::com_ptr<ICoreWebView2> g_webview;
extern wil::com_ptr<ICoreWebView2Controller> g_controller;

extern std::unique_ptr<RtMidiIn> g_midiIn;
extern std::vector<std::string> g_ports;
extern bool g_connected;
extern int g_lastConnectedPort;
extern std::string g_lastConnectedPortName;

extern std::vector<Mapping> g_mappings;
extern std::recursive_mutex g_mappingsMutex;
extern bool g_learning;
extern std::mutex g_learnMutex;
extern ULONGLONG g_learnStartTime;
extern Mapping g_learn_pending;
extern bool g_verboseLogging;

// ── Per-App Profile ──
extern std::map<std::wstring, std::wstring> g_appProfileBindings;
extern std::wstring g_currentApp;
extern std::wstring g_currentWindowTitle;
extern HWINEVENTHOOK g_hWinEventHook;

// ── Gesture State ──
extern std::map<int, KeyState> g_keyStates;
extern std::mutex g_gestureMutex;

// ── Tray Icon ──
extern NOTIFYICONDATA g_nid;
extern bool g_minimizedToTray;

// ── Persistent Config ──
extern std::wstring g_configPath;
extern std::wstring g_lastProfilePath;

// ── Piano Roll State ──
extern int g_pianoVelocity[PIANO_TOTAL_KEYS];
extern std::atomic<bool> g_pianoPhysicalDown[PIANO_TOTAL_KEYS];
extern int g_pianoCC[128];
extern bool g_sustainActive;
extern std::set<int> g_sustainedVKs;
extern std::mutex g_sustainMutex;

// ── Auto Reconnect & App Switching ──
extern bool g_autoReconnect;
extern bool g_appSwitchingEnabled;
extern bool g_velocityZonesEnabled;
extern bool g_minimizeToTrayEnabled;
extern std::string g_aiApiKey;
extern std::string g_aiGlobalPrompt;

// ── Chord Collector ──
extern std::vector<int> g_chordBuffer;
extern std::mutex g_chordMutex;

// ── UI Bridge Queue (Thread Safe) ──
extern std::queue<json> g_uiMessageQueue;
extern std::mutex g_uiMessageMutex;

// ── Profile Slots for MIDI switching ──
extern std::vector<std::wstring> g_profileSlots;

// ── CC Hold State ──
extern std::map<int, bool> g_ccHoldActive;

// ── Hook State ──
extern HHOOK g_hKeyboardHook;

// ── Performance Flags ──
extern std::atomic<bool> g_chordsEnabled;
extern std::set<int> g_gestureNotes;
extern std::mutex g_gestureNotesMutex;
extern ULONGLONG g_lastUiPushTime;
extern const ULONGLONG UI_THROTTLE_MS;
extern const std::string APP_VERSION;

// ── HUD Overlay (Feature 4) ──
extern HWND g_hwndHud;
extern wil::com_ptr<ICoreWebView2> g_hudWebview;
extern wil::com_ptr<ICoreWebView2Controller> g_hudController;
