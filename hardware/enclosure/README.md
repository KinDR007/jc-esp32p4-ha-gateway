# Enclosure for the JC-ESP32P4-M3-DEV

A 3D printable case for the gateway board, parametric in OpenSCAD (`p4_case.scad`):

| Part | Print | |
|---|---|---|
| `bottom` | floor down, no supports | floor with four standoffs under the board holes, vents, XIAO cradle, screw ears |
| `top` | roof down (the STL is already turned), no supports | walls with the connector openings, three flexible buttons, vents, microphone grille, labels, screw bosses |
| `fit_test` | as exported | the lowest 9 mm of the top shell: 20 minutes of printing to check the openings and buttons on the real board before printing the rest |

Four M2.5 screws from below hold everything: through the standoff, the board hole and into the boss
of the top shell (M2.5 x 16, pan or socket head; self-tapping into 2.2 mm, or `pilot_d = 3.6` for
heat-set inserts). The board is clamped between standoffs and bosses; nothing else holds the shells.

Outside: 122.8 x 66.8 x 30.6 mm with the XIAO bay (`xiao_bay = false`: 94.8 x 66.8), ears +11 mm each side; the RJ45 sticks out 1.1 mm.

```sh
hardware/enclosure/make.sh                                 # stl/p4_case_{bottom,top,fit_test}.stl
hardware/enclosure/make.sh -D xiao_bay=false -D ears=false
```

`make.sh` first intersects the board model with both shells (must be empty: no part of the board
model hits the case) and checks that every STL is one body (a part cut loose, e.g. a button tongue,
fails). Preview: open `p4_case.scad` in OpenSCAD, `part = "assembly"` shows the board inside.

PETG or ASA for a box near a heat source; PLA is fine indoors. 0.2 mm layers, 3 perimeters (the
walls are 2 mm), 15 % infill.

## Board layout

Coordinates in the model: origin at the board's front left corner, x to the right, y to the back.
"Front" is the edge with the RS485 and I2C connectors, "right" the USB / Ethernet edge.

| Feature | Position | Source |
|---|---|---|
| Board | 92 x 62 mm over all, the board itself about 90 x 62, 1.6 mm | Guition specification (92 x 62), dimension drawing; the USB-C and RJ45 stick out past the board edge |
| Mounting holes S1-S4 | (2.8, 2.8), (64.8, 2.8), (2.8, 59.2), (64.8, 59.2), about 3.2 mm | drawing: 62 mm and 56.4 mm apart; schematic sheet 1 (S1-S4); 2.8 mm from the edges measured on the photo |
| USB-C x3 (USB1 CH340 console next to the RJ45, USB2 full speed, USB3 high speed OTG) | right edge, centres y = 34.75, 45.1, 56.15; front at x = 92 | schematic sheet 5; photo |
| RJ45 (with magnetics, 16 x 13.5 mm) | right edge, y = 1-17, front at x = 93.5 | sheet 7; photo |
| CN2 UART (MX1.25 4P: VIN, TX, RX, GND) | right edge, y = 18.5-29.9 | sheet 1; photo label CN2 |
| J4 RS485 (PH2.0 4P: A, B, 5 V, GND) | front edge, x = 25-35.2 | sheet 5; photo label J4 |
| J5 (PH2.0 4P: RX1, TX1, 5 V, GND; TTL side of the MAX485) | front edge, x = 37.2-46.5 | sheet 5; photo label J5 |
| CN3 I2C (MX1.25 4P) | front edge, x = 16.1-23.4 | sheet 1; photo label CN3 |
| SW1 BOOT, SW2 RESET, SW3 power key (IP5306) | back edge, side switches pressed from the edge, actuators at x = 7.5, 13.7, 80.4 | sheets 2 and 4; photo labels SW1-SW3 |
| TF card | back edge, x = 46.3-62, inserted from the back | sheet 3; photo |
| Microphone MIC1 | (20.5, 59), sound port on top | sheet 3 (ES8311 MIC1); photo label MIC1 |
| JP1 2x13 header | left edge, x = 0.3-4.6, y = 13.8-46.8 | sheet 2; photo |
| Inside, no opening: CN1 speaker, CN4 battery (back edge), CN5 backlight (left), FPC1 touch, J2 DSI, J3 CSI | | sheets 1, 2, 6, 8 |
| ANT1 chip antenna (CA-C03, C6) | left edge near the back | sheet 3: keep metal away from the left back corner |

Sources: Guition "JC-ESP32P4-M3-DEV Specifications" (size 92 x 62 mm) and the dimension drawing and
schematics in [p1ngb4ck/unofficial_guition_esp32p4_repo](https://github.com/p1ngb4ck/unofficial_guition_esp32p4_repo/tree/main/JC-ESP32P4-M3-Dev)
(`structure_diagram/5.jpg`, `schematics/*.png`). Positions other than the 92 / 62 / 56.4 mm of the
drawing were measured on its photo (5 px per mm, slight perspective): **±0.5-1 mm**. Heights are
typical for the parts (USB-C 3.2 mm, RJ45 13.5 mm, PH2.0 6 mm, MX1.25 4.5 mm).

## Before printing: measure

With a caliper on the board, then change the values at the top of `p4_case.scad` (positions in the
`what is where` block):

1. Board width without connectors (`pcb_w`), front of the USB-C (`usb_front`) and of the RJ45 (`rj_front`).
2. Hole positions from the front left corner (`holes`) and the hole diameter (M2.5 fits 2.9-3.2).
3. Centres of the three USB-C from the front edge (`usb_y`), RJ45 centre (`rj_y`).
4. Button actuators from the left edge (`buttons`) and how far they reach past the board edge
   (`btn_gap`: the gap between the button nub and the board edge line, 0.3 mm).
5. Height of the pins under the board (`standoff_h` must be larger, 5 mm).

Then print `fit_test`, put it over the board and check that every connector sits in its opening and
that BOOT and RST are **not** pressed when nothing touches them (a held RST keeps the board off).

## Buttons

BOOT, RST and PWR are tongues in the back wall, 4.4 mm wide, 9 mm long, 1.2 mm thick, hanging from the
wall above and free at the bottom edge; pressing a tongue bends it onto the switch. Labels on the roof
above them.

## Second radio (XIAO ESP32-C5)

With `xiao_bay` (default), the case is 28 mm longer on the left: a cradle on the floor holds the
XIAO with its USB-C in the left wall (its own 5 V supply, [docs/dual-radio.md](../../docs/dual-radio.md)),
the wires to JP1 run inside, and a 6.6 mm hole above takes an SMA bulkhead for its U.FL antenna (U.FL
to SMA pigtail). Without the bay the left wall is closed.

## Not covered

Display (DSI) and camera (CSI) cables, the speaker and a battery stay inside without openings; the
red power LED is not visible through an opaque roof. JP1 with Dupont plugs needs the 20 mm above the
board (`inner_h`); bend the wires sideways.
