// Enclosure for the Guition JC-ESP32P4-M3-DEV board of the P4 gateway (OpenSCAD, mm).
// Two shells split at the top of the board: the bottom shell carries the board on four standoffs,
// the top shell has the connector openings, three flexible buttons in its back wall and the vents.
// Four M2.5 screws from below go through standoff, board and into the bosses of the top shell.
// Board dimensions and positions: README.md (where each comes from, what to measure).
//
// Coordinates: origin at the board's front left corner, x to the right (USB, Ethernet side),
// y to the back (SD card, buttons), z up from the bed under the bottom shell.

part = "assembly"; // [assembly, bottom, top, fit_test, board, clash]
xiao_bay = true;   // room left of the board for the second radio (XIAO ESP32-C5) and its antenna
xiao_carrier = false;  // the XIAO on the carrier board plugged onto JP1 instead of the floor cradle
ears = true;       // screw ears on the bottom shell (wall mounting)

/* [Board] */
pcb_w = 90;        // without the connectors sticking out at the right
pcb_d = 62;
pcb_t = 1.6;
holes = [[2.8, 2.8], [64.8, 2.8], [2.8, 59.2], [64.8, 59.2]];  // 62 x 56.4 apart
usb_front = 92;    // front of the USB-C receptacles
rj_front = 93.5;   // front of the RJ45 jack

/* [Case] */
clr = 0.4;         // board edge to wall
wall = 2;
floor_t = 2;
roof_t = 2;
standoff_h = 5;    // room for the pins under the board
inner_h = 20;      // board top to roof: RJ45 13.5, JP1 with Dupont plugs about 17
corner_r = 3;
bay = xiao_bay ? 28 : 0;

/* [Screws: M2.5] */
screw_d = 2.9;
pilot_d = 2.2;     // self-tapping into the boss; 3.6 for a heat-set insert
head_d = 5.2;
head_h = 2.5;
screw_len = 16;

/* [Buttons] */
btn_gap = 0.3;     // nub to the board edge (the switch actuators end there)
btn_z = 1.0;       // actuator height above the board
tongue_w = 4.4;
tongue_h = 9;
tongue_t = 1.2;

$fn = 40;

z_pcb = floor_t + standoff_h;   // board bottom
zt = z_pcb + pcb_t;             // board top
z_s = zt - 0.3;                 // split between the shells
z_roof = zt + inner_h;
z_top = z_roof + roof_t;
x0 = -clr - bay - wall;
x1 = pcb_w + clr + wall;
y0 = -clr - wall;
y1 = pcb_d + clr + wall;
boss_depth = screw_len - (z_pcb - head_h) - pcb_t + 1;

function Y(yp) = pcb_d - yp;    // board drawing (y from the back edge) to case y

// ---- what is where on the board (README.md: sources) ----
buttons = [["BOOT", 7.5], ["RST", 13.7], ["PWR", 80.4]];       // side switches SW1, SW2, SW3 at the back edge
usb_y = [Y(5.85), Y(16.9), Y(27.25)];                          // right edge; the front one (next to RJ45) is the CH340 console
cn2_y = Y(37.8);                                               // UART connector CN2, right edge
rj_y = 9.0;                                                    // RJ45 centre, right edge
front = [["I2C", 19.75, 8.5, 5.0], ["RS485", 30.1, 11.6, 6.5], ["J5", 41.85, 11.6, 6.5]];  // label, x, width, height
sd_x = 54;                                                     // TF card slot, back edge
mic = [20.5, Y(3)];                                            // microphone MIC1 (top port)
xiao = [-clr - bay + 1.5, 31 - 8.9];                           // XIAO board front left corner (21 x 17.8)
// carrier board on JP1 (local KiCad project, not in the repository): socket on the JP1 pins, XIAO and
// supercaps under it in the bay; outline and heights as in its design.py
car_z = zt + 2.5 + 8.5;                                        // carrier bottom: JP1 plastic + socket body
car_xiao = [-26.6, 21.1];                                      // XIAO on its bottom, USB-C to the left wall
car_usb_z = car_z - 1.0 - 1.6;                                 // USB-C centre: module PCB, half the receptacle
car_sma_y = 55;                                                // SMA bulkhead behind the carrier
car_sc_h = 14;                                                 // supercap length below the carrier (max)

// ---- helpers ----
module rbox(xa, ya, xb, yb, za, zb, r) {
    translate([xa + r, ya + r, za]) linear_extrude(zb - za) offset(r) square([xb - xa - 2 * r, yb - ya - 2 * r]);
}

// opening seen from outside: w wide, from y=-1 up to h, top corners rounded
module notch2d(w, h, r = 1) {
    hull() {
        translate([-w / 2, -1]) square([w, 1]);
        for (s = [-1, 1]) translate([s * (w / 2 - r), h - r]) circle(r);
    }
}

// openings from the shell split upwards, through the right, front, back or left wall
module cut_right(yc, w, h, r = 1) {
    translate([pcb_w + clr - 1, yc, z_s]) rotate([90, 0, 90]) linear_extrude(wall + 6) notch2d(w, h + 0.3, r);
}
module cut_front(xc, w, h, r = 1) {
    translate([xc, -clr + 1, z_s]) rotate([90, 0, 0]) linear_extrude(wall + 2) notch2d(w, h + 0.3, r);
}
module cut_back(xc, w, h, r = 1) {
    translate([xc, y1 + 1, z_s]) rotate([90, 0, 0]) linear_extrude(wall + 2) notch2d(w, h + 0.3, r);
}

module shell(za, zb, open_top) {
    difference() {
        rbox(x0, y0, x1, y1, za, zb, corner_r);
        if (open_top) rbox(x0 + wall, y0 + wall, x1 - wall, y1 - wall, floor_t, zb + 1, corner_r - wall);
        else rbox(x0 + wall, y0 + wall, x1 - wall, y1 - wall, za - 1, z_roof, corner_r - wall);
    }
}

// ---- bottom shell ----
module xiao_cradle() {
    // rim around the XIAO, open at the USB end and towards the board (wires to JP1 and the back pads)
    translate([xiao[0] - 1.2, xiao[1] - 1.2, floor_t]) difference() {
        cube([21 + 2.4, 17.8 + 2.4, 1.5 + 1.0 + 0.8]);
        translate([1.2 - 0.2, 1.2 - 0.2, 1.5]) cube([21.4, 18.2, 5]);
        translate([2.2, 2.2, -1]) cube([21 - 2, 17.8 - 2, 5]);
        translate([-1, 5, -1]) cube([4, 17.8 + 2.4 - 10, 6]);
        translate([21, 5, -1]) cube([4, 17.8 + 2.4 - 10, 6]);
    }
}

module ear(x, dir) {
    difference() {
        hull() {
            translate([x - dir * 1.5, (y0 + y1) / 2 - 8, 0]) cube([1.5, 16, 3], center = false);
            translate([x + dir * 6, (y0 + y1) / 2, 0]) cylinder(r = 5, h = 3);
        }
        translate([x + dir * 6, (y0 + y1) / 2, -1]) cylinder(d = 4.2, h = 5);
        translate([x + dir * 6, (y0 + y1) / 2, 1.2]) cylinder(d1 = 4.2, d2 = 8.4, h = 2.1);
    }
}

module bottom() {
    difference() {
        union() {
            shell(0, z_s, true);
            for (h = holes) translate([h[0], h[1], 0]) cylinder(d = 6, h = z_pcb);
            if (xiao_bay && !xiao_carrier) xiao_cradle();
            if (ears) { ear(x0, -1); ear(x1, 1); }
        }
        for (h = holes) translate([h[0], h[1], -1]) {
            cylinder(d = screw_d, h = z_pcb + 2);
            cylinder(d = head_d, h = head_h + 1);
        }
        // vents under the module and the power parts
        for (i = [0:7]) translate([18 + i * 5, 14, -1]) hull() {
            cylinder(d = 2, h = floor_t + 2);
            translate([0, 34, 0]) cylinder(d = 2, h = floor_t + 2);
        }
        // XIAO USB-C, open to the top of the bottom shell
        if (xiao_bay && !xiao_carrier) translate([x0 - 1, xiao[1] + 8.9, z_s + 0.01]) rotate([90, 0, 90])
            linear_extrude(wall + 2) mirror([0, 1]) notch2d(9.8, z_s - floor_t - 1.6);
    }
}

// ---- top shell ----
module tongue_cut(xc) {
    s = 0.6;
    // two slots up from the free bottom edge of the wall: the tongue hangs from the wall above
    for (sx = [-1, 1]) translate([xc + sx * (tongue_w + s) / 2 - s / 2, pcb_d + clr - 1, z_s - 1]) cube([s, wall + 2, tongue_h + 1]);
    // thinner from outside: flexes, and the finger finds it
    translate([xc - tongue_w / 2, pcb_d + clr + tongue_t, z_s - 1]) cube([tongue_w, wall, tongue_h + 1]);
}

module nub(xc) {
    d = clr - btn_gap;
    if (d > 0.05) translate([xc - 1.25, pcb_d + btn_gap, zt + btn_z - 0.8]) cube([2.5, d + 0.01, 1.6]);
}

module engrave(x, y, s, txt, rot = 0) {
    translate([x, y, z_top - 0.6]) linear_extrude(1) rotate(rot)
        text(txt, size = s, font = "Liberation Sans:style=Bold", halign = rot ? "left" : "center", valign = "center");
}

module top(bosses = true) {
    difference() {
        union() {
            shell(z_s, z_top, false);
            if (bosses) for (h = holes) translate([h[0], h[1], zt]) cylinder(d1 = 5, d2 = 6.5, h = z_roof - zt + 0.01);
            for (b = buttons) nub(b[1]);
        }
        if (bosses) for (h = holes) translate([h[0], h[1], zt - 1]) cylinder(d = pilot_d, h = boss_depth + 1);
        // right: USB-C x3, CN2, RJ45
        for (y = usb_y) cut_right(y, 9.8, 3.2 + 0.7);
        cut_right(cn2_y, 12, 5.5);
        cut_right(rj_y, 16.8, 13.5 + 0.7);
        // front: I2C (CN3), RS485 (J4), J5
        for (f = front) cut_front(f[1], f[2], f[3]);
        // back: TF card, buttons, microphone slots
        cut_back(sd_x, 14, 2.6, 0.5);
        translate([sd_x, y1, z_s]) scale([1, 0.5, 1]) cylinder(r = 7, h = 6);  // finger recess
        for (b = buttons) tongue_cut(b[1]);
        for (i = [-1:1]) translate([mic[0] + i * 1.8 - 0.5, pcb_d + clr - 1, zt + 4]) cube([1, wall + 2, 8]);
        // roof: vents over the module, microphone grille
        for (i = [0:7]) translate([24 + i * 4.5, 14, z_roof - 1]) hull() {
            cylinder(d = 2, h = roof_t + 2);
            translate([0, 26, 0]) cylinder(d = 2, h = roof_t + 2);
        }
        translate([mic[0], mic[1] - 1, z_roof - 1]) {
            cylinder(d = 1.6, h = roof_t + 2, $fn = 16);
            for (a = [0:60:300]) rotate(a) translate([2.2, 0, 0]) cylinder(d = 1.6, h = roof_t + 2, $fn = 16);
        }
        // XIAO antenna: SMA bulkhead (U.FL pigtail)
        if (xiao_bay && !xiao_carrier) translate([x0 - 1, xiao[1] + 8.9, z_s + 9]) rotate([0, 90, 0]) cylinder(d = 6.6, h = wall + 2);
        if (xiao_bay && xiao_carrier) {
            // USB-C of the XIAO under the carrier, and the SMA bulkhead behind the carrier
            translate([x0 - 1, car_xiao[1] + 8.9, car_usb_z]) rotate([90, 0, 90])
                linear_extrude(wall + 2) offset(1) square([9.8 - 2, 4.2 - 2], center = true);
            translate([x0 - 1, car_sma_y, z_s + 9]) rotate([0, 90, 0]) cylinder(d = 6.6, h = wall + 2);
        }
        // labels
        for (b = buttons) engrave(b[1], pcb_d - 0.5, 2.6, b[0], -90);
        engrave(pcb_w - 4, usb_y[2] + 4.5, 2.4, "UART", -90);
        for (f = front) engrave(f[1], 2.5, 2.6, f[0]);
    }
}

// ---- board, for checking (positions as above, heights typical) ----
module board() {
    color("#1b2a3a") difference() {
        translate([0, 0, z_pcb]) linear_extrude(pcb_t) offset(2) offset(-2) square([pcb_w, pcb_d]);
        for (h = holes) translate([h[0], h[1], z_pcb - 1]) cylinder(d = 3.2, h = 4);
    }
    color("silver") {
        translate([13.5, Y(53), zt]) cube([31.5, 36, 3.2]);                   // P4 module
        translate([73.2, rj_y - 8, zt]) cube([rj_front - 73.2, 16, 13.5]);    // RJ45
        for (y = usb_y) translate([usb_front - 7.4, y - 4.47, zt]) cube([7.4, 8.94, 3.2]);
        translate([46.3, Y(14.9), zt]) cube([15.7, 14.7, 1.9]);               // TF card
    }
    color("ivory") {
        translate([86, cn2_y - 5.5, zt]) cube([5.5, 11, 4.5]);
        for (f = front) translate([f[1] - f[2] / 2 + 0.6, 0, zt]) cube([f[2] - 1.2, 5.5, f[3] - 0.8]);
        for (b = buttons) translate([b[1] - 2, pcb_d - 3, zt]) cube([4, 3, 2]);
    }
    color("black") translate([0.3, Y(48.2), zt]) cube([4.3, 33, 8.5]);         // JP1 2x13
    color("dimgray") translate([mic[0] - 1.9, mic[1] - 1.4, zt]) cube([3.8, 2.8, 1.1]);
    if (xiao_bay && !xiao_carrier) color("seagreen") translate([xiao[0], xiao[1], floor_t + 1.5]) cube([21, 17.8, 1]);
    if (xiao_bay && xiao_carrier) {
        color("darkgreen") translate([0, 0, car_z]) linear_extrude(1.6)
            polygon([[-27.6, 48.5], [10.8, 48.5], [10.8, 12], [-1, 12], [-1, 1], [-27.6, 1]]);
        color("black") translate([-0.1, 13.8, zt + 2.5]) cube([5.1, 33, 8.5]);              // socket J1
        color("seagreen") translate([car_xiao[0], car_xiao[1], car_z - 3.5]) cube([21, 17.8, 3.5]);  // XIAO
        color("silver") translate([car_xiao[0] - 1.5, car_xiao[1] + 8.9 - 4.5, car_usb_z - 1.6]) cube([7.4, 9, 3.2]);
        color("navy") for (c = [-22.2, -11.4]) translate([c, 6.3, car_z - car_sc_h]) cylinder(d = 10, h = car_sc_h);
        color("navy") for (c = [-22.6, -12.8]) translate([c - 3.3, 15.7 - 3.3, car_z - 7.7]) cube([6.6, 6.6, 7.7]);
        color("dimgray") translate([-26.5, 10, car_z + 1.6]) cube([25, 10, 1.2]);           // switch parts on top
    }
}

if (part == "assembly") {
    bottom();
    board();
    %top();
} else if (part == "bottom") {
    bottom();
} else if (part == "top") {
    translate([0, 0, z_top]) rotate([180, 0, 0]) top();
} else if (part == "fit_test") {
    // the lowest 9 mm of the top shell: put it over the board to check openings and buttons
    intersection() {
        top(false);
        translate([x0 - 10, y0 - 10, z_s - 1]) cube([x1 - x0 + 20, y1 - y0 + 20, zt + 9 - z_s + 1]);
    }
} else if (part == "board") {
    board();
} else if (part == "clash") {
    // must be empty: the board model against both shells
    intersection() { board(); union() { bottom(); top(); } }
}
