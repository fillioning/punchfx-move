/*
 * Punch-In FX — Chain UI
 *
 * Shows active effects, pad layout, and parameter controls
 * when editing in Signal Chain mode.
 *
 * FX Triggering: Hold SHIFT + press left-half pads to trigger effects.
 * Pads without Shift continue to play the synth as normal.
 */

import {
    MoveMainKnob,
    MoveKnob1, MoveKnob2, MoveKnob3, MoveKnob4,
    White, Black, Red, Blue, BrightGreen, LightGrey
} from '/data/UserData/schwung/shared/constants.mjs';

import { decodeDelta, setLED } from '/data/UserData/schwung/shared/input_filter.mjs';

import {
    drawMenuHeader as drawHeader,
    drawMenuFooter as drawFooter
} from '/data/UserData/schwung/shared/menu_layout.mjs';

const W = 128;
const H = 64;
const CC_SHIFT = 49;

/* Effect names (short) for display */
const FX_NAMES = [
    "Loop 16", "Loop 12", "Loop S", "Loop XS",
    "Unison", "Uni Low", "Oct Up", "Oct Dn",
    "Stut 4", "Stut 3", "Scratch", "Scr Fst",
    "6/8 Q", "Retrig", "Reverse", "Bypass"
];

/* Pad grid layout labels (4x4, top to bottom) */
const GRID_LABELS = [
    ["L16", "L12", "LSh", "LXs"],
    ["Uni", "UnL", "O+",  "O-" ],
    ["S4",  "S3",  "Scr", "ScF"],
    ["6/8", "Ret", "Rev", "Off"]
];

/*
 * Left-half pad MIDI notes → FX index mapping.
 * Move pads: notes 68-99, 4 rows of 8, bottom-left to top-right.
 * Left 4x4:
 *   Top row (92-95): FX 0-3  (Loops)
 *   Row 3   (84-87): FX 4-7  (Modulation)
 *   Row 2   (76-79): FX 8-11 (Stutter/Scratch)
 *   Bottom  (68-71): FX 12-15 (Time/Transform)
 */
const PAD_TO_FX = {};
[92,93,94,95].forEach((n, i) => PAD_TO_FX[n] = i);
[84,85,86,87].forEach((n, i) => PAD_TO_FX[n] = i + 4);
[76,77,78,79].forEach((n, i) => PAD_TO_FX[n] = i + 8);
[68,69,70,71].forEach((n, i) => PAD_TO_FX[n] = i + 12);

/* LED colors per effect group */
const FX_LED_COLORS = [
    BrightGreen, BrightGreen, BrightGreen, BrightGreen,  /* Loops: green */
    Blue, Blue, Blue, Blue,                                /* Modulation: blue */
    Red, Red, Red, Red,                                    /* Stutter/Scratch: red */
    White, White, White, LightGrey                         /* Time/Transform: white */
];

/* Parameters */
const PARAMS = [
    { key: "bpm",     name: "BPM",     min: 40,  max: 300, step: 1,    fmt: v => `${Math.round(v)}` },
    { key: "mix",     name: "FX Mix",  min: 0,   max: 1,   step: 0.05, fmt: v => `${Math.round(v * 100)}%` },
    { key: "attack",  name: "Attack",  min: 1,   max: 50,  step: 1,    fmt: v => `${Math.round(v)}ms` },
    { key: "release", name: "Release", min: 5,   max: 200, step: 5,    fmt: v => `${Math.round(v)}ms` }
];

/* State */
let paramValues = [120, 1.0, 10, 30];
let selectedParam = 0;
let viewMode = 0;           /* 0 = params, 1 = pad grid */
let shiftHeld = false;
let activeFx = new Set();   /* Currently active FX indices */
let needsRedraw = true;

function fetchParams() {
    for (let i = 0; i < PARAMS.length; i++) {
        const val = host_module_get_param(PARAMS[i].key);
        if (val !== null && val !== undefined) {
            paramValues[i] = parseFloat(val) || PARAMS[i].min;
        }
    }
}

function setParam(index, value) {
    const p = PARAMS[index];
    value = Math.max(p.min, Math.min(p.max, value));
    paramValues[index] = value;
    host_module_set_param(p.key, value.toFixed(2));
}

/* Light up left pads when Shift is held to indicate FX mode */
function updatePadLEDs() {
    for (const [noteStr, fxIdx] of Object.entries(PAD_TO_FX)) {
        const note = parseInt(noteStr);
        if (shiftHeld) {
            /* Show FX color when Shift held */
            if (activeFx.has(fxIdx)) {
                setLED(note, White);        /* Active FX = bright white */
            } else {
                setLED(note, FX_LED_COLORS[fxIdx]);  /* Available = group color */
            }
        } else {
            setLED(note, Black);            /* Shift released = LEDs off (back to synth) */
        }
    }
}

function drawParamView() {
    clear_screen();

    /* Header with shift indicator */
    const title = shiftHeld ? "PUNCH FX [SHIFT]" : "Punch-In FX";
    drawHeader(title);

    /* Show active FX if any */
    if (activeFx.size > 0) {
        const names = [...activeFx].map(i => FX_NAMES[i]).join(" + ");
        print(2, 12, names, 1);
    }

    const y0 = activeFx.size > 0 ? 24 : 16;
    const lh = 11;

    for (let i = 0; i < PARAMS.length; i++) {
        const y = y0 + i * lh;
        const sel = i === selectedParam;

        if (sel) fill_rect(0, y - 1, W, lh, 1);

        const color = sel ? 0 : 1;
        const prefix = sel ? "> " : "  ";
        print(2, y, `${prefix}${PARAMS[i].name}`, color);

        const valStr = PARAMS[i].fmt(paramValues[i]);
        print(W - valStr.length * 6 - 4, y, valStr, color);
    }

    drawFooter({ left: "Shift+Pad:FX", right: "K1-4:adj" });
}

function drawGridView() {
    clear_screen();
    drawHeader(shiftHeld ? "FX PADS [SHIFT]" : "Pad Layout");

    const cellW = 30;
    const cellH = 10;
    const x0 = 2;
    const y0 = 14;

    for (let row = 0; row < 4; row++) {
        for (let col = 0; col < 4; col++) {
            const fxIdx = row * 4 + col;
            const x = x0 + col * cellW;
            const y = y0 + row * cellH;

            if (activeFx.has(fxIdx)) {
                fill_rect(x, y, cellW - 1, cellH - 1, 1);
                print(x + 2, y + 1, GRID_LABELS[row][col], 0);
            } else {
                draw_rect(x, y, cellW - 1, cellH - 1, 1);
                print(x + 2, y + 1, GRID_LABELS[row][col], 1);
            }
        }
    }

    drawFooter({ left: "Shift+Pad:FX", right: "Back:params" });
}

function drawUI() {
    if (viewMode === 0) {
        drawParamView();
    } else {
        drawGridView();
    }
    needsRedraw = false;
}

function init() {
    fetchParams();
    needsRedraw = true;
}

function tick() {
    if (needsRedraw) drawUI();
}

function onMidiMessageInternal(data) {
    const status = data[0] & 0xF0;
    const d1 = data[1];
    const d2 = data[2];

    /* ── CC messages ── */
    if (status === 0xB0) {
        /* Track Shift state */
        if (d1 === CC_SHIFT) {
            const wasShift = shiftHeld;
            shiftHeld = d2 >= 64;

            /* Always forward Shift state to DSP */
            host_module_set_param("shift", shiftHeld ? "1" : "0");

            if (shiftHeld && !wasShift) {
                /* Shift just pressed — light up FX pads */
                viewMode = 1;
                updatePadLEDs();
                needsRedraw = true;
            } else if (!shiftHeld && wasShift) {
                /* Shift released — release all active FX, restore pad LEDs */
                for (const fxIdx of activeFx) {
                    host_module_set_param("pad_off", `${fxIdx}`);
                }
                activeFx.clear();
                updatePadLEDs();
                viewMode = 0;
                needsRedraw = true;
            }
            return;
        }

        /* Jog wheel */
        if (d1 === MoveMainKnob) {
            const delta = decodeDelta(d2);
            if (delta !== 0) {
                if (viewMode === 0) {
                    selectedParam = Math.max(0, Math.min(PARAMS.length - 1, selectedParam + delta));
                }
                needsRedraw = true;
            }
            return;
        }

        /* Knobs 1-4 */
        if (d1 >= MoveKnob1 && d1 <= MoveKnob4) {
            const idx = d1 - MoveKnob1;
            const delta = decodeDelta(d2);
            if (delta !== 0 && idx < PARAMS.length) {
                setParam(idx, paramValues[idx] + delta * PARAMS[idx].step);
                needsRedraw = true;
            }
            return;
        }

        /* Back button (CC 51) → toggle view */
        if (d1 === 51 && d2 >= 64) {
            viewMode = viewMode === 0 ? 1 : 0;
            needsRedraw = true;
            return;
        }
    }

    /* ── Note On ── */
    if (status === 0x90 && d2 > 0) {
        const fxIdx = PAD_TO_FX[d1];
        if (fxIdx !== undefined && shiftHeld) {
            /* Shift + Pad → trigger FX */
            host_module_set_param("pad_on", `${fxIdx} ${d2}`);
            activeFx.add(fxIdx);
            updatePadLEDs();
            needsRedraw = true;
            return;  /* Consume — don't let it reach the synth */
        }
    }

    /* ── Note Off ── */
    if (status === 0x80 || (status === 0x90 && d2 === 0)) {
        const fxIdx = PAD_TO_FX[d1];
        if (fxIdx !== undefined && activeFx.has(fxIdx)) {
            /* Release FX pad */
            host_module_set_param("pad_off", `${fxIdx}`);
            activeFx.delete(fxIdx);
            updatePadLEDs();
            needsRedraw = true;
            return;
        }
    }

    /* ── Polyphonic Aftertouch ── */
    if (status === 0xA0) {
        const fxIdx = PAD_TO_FX[d1];
        if (fxIdx !== undefined && activeFx.has(fxIdx)) {
            host_module_set_param("pad_at", `${fxIdx} ${d2}`);
            return;
        }
    }
}

globalThis.chain_ui = {
    init,
    tick,
    onMidiMessageInternal
};
