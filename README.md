# Cursor Scaler

Cursor find utility like on Plamsa and MacOS for X11 

Event Driven.

## Compile

```bash
gcc -o cursor-scaler cursor-scaler.c -lX11 -lXcursor -lXrender -lXfixes -lXi -lm -O3
./cursor-scaler
```

## Requirements

- X11
- Compositor (picom)

## Usage

Shake mouse fast = big cursor. Stop shaking = normal cursor.

```
