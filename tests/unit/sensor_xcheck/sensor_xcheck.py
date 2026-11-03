"""Compensation of the gateway drivers (C) against the Adafruit CircuitPython libraries on the same register
bytes: BME680/BME688 and BMP388/BMP390, which have no worked example in their datasheets. Not in CI.
    python3 -m venv /tmp/ada && /tmp/ada/bin/pip install --no-deps adafruit-circuitpython-bme680 adafruit-circuitpython-bmp3xx
    python3 tests/unit/sensor_xcheck/sensor_xcheck.py /tmp/ada/lib/python3.*/site-packages
Known difference: Adafruit composes the BME680 humidity calibration H1/H2 with the nibbles of register 0xE2
swapped against the Bosch API (which the gateway follows); the test data use equal nibbles."""
import os, struct, subprocess, sys, tempfile, time
if len(sys.argv) < 2:
    sys.exit(__doc__)
HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, '..', '..', '..'))
sys.path[:0] = [os.path.join(HERE, 'stubs'), sys.argv[1]]
RUN = os.path.join(tempfile.mkdtemp(), 'run')
subprocess.run(['cc', '-std=c11', '-w', '-I', os.path.join(ROOT, 'firmware/gateway/components/sensors'), '-o', RUN,
                os.path.join(HERE, 'run.c'), '-lm'], check=True)
time.sleep = lambda s: None
import adafruit_bme680, adafruit_bmp3xx

def regmap_bme680(variant, adc_t, adc_p, adc_h, gas_adc, gas_range):
    m = bytearray(256)
    m[0xd0] = 0x61; m[0xf0] = variant
    # par_* of a real-looking part
    t1, t2, t3 = 26364, 26314, 3
    p1, p2, p3, p4, p5, p6, p7, p8, p9, p10 = 36474, -10454, 88, 6624, -117, 30, 27, -1883, -2919, 30
    h1, h2, h3, h4, h5, h6, h7 = 760, 1032, 0, 45, 20, 120, -100  # same low nibble: Adafruit takes the E2 nibbles the other way round than Bosch
    g1, g2, g3 = -36, -11463, 18
    struct.pack_into('<hb', m, 0x8a, t2, t3)
    struct.pack_into('<Hhb', m, 0x8e, p1, p2, p3)
    struct.pack_into('<hhbb', m, 0x94, p4, p5, p7, p6)
    struct.pack_into('<hhB', m, 0x9c, p8, p9, p10)
    m[0xe1] = h2 >> 4; m[0xe2] = ((h2 & 0x0f) << 4) | (h1 & 0x0f); m[0xe3] = h1 >> 4
    struct.pack_into('<bbbBb', m, 0xe4, h3, h4, h5, h6, h7)
    struct.pack_into('<Hhbb', m, 0xe9, t1, g2, g1, g3)
    m[0x00] = 40; m[0x02] = 0x10; m[0x04] = 0x00
    m[0x1d] = 0x80
    m[0x1f:0x22] = bytes([adc_p >> 12, (adc_p >> 4) & 0xff, (adc_p & 0xf) << 4])
    m[0x22:0x25] = bytes([adc_t >> 12, (adc_t >> 4) & 0xff, (adc_t & 0xf) << 4])
    m[0x25:0x27] = struct.pack('>H', adc_h)
    g = 0x2c if variant else 0x2a
    m[g] = gas_adc >> 2; m[g + 1] = ((gas_adc & 3) << 6) | 0x30 | gas_range
    return m

class Bme(adafruit_bme680.Adafruit_BME680):
    def __init__(self, m):
        self.m = m; super().__init__()
    def _read(self, reg, n): return bytearray(self.m[reg:reg + n])
    def _write(self, reg, vals):
        for i, v in enumerate(vals):
            if reg != 0xe0: self.m[reg + i] = v

class Bmp3(adafruit_bmp3xx.BMP3XX):
    def __init__(self, m):
        self.m = m; super().__init__()
    def _read_register(self, reg, n): return bytearray(self.m[reg:reg + n])
    def _write_register_byte(self, reg, v):
        if reg != 0x7e: self.m[reg] = v

def c_side(m, drv):
    path = RUN + '.bin'
    open(path, 'wb').write(bytes(m))
    out = subprocess.run([RUN, path, drv], capture_output=True, text=True).stdout.split('\n')
    return {l.split()[0]: float(l.split()[1]) for l in out[1:] if l}

fail = 0
def cmp(name, a, b, tol):
    global fail
    ok = abs(a - b) <= tol
    fail += not ok
    print(f'{"ok " if ok else "BAD"} {name}: C {a:.4f}  Adafruit {b:.4f}')

for variant in (0, 1):
    for adc_t, adc_p, adc_h, gas, rng in ((500000, 400000, 25000, 300, 5), (480000, 380000, 30000, 700, 9)):
        m = regmap_bme680(variant, adc_t, adc_p, adc_h, gas, rng)
        c = c_side(bytearray(m), 'bme680')
        a = Bme(bytearray(m))
        print(f'-- BME68{8 if variant else 0} adc_t {adc_t}')
        cmp('temperature', c['temperature'], a.temperature, 0.02)
        cmp('pressure hPa', c['pressure'], a.pressure, 0.05)
        cmp('humidity', c['humidity'], a.humidity, 0.1)
        cmp('gas ohm (rel)', c['gas_resistance'] / a.gas, 1.0, 0.02)
        a._amb_temp = 25
        cmp('res_heat 320 C', c['res_heat_0'], a._calc_res_heat(320), 1)

for chip, cal, adc in ((0x60, (27000, 19000, -7, -4000, -3000, 30, 0, 25000, 30000, -5, -8, 15000, 4, -60), (8000000, 6600000)),
                       (0x50, (27500, 18800, -10, 3500, 2500, 25, -2, 25500, 30500, 4, -10, 17000, 2, -55), (7200000, 8100000))):
    m = bytearray(256)
    m[0x00] = chip; m[0x03] = 0x70
    struct.pack_into('<HHbhhbbHHbbhbb', m, 0x31, *cal)
    p, t = adc
    m[0x04:0x07] = p.to_bytes(3, 'little'); m[0x07:0x0a] = t.to_bytes(3, 'little')
    c = c_side(bytearray(m), 'bmp3xx')
    pa, tc = Bmp3(bytearray(m))._read()
    print(f'-- BMP3 0x{chip:02x}')
    cmp('temperature', c['temperature'], tc, 0.01)
    cmp('pressure hPa', c['pressure'], pa / 100, 0.01)
print('FAILED' if fail else 'all match')
sys.exit(1 if fail else 0)
