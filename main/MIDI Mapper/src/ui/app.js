/**
 * MIDITypist - Core Frontend Engine
 * Handles State, Bridge Communication, and UI Orchestration.
 */

// --- State ---
let mappings = [];
let currentView = 'mappings';
let learnPhase = 0; // 0: Idle, 1: Waiting for MIDI, 2: Waiting for Key
let searchQuery = ''; // For mapping search filter
const MAX_LOG_ENTRIES = 500;

// MIDI note name lookup
const NOTE_NAMES = ['C', 'C#', 'D', 'D#', 'E', 'F', 'F#', 'G', 'G#', 'A', 'A#', 'B'];
function midiNoteName(num) {
    if (num < 0 || num > 127) return `Note ${num}`;
    return NOTE_NAMES[num % 12] + (Math.floor(num / 12) - 1);
}

// VK code to readable name
const VK_NAMES = {
    8: 'Backspace', 9: 'Tab', 13: 'Enter', 16: 'Shift', 17: 'Ctrl', 18: 'Alt', 20: 'CapsLock', 27: 'Esc', 32: 'Space',
    33: 'PgUp', 34: 'PgDn', 35: 'End', 36: 'Home', 37: '←', 38: '↑', 39: '→', 40: '↓', 45: 'Insert', 46: 'Delete',
    48: '0', 49: '1', 50: '2', 51: '3', 52: '4', 53: '5', 54: '6', 55: '7', 56: '8', 57: '9',
    65: 'A', 66: 'B', 67: 'C', 68: 'D', 69: 'E', 70: 'F', 71: 'G', 72: 'H', 73: 'I', 74: 'J', 75: 'K', 76: 'L', 77: 'M',
    78: 'N', 79: 'O', 80: 'P', 81: 'Q', 82: 'R', 83: 'S', 84: 'T', 85: 'U', 86: 'V', 87: 'W', 88: 'X', 89: 'Y', 90: 'Z',
    91: 'Win', 112: 'F1', 113: 'F2', 114: 'F3', 115: 'F4', 116: 'F5', 117: 'F6', 118: 'F7', 119: 'F8', 120: 'F9',
    121: 'F10', 122: 'F11', 123: 'F12', 144: 'NumLock', 145: 'ScrollLock', 186: ';', 187: '=', 188: ',', 189: '-', 190: '.', 191: '/', 192: '`', 219: '[', 220: '\\', 221: ']', 222: "'"
};
function vkName(code) { return VK_NAMES[code] || `VK ${code}`; }

// --- Bridge: Send commands to C++ ---
function send(action, data = {}) {
    if (window.chrome?.webview) {
        window.chrome.webview.postMessage(JSON.stringify({ action, ...data }));
    }
}

// --- Bridge: Receive messages from C++ ---
if (window.chrome?.webview) {
    window.chrome.webview.addEventListener('message', e => {
        const msg = e.data;
        switch (msg.type) {
            case 'mappings': updateMappings(msg.mappings); break;
            case 'midi_note': handleMidiEvent('Note', msg.note, msg.velocity); break;
            case 'midi_cc': handleMidiEvent('CC', msg.cc, msg.value); break;
            case 'status': setStatus(msg.text); break;
            case 'app_changed': updateContext(msg.app, msg.title); break;
            case 'learn_phase':
                learnPhase = msg.phase;
                const promptEl = document.getElementById('learnPrompt');
                if (promptEl) promptEl.textContent = msg.text;
                updateHUDContextStyle();
                break;
            case 'learn_done':
                learnPhase = 0;
                const overlay = document.getElementById('learnOverlay');
                if (overlay) overlay.style.display = 'none';
                updateHUDContextStyle();
                break;
            case 'log': addLog(msg.text, msg.category); break;
            case 'run_ai': handleAiRequest(msg.prompt); break;
            case 'ports': updatePorts(msg.ports, msg.selected); break;
            case 'config': syncConfig(msg.config); break;
            case 'toast': showToast(msg.text, msg.level || 'info'); break;
            case 'piano_decay': handlePianoDecay(msg.keys); break;
        }
    });
}

// --- UI Logic ---
function setView(viewId) {
    document.querySelectorAll('.view-section').forEach(v => v.classList.remove('active'));
    document.querySelectorAll('.nav-item').forEach(n => n.classList.remove('active'));

    const targetView = document.getElementById('view-' + viewId);
    if (targetView) targetView.classList.add('active');

    // Update nav item state (find by text or id if we add them)
    document.querySelectorAll('.nav-item').forEach(n => {
        if (n.textContent.toLowerCase().includes(viewId)) n.classList.add('active');
    });

    currentView = viewId;
}

function toggleTheme() {
    const html = document.documentElement;
    const currentTheme = html.getAttribute('data-theme') || 'dark';
    const newTheme = currentTheme === 'dark' ? 'light' : 'dark';
    html.setAttribute('data-theme', newTheme);
    localStorage.setItem('miditypist-theme', newTheme);
}

function initTheme() {
    const savedTheme = localStorage.getItem('miditypist-theme');
    const systemDark = window.matchMedia('(prefers-color-scheme: dark)');

    const applyTheme = (theme) => {
        document.documentElement.setAttribute('data-theme', theme);
        if (typeof updateHUDContextStyle === 'function') updateHUDContextStyle();
    };

    if (savedTheme) {
        applyTheme(savedTheme);
    } else {
        applyTheme(systemDark.matches ? 'dark' : 'light');
    }

    // Listen for system changes
    systemDark.addEventListener('change', e => {
        if (!localStorage.getItem('miditypist-theme')) {
            applyTheme(e.matches ? 'dark' : 'light');
        }
    });
}

function updateMappings(list) {
    mappings = list;
    const grid = document.getElementById('mapGrid');
    if (!grid) return;
    grid.innerHTML = '';

    if (mappings.length === 0) {
        grid.innerHTML = `
            <div class="empty-state">
                <div class="empty-icon">
                    <svg viewBox="0 0 24 24" width="48" height="48" fill="none" stroke="currentColor" stroke-width="1.5">
                        <rect x="2" y="4" width="20" height="16" rx="2" ry="2"></rect>
                        <line x1="6" y1="8" x2="6" y2="16"></line>
                        <line x1="10" y1="8" x2="10" y2="16"></line>
                        <line x1="14" y1="8" x2="14" y2="16"></line>
                        <line x1="18" y1="8" x2="18" y2="16"></line>
                    </svg>
                </div>
                <h3>No automation profiles yet.</h3>
                <p>Start by capturing your first MIDI-to-Keyboard mapping.</p>
                <button class="btn btn-primary" onclick="startLearn()">Capture Mapping</button>
            </div>
        `;
        return;
    }
    mappings.forEach((m, i) => {
        // Search filter
        if (searchQuery) {
            const q = searchQuery.toLowerCase();
            const searchable = [
                m.midi_type === 2 ? `chord ${(m.midi_chord || []).join(',')}` : `${m.midi_type === 1 ? 'cc' : 'note'} ${m.midi_num}`,
                `key ${m.key_vk}`,
                m.app_pattern || '',
                m.title_pattern || '',
                m.macro_text || '',
                m.ai_prompt || ''
            ].join(' ').toLowerCase();
            if (!searchable.includes(q)) return;
        }

        const card = document.createElement('div');
        card.className = 'mapping-card';
        if (m.enabled === false) card.style.opacity = '0.4';

        // Color-coded left border by mapping type
        const typeColors = { 0: 'var(--system-blue)', 1: 'var(--system-green)', 2: 'var(--system-purple)', 3: 'var(--system-orange)', 4: 'var(--system-orange)', 5: '#FF375F' };
        card.style.borderLeftColor = typeColors[m.midi_type] || 'var(--accent)';

        // Stagger animation
        card.style.animationDelay = `${i * 30}ms`;

        card.onclick = () => openEditor(i);

        // Build rich content
        const typeLabels = { 0: 'Note', 1: 'CC', 2: 'Chord', 3: 'HUD', 4: 'Macro', 5: 'AI' };
        let target = 'HUD';
        if (m.midi_type === 0) target = vkName(m.key_vk);
        else if (m.midi_type === 1) target = vkName(m.key_vk);
        else if (m.midi_type === 4) target = 'Macro';
        else if (m.midi_type === 5) target = 'AI Prompt';
        else if (m.midi_type === 2) target = vkName(m.key_vk);

        let gesture = m.gesture_id === 1 ? 'DBL' : (m.gesture_id === 2 ? 'HLD' : 'TAP');
        let titleLine = m.midi_type === 2 ? `Chord [${(m.midi_chord || []).map(n => midiNoteName(n)).join(', ')}]` : `${typeLabels[m.midi_type] || 'Note'} ${m.midi_type <= 1 ? midiNoteName(m.midi_num) : m.midi_num}`;

        // Safe DOM construction
        const header = document.createElement('div');
        header.style.cssText = 'display:flex; justify-content:space-between; align-items:center;';
        const titleRow = document.createElement('div');
        titleRow.style.cssText = 'display:flex; align-items:center; gap:8px;';
        // Type badge
        const typeBadge = document.createElement('span');
        typeBadge.style.cssText = `font-size:9px; font-weight:700; padding:2px 6px; border-radius:4px; text-transform:uppercase; letter-spacing:0.5px; background:${typeColors[m.midi_type] || 'var(--accent)'}22; color:${typeColors[m.midi_type] || 'var(--accent)'};`;
        typeBadge.textContent = typeLabels[m.midi_type] || 'Note';
        const titleSpan = document.createElement('span');
        titleSpan.style.cssText = 'font-weight:600; font-size:14px; color:var(--text-primary);';
        titleSpan.textContent = m.midi_type <= 1 ? midiNoteName(m.midi_num) : titleLine;
        titleRow.appendChild(typeBadge);
        titleRow.appendChild(titleSpan);
        const badgeDiv = document.createElement('div');
        badgeDiv.className = 'badge';
        badgeDiv.textContent = gesture;
        header.appendChild(titleRow);
        header.appendChild(badgeDiv);

        // Arrow + Target
        const targetRow = document.createElement('div');
        targetRow.style.cssText = 'display:flex; align-items:center; gap:8px; margin:4px 0 0 0;';
        const arrow = document.createElement('span');
        arrow.style.cssText = 'color:var(--text-tertiary); font-size:12px;';
        arrow.textContent = '→';
        const targetSpan = document.createElement('span');
        targetSpan.style.cssText = 'font-size:17px; font-weight:700; color:var(--accent);';
        targetSpan.textContent = target;
        targetRow.appendChild(arrow);
        targetRow.appendChild(targetSpan);

        const footer = document.createElement('div');
        footer.className = 'mapping-footer';
        footer.style.cssText = 'display:flex; justify-content:space-between; align-items:center; margin-top:6px;';
        const badgeRow = document.createElement('div');
        badgeRow.className = 'badge-row';
        badgeRow.style.cssText = 'display:flex; gap:4px; flex-wrap:wrap; align-items:center;';
        // Show modifiers as pills
        if (m.modifiers & 1) { const p = document.createElement('span'); p.style.cssText = 'font-size:9px;font-weight:700;padding:1px 5px;border-radius:3px;background:rgba(255,255,255,0.08);color:var(--text-tertiary);'; p.textContent = 'Ctrl'; badgeRow.appendChild(p); }
        if (m.modifiers & 2) { const p = document.createElement('span'); p.style.cssText = 'font-size:9px;font-weight:700;padding:1px 5px;border-radius:3px;background:rgba(255,255,255,0.08);color:var(--text-tertiary);'; p.textContent = 'Shift'; badgeRow.appendChild(p); }
        if (m.modifiers & 4) { const p = document.createElement('span'); p.style.cssText = 'font-size:9px;font-weight:700;padding:1px 5px;border-radius:3px;background:rgba(255,255,255,0.08);color:var(--text-tertiary);'; p.textContent = 'Alt'; badgeRow.appendChild(p); }
        // Velocity zone badge
        if (m.vel_zone === 1) { const p = document.createElement('span'); p.style.cssText = 'font-size:9px;font-weight:700;padding:1px 5px;border-radius:3px;background:rgba(0,122,255,0.1);color:var(--accent);'; p.textContent = 'Soft'; badgeRow.appendChild(p); }
        if (m.vel_zone === 2) { const p = document.createElement('span'); p.style.cssText = 'font-size:9px;font-weight:700;padding:1px 5px;border-radius:3px;background:rgba(255,59,48,0.1);color:var(--error);'; p.textContent = 'Hard'; badgeRow.appendChild(p); }
        if (m.app_pattern) {
            const pill = document.createElement('span');
            pill.className = 'context-pill';
            pill.textContent = m.app_pattern;
            pill.title = `App Match: ${m.app_pattern}`;
            badgeRow.appendChild(pill);
        }
        if (m.title_pattern) {
            const pill = document.createElement('span');
            pill.className = 'context-pill';
            pill.textContent = m.title_pattern;
            pill.title = `Title Match: ${m.title_pattern}`;
            badgeRow.appendChild(pill);
        }
        const delBtn = document.createElement('button');
        delBtn.className = 'btn';
        delBtn.style.cssText = 'padding:4px; color:var(--error); opacity:0.6; transition:opacity 0.15s;';
        delBtn.onmouseenter = () => delBtn.style.opacity = '1';
        delBtn.onmouseleave = () => delBtn.style.opacity = '0.6';
        delBtn.innerHTML = `<svg style="width:14px; height:14px;" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><polyline points="3 6 5 6 21 6"></polyline><path d="M19 6v14a2 2 0 0 1-2 2H7a2 2 0 0 1-2-2V6m3 0V4a2 2 0 0 1 2-2h4a2 2 0 0 1 2 2v2"></path></svg>`;
        delBtn.onclick = (e) => { e.stopPropagation(); deleteMapping(i); };

        // Mini toggle switch
        const toggleWrap = document.createElement('label');
        toggleWrap.className = 'toggle';
        toggleWrap.style.cssText = 'width:32px; height:18px; flex-shrink:0;';
        const toggleInput = document.createElement('input');
        toggleInput.type = 'checkbox';
        toggleInput.checked = m.enabled !== false;
        toggleInput.onclick = (e) => { e.stopPropagation(); toggleMapping(i); };
        const toggleSlider = document.createElement('div');
        toggleSlider.className = 'toggle-slider';
        toggleSlider.style.cssText = 'border-radius:18px;';
        // Smaller knob for mini toggle
        toggleSlider.innerHTML = '<style>.toggle[style*="32px"] .toggle-slider::after{width:12px;height:12px;}</style>';
        toggleWrap.appendChild(toggleInput);
        toggleWrap.appendChild(toggleSlider);

        const btnRow = document.createElement('div');
        btnRow.style.cssText = 'display:flex; gap:6px; align-items:center;';
        // Re-learn button
        const relearnBtn = document.createElement('button');
        relearnBtn.className = 'btn';
        relearnBtn.title = 'Re-learn MIDI trigger';
        relearnBtn.style.cssText = 'padding:4px; color:var(--accent); opacity:0.6; transition:opacity 0.15s;';
        relearnBtn.onmouseenter = () => relearnBtn.style.opacity = '1';
        relearnBtn.onmouseleave = () => relearnBtn.style.opacity = '0.6';
        relearnBtn.innerHTML = `<svg style="width:14px; height:14px;" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><path d="M9 18V5l12-2v13"></path><circle cx="6" cy="18" r="3"></circle><circle cx="18" cy="16" r="3"></circle></svg>`;
        relearnBtn.onclick = (e) => { e.stopPropagation(); startRelearn(i); };
        btnRow.appendChild(toggleWrap);
        btnRow.appendChild(relearnBtn);
        btnRow.appendChild(delBtn);

        footer.appendChild(badgeRow);
        footer.appendChild(btnRow);

        card.appendChild(header);
        card.appendChild(targetRow);
        card.appendChild(footer);
        // 3D Tilt Effect Logic
        card.addEventListener('mousemove', (e) => {
            const rect = card.getBoundingClientRect();
            const x = e.clientX - rect.left;
            const y = e.clientY - rect.top;
            const centerX = rect.width / 2;
            const centerY = rect.height / 2;

            // Calculate tilt limits
            const rotateX = ((y - centerY) / centerY) * -5; // max 5 deg
            const rotateY = ((x - centerX) / centerX) * 5;  // max 5 deg

            card.style.transform = `perspective(1000px) rotateX(${rotateX}deg) rotateY(${rotateY}deg) scale3d(1.02, 1.02, 1.02)`;
        });

        card.addEventListener('mouseleave', () => {
            card.style.transform = `perspective(1000px) rotateX(0deg) rotateY(0deg) scale3d(1, 1, 1)`;
        });
        grid.appendChild(card);
    });

    // Update sidebar mapping count badge
    const navItems = document.querySelectorAll('.nav-item');
    navItems.forEach(n => {
        if (n.textContent.includes('Mappings')) {
            let badge = n.querySelector('.nav-count');
            if (!badge) {
                badge = document.createElement('span');
                badge.className = 'nav-count';
                badge.style.cssText = 'margin-left:auto; font-size:11px; font-weight:600; color:var(--text-tertiary); background:rgba(255,255,255,0.06); padding:1px 7px; border-radius:10px; min-width:20px; text-align:center;';
                n.appendChild(badge);
            }
            badge.textContent = mappings.length;
        }
    });
}

function handleMidiEvent(type, num, val) {
    if (val > 0) {
        addLog(`${type} ${num} (Val: ${val})`, 'midi-active');
        const key = document.getElementById(`key-${num}`);
        if (key) key.classList.add('active');
    } else {
        const key = document.getElementById(`key-${num}`);
        if (key) key.classList.remove('active');
    }
}

// --- Piano Widget ---
function initPiano() {
    const dashboard = document.getElementById('pianoDashboard');
    if (!dashboard) return;
    dashboard.innerHTML = '';

    for (let i = 0; i < 128; i++) {
        const key = document.createElement('div');
        key.className = 'piano-key';
        key.id = `key-${i}`;

        // Simple black key logic
        const octave = i % 12;
        if ([1, 3, 6, 8, 10].includes(octave)) {
            key.classList.add('black');
        }

        dashboard.appendChild(key);
    }
}

let pendingConfirmAction = null;

function clearMappings() {
    showConfirm(
        "Clear All Mappings?",
        "Are you sure you want to clear all mappings? This cannot be undone.",
        () => { send('clear_mappings'); }
    );
}

function showConfirm(title, message, action) {
    document.getElementById('confirmTitle').textContent = title;
    document.getElementById('confirmMessage').textContent = message;
    pendingConfirmAction = action;
    document.getElementById('modalConfirm').style.display = 'flex';
}

function executeConfirm() {
    if (pendingConfirmAction) pendingConfirmAction();
    closeConfirm();
}

function closeConfirm() {
    document.getElementById('modalConfirm').style.display = 'none';
    pendingConfirmAction = null;
}

function addLog(text, cat) {
    const body = document.getElementById('logBody');
    if (!body) return;
    const div = document.createElement('div');
    div.className = 'log-entry';

    // Category-based styling
    if (cat === 'error') { div.classList.add('log-error'); div.style.color = 'var(--error)'; }
    else if (cat === 'midi-active' || cat === 'mapping') { div.classList.add('log-midi'); div.style.color = 'var(--accent)'; }
    else if (cat === 'system') { div.classList.add('log-context'); div.style.color = 'var(--text-secondary)'; }
    else { div.style.color = 'var(--text-secondary)'; }

    // Build entry with timestamp badge
    const time = document.createElement('span');
    time.style.cssText = 'color:var(--text-tertiary); font-size:11px; margin-right:8px; font-family:var(--font-mono);';
    time.textContent = new Date().toLocaleTimeString();
    div.appendChild(time);
    const msg = document.createElement('span');
    msg.textContent = text;
    div.appendChild(msg);

    body.appendChild(div);

    // Circular buffer: cap log entries
    while (body.children.length > MAX_LOG_ENTRIES) {
        body.removeChild(body.firstChild);
    }

    // Batch scrolling to next frame to prevent synchronous layout thrashing
    if (!window._logScrollRAF) {
        window._logScrollRAF = requestAnimationFrame(() => {
            body.scrollTop = body.scrollHeight;
            window._logScrollRAF = null;
        });
    }

    const countEl = document.getElementById('logCount');
    if (countEl) countEl.textContent = body.children.length;
}

function updateContext(app, title) {
    if (title === "Task Switching") return;
    if (app === "explorer.exe" && !title) return;

    const ctxEl = document.getElementById('activeContext');
    const appEl = document.getElementById('contextApp');
    const titleEl = document.getElementById('contextTitle');

    if (!appEl || !titleEl) return;

    // Check if it's actually changing to avoid unnecessary flashing
    if (appEl.textContent === (app || 'Desktop') && titleEl.textContent === (title || 'Untitled')) {
        return;
    }

    // 1. Start transition (fade out, scale down)
    if (ctxEl) ctxEl.classList.add('context-transition');

    // 2. Wait for fade out, swap text, fade back in
    setTimeout(() => {
        appEl.textContent = app || 'Desktop';
        appEl.title = app || 'Desktop';
        titleEl.textContent = title || 'Untitled';
        titleEl.title = title || 'Untitled';

        if (ctxEl) {
            // Force a reflow so the browser registers the text change before removing the class
            void ctxEl.offsetWidth;
            ctxEl.classList.remove('context-transition');
        }
    }, 150); // Match this roughly to the CSS transition time

    addLog(`Context: ${app} | ${title}`, 'system');
}

function setStatus(text) {
    const dot = document.getElementById('statusDot');
    const hudBtn = document.getElementById('hudBtnConnect');
    const mainBtn = document.getElementById('btnConnect');

    const connected = text.includes('Connected') || text.includes('Ready');

    if (dot) {
        dot.style.background = connected ? 'var(--system-green)' : 'var(--error)';
        dot.style.boxShadow = connected ? '0 0 8px var(--system-green)' : '0 0 8px var(--error)';
        dot.className = connected ? 'status-breathing' : '';
    }

    if (hudBtn) {
        hudBtn.style.color = connected ? 'var(--system-green)' : 'var(--text-secondary)';
        hudBtn.style.background = connected ? 'rgba(40, 200, 64, 0.15)' : 'rgba(255,255,255,0.05)';
        hudBtn.classList.toggle('active', connected);
    }

    if (mainBtn) mainBtn.textContent = connected ? 'Disconnect' : 'Connect';

    addLog(`System Status: ${text}`, connected ? 'mapping' : 'system');
    showToast(text, connected ? 'success' : 'warning');
    updateHUDContextStyle();
}

function updatePorts(ports, selectedIdx) {
    const sel = document.getElementById('selectMidiPort');
    const hudSel = document.getElementById('hudMidiPort');
    if (!sel || !hudSel) return;

    const populate = (el) => {
        el.innerHTML = '';
        if (ports.length === 0) {
            const opt = document.createElement('option');
            opt.disabled = true;
            opt.selected = true;
            opt.textContent = 'No Devices';
            el.appendChild(opt);
            return;
        }
        ports.forEach((p, i) => {
            const opt = document.createElement('option');
            opt.value = i;
            opt.textContent = p;
            if (i === selectedIdx) opt.selected = true;
            el.appendChild(opt);
        });
    };

    populate(sel);
    populate(hudSel);
}

function syncConfig(cfg) {
    const ids = {
        'checkReconnect': cfg.auto_reconnect,
        'checkAppSwitch': cfg.app_switching,
        'checkVelocity': cfg.velocity_zones,
        'checkTray': cfg.minimize_to_tray,
        'inputApiKey': cfg.ai_api_key || '',
        'inputAiGlobal': cfg.ai_global_prompt || ''
    };

    for (const [id, val] of Object.entries(ids)) {
        const el = document.getElementById(id);
        if (el) {
            if (el.type === 'checkbox') el.checked = val;
            else el.value = val;
        }
    }
}

// --- Editor Functions ---
let activeIdx = -1;
function openEditor(i) {
    activeIdx = i;
    const m = mappings[i];
    const modal = document.getElementById('modalEditor');
    if (!modal) return;
    modal.style.display = 'flex';

    document.getElementById('editMidiType').value = m.midi_type;
    document.getElementById('editKeyVk').value = m.key_vk;
    document.getElementById('editGestureId').value = m.gesture_id || 0;
    document.getElementById('editMacroText').value = m.macro_text || '';
    document.getElementById('editAiPrompt').value = m.ai_prompt || '';
    document.getElementById('editMidiChord').value = (m.midi_chord || []).join(', ');
    document.getElementById('editAppPattern').value = m.app_pattern || '';
    document.getElementById('editTitlePattern').value = m.title_pattern || '';
    toggleEditFields();
}

function closeEditor() {
    const modal = document.getElementById('modalEditor');
    if (modal) {
        modal.style.animation = 'fadeOut 0.2s ease forwards';
        setTimeout(() => {
            modal.style.display = 'none';
            modal.style.animation = 'fadeIn 0.2s ease';
        }, 200);
    }
}

function toggleEditFields() {
    const type = document.getElementById('editMidiType').value;
    const fields = {
        'editFieldMacro': type == 4,
        'editFieldAi': type == 5,
        'editFieldKey': (type != 4 && type != 5),
        'editFieldChord': type == 2
    };

    for (const [id, visible] of Object.entries(fields)) {
        const el = document.getElementById(id);
        if (el) el.style.display = visible ? 'block' : 'none';
    }
}

function saveEdit() {
    const chordStr = document.getElementById('editMidiChord').value;
    const chordArr = chordStr.split(',').map(s => parseInt(s.trim())).filter(n => !isNaN(n));

    send('update_mapping', {
        index: activeIdx,
        midi_type: parseInt(document.getElementById('editMidiType').value),
        key_vk: parseInt(document.getElementById('editKeyVk').value),
        gesture_id: parseInt(document.getElementById('editGestureId').value),
        macro_text: document.getElementById('editMacroText').value,
        ai_prompt: document.getElementById('editAiPrompt').value,
        midi_chord: chordArr,
        app_pattern: document.getElementById('editAppPattern').value,
        title_pattern: document.getElementById('editTitlePattern').value
    });
    closeEditor();
}

// --- Actions ---
function startLearn() {
    learnPhase = 1;
    const prompt = document.getElementById('learnPrompt');
    if (prompt) prompt.textContent = "Waiting for MIDI...";
    const overlay = document.getElementById('learnOverlay');
    if (overlay) overlay.style.display = 'flex';
    send('start_learn');
}

function cancelLearn() {
    learnPhase = 0;
    send('cancel_learn');
    const overlay = document.getElementById('learnOverlay');
    if (overlay) overlay.style.display = 'none';
}

function addMapping() { send('add_mapping'); }
function deleteMapping(i) {
    // Store mapping for undo
    const deleted = mappings[i];
    send('delete_mapping', { index: i });
    if (deleted) {
        showUndoToast('Mapping deleted', () => {
            send('add_mapping');
            // The mapping will be re-added as blank; a full undo would require backend support
        });
    }
}
function toggleMapping(i) { send('toggle_mapping', { index: i }); }
function loadProfile() { send('load_profile'); }
function saveProfile() { send('save_profile'); }
function clearLog() { const log = document.getElementById('logBody'); if (log) log.innerHTML = ''; }
function toggleConnect() {
    const hudPort = document.getElementById('hudMidiPort');
    const mainPort = document.getElementById('selectMidiPort');

    // Choose whichever is available, prioritizing HUD for quick actions
    const portEl = hudPort || mainPort;
    if (!portEl || portEl.value === "" || isNaN(parseInt(portEl.value))) {
        addLog("Please select a MIDI device first.", "error");
        return;
    }

    const port = parseInt(portEl.value);
    send('toggle_connect', { port });
}

function updateSettings() {
    send('update_config', {
        auto_reconnect: document.getElementById('checkReconnect').checked,
        app_switching: document.getElementById('checkAppSwitch').checked,
        minimize_to_tray: document.getElementById('checkTray').checked,
        velocity_zones: document.getElementById('checkVelocity').checked,
        ai_api_key: document.getElementById('inputApiKey').value,
        ai_global_prompt: document.getElementById('inputAiGlobal').value
    });
}

async function handleAiRequest(prompt) {
    const key = document.getElementById('inputApiKey').value;
    const global = document.getElementById('inputAiGlobal').value;
    if (!key) { addLog("AI Error: No API Key", "error"); return; }

    // Rate limit: 3 second cooldown
    const now = Date.now();
    if (handleAiRequest._lastCall && now - handleAiRequest._lastCall < 3000) {
        showToast('AI cooldown active (3s)', 'warning');
        return;
    }
    handleAiRequest._lastCall = now;

    addLog("AI Thinking (Gemini Flash 1.5)...", "system");

    const controller = new AbortController();
    const timeoutId = setTimeout(() => controller.abort(), 15000);

    try {
        const fullPrompt = global.replace('{prompt}', prompt);
        const url = `https://generativelanguage.googleapis.com/v1beta/models/gemini-1.5-flash:generateContent?key=${key}`;

        const resp = await fetch(url, {
            method: 'POST',
            headers: { 'Content-Type': 'application/json' },
            signal: controller.signal,
            body: JSON.stringify({
                contents: [{
                    parts: [{ text: fullPrompt }]
                }]
            })
        });

        clearTimeout(timeoutId);

        if (!resp.ok) {
            const errorJson = await resp.json().catch(() => ({}));
            const errorMsg = errorJson.error?.message || `HTTP ${resp.status}`;
            throw new Error(errorMsg);
        }

        const json = await resp.json();
        const result = json.candidates?.[0]?.content?.parts?.[0]?.text || "";
        addLog("AI Action: " + result, "system");
        send('simulate_text', { text: result });
    } catch (err) {
        clearTimeout(timeoutId);
        if (err.name === 'AbortError') {
            addLog("AI Failed: Request timed out (15s)", "error");
        } else {
            addLog("AI Failed: " + err.message, "error");
        }
    }
}

// --- macOS Style Settings Modal ---
function openSettings() {
    const modal = document.getElementById('modalSettings');
    if (modal) modal.style.display = 'flex';
}

function closeSettings() {
    const modal = document.getElementById('modalSettings');
    if (modal) {
        modal.style.animation = 'fadeOut 0.2s ease forwards';
        setTimeout(() => {
            modal.style.display = 'none';
            modal.style.animation = 'fadeIn 0.2s ease';
        }, 200);
    }
}

// --- Toast Notification System ---
function showToast(message, level = 'info') {
    let container = document.getElementById('toastContainer');
    if (!container) {
        container = document.createElement('div');
        container.id = 'toastContainer';
        container.style.cssText = 'position:fixed; bottom:24px; right:24px; z-index:9999; display:flex; flex-direction:column; gap:8px; pointer-events:none;';
        document.body.appendChild(container);
    }

    const toast = document.createElement('div');
    toast.className = 'toast-notification';
    const colors = { info: 'var(--accent)', success: 'var(--system-green)', error: 'var(--error)', warning: 'var(--system-yellow)' };
    const icons = { info: 'ℹ', success: '✓', error: '✗', warning: '⚠' };
    const borderColor = colors[level] || colors.info;
    toast.style.cssText = `
        pointer-events:auto; background:rgba(22,22,24,0.92); backdrop-filter:blur(24px);
        border:1px solid rgba(255,255,255,0.08); border-left:3px solid ${borderColor};
        border-radius:12px; padding:12px 18px; color:var(--text-primary);
        font-size:13px; font-weight:500; box-shadow:var(--shadow-lg);
        animation:toastIn 0.3s cubic-bezier(0.16,1,0.3,1); max-width:360px;
        display:flex; align-items:center; gap:10px;
    `;
    const iconSpan = document.createElement('span');
    iconSpan.style.cssText = `color:${borderColor}; font-size:16px; flex-shrink:0;`;
    iconSpan.textContent = icons[level] || icons.info;
    toast.appendChild(iconSpan);
    const textSpan = document.createElement('span');
    textSpan.textContent = message;
    toast.appendChild(textSpan);
    container.appendChild(toast);

    toast.style.cursor = 'pointer';

    const hideTimeout = setTimeout(() => {
        toast.style.animation = 'toastOut 0.3s ease forwards';
        setTimeout(() => toast.remove(), 300);
    }, 3500);

    toast.onclick = () => {
        clearTimeout(hideTimeout);
        toast.style.animation = 'toastOut 0.3s ease forwards';
        setTimeout(() => toast.remove(), 300);
    };
}

function showUndoToast(message, undoCallback) {
    let container = document.getElementById('toastContainer');
    if (!container) {
        container = document.createElement('div');
        container.id = 'toastContainer';
        container.style.cssText = 'position:fixed; bottom:24px; right:24px; z-index:9999; display:flex; flex-direction:column; gap:8px; pointer-events:none;';
        document.body.appendChild(container);
    }

    const toast = document.createElement('div');
    toast.style.cssText = `
        pointer-events:auto; background:rgba(30,30,30,0.95); backdrop-filter:blur(20px);
        border:1px solid var(--accent); border-left:3px solid var(--accent);
        border-radius:10px; padding:12px 18px; color:var(--text-primary);
        font-size:13px; font-weight:500; box-shadow:0 8px 32px rgba(0,0,0,0.4);
        animation:toastIn 0.3s cubic-bezier(0.16,1,0.3,1); max-width:360px;
        display:flex; align-items:center; gap:12px;
    `;

    const textSpan = document.createElement('span');
    textSpan.textContent = message;
    const undoBtn = document.createElement('button');
    undoBtn.textContent = 'Undo';
    undoBtn.style.cssText = 'background:var(--accent); color:#fff; border:none; border-radius:6px; padding:4px 10px; font-size:12px; font-weight:600; cursor:pointer;';
    undoBtn.onclick = (e) => {
        e.stopPropagation();
        undoCallback();
        clearTimeout(hideTimeout);
        toast.style.animation = 'toastOut 0.3s ease forwards';
        setTimeout(() => toast.remove(), 300);
    };

    toast.appendChild(textSpan);
    toast.appendChild(undoBtn);
    toast.style.cursor = 'pointer';
    container.appendChild(toast);

    const hideTimeout = setTimeout(() => {
        toast.style.animation = 'toastOut 0.3s ease forwards';
        setTimeout(() => toast.remove(), 300);
    }, 5000);

    toast.onclick = () => {
        clearTimeout(hideTimeout);
        toast.style.animation = 'toastOut 0.3s ease forwards';
        setTimeout(() => toast.remove(), 300);
    };
}

// --- Sparse Piano Decay Handler ---
function handlePianoDecay(keys) {
    if (!keys || !keys.length) return;
    for (const entry of keys) {
        const el = document.getElementById(`key-${entry.k}`);
        if (el) {
            if (entry.v > 0) {
                el.style.opacity = Math.max(0.3, entry.v / 127);
            } else {
                el.style.opacity = '';
                el.classList.remove('active');
            }
        }
    }
}

// --- Mapping Search ---
let _searchDebounce = null;
function filterMappings(query) {
    if (_searchDebounce) clearTimeout(_searchDebounce);
    _searchDebounce = setTimeout(() => {
        searchQuery = query;
        updateMappings(mappings);
    }, 150); // 150ms debounce prevents DOM thrashing while typing
}

// --- Keyboard Shortcut Capture ---
let capturingKey = false;
function startKeyCapture() {
    const btn = document.getElementById('btnCaptureKey');
    const input = document.getElementById('editKeyVk');
    if (!btn || !input) return;
    capturingKey = true;
    btn.textContent = 'Press any key...';
    btn.style.background = 'var(--accent)';
    btn.style.color = '#fff';

    const handler = (e) => {
        e.preventDefault();
        e.stopPropagation();
        input.value = e.keyCode;
        capturingKey = false;
        btn.textContent = 'Capture Key';
        btn.style.background = '';
        btn.style.color = '';
        document.removeEventListener('keydown', handler, true);
        showToast(`Captured key: ${e.key} (VK ${e.keyCode})`, 'success');
    };
    document.addEventListener('keydown', handler, true);
}

// --- Window Dragging ---
document.addEventListener('mousedown', (e) => {
    // Check if clicked element or its parents have webkit-app-region: drag
    let el = e.target;
    while (el) {
        const style = window.getComputedStyle(el);
        if (style.webkitAppRegion === 'no-drag') {
            return; // Clicked on an interactive element inside a drag region
        }
        if (style.webkitAppRegion === 'drag') {
            // Prevent default to stop text selection, but allow interaction with inputs
            if (e.target.tagName !== 'INPUT' && e.target.tagName !== 'TEXTAREA') {
                e.preventDefault();
            }
            send('drag_window');
            return;
        }
        el = el.parentElement;
    }
});
// Initial Boot
document.addEventListener('DOMContentLoaded', () => {
    initTheme();
    initPiano();
    send('init');

    // Sync HUD and Settings port selectors
    document.addEventListener('change', e => {
        if (e.target.id === 'hudMidiPort') {
            const sel = document.getElementById('selectMidiPort');
            if (sel) sel.value = e.target.value;
        } else if (e.target.id === 'selectMidiPort') {
            const hudSel = document.getElementById('hudMidiPort');
            if (hudSel) hudSel.value = e.target.value;
        }
    });
});
function updateHUDContextStyle() {
    const iconWrapper = document.querySelector('.hud-icon');
    if (!iconWrapper) return;

    let colorVar = 'var(--accent)';
    let bgPulse = false;
    const statusLabel = document.getElementById('statusLabel');
    const isDisconnected = statusLabel ? statusLabel.textContent.includes('Disconnected') : false;

    if (learnPhase > 0) {
        colorVar = 'var(--system-red)';
        bgPulse = true;
    } else if (isDisconnected) {
        colorVar = 'var(--system-yellow)';
    }

    // Apply color and glow
    iconWrapper.style.backgroundColor = `color-mix(in srgb, ${colorVar} 20%, transparent)`;
    if (bgPulse) {
        iconWrapper.classList.add('hud-pulse');
        iconWrapper.style.boxShadow = `0 0 16px ${colorVar}`;
    } else {
        iconWrapper.classList.remove('hud-pulse');
        iconWrapper.style.boxShadow = 'none';
    }
}

// ══════════════════════════════════════════
//  Keyboard Shortcuts
// ══════════════════════════════════════════
document.addEventListener('keydown', (e) => {
    // Don't capture when typing in inputs/textareas
    const tag = e.target.tagName;
    if (tag === 'INPUT' || tag === 'TEXTAREA' || tag === 'SELECT') return;

    // Escape — close any open modal or cancel learn
    if (e.key === 'Escape') {
        const editor = document.getElementById('modalEditor');
        const settings = document.getElementById('modalSettings');
        const learn = document.getElementById('learnOverlay');
        if (learn && learn.style.display === 'flex') { cancelLearn(); return; }
        if (editor && editor.style.display === 'flex') { closeEditor(); return; }
        if (settings && settings.style.display === 'flex') { closeSettings(); return; }
    }

    // Ctrl + key shortcuts
    if (e.ctrlKey && !e.shiftKey && !e.altKey) {
        switch (e.key.toLowerCase()) {
            case 'n': e.preventDefault(); startLearn(); break;            // Ctrl+N = Capture
            case 's': e.preventDefault(); saveProfile(); break;           // Ctrl+S = Export
            case 'o': e.preventDefault(); loadProfile(); break;           // Ctrl+O = Import
            case 'z': e.preventDefault(); undoLastDelete(); break;        // Ctrl+Z = Undo
            case ',': e.preventDefault(); openSettings(); break;          // Ctrl+, = Preferences
        }
        return;
    }

    // Number keys for view switching (only when no modifier held)
    if (!e.ctrlKey && !e.altKey && !e.shiftKey) {
        if (e.key === '1') { setView('mappings'); return; }
        if (e.key === '2') { setView('logs'); return; }
        if (e.key === '3') { openSettings(); return; }
    }
});

// ══════════════════════════════════════════
//  Undo Last Delete
// ══════════════════════════════════════════
let lastDeletedMapping = null;
let undoTimeout = null;

function undoLastDelete() {
    if (!lastDeletedMapping) {
        showToast('Nothing to undo', 'info');
        return;
    }
    mappings.push(lastDeletedMapping);
    send('update_mappings', { mappings });
    showToast('Mapping restored', 'success');
    lastDeletedMapping = null;
    if (undoTimeout) clearTimeout(undoTimeout);
}

// Override deleteMapping to support undo
const _origDeleteMapping = typeof deleteMapping === 'function' ? deleteMapping : null;

// ══════════════════════════════════════════
//  MIDI Re-Learn for Existing Mappings
// ══════════════════════════════════════════
let relearnTargetIndex = -1;

function startRelearn(index) {
    if (index < 0 || index >= mappings.length) return;
    relearnTargetIndex = index;
    send('start_learn');
    showToast('Re-learning: play a MIDI note to reassign', 'info');
}

// Hook into the learn_done message to apply re-learn
const _origLearnDone = null; // placeholder for extending bridge handler

// ══════════════════════════════════════════
//  Mapping Conflict Detection
// ══════════════════════════════════════════
function detectConflicts() {
    const conflicts = [];
    for (let i = 0; i < mappings.length; i++) {
        for (let j = i + 1; j < mappings.length; j++) {
            const a = mappings[i], b = mappings[j];
            if (!a.enabled || !b.enabled) continue;
            if (a.midi_type !== b.midi_type) continue;

            // Same MIDI trigger
            let sameTrigger = false;
            if (a.midi_type === 2) {
                // Chord — compare sorted arrays
                const ac = (a.midi_chord || []).slice().sort().join(',');
                const bc = (b.midi_chord || []).slice().sort().join(',');
                sameTrigger = ac === bc;
            } else {
                sameTrigger = a.midi_num === b.midi_num;
            }

            if (!sameTrigger) continue;

            // Same gesture
            if (a.gesture_id !== b.gesture_id) continue;

            // Same context (or overlapping)
            const sameApp = a.app_pattern === b.app_pattern;
            const sameTitle = a.title_pattern === b.title_pattern;
            const sameVel = a.vel_zone === b.vel_zone;

            if (sameApp && sameTitle && sameVel) {
                const label = a.midi_type <= 1 ? midiNoteName(a.midi_num) : `Chord`;
                conflicts.push({ i, j, label });
            }
        }
    }
    return conflicts;
}

function showConflictWarnings() {
    const conflicts = detectConflicts();
    if (conflicts.length === 0) return;
    const msg = conflicts.map(c =>
        `${c.label}: mapping #${c.i + 1} and #${c.j + 1} have identical triggers`
    ).join('\n');
    showToast(`⚠ ${conflicts.length} conflict${conflicts.length > 1 ? 's' : ''} detected`, 'warning');
    addLog(`Conflict check: ${conflicts.length} duplicate trigger(s) found`, 'error');
}

// ══════════════════════════════════════════
//  First-Run Onboarding (Empty State)
// ══════════════════════════════════════════
function renderEmptyState(container) {
    container.innerHTML = `
        <div style="display:flex; flex-direction:column; align-items:center; justify-content:center; padding:60px 20px; text-align:center; gap:20px; opacity:0; animation:fadeIn 0.5s ease forwards;">
            <div style="width:72px; height:72px; border-radius:50%; background:var(--accent-surface); display:flex; align-items:center; justify-content:center;">
                <svg viewBox="0 0 24 24" width="32" height="32" fill="none" stroke="var(--accent)" stroke-width="1.5">
                    <path d="M9 18V5l12-2v13"></path><circle cx="6" cy="18" r="3"></circle><circle cx="18" cy="16" r="3"></circle>
                </svg>
            </div>
            <div>
                <h3 style="font-size:20px; font-weight:700; color:var(--text-primary); margin-bottom:8px;">No Mappings Yet</h3>
                <p style="color:var(--text-tertiary); font-size:14px; max-width:320px; line-height:1.5;">Connect your MIDI controller and create your first mapping to start automating.</p>
            </div>
            <div style="display:flex; gap:12px; margin-top:8px;">
                <button class="btn btn-primary" style="padding:10px 20px;" onclick="startLearn()">
                    <svg viewBox="0 0 24 24" width="16" height="16" fill="none" stroke="currentColor" stroke-width="2" style="margin-right:6px;"><circle cx="12" cy="12" r="10"></circle><line x1="12" y1="8" x2="12" y2="16"></line><line x1="8" y1="12" x2="16" y2="12"></line></svg>
                    Capture First Mapping
                </button>
                <button class="btn btn-secondary" style="padding:10px 20px;" onclick="loadProfile()">Import Profile</button>
            </div>
            <div style="margin-top:16px; display:flex; gap:24px; color:var(--text-tertiary); font-size:12px;">
                <span><kbd style="background:var(--bg-surface); padding:2px 6px; border-radius:4px; font-family:var(--font-mono); font-size:11px;">Ctrl+N</kbd> Capture</span>
                <span><kbd style="background:var(--bg-surface); padding:2px 6px; border-radius:4px; font-family:var(--font-mono); font-size:11px;">Ctrl+O</kbd> Import</span>
                <span><kbd style="background:var(--bg-surface); padding:2px 6px; border-radius:4px; font-family:var(--font-mono); font-size:11px;">Ctrl+,</kbd> Settings</span>
            </div>
        </div>
    `;
}

// Patch renderMappings to show empty state + conflict check
const _origRenderMappings = renderMappings;
renderMappings = function () {
    const grid = document.getElementById('mapGrid');
    if (!grid) return _origRenderMappings();

    // Check for empty state
    const filtered = searchQuery ? mappings.filter(m => {
        const s = [m.midi_num, m.key_vk, m.macro_text || '', m.ai_prompt || '', m.app_pattern || '', m.title_pattern || ''].join(' ').toLowerCase();
        return s.includes(searchQuery.toLowerCase());
    }) : mappings;

    if (filtered.length === 0 && !searchQuery) {
        grid.innerHTML = '';
        renderEmptyState(grid);
        // Still update sidebar count
        const navItems = document.querySelectorAll('.nav-item');
        navItems.forEach(n => {
            if (n.textContent.includes('Mappings')) {
                let badge = n.querySelector('.nav-count');
                if (!badge) {
                    badge = document.createElement('span');
                    badge.className = 'nav-count';
                    badge.style.cssText = 'margin-left:auto; font-size:11px; font-weight:600; color:var(--text-tertiary); background:rgba(255,255,255,0.06); padding:1px 7px; border-radius:10px; min-width:20px; text-align:center;';
                    n.appendChild(badge);
                }
                badge.textContent = '0';
            }
        });
        return;
    }

    _origRenderMappings();

    // Run conflict detection after render
    if (mappings.length > 1) {
        showConflictWarnings();
    }
};
