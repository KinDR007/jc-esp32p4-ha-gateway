# Test results

| Date | Firmware | Result | Report |
|---|---|---|---|
| 2026-10-28 | gateway `c712ab9` | TC01–TC13 automated: 10/10 in the final regression; M01 partly (OTBR in HA) | [2026-10-28/report.md](2026-10-28/report.md) |
| 2026-10-28 | gateway with sound, layout v2 | TC04–TC16 with sound on: all PASS after test fixes | [2026-10-28-sound/report.md](2026-10-28-sound/report.md) |

Runs of 2026-10-28, in order:

| File | Content |
|---|---|
| `hw_test_run1.*` | first run after a clean flash: TC01–TC03 pass, then the gateway stopped answering after the C6 backup (not reproduced since) |
| `hw_test_run2.*` | TC04–TC13: TC07 fails (Zigbee router link baud) |
| `hw_test_tc07.json`, `hw_test_run3.json` | TC07, TC10, TC12 after the fixes |
| `hw_test_final.*` | regression: TC04 timing, TC09 ESPHome slot wiped by TC10 (both fixed) |
| `hw_test_final2.*` | final regression, 10/10 PASS |
| `serial_first_boot.log` | first boot of the gateway on the board |

New runs: `tests/hw_test.py --report tests/results/<date>/hw_test.json`, copy the log next to it
and add a row above.
