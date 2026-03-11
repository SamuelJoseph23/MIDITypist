#include "app_context.h"
#include "bridge.h"
#include "config.h"

// ══════════════════════════════════════════
//  Per-App Switching
// ══════════════════════════════════════════

void StartAppMonitoring() {
    if (!g_hWinEventHook) {
        g_hWinEventHook = SetWinEventHook(
            EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND,
            NULL, WinEventProc, 0, 0,
            WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
    }
}

void StopAppMonitoring() {
    if (g_hWinEventHook) {
        UnhookWinEvent(g_hWinEventHook);
        g_hWinEventHook = nullptr;
    }
}

void CALLBACK WinEventProc(HWINEVENTHOOK hWinEventHook, DWORD event, HWND hwnd,
    LONG idObject, LONG idChild, DWORD dwEventThread, DWORD dwmsEventTime) {
    if (event == EVENT_SYSTEM_FOREGROUND && hwnd) {
        wchar_t path[MAX_PATH];
        DWORD pid;
        GetWindowThreadProcessId(hwnd, &pid);
        HANDLE hProcess = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        if (hProcess) {
            if (GetProcessImageFileNameW(hProcess, path, MAX_PATH)) {
                wchar_t* filename = wcsrchr(path, L'\\');
                if (filename) {
                    g_currentApp = filename + 1;
                    
                    // Capture title too
                    wchar_t title[512];
                    if (GetWindowTextW(hwnd, title, 512)) {
                        g_currentWindowTitle = title;
                    } else {
                        g_currentWindowTitle = L"";
                    }

                    PostToWebView({ 
                        {"type", "app_changed"}, 
                        {"app", WideToUtf8(g_currentApp)},
                        {"title", WideToUtf8(g_currentWindowTitle)}
                    });
                    
                    // Trigger profile switch if bound
                    if (g_appProfileBindings.count(g_currentApp)) {
                        LoadMappings(g_appProfileBindings[g_currentApp]);
                        SendLog("Auto-switched profile for: " + WideToUtf8(g_currentApp));
                        PostToWebView({ {"type", "toast"}, {"text", "Workspace: " + WideToUtf8(g_currentApp)}, {"level", "info"} });
                    }
                }
            }
            CloseHandle(hProcess);
        }
    }
}
