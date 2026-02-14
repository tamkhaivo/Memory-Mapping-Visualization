/**
 * Memory Mapper — Real-Time Canvas Visualizer
 *
 * Connects to the C++ WebSocket server, processes allocation events,
 * and renders the arena memory map on an HTML5 Canvas.
 */

// ─── State ──────────────────────────────────────────────────────

const HEATMAP_BUCKETS = 256;     // Number of buckets for heatmap

const state = {
    ws: null,
    connected: false,
    capacity: 0,
    blocks: new Map(),         // offset → { offset, size, actual_size, alignment, tag, age }
    recentDeallocs: new Map(), // offset → { size, fadeStart }
    events: [],                // Event log for the timeline panel
    stats: {
        totalAllocated: 0,
        totalFree: 0,
        fragPct: 0,
        freeBlockCount: 0,
    },
    hover: null,               // Currently hovered block or null
    eventCount: 0,
    // Heatmap state
    heatmapEnabled: false,
    heatmap: new Float64Array(HEATMAP_BUCKETS), // Per-bucket access frequency
    heatmapMax: 1,             // Maximum bucket value (for normalization)
    // Replay state
    importedEvents: null,      // Loaded JSON events awaiting replay
    replaying: false,
    replayAbort: null,         // AbortController for cancelling replay
};

// ─── DOM References ─────────────────────────────────────────────

const dom = {
    canvas: document.getElementById('memoryCanvas'),
    canvasContainer: document.getElementById('canvasContainer'),
    tooltip: document.getElementById('tooltip'),
    timeline: document.getElementById('timeline'),
    connectionStatus: document.getElementById('connectionStatus'),
    fragBar: document.getElementById('fragBar'),
    statCapacity: document.getElementById('statCapacity'),
    statAllocated: document.getElementById('statAllocated'),
    statFree: document.getElementById('statFree'),
    statFrag: document.getElementById('statFrag'),
    statFreeBlocks: document.getElementById('statFreeBlocks'),
    statEvents: document.getElementById('statEvents'),
    btnClear: document.getElementById('btnClear'),
    btnHeatmap: document.getElementById('btnHeatmap'),
    btnExport: document.getElementById('btnExport'),
    fileImport: document.getElementById('fileImport'),
    btnReplay: document.getElementById('btnReplay'),
    replayStatus: document.getElementById('replayStatus'),
    // Stress test controls
    btnBurst: document.getElementById('btnBurst'),
    btnFrag: document.getElementById('btnFrag'),
    btnLarge: document.getElementById('btnLarge'),
    btnCleanup: document.getElementById('btnCleanup'),
    btnStop: document.getElementById('btnStop'),
    stressStatus: document.getElementById('stressStatus'),
};

const ctx = dom.canvas.getContext('2d');

// ─── Constants ──────────────────────────────────────────────────

const COLORS = {
    bg: '#0a0e17',
    free: '#111827',
    freeBorder: '#1e293b',
    allocGrad1: '#4ade80',
    allocGrad2: '#22c55e',
    allocDim: '#166534',
    dealloc: '#f87171',
    deallocFade: '#7f1d1d',
    hover: '#fbbf24',
    hoverBorder: '#f59e0b',
    gridLine: '#1a2235',
    text: '#94a3b8',
    textBright: '#e2e8f0',
};

const BLOCK_PADDING = 1;         // px between blocks
const DEALLOC_FADE_MS = 1200;    // How long freed blocks glow red
const CANVAS_PADDING = 12;       // px padding inside canvas
const ROW_HEIGHT = 28;           // px height per row in grid view
const MAX_TIMELINE_EVENTS = 200;

// ─── Canvas Setup ───────────────────────────────────────────────

function resizeCanvas() {
    const rect = dom.canvasContainer.getBoundingClientRect();
    const dpr = window.devicePixelRatio || 1;
    dom.canvas.width = rect.width * dpr;
    dom.canvas.height = rect.height * dpr;
    ctx.scale(dpr, dpr);
    dom.canvas.style.width = rect.width + 'px';
    dom.canvas.style.height = rect.height + 'px';
}

window.addEventListener('resize', resizeCanvas);
resizeCanvas();

// ─── WebSocket ──────────────────────────────────────────────────

function connect() {
    const wsUrl = `ws://${window.location.host}`;
    state.ws = new WebSocket(wsUrl);

    state.ws.onopen = () => {
        state.connected = true;
        dom.connectionStatus.classList.add('connected');
        dom.connectionStatus.querySelector('.status-text').textContent = 'Connected';
        console.log('[WS] Connected');
    };

    state.ws.onclose = () => {
        state.connected = false;
        dom.connectionStatus.classList.remove('connected');
        dom.connectionStatus.querySelector('.status-text').textContent = 'Disconnected';
        console.log('[WS] Disconnected, reconnecting in 2s...');
        setTimeout(connect, 2000);
    };

    state.ws.onerror = (err) => {
        console.error('[WS] Error:', err);
    };

    state.ws.onmessage = (event) => {
        try {
            const data = JSON.parse(event.data);
            handleMessage(data);
        } catch (e) {
            console.error('[WS] Parse error:', e);
        }
    };
}

// ─── Message Handling ───────────────────────────────────────────

function handleMessage(data) {
    if (data.type === 'snapshot') {
        handleSnapshot(data);
    } else if (data.type === 'allocate') {
        handleAllocate(data);
    } else if (data.type === 'deallocate') {
        handleDeallocate(data);
    }
}

function handleSnapshot(data) {
    state.capacity = data.capacity;
    state.blocks.clear();

    for (const block of data.blocks) {
        state.blocks.set(block.offset, {
            offset: block.offset,
            size: block.size,
            actual_size: block.actual_size,
            alignment: block.alignment,
            tag: block.tag || '',
            age: 0,
        });
    }

    state.stats.totalAllocated = data.total_allocated;
    state.stats.totalFree = data.total_free;
    state.stats.fragPct = data.fragmentation_pct;
    state.stats.freeBlockCount = data.free_block_count;

    updateStatsUI();
}

function handleAllocate(data) {
    state.blocks.set(data.offset, {
        offset: data.offset,
        size: data.size,
        actual_size: data.actual_size,
        alignment: data.alignment,
        tag: data.tag || '',
        age: performance.now(),
    });

    state.stats.totalAllocated = data.total_allocated;
    state.stats.totalFree = data.total_free;
    state.stats.fragPct = data.fragmentation_pct;
    state.stats.freeBlockCount = data.free_block_count;

    // Infer capacity from first event if snapshot was missed.
    if (state.capacity === 0 && data.total_allocated + data.total_free > 0) {
        state.capacity = data.total_allocated + data.total_free;
    }

    bumpHeatmap(data.offset, data.actual_size || data.size);

    state.eventCount++;
    addTimelineEvent(data);
    updateStatsUI();
}

function handleDeallocate(data) {
    const block = state.blocks.get(data.offset);
    if (block) {
        // Move to recent deallocs for fade animation.
        state.recentDeallocs.set(data.offset, {
            offset: data.offset,
            size: block.actual_size || data.actual_size,
            fadeStart: performance.now(),
        });
        state.blocks.delete(data.offset);
    }

    state.stats.totalAllocated = data.total_allocated;
    state.stats.totalFree = data.total_free;
    state.stats.fragPct = data.fragmentation_pct;
    state.stats.freeBlockCount = data.free_block_count;

    bumpHeatmap(data.offset, data.actual_size || data.size);

    state.eventCount++;
    addTimelineEvent(data);
    updateStatsUI();
}

// ─── Stats UI ───────────────────────────────────────────────────

function formatBytes(bytes) {
    if (bytes >= 1024 * 1024) return (bytes / (1024 * 1024)).toFixed(1) + ' MB';
    if (bytes >= 1024) return (bytes / 1024).toFixed(1) + ' KB';
    return bytes + ' B';
}

function updateStatsUI() {
    dom.statCapacity.textContent = formatBytes(state.capacity);
    dom.statAllocated.textContent = formatBytes(state.stats.totalAllocated);
    dom.statFree.textContent = formatBytes(state.stats.totalFree);
    dom.statFrag.textContent = state.stats.fragPct + '%';
    dom.statFreeBlocks.textContent = state.stats.freeBlockCount;
    dom.statEvents.textContent = state.eventCount;
    dom.fragBar.style.width = state.stats.fragPct + '%';
}

// ─── Timeline ───────────────────────────────────────────────────

function addTimelineEvent(data) {
    if (state.events.length === 0) {
        dom.timeline.innerHTML = ''; // Remove "Waiting for events..." placeholder.
    }

    state.events.push(data);
    if (state.events.length > MAX_TIMELINE_EVENTS) {
        state.events.shift();
        // We'll re-render in next efficient pass; for now just append.
    }

    const isAlloc = data.type === 'allocate';
    const row = document.createElement('div');
    row.className = 'event-row';
    row.innerHTML = `
        <span class="event-id">#${data.event_id}</span>
        <span class="event-type ${isAlloc ? 'alloc' : 'dealloc'}">${isAlloc ? 'ALLOC' : 'FREE'}</span>
        <span class="event-tag">${data.tag || '—'}</span>
        <span class="event-size">${formatBytes(data.size)}</span>
        <span class="event-offset">0x${data.offset.toString(16).padStart(6, '0')}</span>
        <span class="event-frag">${data.fragmentation_pct}%</span>
    `;

    // Highlight block on hover.
    row.addEventListener('mouseenter', () => {
        state.hover = { offset: data.offset, size: data.actual_size || data.size };
    });
    row.addEventListener('mouseleave', () => {
        state.hover = null;
    });

    dom.timeline.appendChild(row);
    dom.timeline.scrollTop = dom.timeline.scrollHeight;
}

dom.btnClear.addEventListener('click', () => {
    state.events = [];
    dom.timeline.innerHTML = '<div class="timeline-empty">Cleared.</div>';
});

// ─── WebSocket Command Sender ───────────────────────────────────

function sendCommand(obj) {
    if (state.ws && state.ws.readyState === WebSocket.OPEN) {
        state.ws.send(JSON.stringify(obj));
    } else {
        console.warn('[cmd] WebSocket not connected');
    }
}

// ─── Stress Test Controls ───────────────────────────────────────

dom.btnBurst.addEventListener('click', () => {
    sendCommand({ command: 'stress_test', pattern: 'random_burst' });
    dom.stressStatus.textContent = 'Running: Random Burst...';
});

dom.btnFrag.addEventListener('click', () => {
    sendCommand({ command: 'stress_test', pattern: 'frag_storm' });
    dom.stressStatus.textContent = 'Running: Frag Storm...';
});

dom.btnLarge.addEventListener('click', () => {
    sendCommand({ command: 'stress_test', pattern: 'large_blocks' });
    dom.stressStatus.textContent = 'Running: Large Blocks...';
});

dom.btnCleanup.addEventListener('click', () => {
    sendCommand({ command: 'cleanup' });
    dom.stressStatus.textContent = 'Cleaning up...';
});

dom.btnStop.addEventListener('click', () => {
    sendCommand({ command: 'stop' });
    dom.stressStatus.textContent = 'Stopped';
});

// ─── Heatmap ────────────────────────────────────────────────────

function bumpHeatmap(offset, size) {
    if (state.capacity === 0) return;
    const bucketSize = state.capacity / HEATMAP_BUCKETS;
    const startBucket = Math.floor(offset / bucketSize);
    const endBucket = Math.min(HEATMAP_BUCKETS - 1, Math.floor((offset + size) / bucketSize));
    for (let b = startBucket; b <= endBucket; b++) {
        state.heatmap[b] += 1;
        if (state.heatmap[b] > state.heatmapMax) {
            state.heatmapMax = state.heatmap[b];
        }
    }
}

function heatmapColor(intensity) {
    // 0 = cold (blue #3b82f6) → 0.5 = warm (orange #f97316) → 1.0 = hot (red #ef4444)
    let r, g, b;
    if (intensity < 0.5) {
        const t = intensity * 2;
        r = Math.round(59 + (249 - 59) * t);
        g = Math.round(130 + (115 - 130) * t);
        b = Math.round(246 + (22 - 246) * t);
    } else {
        const t = (intensity - 0.5) * 2;
        r = Math.round(249 + (239 - 249) * t);
        g = Math.round(115 + (68 - 115) * t);
        b = Math.round(22 + (68 - 22) * t);
    }
    return `rgba(${r}, ${g}, ${b}, 0.35)`;
}

function drawHeatmapOverlay(pad, drawW, rows, bytesPerRow) {
    const bucketSize = state.capacity / HEATMAP_BUCKETS;
    for (let b = 0; b < HEATMAP_BUCKETS; b++) {
        if (state.heatmap[b] === 0) continue;
        const intensity = state.heatmap[b] / state.heatmapMax;
        const bucketOffset = b * bucketSize;
        const start = offsetToPixel(bucketOffset, pad, drawW, rows, bytesPerRow);
        const pixelWidth = Math.max(1, (bucketSize / bytesPerRow) * drawW);
        ctx.fillStyle = heatmapColor(intensity);
        ctx.fillRect(start.x, start.y, pixelWidth, ROW_HEIGHT - 2);
    }
}

dom.btnHeatmap.addEventListener('click', () => {
    state.heatmapEnabled = !state.heatmapEnabled;
    dom.btnHeatmap.textContent = state.heatmapEnabled ? 'Heatmap: ON' : 'Heatmap: OFF';
    dom.btnHeatmap.classList.toggle('active', state.heatmapEnabled);
});

// ─── Export / Import / Replay ───────────────────────────────────

dom.btnExport.addEventListener('click', () => {
    if (state.events.length === 0) return;
    const payload = {
        exported_at: new Date().toISOString(),
        capacity: state.capacity,
        event_count: state.events.length,
        events: state.events,
    };
    const blob = new Blob([JSON.stringify(payload, null, 2)], { type: 'application/json' });
    const url = URL.createObjectURL(blob);
    const a = document.createElement('a');
    a.href = url;
    a.download = `memory_events_${Date.now()}.json`;
    a.click();
    URL.revokeObjectURL(url);
});

dom.fileImport.addEventListener('change', async (e) => {
    const file = e.target.files[0];
    if (!file) return;
    try {
        const text = await file.text();
        const data = JSON.parse(text);
        if (!data.events || !Array.isArray(data.events)) {
            console.error('[Import] Invalid format: missing events array');
            return;
        }
        state.importedEvents = data;
        dom.btnReplay.disabled = false;
        dom.replayStatus.textContent = `Loaded ${data.events.length} events`;
    } catch (err) {
        console.error('[Import] Parse error:', err);
        dom.replayStatus.textContent = 'Import failed';
    }
    dom.fileImport.value = ''; // Reset.
});

dom.btnReplay.addEventListener('click', async () => {
    if (!state.importedEvents || state.replaying) {
        // If replaying, abort.
        if (state.replaying && state.replayAbort) {
            state.replayAbort.abort();
        }
        return;
    }

    const events = state.importedEvents.events;
    if (events.length === 0) return;

    state.replaying = true;
    dom.btnReplay.classList.add('replaying');
    dom.btnReplay.textContent = '⏹ Stop';
    state.replayAbort = new AbortController();
    const signal = state.replayAbort.signal;

    // Reset state for replay.
    state.blocks.clear();
    state.recentDeallocs.clear();
    state.events = [];
    state.eventCount = 0;
    state.heatmap.fill(0);
    state.heatmapMax = 1;
    dom.timeline.innerHTML = '';

    if (state.importedEvents.capacity) {
        state.capacity = state.importedEvents.capacity;
    }

    const REPLAY_SPEED = 4; // Nx speed multiplier
    const BASE_DELAY_MS = 80; // Minimum delay between events

    for (let i = 0; i < events.length; i++) {
        if (signal.aborted) break;

        const ev = events[i];
        dom.replayStatus.textContent = `Replaying ${i + 1}/${events.length}`;
        handleMessage(ev);

        // Compute delay: use event_id spacing or fixed delay.
        let delay = BASE_DELAY_MS;
        if (i > 0 && events[i].event_id !== undefined && events[i - 1].event_id !== undefined) {
            const gap = events[i].event_id - events[i - 1].event_id;
            delay = Math.max(20, Math.min(200, gap * 40)) / REPLAY_SPEED;
        }

        await new Promise((resolve) => {
            const timer = setTimeout(resolve, delay);
            signal.addEventListener('abort', () => { clearTimeout(timer); resolve(); }, { once: true });
        });
    }

    state.replaying = false;
    dom.btnReplay.classList.remove('replaying');
    dom.btnReplay.textContent = '▶ Replay';
    dom.replayStatus.textContent = signal.aborted ? 'Stopped' : 'Replay complete';
    state.replayAbort = null;
});

// ─── Canvas Rendering ───────────────────────────────────────────

function render() {
    const now = performance.now();
    const rect = dom.canvasContainer.getBoundingClientRect();
    const w = rect.width;
    const h = rect.height;

    // Clear.
    ctx.fillStyle = COLORS.bg;
    ctx.fillRect(0, 0, w, h);

    if (state.capacity === 0) {
        // Draw "waiting" state.
        ctx.fillStyle = COLORS.text;
        ctx.font = '14px Inter, sans-serif';
        ctx.textAlign = 'center';
        ctx.fillText('Waiting for connection...', w / 2, h / 2);
        requestAnimationFrame(render);
        return;
    }

    // ─── Grid-based linear memory map ───────────────────────────

    const pad = CANVAS_PADDING;
    const drawW = w - pad * 2;
    const drawH = h - pad * 2;

    // Draw the entire arena as rows of memory.
    const bytesPerPixel = state.capacity / (drawW * Math.floor(drawH / ROW_HEIGHT));
    const rows = Math.floor(drawH / ROW_HEIGHT);
    const bytesPerRow = state.capacity / rows;

    // Draw free background.
    ctx.fillStyle = COLORS.free;
    for (let r = 0; r < rows; r++) {
        const y = pad + r * ROW_HEIGHT;
        roundRect(ctx, pad, y, drawW, ROW_HEIGHT - 2, 3);
        ctx.fill();

        // Subtle border.
        ctx.strokeStyle = COLORS.freeBorder;
        ctx.lineWidth = 0.5;
        roundRect(ctx, pad, y, drawW, ROW_HEIGHT - 2, 3);
        ctx.stroke();
    }

    // Row offset labels.
    ctx.fillStyle = COLORS.text;
    ctx.font = '9px JetBrains Mono, monospace';
    ctx.textAlign = 'right';
    for (let r = 0; r < rows; r++) {
        const offset = Math.floor(r * bytesPerRow);
        const y = pad + r * ROW_HEIGHT + ROW_HEIGHT / 2 + 3;
        // Draw label to the left (outside the draw area, in the padding).
        // Actually, overlay on the left edge.
    }

    // ─── Draw allocated blocks ──────────────────────────────────

    for (const [offset, block] of state.blocks) {
        drawBlock(offset, block.actual_size || block.size, block, now, pad, drawW, rows, bytesPerRow, false);
    }

    // ─── Draw recent deallocations (fading red) ─────────────────

    const expiredDeallocs = [];
    for (const [offset, dealloc] of state.recentDeallocs) {
        const elapsed = now - dealloc.fadeStart;
        if (elapsed > DEALLOC_FADE_MS) {
            expiredDeallocs.push(offset);
            continue;
        }
        const alpha = 1 - elapsed / DEALLOC_FADE_MS;
        drawDeallocBlock(offset, dealloc.size, alpha, pad, drawW, rows, bytesPerRow);
    }
    for (const key of expiredDeallocs) {
        state.recentDeallocs.delete(key);
    }

    // ─── Draw heatmap overlay ───────────────────────────────────

    if (state.heatmapEnabled && state.heatmapMax > 0) {
        drawHeatmapOverlay(pad, drawW, rows, bytesPerRow);
    }

    // ─── Draw hover highlight ───────────────────────────────────

    if (state.hover) {
        const { offset, size } = state.hover;
        drawHighlight(offset, size, pad, drawW, rows, bytesPerRow);
    }

    requestAnimationFrame(render);
}

function offsetToPixel(offset, pad, drawW, rows, bytesPerRow) {
    const row = Math.min(Math.floor(offset / bytesPerRow), rows - 1);
    const withinRow = offset - row * bytesPerRow;
    const x = pad + (withinRow / bytesPerRow) * drawW;
    const y = pad + row * ROW_HEIGHT;
    return { x, y, row };
}

function drawBlock(offset, size, block, now, pad, drawW, rows, bytesPerRow, isHighlight) {
    const start = offsetToPixel(offset, pad, drawW, rows, bytesPerRow);
    const pixelWidth = Math.max(2, (size / bytesPerRow) * drawW);

    // Green gradient with age-based brightness.
    const age = block.age ? (now - block.age) / 1000 : 10; // seconds
    const brightness = Math.max(0.4, 1 - age * 0.05); // Fade slightly over time.

    const grad = ctx.createLinearGradient(start.x, start.y, start.x + pixelWidth, start.y + ROW_HEIGHT);
    grad.addColorStop(0, adjustAlpha(COLORS.allocGrad1, brightness));
    grad.addColorStop(1, adjustAlpha(COLORS.allocGrad2, brightness));

    ctx.fillStyle = grad;
    roundRect(ctx, start.x + BLOCK_PADDING, start.y + 1, pixelWidth - BLOCK_PADDING * 2, ROW_HEIGHT - 4, 2);
    ctx.fill();

    // Tag label if block is wide enough.
    if (pixelWidth > 40 && block.tag) {
        ctx.fillStyle = '#0a0e17';
        ctx.font = 'bold 9px JetBrains Mono, monospace';
        ctx.textAlign = 'left';
        const maxTextW = pixelWidth - 8;
        let label = block.tag;
        if (ctx.measureText(label).width > maxTextW) {
            while (label.length > 2 && ctx.measureText(label + '…').width > maxTextW) {
                label = label.slice(0, -1);
            }
            label += '…';
        }
        ctx.fillText(label, start.x + 4, start.y + ROW_HEIGHT / 2 + 3);
    }
}

function drawDeallocBlock(offset, size, alpha, pad, drawW, rows, bytesPerRow) {
    const start = offsetToPixel(offset, pad, drawW, rows, bytesPerRow);
    const pixelWidth = Math.max(2, (size / bytesPerRow) * drawW);

    ctx.fillStyle = `rgba(248, 113, 113, ${alpha * 0.6})`;
    roundRect(ctx, start.x + BLOCK_PADDING, start.y + 1, pixelWidth - BLOCK_PADDING * 2, ROW_HEIGHT - 4, 2);
    ctx.fill();

    // Glow effect.
    ctx.shadowColor = COLORS.dealloc;
    ctx.shadowBlur = 8 * alpha;
    ctx.fillStyle = `rgba(248, 113, 113, ${alpha * 0.3})`;
    roundRect(ctx, start.x + BLOCK_PADDING, start.y + 1, pixelWidth - BLOCK_PADDING * 2, ROW_HEIGHT - 4, 2);
    ctx.fill();
    ctx.shadowBlur = 0;
}

function drawHighlight(offset, size, pad, drawW, rows, bytesPerRow) {
    const start = offsetToPixel(offset, pad, drawW, rows, bytesPerRow);
    const pixelWidth = Math.max(2, (size / bytesPerRow) * drawW);

    ctx.strokeStyle = COLORS.hoverBorder;
    ctx.lineWidth = 2;
    roundRect(ctx, start.x, start.y, pixelWidth, ROW_HEIGHT - 2, 3);
    ctx.stroke();

    ctx.fillStyle = `rgba(251, 191, 36, 0.1)`;
    roundRect(ctx, start.x, start.y, pixelWidth, ROW_HEIGHT - 2, 3);
    ctx.fill();
}

function roundRect(ctx, x, y, w, h, r) {
    ctx.beginPath();
    ctx.moveTo(x + r, y);
    ctx.lineTo(x + w - r, y);
    ctx.quadraticCurveTo(x + w, y, x + w, y + r);
    ctx.lineTo(x + w, y + h - r);
    ctx.quadraticCurveTo(x + w, y + h, x + w - r, y + h);
    ctx.lineTo(x + r, y + h);
    ctx.quadraticCurveTo(x, y + h, x, y + h - r);
    ctx.lineTo(x, y + r);
    ctx.quadraticCurveTo(x, y, x + r, y);
    ctx.closePath();
}

function adjustAlpha(hex, alpha) {
    const r = parseInt(hex.slice(1, 3), 16);
    const g = parseInt(hex.slice(3, 5), 16);
    const b = parseInt(hex.slice(5, 7), 16);
    return `rgba(${r}, ${g}, ${b}, ${alpha})`;
}

// ─── Mouse Interaction ──────────────────────────────────────────

dom.canvas.addEventListener('mousemove', (e) => {
    if (state.capacity === 0) return;

    const rect = dom.canvasContainer.getBoundingClientRect();
    const mx = e.clientX - rect.left;
    const my = e.clientY - rect.top;

    const pad = CANVAS_PADDING;
    const drawW = rect.width - pad * 2;
    const drawH = rect.height - pad * 2;
    const rows = Math.floor(drawH / ROW_HEIGHT);
    const bytesPerRow = state.capacity / rows;

    // Determine which row the mouse is in.
    const row = Math.floor((my - pad) / ROW_HEIGHT);
    if (row < 0 || row >= rows) {
        hideTooltip();
        state.hover = null;
        return;
    }

    // Determine offset within row.
    const xRatio = Math.max(0, Math.min(1, (mx - pad) / drawW));
    const mouseOffset = row * bytesPerRow + xRatio * bytesPerRow;

    // Find block under cursor.
    let found = null;
    for (const [offset, block] of state.blocks) {
        const blockEnd = offset + (block.actual_size || block.size);
        if (mouseOffset >= offset && mouseOffset < blockEnd) {
            found = block;
            break;
        }
    }

    if (found) {
        state.hover = { offset: found.offset, size: found.actual_size || found.size };
        showTooltip(e.clientX - rect.left, e.clientY - rect.top, found);
    } else {
        state.hover = null;
        hideTooltip();
    }
});

dom.canvas.addEventListener('mouseleave', () => {
    state.hover = null;
    hideTooltip();
});

function showTooltip(x, y, block) {
    const tt = dom.tooltip;
    tt.innerHTML = `
        <div class="tt-tag">${block.tag || 'unnamed'}</div>
        <div><span class="tt-label">Offset: </span><span class="tt-value">0x${block.offset.toString(16).padStart(6, '0')}</span></div>
        <div><span class="tt-label">Size: </span><span class="tt-value">${formatBytes(block.size)}</span></div>
        <div><span class="tt-label">Actual: </span><span class="tt-value">${formatBytes(block.actual_size)}</span></div>
        <div><span class="tt-label">Align: </span><span class="tt-value">${block.alignment}B</span></div>
    `;

    // Position tooltip, keeping it within the canvas container.
    const containerRect = dom.canvasContainer.getBoundingClientRect();
    const ttW = 200;  // Approximate width.
    let left = x + 16;
    let top = y - 10;

    if (left + ttW > containerRect.width) left = x - ttW - 10;
    if (top < 0) top = 10;

    tt.style.left = left + 'px';
    tt.style.top = top + 'px';
    tt.classList.add('visible');
}

function hideTooltip() {
    dom.tooltip.classList.remove('visible');
}

// ─── Boot ───────────────────────────────────────────────────────

connect();
requestAnimationFrame(render);
