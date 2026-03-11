#include "midi_engine.h"
#include "bridge.h"
#include "input_simulation.h"
#include "config.h"

// ══════════════════════════════════════════
//  MIDI Callback
// ══════════════════════════════════════════

void midiCallback(double, std::vector<unsigned char>* msg, void*) {
    if (msg->size() < 3) return;
    int status = (*msg)[0];
    int number = (*msg)[1];
    int velocity = (*msg)[2];

    bool isNoteOn = (status & 0xF0) == 0x90 && velocity > 0;
    bool isNoteOff = (status & 0xF0) == 0x80 || ((status & 0xF0) == 0x90 && velocity == 0);
    bool isCC = (status & 0xF0) == 0xB0;

    // Learning mode check
    {
        std::lock_guard<std::mutex> lock(g_learnMutex);
        if (g_learning && g_learn_pending.midi_type == -1) {
            bool learnThis = false;
            int type = 0;
            if (isNoteOn) {
                type = 0;
                learnThis = true;
            } else if (isCC && number < 120) { // Filter CC noise
                type = 1;
                learnThis = true;
            }

            if (learnThis) {
                // Post to main thread to handle transition
                PostMessage(g_hwndMain, WM_LEARN_MIDI_SIGNAL, (WPARAM)type, (LPARAM)number);
                return; // Exit callback if learning
            }
        }
    }

    // Track physical state
    if (isNoteOn) g_pianoPhysicalDown[number].store(true, std::memory_order_relaxed);
    if (isNoteOff) g_pianoPhysicalDown[number].store(false, std::memory_order_relaxed);

    // CC immediately if not learning
    if (isCC) {
        ProcessMIDIEvent(status & 0xF0, number, velocity);
    }
    
    // Chord grouping logic or Instant Trigger
    if (isNoteOn) {
        if (g_chordsEnabled.load(std::memory_order_acquire)) {
            std::lock_guard<std::mutex> lock(g_chordMutex);
            g_chordBuffer.push_back(number);
            PostMessage(g_hwndMain, WM_CHORD_SIGNAL, 0, 0);
        } else {
            // Bypass chord detection for zero-latency
            ProcessMIDIEvent(0x90, number, velocity);
        }
    }
    
    // Process Note Off immediately
    if (isNoteOff) {
        ProcessMIDIEvent(status & 0xF0, number, velocity);
    }
}

void ProcessChord(const std::vector<int>& chord) {
    if (chord.empty()) return;

    std::vector<int> sortedChord = chord;
    std::sort(sortedChord.begin(), sortedChord.end());
    sortedChord.erase(std::unique(sortedChord.begin(), sortedChord.end()), sortedChord.end());

    std::string chordStr = "";
    for (int n : sortedChord) chordStr += std::to_string(n) + " ";
    SendLog("Processing MIDI chord: [ " + chordStr + "]");
    
    std::lock_guard<std::recursive_mutex> lock(g_mappingsMutex);
    bool found = false;

    // 1. Try to find a specific chord mapping
    if (sortedChord.size() > 1) {
        for (const auto& m : g_mappings) {
            if (m.midi_type != 2) continue;

            // Context Stack Filtering
            if (!m.title_pattern.empty()) {
                std::string currentTitle = WideToUtf8(g_currentWindowTitle);
                if (currentTitle.find(m.title_pattern) == std::string::npos) continue;
            }
            if (!m.app_pattern.empty()) {
                std::string currentApp = WideToUtf8(g_currentApp);
                if (currentApp.find(m.app_pattern) == std::string::npos) continue;
            }

            // Compare sorted notes
            std::vector<int> targetChord = m.midi_chord;
            std::sort(targetChord.begin(), targetChord.end());
            targetChord.erase(std::unique(targetChord.begin(), targetChord.end()), targetChord.end());

            if (targetChord == sortedChord) {
                SimulateKeyCombo(m.key_vk, m.modifiers);
                SendLog("Match found! Triggering VK " + std::to_string(m.key_vk));
                found = true;
                break;
            }
        }
    }

    // 2. If no chord mapping or single note, process individual mappings
    if (!found) {
        for (int note : sortedChord) {
            ProcessMIDIEvent(0x90, note, 100); // Trigger as standard Note On
        }
    }
}

void ProcessMIDIEvent(int type, int number, int velocity) {
    bool isNoteOn = (type == 0x90) && velocity > 0;
    bool isNoteOff = (type == 0x80) || ((type == 0x90) && velocity == 0);
    bool isCC = (type == 0xB0);

    // Update Piano Roll and Gesture state
    if (isNoteOn && number >= 0 && number < 128) {
        g_pianoVelocity[number] = velocity;
        
        // UI Throttling
        ULONGLONG now = GetTickCount64();
        if (now - g_lastUiPushTime > UI_THROTTLE_MS) {
            PostToWebView({ {"type", "midi_note"}, {"note", number}, {"velocity", velocity} });
            g_lastUiPushTime = now;
        }
        
        // Gesture Logic (Only if needed for this note)
        bool hasGesture = false;
        {
            std::lock_guard<std::mutex> gLock(g_gestureNotesMutex);
            hasGesture = g_gestureNotes.count(number) > 0;
        }
        if (hasGesture) {
            std::lock_guard<std::mutex> lock(g_gestureMutex);
            auto& state = g_keyStates[number];
            ULONGLONG pressTime = GetTickCount64();
            if (pressTime - state.lastPressTime < GESTURE_WINDOW_MS) {
                state.tapCount++;
            } else {
                state.tapCount = 1;
                SetTimer(g_hwndMain, GESTURE_TIMER_ID + number, GESTURE_WINDOW_MS, NULL);
            }
            state.lastPressTime = pressTime;
            state.processingGesture = true;
            state.holdTriggered = false;
        }
    }
    else if (isNoteOff && number >= 0 && number < 128) {
        g_pianoVelocity[number] = 0;
        
        ULONGLONG now = GetTickCount64();
        if (now - g_lastUiPushTime > UI_THROTTLE_MS) {
            PostToWebView({ {"type", "midi_note"}, {"note", number}, {"velocity", 0} });
            g_lastUiPushTime = now;
        }
        
        bool hasGestureOff = false;
        {
            std::lock_guard<std::mutex> gLock(g_gestureNotesMutex);
            hasGestureOff = g_gestureNotes.count(number) > 0;
        }
        if (hasGestureOff) {
            std::lock_guard<std::mutex> lock(g_gestureMutex);
            auto& state = g_keyStates[number];
            ULONGLONG duration = GetTickCount64() - state.lastPressTime;
            if (duration >= LONG_HOLD_MS && !state.holdTriggered) {
                state.holdTriggered = true;
                KillTimer(g_hwndMain, GESTURE_TIMER_ID + number);
                state.tapCount = 0;
                state.processingGesture = false;
                ResolveGesture(number, 2); // Long Hold
            }
        }
    }

    int oldCCVal = -1;
    if (isCC && number >= 0 && number < 128) {
        oldCCVal = g_pianoCC[number];
        g_pianoCC[number] = velocity;
        PostToWebView({ {"type", "midi_cc"}, {"cc", number}, {"value", velocity} });

        // Global Sustain Pedal Support (CC 64)
        if (number == 64) {
            std::lock_guard<std::mutex> lock(g_sustainMutex);
            if (velocity > 63 && !g_sustainActive) {
                g_sustainActive = true;
                SendLog("Sustain Pedal: ON", "mapping");
            } else if (velocity <= 63 && g_sustainActive) {
                g_sustainActive = false;
                SendLog("Sustain Pedal: OFF", "mapping");
                for (int vk : g_sustainedVKs) {
                    SendKeyInput(vk, false);
                }
                g_sustainedVKs.clear();
            }
        }
    }

    // CC edge detection flags
    bool ccCrossedUp = (isCC && oldCCVal <= 63 && velocity > 63);
    bool ccCrossedDown = (isCC && oldCCVal > 63 && velocity <= 63);

    // Execute mappings
    // Pre-compute context strings once (not per-mapping)
    std::string cachedTitle = WideToUtf8(g_currentWindowTitle);
    std::string cachedApp = WideToUtf8(g_currentApp);

    std::lock_guard<std::recursive_mutex> lock(g_mappingsMutex);
    for (const auto& m : g_mappings) {
        if (!m.enabled) continue; // Skip disabled mappings

        // Context filtering (using pre-computed strings)
        if (!m.title_pattern.empty()) {
            if (cachedTitle.find(m.title_pattern) == std::string::npos) continue;
        }
        if (!m.app_pattern.empty()) {
            if (cachedApp.find(m.app_pattern) == std::string::npos) continue;
        }

        // Profile switching
        if (m.profile_switch >= 0) {
            if (m.midi_type == 0 && isNoteOn && number == m.midi_num) {
                if (m.profile_switch < (int)g_profileSlots.size()) {
                    PostMessage(g_hwndMain, WM_USER + 100, m.profile_switch, 0);
                }
            }
            continue;
        }

        // Note-to-Key Mapping
        if (m.midi_type == 0 && number == m.midi_num && m.gesture_id == 0) {
            if (isNoteOn) {
                if (!g_pianoPhysicalDown[number].load(std::memory_order_relaxed)) continue; // Rapid tap safety
                if (velocity < m.vel_min) continue;
                if (g_velocityZonesEnabled) {
                    if (m.vel_zone == 1 && velocity > 63) continue;
                    if (m.vel_zone == 2 && velocity < 64) continue;
                }
                SendKeyInput(m.key_vk, true, m.modifiers);
                if (g_verboseLogging) SendLog("Note " + std::to_string(number) + " -> Key Down: " + std::to_string(m.key_vk), "mapping");
            }
            else if (isNoteOff) {
                std::lock_guard<std::mutex> lock(g_sustainMutex);
                if (g_sustainActive) {
                    g_sustainedVKs.insert(m.key_vk);
                    if (g_verboseLogging) SendLog("Note " + std::to_string(number) + " -> Sustaining VK " + std::to_string(m.key_vk), "mapping");
                } else {
                    SendKeyInput(m.key_vk, false, m.modifiers);
                    if (g_verboseLogging) SendLog("Note " + std::to_string(number) + " -> Key Up: " + std::to_string(m.key_vk), "mapping");
                }
            }
        }

        // CC-to-Action Mapping (Edge Detected)
        if (m.midi_type == 1 && isCC && number == m.midi_num) {
            switch (m.cc_action) {
            case 0: // Keypress (Now Momentary by default for games)
                if (ccCrossedUp) {
                    SendKeyInput(m.key_vk, true, m.modifiers);
                    if (g_verboseLogging) SendLog("CC " + std::to_string(number) + " -> Key Down: " + std::to_string(m.key_vk), "mapping");
                } else if (ccCrossedDown) {
                    SendKeyInput(m.key_vk, false, m.modifiers);
                    if (g_verboseLogging) SendLog("CC " + std::to_string(number) + " -> Key Up: " + std::to_string(m.key_vk), "mapping");
                }
                break;
            case 1: SimulateMouseMove((velocity - 64) * 2, 0); break;
            case 2: SimulateMouseMove(0, (velocity - 64) * 2); break;
            case 3: SimulateScroll((velocity - 64) * 20); break;
            case 4: // Hold Key (Dedicated toggle behavior or held state)
                if (ccCrossedUp && !g_ccHoldActive[m.midi_num]) {
                    SendKeyInput(m.key_vk, true);
                    g_ccHoldActive[m.midi_num] = true;
                }
                else if (ccCrossedDown && g_ccHoldActive[m.midi_num]) {
                    SendKeyInput(m.key_vk, false);
                    g_ccHoldActive[m.midi_num] = false;
                }
                break;
            }
        }
        
        // Macros, AI, HUD
        if (m.midi_type == 4 && isNoteOn && number == m.midi_num && m.gesture_id == 0) {
            // Feature 9: Route macros with delay tokens through JS engine
            if (m.macro_text.find("{delay:") != std::string::npos ||
                m.macro_text.find("{enter}") != std::string::npos ||
                m.macro_text.find("{tab}") != std::string::npos ||
                m.macro_text.find("{esc}") != std::string::npos ||
                m.macro_text.find("{space}") != std::string::npos ||
                m.macro_text.find("{backspace}") != std::string::npos) {
                PostToWebView({ {"type", "execute_macro"}, {"text", m.macro_text} });
            } else {
                SimulateText(m.macro_text);
            }
            // Feature 4: HUD notification
            PostToWebView({ {"type", "hud_notify"}, {"text", "Macro Executed"}, {"level", "success"} });
        }
        if (m.midi_type == 5 && isNoteOn && number == m.midi_num && m.gesture_id == 0) {
            PostToWebView({ {"type", "run_ai"}, {"prompt", m.ai_prompt} });
            SendLog("AI Prompt sent: " + m.ai_prompt);
        }
        if (m.midi_type == 3 && number == m.midi_num) {
            if (isNoteOn) PostToWebView({ {"type", "hud"}, {"active", true}, {"title", WideToUtf8(GetModifierString(m.modifiers) + GetKeyName(m.key_vk))} });
            else if (isNoteOff) PostToWebView({ {"type", "hud"}, {"active", false} });
        }
    }
}

void ResolveGesture(int midi_num, int gesture_id) {
    std::string cachedTitle = WideToUtf8(g_currentWindowTitle);
    std::string cachedApp = WideToUtf8(g_currentApp);
    std::lock_guard<std::recursive_mutex> lock(g_mappingsMutex);
    for (const auto& m : g_mappings) {
        if (!m.enabled) continue;
        if (m.midi_num != midi_num) continue;
        
        // Exact gesture match, and ignore gesture 0 here because it's handled immediately in ProcessMIDIEvent
        if (m.gesture_id != gesture_id || m.gesture_id == 0) continue;

        // Context check
        if (!m.title_pattern.empty()) {
            if (cachedTitle.find(m.title_pattern) == std::string::npos) continue;
        }
        if (!m.app_pattern.empty()) {
            if (cachedApp.find(m.app_pattern) == std::string::npos) continue;
        }

        // Execute (Simplified trigger for gesture demo)
        if (m.midi_type == 0) {
            SimulateKeyCombo(m.key_vk, m.modifiers);
            PostToWebView({ {"type", "hud_notify"}, {"text", "Gesture -> Key"}, {"level", "success"} });
        }
        else if (m.midi_type == 4) {
            // Feature 9: Route macros with delay tokens through JS engine
            if (m.macro_text.find("{delay:") != std::string::npos ||
                m.macro_text.find("{enter}") != std::string::npos ||
                m.macro_text.find("{tab}") != std::string::npos) {
                PostToWebView({ {"type", "execute_macro"}, {"text", m.macro_text} });
            } else {
                SimulateText(m.macro_text);
            }
            PostToWebView({ {"type", "hud_notify"}, {"text", "Macro Executed"}, {"level", "success"} });
        }
        else if (m.midi_type == 5) PostToWebView({ {"type", "run_ai"}, {"prompt", m.ai_prompt} });
    }
}

// ══════════════════════════════════════════
//  MIDI Port Management & Auto-Reconnect
// ══════════════════════════════════════════

void ScanMidiPorts() {
    g_ports.clear();
    RtMidiIn tempIn;
    int n = tempIn.getPortCount();
    json portsArr = json::array();
    for (int i = 0; i < n; ++i) {
        std::string name = tempIn.getPortName(i);
        g_ports.push_back(name);
        portsArr.push_back(name);
    }
    PostToWebView({ {"type", "ports"}, {"ports", portsArr} });
}

void ConnectMidi(int portIndex) {
    if (portIndex < 0 || portIndex >= (int)g_ports.size()) return;
    try {
        g_midiIn = std::make_unique<RtMidiIn>();
        g_midiIn->openPort(portIndex);
        g_midiIn->setCallback(&midiCallback);
        g_connected = true;
        g_lastConnectedPort = portIndex;
        g_lastConnectedPortName = g_ports[portIndex];
        PostToWebView({ {"type", "connected"}, {"portName", g_ports[portIndex]} });
        SendLog("Connected to: " + g_ports[portIndex]);
        SendStatus("Connected.");
        SaveConfig();
    }
    catch (RtMidiError& e) {
        SendLog("Connection failed: " + std::string(e.getMessage()));
        g_midiIn.reset();
    }
}

void DisconnectMidi() {
    // Release any sustained keys before disconnecting
    {
        std::lock_guard<std::mutex> lock(g_sustainMutex);
        for (int vk : g_sustainedVKs) {
            SendKeyInput(vk, false);
        }
        g_sustainedVKs.clear();
        g_sustainActive = false;
    }
    // Release all physically held keys
    for (int i = 0; i < 128; i++) {
        if (g_pianoPhysicalDown[i].load(std::memory_order_relaxed)) {
            g_pianoPhysicalDown[i].store(false, std::memory_order_relaxed);
        }
    }
    g_midiIn.reset();
    g_connected = false;
    PostToWebView({ {"type", "disconnected"} });
    SendLog("MIDI disconnected.");
    SendStatus("Disconnected.");
}

void TryAutoReconnect() {
    if (g_connected || !g_autoReconnect || g_lastConnectedPortName.empty()) return;
    RtMidiIn tempIn;
    int n = tempIn.getPortCount();
    for (int i = 0; i < n; i++) {
        if (tempIn.getPortName(i) == g_lastConnectedPortName) {
            ScanMidiPorts();
            ConnectMidi(i);
            if (g_connected) {
                SendLog("Auto-reconnected to: " + g_lastConnectedPortName);
                if (!g_lastProfilePath.empty()) {
                    LoadMappings(g_lastProfilePath);
                    SendLog("Auto-loaded last profile.");
                }
            }
            return;
        }
    }
}
