"""Converts a BLITZ_INPUT script ("<frame>:<BTN[+BTN]>[*hold],...") into a Dolphin input movie (.dtm)
for port 1, so Dolphin can be driven to the same moments as the port (reference frames/audio).

Usage: python -I tools/dtm_from_script.py <script.txt> <out.dtm> [total_frames] [polls_per_frame]
Assumes the game polls the controller once per video frame unless polls_per_frame says otherwise.
"""
import struct
import sys
import time

BUTTONS = {  # (byte, bit) in Dolphin's 8-byte GameCube controller state
    'START': (0, 0x01), 'A': (0, 0x02), 'B': (0, 0x04), 'X': (0, 0x08), 'Y': (0, 0x10), 'Z': (0, 0x20),
    'UP': (0, 0x40), 'DOWN': (0, 0x80), 'LEFT': (1, 0x01), 'RIGHT': (1, 0x02), 'L': (1, 0x04), 'R': (1, 0x08),
}
STICK = {'STICKUP': (0, 100), 'STICKDOWN': (0, -100), 'STICKLEFT': (-100, 0), 'STICKRIGHT': (100, 0)}


def main():
    script = open(sys.argv[1]).read().strip()
    out = sys.argv[2]
    total = int(sys.argv[3]) if len(sys.argv) > 3 else 0
    polls = int(sys.argv[4]) if len(sys.argv) > 4 else 1
    steps = []
    for item in script.split(','):
        if ':' not in item:
            continue
        frame, rest = item.split(':', 1)
        hold = 6
        if '*' in rest:
            rest, h = rest.split('*', 1)
            hold = int(h)
        steps.append((int(frame), hold, rest.split('+')))
    last = max(f + h for f, h, _ in steps) + 60
    total = max(total, last)
    data = bytearray()
    for frame in range(total):
        # 0x40: connected; 0x80: origin reported (the game waits for it after PADReset at boot)
        state = bytearray([0, 0x40 | (0x80 if frame < 120 else 0), 0, 0, 128, 128, 128, 128])
        for f, hold, names in steps:
            if f <= frame < f + hold:
                for n in names:
                    if n in BUTTONS:
                        b, bit = BUTTONS[n]
                        state[b] |= bit
                    elif n in STICK:
                        sx, sy = STICK[n]
                        state[4] = 128 + sx
                        state[5] = 128 + sy
        data += bytes(state) * polls
    hdr = bytearray(256)
    hdr[0:4] = b'DTM\x1a'
    hdr[4:10] = b'GO3E5D'
    hdr[0x0A] = 0           # GameCube
    hdr[0x0B] = 0x01        # port 1 controller
    hdr[0x0C] = 0           # from boot
    struct.pack_into('<Q', hdr, 0x0D, total)             # VI count
    struct.pack_into('<Q', hdr, 0x15, total * polls)     # input count
    hdr[0x31:0x31 + 9] = b'blitz-pc\0'
    struct.pack_into('<Q', hdr, 0x81, int(time.time()))  # RTC at boot (0 = 1970, before the GameCube epoch)
    struct.pack_into('<Q', hdr, 0xED, total * 8_200_000)  # CPU ticks (486 MHz / 59.94 Hz): playback ends after
    hdr[0x97] = 0x00        # no memory card (Dolphin cannot resolve the card path for movies here)
    hdr[0x98] = 1           # blank card
    open(out, 'wb').write(bytes(hdr) + bytes(data))
    print(f'{total} frames, {len(steps)} script steps -> {out}')


main()
