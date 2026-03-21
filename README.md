# Punch-In FX

PO-33 K.O. style punch-in effects for [Ableton Move](https://www.ableton.com/move/), built on the [Schwung](https://github.com/charlesvestal/move-everything) framework.

16 pressure-sensitive effects mapped to the left 4x4 pad grid. Hold **Shift + Pad** to trigger. Up to 3 effects can be stacked simultaneously in series.

## Effects

| Row | Pad 1 | Pad 2 | Pad 3 | Pad 4 |
|-----|-------|-------|-------|-------|
| **Top** | Loop 16th | Loop Triplet | Loop 32nd | Loop 48th |
| **Row 3** | Unison | Unison Low | Octave Up | Octave Down |
| **Row 2** | Stutter 8th | Stutter Trip | Scratch | Scratch Fast |
| **Bottom** | 6/8 Quantize | Retrigger | Reverse | Kill All |

## Knobs

| Knob | Parameter | Range | Default |
|------|-----------|-------|---------|
| 1 | BPM | 40-300 | 120 (auto-syncs to MIDI clock) |
| 2 | Volume | 0-200% | 100% |
| 3 | Dry/Wet | 0-100% | 100% wet |

## Pressure Control

Pad velocity and aftertouch modulate effect character:
- **Loops**: full wet on trigger
- **Unison/Chorus**: detune depth
- **Stutter**: duty cycle (light = sparse, hard = dense)
- **Scratch**: speed and direction change rate
- **Retrigger**: chunk shuffle randomization
- **Reverse**: playback speed

## Install

### From Module Store
Available in the Schwung Module Store (on-device and desktop installer).

### Manual Install
```bash
./scripts/build.sh
./scripts/install.sh
```

## Credits

- **DSP & Design**: Vincent Fillion ([fillioning](https://github.com/fillioning))
- **Framework**: [Schwung](https://github.com/charlesvestal/move-everything) by Charles Vestal
- **Inspiration**: Teenage Engineering PO-33 K.O. punch-in effects

## License

MIT License. See [LICENSE](LICENSE).
