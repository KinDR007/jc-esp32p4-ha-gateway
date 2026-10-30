# Documentation

User documentation of the P4 Gateway in Czech and English, styled like the web UI:
open [index.html](index.html), [cs/index.html](cs/index.html) or [en/index.html](en/index.html) in a browser.

| Path | Content |
|---|---|
| `cs/`, `en/` | the pages (generated, do not edit) |
| `assets/` | style (tokens of the web UI dark theme) and icon |
| `screenshots/` | web UI screenshots, whole pages and single cards; `index.json` lists the cards per page |

Regenerate:

1. Screenshots (needs a gateway and an admin account without 2FA; switching roles is done by hand
   between the phases, the gateway restarts each time):

   ```sh
   export P4GW_URL=http://p4gw-xxxxxx.local P4GW_USER=<admin> P4GW_PASS=<password>
   PHASE=thread node <browser-automation skill>/browser.mjs $P4GW_URL --script tools/docs/shoot.mjs   # Thread border router role
   PHASE=zigbee node <browser-automation skill>/browser.mjs $P4GW_URL --script tools/docs/shoot.mjs   # Zigbee coordinator role
   PHASE=router node <browser-automation skill>/browser.mjs $P4GW_URL --script tools/docs/shoot.mjs   # Zigbee router role
   ```

2. Pages: `python3 tools/docs/build_docs.py` (texts in that script, card names and icons from the web UI).

Before committing new screenshots, check them for secrets: the Thread network key and API tokens
are never shown unless asked for, but log lines and addresses are.
