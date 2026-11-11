# HTTP API

Generated from [firmware/gateway/web/api.json](../firmware/gateway/web/api.json) by `tools/docs/api_md.py`; do not edit by hand. The gateway serves the same description at `GET /api/help`, and its web UI shows it under System › API with examples built for that gateway (its address, a token typed in there, its radios and roles) and a button that tries GET requests.

## Authentication

A gateway without any user account is open: no sign-in is needed and every endpoint works (create the first admin with POST /api/users). Once an account exists, every endpoint except /api/help, /api/auth/state, /api/auth/login and /api/auth/reset-request needs one of three proofs, tried in this order: an API token sent as Authorization: Bearer <token> (an admin creates it in System > User management > API tokens, or with POST /api/tokens, and it has the role chosen then), HTTP Basic with user name and password (for clients of older versions; works only for users without two-factor sign-in), or the session cookie p4s that POST /api/auth/login sets for the web UI. Missing or wrong credentials give 401; insufficient rights give 403 forbidden; an account that must change its password or set up two-factor gets 403 password_change_required or totp_setup_required. Each endpoint's perm is the page of the web UI it belongs to and level 1 (read) or 2 (edit); GET needs the read level, other methods edit, admin means admins only, public means no sign-in, * means any signed-in user, self means the own account. All errors are JSON {"error": "message"}; successful replies are JSON unless noted.

Errors come as `{"error": "..."}` with 400 (invalid input), 401 (sign-in needed), 403 (permission), 404, 409 (not possible in the current state) or 413 (upload too large). Permissions name the web UI page whose right a user needs (read or edit); see [users.md](users.md).

```sh
curl -s -H 'Authorization: Bearer p4gw_...' http://192.168.1.10/api/status
```

## Endpoints

### Status and system

| Method | Path | What it does | Permission |
|---|---|---|---|
| GET | `/api/help` | This API description (the content of api.json), no sign-in needed. | no sign-in |
| POST | `/api/identify` | Makes the identify icon blink in the web UI header, to find the gateway. | page `overview`, edit |
| GET | `/api/status` | Full gateway state: firmware, uptime, memory, roles of both radios, Ethernet, flash slots, radio firmware, hardware, update and link state. | any signed-in client |
| GET | `/api/ws` | WebSocket push channel for Home Assistant: the gateway sends text frames, frames from the client are read and dropped. | any signed-in client |

#### GET `/api/help`

Response:

```json
{
  "version": 1,
  "auth": {
    "en": "...",
    "cs": "..."
  },
  "groups": [
    {
      "id": "status",
      "title": {
        "en": "Status and system",
        "cs": "Stav a systém"
      },
      "endpoints": [
        "..."
      ]
    }
  ]
}
```

Effects: Sent gzip-compressed (Content-Encoding: gzip), cached for an hour. It contains no secrets.

#### POST `/api/identify`

Body:

```json
{
  "seconds": 10
}
```

Response:

```json
{
  "identify_s": 10
}
```

Effects: seconds is clamped to 0..120 (default 10); 0 stops the blinking.

#### GET `/api/status`

Response:

```json
{
  "hostname": "p4gw-a1b2c3",
  "version": "v1.9",
  "idf": "v5.5.1",
  "built": "Nov  2 2026",
  "built_time": "10:15:00",
  "time": 1793610000,
  "tz_name": "Europe/Prague",
  "matter_url": "ws://192.168.1.10:5580/ws",
  "chip_revision": 301,
  "uptime_s": 120,
  "reset_reason": "software",
  "auth": true,
  "temperature_c": 48.3,
  "heap": {
    "internal_free": 120000,
    "internal_min": 90000,
    "internal_largest": 65536,
    "psram_free": 20000000,
    "psram_heap": 30000000,
    "psram_parts": [
      {
        "name": "c6fw",
        "bytes": 2097152
      }
    ]
  },
  "role": {
    "active": "thread_br",
    "desired": "thread_br",
    "maintenance": "",
    "stable": true,
    "b": {
      "active": "none",
      "desired": "none",
      "maintenance": "",
      "wired": true,
      "chip": "ESP32-C5",
      "bundle": "",
      "chip_info": {
        "flashes": 0
      }
    }
  },
  "eth": {
    "...": "link, speed, IP addresses"
  },
  "slots": [
    {
      "label": "ota_0",
      "size": 3145728,
      "project": "p4gw",
      "version": "v1.9",
      "state": "valid",
      "running": true,
      "boot": true
    }
  ],
  "c6": {
    "bundle": "ot_rcp",
    "adopted": true,
    "images": [
      {
        "name": "ot_rcp",
        "version": "1.2",
        "size": 600000
      }
    ],
    "chip": {
      "mac": "aa:bb:cc:dd:ee:ff",
      "revision": 0,
      "flash_size": 4194304,
      "flashes": 1
    },
    "flash": {
      "busy": false,
      "percent": 0,
      "stage": "",
      "last_error": ""
    },
    "snapshot_days": 1,
    "snapshots": []
  },
  "hw": {
    "cpu_mhz": 360,
    "cores": 2,
    "cpu_load": [
      12.5,
      8.1
    ],
    "tasks": 40,
    "flash_size": 16777216,
    "psram_size": 33554432,
    "mac_base": "aa:bb:cc:dd:ee:ff",
    "app_slot": "ota_0",
    "nvs": {
      "used": 300,
      "total": 700,
      "namespaces": 12
    },
    "http_register_errors": 0
  },
  "update": {
    "installed_version": "v1.9",
    "latest_version": "v1.9",
    "available": false
  },
  "identify_s": 0,
  "links": {
    "mqtt": "connected",
    "ha_clients": 1,
    "internet": true,
    "internet_age_s": 20
  },
  "role_status": {
    "mode": "border_router",
    "phase": "running"
  },
  "role_status_b": {
    "...": "status summary of the role on radio B, when it has one"
  }
}
```

Effects: Read only. role_status and role_status_b hold the short status of the role running on radio A and B.

#### GET `/api/ws`

Response:

```json
{
  "topic": "notify",
  "payload": "{\"seq\":12,\"topic\":\"system\",\"level\":\"info\",\"text\":\"...\"}"
}
```

Errors: 401 Handshake without valid sign-in.

Effects: Upgrade request; the sign-in is checked in the handshake. Each frame is a JSON object with topic and payload (payload is a string). Up to 48 messages wait in a queue, newer ones are dropped when it is full.

### Sign-in and own account

| Method | Path | What it does | Permission |
|---|---|---|---|
| POST | `/api/auth/login` | Signs in and sets the session cookie p4s (HttpOnly, SameSite=Strict). | no sign-in |
| POST | `/api/auth/logout` | Ends the current session and clears the cookie. | own account |
| GET | `/api/auth/me` | The signed-in identity with its page levels. | own account |
| POST | `/api/auth/password` | Changes the own password. | own account |
| POST | `/api/auth/reset-request` | Asks the admins to reset a forgotten password; no sign-in needed. | no sign-in |
| GET | `/api/auth/sessions` | Lists the own sessions. | own account |
| POST | `/api/auth/sessions/revoke` | Ends all own sessions except the current one. | own account |
| GET | `/api/auth/state` | Login page state, no sign-in needed. | no sign-in |
| POST | `/api/auth/totp/disable` | Turns two-factor off for the own account. | own account |
| POST | `/api/auth/totp/enable` | Confirms the pending secret with a code and turns two-factor on. | own account |
| POST | `/api/auth/totp/setup` | Starts two-factor setup: creates a pending secret and a QR code. | own account |

#### POST `/api/auth/login`

Body:

```json
{
  "user": "jiri",
  "password": "secret-password",
  "code": "123456",
  "remember": true
}
```

Response:

```json
{
  "name": "jiri",
  "role": "admin",
  "must_change": false,
  "totp": true,
  "totp_required": false,
  "recovery_left": 10,
  "levels": {
    "overview": 2,
    "role": 2,
    "system/maint": 2
  }
}
```

Errors: 401 Unknown user, wrong password or wrong code.; 403 Account locked (body has error: locked and retry_s) or disabled.; 409 No user exists, the gateway is open.

Effects: With two-factor on and no code the reply is {code_required: true, clock_set} and no session is created. code is a 6-digit authenticator code or a recovery code. remember keeps the session for the policy's remember_d days. Too many failures lock the account and send a notification; at most 24 sessions are kept, the oldest is dropped.

#### POST `/api/auth/logout`

Response:

```json
{}
```

Effects: Does nothing for token and Basic clients, they have no session.

#### GET `/api/auth/me`

Response:

```json
{
  "name": "jiri",
  "role": "admin",
  "must_change": false,
  "totp": false,
  "totp_required": false,
  "recovery_left": 0,
  "levels": {
    "overview": 2,
    "system/maint": 2
  },
  "requests": 0
}
```

Effects: Token clients get {name, role, token: true, levels}. Without any user account: {name: "", role: "admin", open: true}. requests counts open password reset requests (admins only, else 0).

#### POST `/api/auth/password`

Body:

```json
{
  "old": "old-password",
  "new": "new-password-123"
}
```

Response:

```json
{
  "name": "jiri",
  "role": "admin",
  "must_change": false,
  "totp": false
}
```

Errors: 400 New password outside the policy length (password_min to 64 characters).; 403 Current password is wrong.; 409 Not a signed-in user (token or open gateway).

Effects: Clears must_change, ends all other sessions of the user and sends a notification.

#### POST `/api/auth/reset-request`

Body:

```json
{
  "user": "jiri"
}
```

Response:

```json
{}
```

Errors: 409 More than 4 requests in 10 minutes from anyone.

Effects: The answer is the same for unknown names. For a known user it stores the request (name, time, client IP) and sends a security notification; an admin answers by setting a new password.

#### GET `/api/auth/sessions`

Response:

```json
[
  {
    "id": 3,
    "current": true,
    "remember": false,
    "idle_s": 12
  }
]
```

Effects: Empty for token clients and the open gateway.

#### POST `/api/auth/sessions/revoke`

Response:

```json
{}
```

#### GET `/api/auth/state`

Response:

```json
{
  "enabled": true,
  "clock_set": true,
  "hostname": "p4gw-a1b2c3"
}
```

Effects: enabled is false while no user account exists (open gateway).

#### POST `/api/auth/totp/disable`

Body:

```json
{
  "password": "current-password"
}
```

Response:

```json
{
  "name": "jiri",
  "role": "admin",
  "must_change": false,
  "totp": false
}
```

Errors: 403 Wrong password.; 409 Two-factor is not on, or the policy requires it for the role.

Effects: Erases the secret and recovery codes; sends a warning notification.

#### POST `/api/auth/totp/enable`

Body:

```json
{
  "code": "123456"
}
```

Response:

```json
{
  "recovery_codes": [
    "ABCD-EFGH",
    "JKMN-PQRS"
  ]
}
```

Errors: 400 Invalid code.; 409 Setup not started, or the gateway clock is not set (no SNTP).

Effects: Returns 10 one-time recovery codes, shown only now. Ends the user's other sessions.

#### POST `/api/auth/totp/setup`

Response:

```json
{
  "secret": "JBSWY3DPEHPK3PXP",
  "uri": "otpauth://totp/P4%20Gateway:jiri@p4gw-a1b2c3?secret=JBSWY3DPEHPK3PXP&issuer=P4%20Gateway&digits=6&period=30",
  "qr_svg": "<svg ...>",
  "clock_set": true
}
```

Errors: 409 Not a signed-in user.

Effects: The secret takes effect only after /api/auth/totp/enable with a valid code.

### Users, roles and tokens

| Method | Path | What it does | Permission |
|---|---|---|---|
| GET | `/api/auth/policy` | Sign-in policy: sessions, lockout, password length, required two-factor. | administrators |
| POST | `/api/auth/policy` | Changes the sign-in policy; send only the fields to change. | administrators |
| GET | `/api/auth/requests` | Open password reset requests. | administrators |
| POST | `/api/auth/requests/dismiss` | Dismisses the reset request of a user. | administrators |
| GET | `/api/auth/roles` | Page levels of the roles user and viewer, with factory defaults. | administrators |
| POST | `/api/auth/roles` | Sets page levels of the roles user and viewer. | administrators |
| GET | `/api/tokens` | Lists API tokens (the secret value is never shown again). | administrators |
| POST | `/api/tokens` | Creates an API token. | administrators |
| POST | `/api/tokens/delete` | Revokes an API token. | administrators |
| GET | `/api/users` | Lists user accounts and the page names used for levels. | administrators |
| POST | `/api/users` | Creates a user, or changes an existing one (by name). | administrators |
| POST | `/api/users/delete` | Deletes a user account. | administrators |

#### GET `/api/auth/policy`

Response:

```json
{
  "idle_h": 12,
  "remember_d": 30,
  "lock_attempts": 5,
  "lock_min": 15,
  "password_min": 8,
  "totp_required": [
    "admin"
  ]
}
```

#### POST `/api/auth/policy`

Body:

```json
{
  "idle_h": 12,
  "remember_d": 30,
  "lock_attempts": 5,
  "lock_min": 15,
  "password_min": 10,
  "totp_required": [
    "admin",
    "user"
  ]
}
```

Response:

```json
{
  "idle_h": 12,
  "remember_d": 30,
  "lock_attempts": 5,
  "lock_min": 15,
  "password_min": 10,
  "totp_required": [
    "admin",
    "user"
  ]
}
```

Errors: 400 A field is out of range (the message names it): idle_h 1..720, remember_d 0..365, lock_attempts 3..20, lock_min 1..1440, password_min 8..64; totp_required must list admin, user or viewer.

Effects: totp_required replaces the whole list; users of such a role must set up two-factor before using the API (403 totp_setup_required). Nothing is applied when any field is invalid.

#### GET `/api/auth/requests`

Response:

```json
[
  {
    "name": "petr",
    "time": 1793600000,
    "ip": "192.168.1.20"
  }
]
```

#### POST `/api/auth/requests/dismiss`

Body:

```json
{
  "name": "petr"
}
```

Response:

```json
[]
```

Effects: Returns the remaining requests like GET.

#### GET `/api/auth/roles`

Response:

```json
{
  "user": {
    "overview": 2,
    "system/maint": 1
  },
  "viewer": {
    "overview": 1,
    "system/maint": 0
  },
  "defaults": {
    "user": {
      "overview": 2
    },
    "viewer": {
      "overview": 1
    }
  }
}
```

Effects: Levels: 0 hidden, 1 read, 2 edit. The admin role always has full access and is not listed.

#### POST `/api/auth/roles`

Body:

```json
{
  "user": {
    "system/firmware": 1
  },
  "viewer": {
    "thread/settings": 0
  },
  "reset": false
}
```

Response:

```json
{
  "user": {
    "overview": 2
  },
  "viewer": {
    "overview": 1
  },
  "defaults": {
    "...": "..."
  }
}
```

Effects: Only the listed pages with values 0..2 change; unknown pages are ignored. reset: true restores the factory levels of both roles. Sends a notification and returns the new state like GET.

#### GET `/api/tokens`

Response:

```json
[
  {
    "name": "homeassistant",
    "role": "admin",
    "created": 1793000000,
    "used_s_ago": 30
  }
]
```

Effects: used_s_ago is -1 for a token not used since the gateway started.

#### POST `/api/tokens`

Body:

```json
{
  "name": "homeassistant",
  "role": "admin"
}
```

Response:

```json
{
  "name": "homeassistant",
  "token": "p4gw_0123456789abcdef0123456789abcdef01234567"
}
```

Errors: 400 name missing or longer than 31 characters, or role not admin, user or viewer.; 409 A token with this name exists, or all 16 slots are used.

Effects: The token value is returned only in this response, the gateway keeps only its SHA-256 hash. Use it as Authorization: Bearer <token>. Sends a notification.

#### POST `/api/tokens/delete`

Body:

```json
{
  "name": "homeassistant"
}
```

Response:

```json
[]
```

Errors: 404 No such token.

Effects: Returns the remaining tokens like GET. Clients using it get 401 at once.

#### GET `/api/users`

Response:

```json
{
  "users": [
    {
      "name": "jiri",
      "role": "admin",
      "disabled": false,
      "must_change": false,
      "totp": false,
      "recovery_left": 0,
      "locked": false,
      "created": 1793000000,
      "last_login": 1793600000,
      "sessions": 1,
      "levels": {
        "system/maint": 1
      }
    }
  ],
  "pages": [
    "overview",
    "role",
    "thread/info"
  ],
  "max": 16
}
```

Effects: levels lists only the pages where this user has an own level instead of the role's.

#### POST `/api/users`

Body:

```json
{
  "name": "petr",
  "role": "user",
  "password": "start-password-1",
  "must_change": true,
  "disabled": false,
  "totp_reset": false,
  "unlock": false,
  "levels": {
    "system/maint": 1,
    "thread/settings": -1
  }
}
```

Response:

```json
{
  "name": "petr",
  "role": "user",
  "disabled": false,
  "must_change": true,
  "totp": false,
  "locked": false
}
```

Errors: 400 Invalid name (2 to 31 characters a-z, 0-9, . _ -), role (admin, user, viewer) or password length; first account must be an admin; no room (max 16); the last admin cannot lose the role or be disabled.

Effects: A new name creates the account (must_change defaults to true, except for the first account); role and password are required then. For an existing name every field is optional: password sets a new one (ends the user's sessions, answers a reset request), disabled blocks sign-in, totp_reset removes two-factor, unlock clears a lockout, levels maps page to 0 hidden, 1 read, 2 edit, or -1 for the role's level. Works without sign-in only to create the very first account on an open gateway. Sends a notification.

#### POST `/api/users/delete`

Body:

```json
{
  "name": "petr"
}
```

Response:

```json
{}
```

Errors: 400 The last admin cannot be deleted while other users exist.; 404 No such user.

Effects: Ends the user's sessions and sends a warning notification. Deleting the only remaining account makes the gateway open again.

### Roles and radios

| Method | Path | What it does | Permission |
|---|---|---|---|
| GET | `/api/c6/backup` | Downloads a full flash dump of a radio. | page `system/maint`, edit |
| POST | `/api/c6/reflash` | Marks a radio to be reflashed with the firmware of its role and restarts the gateway. | page `system/maint`, edit |
| GET | `/api/radios` | State of both radios: chip, flashed firmware, flashing progress, role. | page `system/maint`, read |
| GET | `/api/radios/b/console` | Raw text the radio B prints on its UART (ROM banner, boot log, panic), for diagnostics. | page `system/maint`, edit |
| POST | `/api/radios/b/flash` | Flashes firmware of a chosen bundle into radio B (only while it has no role). | page `system/maint`, edit |
| POST | `/api/radios/b/probe` | Wiring test of radio B: resets it and listens on the UART. | page `system/maint`, edit |
| POST | `/api/radios/thread/forget` | Erases the stored Thread network (dataset and settings) of a radio, so its next Thread role starts a new network. | page `thread/settings`, edit |
| POST | `/api/role` | Switches the role of a radio, or swaps the roles of the two radios. | page `role`, edit |

#### GET `/api/c6/backup`

Query: `radio=a|b` Radio, default a.

Response: application/octet-stream c6_flash_backup.bin (radio_b_flash_backup.bin for b).

Errors: 409 The radio has a role (maintenance mode only) or the radio is busy.; 503 Radio did not answer before the transfer started (message is the flasher's last error).

Effects: Reads the radio flash over the boot ROM, takes some minutes. A successful dump marks the radio's factory firmware as backed up (adopted), which unlocks role changes. A failure after the transfer started aborts the connection.

#### POST `/api/c6/reflash`

Query: `radio=a|b` Radio, default a.

Response:

```json
{}
```

Errors: 409 Radio B has no role.

Effects: Sets a force flag in NVS and restarts after 0.5 s; the radio is flashed at the next start.

#### GET `/api/radios`

Response:

```json
[
  {
    "radio": "a",
    "chip": "ESP32-C6",
    "bundle": "ot_rcp",
    "package_id": "1a2b3c4d",
    "busy": false,
    "percent": 0,
    "stage": "",
    "error": "",
    "chip_info": {
      "mac": "aa:bb:cc:dd:ee:01",
      "revision": 0,
      "flash_size": 4194304,
      "flashes": 1
    },
    "images": [
      {
        "name": "ot_rcp",
        "version": "1.2"
      }
    ],
    "role": "thread_br",
    "desired": "thread_br",
    "maintenance": ""
  },
  {
    "radio": "b",
    "chip": "ESP32-C5",
    "bundle": "",
    "busy": false,
    "role": "none",
    "desired": "none",
    "maintenance": "",
    "wired": true,
    "wiring": "...",
    "package_in_psram": "c5",
    "telemetry": {
      "...": "..."
    }
  }
]
```

Effects: Array of two objects, radio a first. Radio B also has wired and wiring after a wiring test, telemetry and package_in_psram.

#### GET `/api/radios/b/console`

Query: `baud=9600..2000000` Baud rate, default 115200., `ms=100..10000` How long to listen, default 3000., `reset=0|1` 1 resets the radio first., `raw=0|1` 1 keeps binary bytes; 0 replaces them with dots.

Response: text/plain console output (application/octet-stream with raw=1), up to 8 KB.

Errors: 400 baud or ms out of range.; 409 Radio B is in use (role, flashing).; 500 UART unavailable.

Effects: Holds the request for ms milliseconds.

#### POST `/api/radios/b/flash`

Body:

```json
{
  "bundle": "ot_rcp",
  "force": false
}
```

Response:

```json
{}
```

Errors: 400 bundle is not ot_rcp, zigbee_ncp, zigbee_router, scanner, esphome or wifi_uplink.; 409 Radio B runs a role, or is already being flashed.; 500 No memory for the flashing task.

Effects: Answers at once and flashes in a background task; follow it in GET /api/radios (busy, percent, stage, error). force: true flashes even when the bundle is already there.

#### POST `/api/radios/b/probe`

Response:

```json
{
  "wired": true,
  "banner": true,
  "download_mode": false,
  "verdict": "ESP32-C5 answers",
  "chip": "ESP32-C5",
  "mac": "aa:bb:cc:dd:ee:02",
  "revision": 0,
  "flash_size": 8388608
}
```

Errors: 409 Radio B has a role or is being flashed.

Effects: Blocks the web server for a few seconds. Stores the result (wired, chip) in the runtime state shown in /api/status.

#### POST `/api/radios/thread/forget`

Body:

```json
{
  "radio": "a"
}
```

Response:

```json
{}
```

Errors: 400 radio missing or not a or b.; 409 Thread currently runs on that radio.

Effects: Erases OpenThread settings in NVS and the stored dataset of that radio; sends a notification.

#### POST `/api/role`

Body:

```json
{
  "role": "zigbee_coordinator",
  "radio": "a",
  "skip_backup": false,
  "swap": false
}
```

Response:

```json
{
  "role": "zigbee_coordinator",
  "restarting": true
}
```

Errors: 400 Unknown role (none is allowed for radio B only) or radio not a or b.; 409 Conflict between radios (one Thread network, one Zigbee network, one ESPHome proxy, one Wi-Fi uplink), c6_backup_required or radio_b_backup_required (factory firmware not backed up yet), radio flashing in progress, or swap without a Wi-Fi uplink / without radio B.

Effects: Roles: thread_br, thread_router, zigbee_coordinator, zigbee_router, esphome, wifi_uplink, none (radio B only). radio defaults to a. skip_backup: true accepts running without a backup of the factory firmware. swap: true ignores role and radio and exchanges the roles of A and B (needs the Wi-Fi uplink on one radio). Stores the role in NVS, sends a notification and restarts the gateway after 0.5 s; the radio is reflashed at start if its firmware differs.

### Thread

| Method | Path | What it does | Permission |
|---|---|---|---|
| POST | `/api/thread/cli` | Runs an OpenThread CLI command and returns its output. | page `thread/diag`, edit |
| GET | `/api/thread/dataset` | The active Thread dataset. | page `thread/settings`, edit |
| POST | `/api/thread/dataset` | Applies a Thread dataset (join or replace the network). | page `thread/settings`, edit |
| GET | `/api/thread/status` | Thread network and OpenThread state. | page `thread/info`, read |
| GET | `/api/thread/topology` | Discovers the Thread mesh: routers, their children and IPv6 addresses. | page `thread/topology`, read |
| POST | `/api/thread/txpower` | Sets the transmit power of the Thread radio. | page `thread/settings`, edit |

#### POST `/api/thread/cli`

Body:

```json
{
  "cmd": "state",
  "wait_ms": 600
}
```

Response:

```json
{
  "output": "leader\r\nDone\r\n"
}
```

Errors: 400 cmd missing or longer than 200 characters.; 409 Another CLI command is running.; 503 OpenThread not running.

Effects: wait_ms (100..15000, default 600) is how long the output is collected, for commands that answer later such as ping or scan. Any CLI command can change the network. Registered only while this role runs on a radio, otherwise the server answers 404.

#### GET `/api/thread/dataset`

Query: `reveal=1` 1 includes the network key and the TLV hex string; without it network_key is "hidden".

Response:

```json
{
  "network_key": "hidden",
  "network_name": "OpenThread-1234",
  "channel": 15,
  "pan_id": "0x1234",
  "ext_pan_id": "dead00beef00cafe",
  "active_timestamp": 1
}
```

Errors: 404 No active dataset.; 503 OpenThread not running.

Effects: Needs edit level because it can reveal the network key. Registered only while this role runs on a radio, otherwise the server answers 404.

#### POST `/api/thread/dataset`

Body:

```json
{
  "tlvs": "0e080000000000010000000300000f35060004001fffe0"
}
```

Response:

```json
{
  "network_name": "OpenThread-1234"
}
```

Errors: 400 tlvs missing or not hex, not a valid active dataset or without a network key, or OpenThread rejected it.; 503 OpenThread not running.

Effects: tlvs is the hex active dataset (as in Home Assistant). A router role also stores it in NVS to join after restarts. Registered only while this role runs on a radio, otherwise the server answers 404.

#### GET `/api/thread/status`

Response:

```json
{
  "mode": "border_router",
  "rcp_failures": 0,
  "rest_port": 8081,
  "phase": "running",
  "role": "leader",
  "rcp_version": "OPENTHREAD/...",
  "tx_power": 8,
  "tx_power_set": false,
  "network_name": "OpenThread-1234",
  "channel": 15,
  "pan_id": "0x1234",
  "ext_pan_id": "dead00beef00cafe",
  "ext_address": "...",
  "rloc16": "0x0800",
  "partition_id": 123456,
  "leader_router_id": 2,
  "mesh_local_prefix": "fd00:db8::/64",
  "border_router": {
    "routing": "running",
    "omr_prefix": "...",
    "on_link_prefix": "...",
    "nat64": "idle",
    "border_agent": true,
    "srp_server": "running",
    "srp_hosts": 3,
    "srp_services": 5
  },
  "neighbors": [
    {
      "ext_address": "...",
      "rloc16": "0x0801",
      "type": "router",
      "avg_rssi": -60,
      "lqi_in": 3
    }
  ],
  "routers": [
    {
      "id": 2,
      "rloc16": "0x0800",
      "path_cost": 1,
      "link": true
    }
  ],
  "mac_counters": {
    "tx_total": 100,
    "rx_total": 120
  }
}
```

Effects: Registered only while this role runs on a radio, otherwise the server answers 404. When OpenThread is not up, only mode, rcp_failures and phase are returned.

#### GET `/api/thread/topology`

Response:

```json
{
  "routers": [
    {
      "rloc16": "0x0400",
      "id": 1,
      "ext": "6a9bc32b75bee2ef",
      "version": 5,
      "leader": false,
      "br": false,
      "this": false,
      "ip6": [
        "fd35:4095:6f9e:1:bc13:e798:63d:1a2b"
      ],
      "links": [
        {
          "id": 5,
          "lq": 3
        }
      ],
      "children": [
        {
          "rloc16": "0x0401",
          "lq": 3,
          "rx_on": false,
          "ftd": false,
          "br": false,
          "ip6": [
            "fd35:4095:6f9e:1::1"
          ]
        }
      ],
      "vendor": "Nanoleaf",
      "model": "NL67",
      "sw": "4.1.3"
    }
  ],
  "error": "ResponseTimeout",
  "ms": 10119
}
```

Errors: 409 OpenThread not running, or a discovery is already running.

Effects: Slow: OpenThread mesh diagnostics with a 10 s answer timeout per router (30 s in all), then a query of the IPv6 addresses of each router's children (12 s at most each). routers[]: rloc16, router id, ext (extended address), version (Thread version, when known), leader, br (border router), this (this gateway), ip6, links (router id and link quality 1..3 to each neighbouring router), children (rloc16, lq, rx_on: always listening, ftd: full Thread device, br, ip6) and vendor, model, sw when the device reports them. error names the OpenThread error when a router did not answer (the map is then partial). Registered only while a radio runs a Thread role, otherwise 404.

#### POST `/api/thread/txpower`

Body:

```json
{
  "dbm": 8
}
```

Response:

```json
{
  "tx_power": 8
}
```

Errors: 400 dbm outside -15..20.; 409 OpenThread not running.

Effects: Applied at once and stored in NVS (restored at start). Registered only while this role runs on a radio, otherwise the server answers 404.

### Zigbee

| Method | Path | What it does | Permission |
|---|---|---|---|
| GET | `/api/c6/zigbee-snapshot` | Downloads the stored backup (NVRAM snapshot) of the Zigbee network of a radio. | page `zigbee/backups`, edit |
| POST | `/api/c6/zigbee-snapshot` | Takes a new backup of the Zigbee network now. | page `zigbee/backups`, edit |
| POST | `/api/zigbee/command` | Sends a command to the Zigbee router firmware. | page `zigbee/diag`, edit |
| GET | `/api/zigbee/net` | Zigbee2MQTT view over MQTT: instances, devices, states and the last network map. | page `zigbee/network`, read |
| POST | `/api/zigbee/net/base` | Sets the MQTT base topic of Zigbee2MQTT. | page `zigbee/network`, edit |
| POST | `/api/zigbee/net/map` | Asks Zigbee2MQTT for a network map. | page `zigbee/network`, edit |
| POST | `/api/zigbee/net/request` | Runs an operation through Zigbee2MQTT: permit join, rename, remove, interview, identify, firmware update. | page `zigbee/network`, edit |
| POST | `/api/zigbee/reset` | Resets the Zigbee radio. | page `zigbee/diag`, edit |
| GET | `/api/zigbee/status` | Status of the Zigbee role (coordinator bridge or router). | page `zigbee/info`, read |

#### GET `/api/c6/zigbee-snapshot`

Query: `radio=a|b` Radio, default a., `bundle=zigbee_ncp|zigbee_router` Which firmware's network, default zigbee_ncp.

Response: application/octet-stream <bundle>_nvram_<generation>.bin (radio_b_ prefix for b); it holds the network key.

Errors: 404 No Zigbee snapshot.

#### POST `/api/c6/zigbee-snapshot`

Response:

```json
{
  "restarting": true
}
```

Errors: 409 No Zigbee role runs, or the radio is being flashed.

Effects: Sets a request flag and restarts the gateway after 1 s; the backup is taken at start before the role runs, so Zigbee is out for about a minute.

#### POST `/api/zigbee/command`

Body:

```json
{
  "cmd": "permit"
}
```

Response:

```json
{}
```

Errors: 400 cmd not one of status, steer, permit, leave, factory_reset, txpower; for txpower value outside -15..20.

Effects: Only in the zigbee_router role (not registered in the coordinator role). For txpower send {"cmd": "txpower", "value": 8}. leave and factory_reset remove the router from its network. The reply is empty; the result shows in /api/zigbee/status.

#### GET `/api/zigbee/net`

Response:

```json
{
  "mqtt": true,
  "instances": [
    {
      "...": "..."
    }
  ],
  "base": "zigbee2mqtt",
  "devices": [
    {
      "friendly_name": "lamp"
    }
  ],
  "states": {
    "lamp": {
      "state": "ON"
    }
  },
  "map": null,
  "map_running": false,
  "map_error": "",
  "now": 1793610000
}
```

Effects: Always available (also when no Zigbee role runs); empty until Zigbee2MQTT publishes on the MQTT broker. The exact shape of devices and states follows Zigbee2MQTT.

#### POST `/api/zigbee/net/base`

Body:

```json
{
  "base": "zigbee2mqtt"
}
```

Response:

```json
{
  "mqtt": true,
  "base": "zigbee2mqtt",
  "...": "same as GET /api/zigbee/net"
}
```

Errors: 400 base empty, too long or containing + or #.

Effects: Stored in NVS. A changed base clears the cached devices, states and map and resubscribes.

#### POST `/api/zigbee/net/map`

Response:

```json
{
  "started": true
}
```

Errors: 409 No Zigbee2MQTT found on the broker, or a map request is already running.; 503 MQTT not connected.

Effects: Returns at once; the map arrives later in GET /api/zigbee/net (map, map_running, map_error). Gives up after 10 minutes.

#### POST `/api/zigbee/net/request`

Body:

```json
{
  "action": "permit_join",
  "time": 120,
  "device": "lamp",
  "to": "new-name",
  "force": false
}
```

Response:

```json
{
  "status": "ok",
  "data": {
    "time": 120
  },
  "transaction": "p4gw-3"
}
```

Errors: 400 Unknown action or a required device missing.; 409 No Zigbee2MQTT on the broker, or another operation is running.; 503 MQTT not connected.

Effects: action: permit_join (time 0..254 s, default 254; optional device to open one router), rename (device, to), remove (device, force), interview (device), ota_check (device), identify (device), ota_update (device). The reply is the Zigbee2MQTT response, or {status: "sent", error: "no answer from Zigbee2MQTT yet"} when it does not answer in time; identify and ota_update answer {status: "sent"} / {status: "started"} without waiting. remove deletes the device from the network.

#### POST `/api/zigbee/reset`

Response:

```json
{}
```

Effects: Coordinator: drops the connected Zigbee2MQTT client, which has to reinitialise the NCP. Registered only while this role runs on a radio, otherwise the server answers 404.

#### GET `/api/zigbee/status`

Response:

```json
{
  "mode": "coordinator",
  "uart_baud": 460800,
  "rx_bytes": 123456,
  "tx_bytes": 65432,
  "uart_errors": 0,
  "c6_resets": 1,
  "tcp_port": 6638,
  "client_connected": true,
  "client": "192.168.1.10",
  "session_s": 3600,
  "sessions": 2,
  "frames_rx": 5000,
  "frames_tx": 4800,
  "dropped_bytes": 0,
  "telemetry_frames": 100,
  "telemetry_bad": 0,
  "last_rx_s": 0.5,
  "last_tx_s": 1.2
}
```

Effects: Router mode returns mode, the UART counters, router (the last status object of the router firmware, or null) and router_status_age_s instead of the TCP bridge fields. Registered only while this role runs on a radio, otherwise the server answers 404.

### ESPHome

| Method | Path | What it does | Permission |
|---|---|---|---|
| GET | `/api/esphome/wifi` | Wi-Fi network given to the ESPHome proxy radio. | page `ha/esphome`, edit |
| POST | `/api/esphome/wifi` | Sets the Wi-Fi network for the ESPHome proxy radio. | page `ha/esphome`, edit |

#### GET `/api/esphome/wifi`

Response:

```json
{
  "ssid": "MyWifi",
  "password_set": true
}
```

Effects: The password is never returned.

#### POST `/api/esphome/wifi`

Body:

```json
{
  "ssid": "MyWifi",
  "password": "wifi-password"
}
```

Response:

```json
{
  "ssid": "MyWifi",
  "password_set": true
}
```

Errors: 400 ssid longer than 32 characters or missing, or password not 8..64 characters (empty for an open network).

Effects: Stored in NVS; the radio is provisioned over Improv at its next start in the esphome role. Leaving password out keeps the old one. The Wi-Fi uplink role falls back to this network when it has none of its own.

### Sound recognition

| Method | Path | What it does | Permission |
|---|---|---|---|
| GET | `/api/sound/classes` | Names of the 521 sound classes. | any signed-in client |
| GET | `/api/sound/clip` | Downloads the latest seconds of microphone audio as a WAV file. | page `sound/live`, edit |
| POST | `/api/sound/config` | Changes microphone and recognition settings and the noise level calibration; send only the fields to change. | page `sound/live`, edit |
| GET | `/api/sound/events` | Configured sound events with live counters, plus the list of presets. | page `sound/events`, read |
| PUT | `/api/sound/events` | Replaces the whole event configuration. | page `sound/events`, edit |
| POST | `/api/sound/events/preset` | Replaces the event configuration with a preset. | page `sound/events`, edit |
| DELETE | `/api/sound/learn` | Deletes one sample slot, or all samples of a profile. | page `sound/learn`, edit |
| GET | `/api/sound/learn` | Learned sounds: profiles, samples, background recordings and the recording state. | page `sound/learn`, read |
| POST | `/api/sound/learn/record` | Starts recording a sample of a profile, or background recordings, from the microphone. | page `sound/learn`, edit |
| GET | `/api/sound/learn/sample` | Downloads a stored sample as a WAV file. | page `sound/learn`, edit |
| POST | `/api/sound/learn/upload` | Adds a sample or background from an uploaded WAV, or tests a WAV against the profiles. | page `sound/learn`, edit |
| GET | `/api/sound/log` | Log of recent detections (up to 64), newest first. | page `sound/log`, read |
| GET | `/api/sound/scores` | Latest scores of all 521 sound classes. | page `sound/live`, read |
| GET | `/api/sound/status` | State of the microphone, noise level and sound recognition. | page `sound/live`, read |

#### GET `/api/sound/classes`

Response: application/json [[index, name, category, group], ...] (AudioSet classes).

#### GET `/api/sound/clip`

Query: `seconds=1..9` Length, default 5.

Response: audio/wav p4gw_clip.wav (16 kHz, mono, 16-bit).

Errors: 409 Microphone is off.

Effects: Needs edit level because it is a recording of the room.

#### POST `/api/sound/config`

Body:

```json
{
  "enabled": true,
  "gain_db": 30,
  "gate_db": -60,
  "inference": true,
  "spl_reference": 45.0
}
```

Response:

```json
{
  "enabled": true,
  "running": true,
  "gain_db": 30,
  "gate_db": -60,
  "restart_required": false
}
```

Errors: 400 spl_reference outside 20..130 dB(A) or spl_offset outside 60..220.; 409 spl_reference given while the microphone is off.

Effects: enabled turns the microphone on or off in NVS; a change needs a restart (restart_required: true, the gateway does not restart by itself). gain_db 0..42 and gate_db -90..0 (quiet gate for recognition) apply at once and are clamped. inference switches recognition on or off. spl_reference is the reading of a sound level meter next to the gateway in dB(A) and calibrates the offset; spl_offset sets the offset directly; spl_reset: true drops the calibration. The reply is the status like GET /api/sound/status plus restart_required.

#### GET `/api/sound/events`

Response:

```json
{
  "preset": "household",
  "exclude": [
    0,
    1
  ],
  "events": [
    {
      "id": "doorbell",
      "name": "Doorbell",
      "name_cs": "Zvonek",
      "kind": "impulse",
      "device_class": "sound",
      "profile": "",
      "enabled": true,
      "threshold": 0.5,
      "windows": 1,
      "hold_s": 3,
      "cooldown_s": 0,
      "classes": [
        380
      ],
      "suppress": [],
      "active": false,
      "count": 4,
      "last_ago_s": 600
    }
  ],
  "presets": [
    {
      "key": "household",
      "name": "Household"
    }
  ]
}
```

Effects: active, count and last_ago_s are runtime values and are not stored.

#### PUT `/api/sound/events`

Body:

```json
{
  "preset": "custom",
  "exclude": [
    0,
    1
  ],
  "events": [
    {
      "id": "doorbell",
      "name": "Doorbell",
      "name_cs": "Zvonek",
      "kind": "impulse",
      "device_class": "sound",
      "profile": "",
      "enabled": true,
      "threshold": 0.5,
      "windows": 1,
      "hold_s": 3,
      "cooldown_s": 0,
      "classes": [
        380
      ],
      "suppress": []
    }
  ]
}
```

Response:

```json
{
  "preset": "custom",
  "events": [
    "..."
  ],
  "presets": [
    "..."
  ]
}
```

Errors: 400 Invalid JSON or configuration: events not an array of at most 32; id not a-z, 0-9, _ (max 23) or duplicated; class index outside 0..520; profile not a-z, 0-9, _.; 413 Body empty or over 32 kB.

Effects: Per event: kind impulse|continuous, enabled (default true), threshold 0.05..1 (0.5), windows 1..20 (1), hold_s 1..600 (3), cooldown_s 0..3600 (0), classes and suppress are class indexes 0..520, profile names a learned sound. Stored in NVS. Events that disappear or get disabled are removed from Home Assistant discovery. The reply is the new state like GET.

#### POST `/api/sound/events/preset`

Body:

```json
{
  "preset": "household"
}
```

Response:

```json
{
  "preset": "household",
  "events": [
    "..."
  ],
  "presets": [
    "..."
  ]
}
```

Errors: 400 Unknown preset (household, security, animals, minimal).

Effects: Overwrites all current events and stores the result. Returns the new state like GET /api/sound/events.

#### DELETE `/api/sound/learn`

Query: `slot=0..63` Slot number to delete., `profile=profile id|_background` Delete all samples of the profile, or all background recordings with _background. Used when slot is not given.

Response:

```json
{
  "deleted": 1
}
```

Errors: 404 No such sample.; 409 A recording is running.

Effects: Erases flash sectors of the sounds partition and rebuilds the profiles.

#### GET `/api/sound/learn`

Response:

```json
{
  "slots_total": 64,
  "slots_free": 58,
  "ready": true,
  "recording": {
    "active": false,
    "result": "sample saved (-30.2 dBFS)",
    "ok": true
  },
  "profiles": [
    {
      "id": "my_bell",
      "type": "embedding",
      "samples": 3,
      "self_min": 0.82,
      "background_max": 0.61,
      "threshold": 0.75,
      "separable": true,
      "slots": [
        {
          "slot": 4,
          "level_db": -30.2,
          "saved": 1793600000
        }
      ]
    }
  ],
  "background": [
    {
      "slot": 10,
      "level_db": -50.1,
      "saved": 1793600100
    }
  ]
}
```

Effects: ready is true only while the microphone, the model and recognition all run. Tone profiles also have tone_hz. score appears for 5 s after a profile matched.

#### POST `/api/sound/learn/record`

Body:

```json
{
  "profile": "my_bell",
  "kind": "sample",
  "type": "embedding"
}
```

Response:

```json
{
  "seconds": 4
}
```

Errors: 409 Recognition not running, a recording is running, invalid profile name (a-z, 0-9, _, max 23), profile exists with the other type, or no free sample slot.

Effects: kind sample (default) keeps the loudest 0.96 s window within about 4 s and needs profile; kind background records 5 windows (about 5 s) of room sound and ignores profile. type embedding (default) or tone. Returns at once; follow it in GET /api/sound/learn (recording).

#### GET `/api/sound/learn/sample`

Query: `slot=0..63` Slot number.

Response: audio/wav (16 kHz, mono, 16-bit, 0.96 s).

Errors: 404 No such sample.

#### POST `/api/sound/learn/upload`

Query: `profile=a-z, 0-9, _ (max 23)` Profile id (not needed for background or test)., `type=embedding|tone` Profile type, default embedding., `kind=sample|background` Default sample., `test=1` 1 scores the WAV against all profiles and saves nothing.

Body: the raw file (`sample.wav`).

Response:

```json
{
  "result": "sample saved (-28.4 dBFS)"
}
```

Errors: 400 Not a 16 kHz mono 16-bit PCM WAV of 1 to 10 s, or processing failed.; 409 Same conditions as record: recognition not running, a recording running, bad profile, no free slot.; 503 Recognition did not process the recording in 30 s.

Effects: Raw WAV bytes as the request body (not JSON). Sample keeps the loudest window, background up to 5 windows. With test=1 the reply is {windows, scores: {profile: 0..1}} (best score per profile).

#### GET `/api/sound/log`

Response:

```json
[
  {
    "ago_s": 30,
    "event": "doorbell",
    "class": 380,
    "class_name": "Doorbell",
    "score": 0.91,
    "level_db": -32.5,
    "duration_s": 1.9
  }
]
```

Effects: event is empty for a strong class that no event covers. Ongoing detections have ongoing: true instead of duration_s.

#### GET `/api/sound/scores`

Response:

```json
{
  "age_s": 0.4,
  "scores": [
    0,
    1,
    85,
    0
  ]
}
```

Effects: scores is an array of 521 integers, 0..100 (percent), in class index order; age_s is -1 before the first recognition.

#### GET `/api/sound/status`

Response:

```json
{
  "enabled": true,
  "running": true,
  "error": "",
  "gain_db": 30,
  "gate_db": -60,
  "level_db": -45.2,
  "peak_db": -30.1,
  "floor_db": -55.0,
  "seconds": 3600.5,
  "read_errors": 0,
  "spl": {
    "las": 38.2,
    "laeq": 40.1,
    "lamax": 62.3,
    "overload": false,
    "window_s": 60,
    "partial": false,
    "offset": 120.0,
    "calibrated": false
  },
  "model": {
    "stored_bytes": 4000000,
    "loaded": true,
    "load_ms": 900,
    "error": ""
  },
  "inference": {
    "enabled": true,
    "runs": 1200,
    "skipped_quiet": 300,
    "avg_ms": 120.5,
    "cpu_pct": 18.0,
    "top": [
      {
        "index": 0,
        "name": "Speech",
        "score": 0.82
      }
    ]
  }
}
```

Effects: level_db, peak_db, floor_db and spl appear only while the microphone runs; inference.top only after the first recognition.

### Network

| Method | Path | What it does | Permission |
|---|---|---|---|
| GET | `/api/net/lan` | Ethernet settings and the state of an unconfirmed change. | page `system/lan`, read |
| POST | `/api/net/lan` | Changes Ethernet settings (DHCP or static, link speed). Must be confirmed or it is undone. | page `system/lan`, edit |
| POST | `/api/net/lan/confirm` | Confirms a pending LAN change and stores it. | page `system/lan`, edit |
| POST | `/api/net/lan/outage` | Switches Ethernet off for a while, to test the Wi-Fi uplink taking over. | page `system/maint`, edit |
| GET | `/api/net/scan` | State and result of the last radio environment scan (Wi-Fi, 802.15.4 channels). | page `system/netscan`, read |
| POST | `/api/net/scan` | Starts a quick scan (Thread role) or a full scan with the scanner firmware. | page `system/netscan`, edit |
| GET | `/api/net/traffic` | Ethernet traffic counters per service, since boot. | page `system/lan`, read |
| GET | `/api/net/wifi` | Settings and status of the Wi-Fi uplink. | page `system/lan`, read |
| POST | `/api/net/wifi` | Sets the Wi-Fi network of the uplink. | page `system/lan`, edit |
| POST | `/api/net/wifi/restart` | Restarts the Wi-Fi uplink radio. | page `system/lan`, edit |
| GET | `/api/net/wifi/scan` | Wi-Fi networks in range, seen by the uplink radio. | page `system/lan`, edit |
| GET | `/api/net/wifi/traffic` | Traffic counters of the Wi-Fi uplink. | page `system/lan`, read |

#### GET `/api/net/lan`

Response:

```json
{
  "mode": "static",
  "link": "auto",
  "ip": "192.168.1.50",
  "mask": "255.255.255.0",
  "gw": "192.168.1.1",
  "dns1": "192.168.1.1",
  "confirm_s": 0
}
```

Effects: confirm_s is the time left to confirm a pending change, 0 when none waits. ip, mask, gw, dns1, dns2 appear only in static mode.

#### POST `/api/net/lan`

Body:

```json
{
  "mode": "static",
  "ip": "192.168.1.50",
  "mask": "255.255.255.0",
  "gw": "192.168.1.1",
  "dns1": "192.168.1.1",
  "dns2": "",
  "link": "auto"
}
```

Response:

```json
{
  "mode": "static",
  "link": "auto",
  "ip": "192.168.1.50",
  "confirm_s": 120
}
```

Errors: 400 mode not dhcp or static; address or netmask missing or invalid; gateway outside the subnet; link not auto, 100full, 100half, 10full or 10half.

Effects: The new settings apply after 0.5 s, so the connection may drop. They are stored only when POST /api/net/lan/confirm is called (from the new address) within 120 s; otherwise the previous settings return, also after a restart.

#### POST `/api/net/lan/confirm`

Response:

```json
{
  "mode": "static",
  "link": "auto",
  "ip": "192.168.1.50",
  "confirm_s": 0
}
```

Errors: 409 No LAN change waits for confirmation.

Effects: Writes the settings to NVS and sends a notification.

#### POST `/api/net/lan/outage`

Body:

```json
{
  "seconds": 90
}
```

Response:

```json
{
  "seconds": 90
}
```

Errors: 400 seconds outside 10..600.

Effects: Answers, then stops the Ethernet driver for the given time; the web UI is unreachable over LAN meanwhile.

#### GET `/api/net/scan`

Response:

```json
{
  "state": "idle",
  "last": {
    "mode": "full",
    "at": 1793600000,
    "duration_ms": 90000,
    "radio": "b",
    "scanner": "1.0",
    "wifi": [
      {
        "ssid": "MyWifi",
        "rssi": -55,
        "channel": 6
      }
    ],
    "beacons": [],
    "channels": [],
    "errors": []
  },
  "quick_available": true,
  "now": 1793610000
}
```

Effects: last is null before the first scan. state is idle, quick or full. The exact contents of wifi, beacons and channels come from the scanner firmware and the Thread scan and are not fixed here.

#### POST `/api/net/scan`

Body:

```json
{
  "mode": "full",
  "radio": "b"
}
```

Response:

```json
{
  "restarting": true
}
```

Errors: 400 mode not quick or full, or radio not a or b.; 409 A scan is running; quick needs a Thread role; full on radio B needs radio B detected first (wiring test).; 500 No memory.

Effects: quick runs in the background and answers {started: true}. full stores a request in NVS and restarts the gateway after 0.8 s; at start the radio (a by default) is flashed with the scanner firmware, scans and the gateway restores the normal role. Takes minutes during which the radio's role is not running.

#### GET `/api/net/traffic`

Response:

```json
{
  "uptime_ms": 3600000,
  "services": {
    "http": {
      "rx": 1000,
      "tx": 5000,
      "rx_packets": 10,
      "tx_packets": 12
    }
  },
  "rx": 123456,
  "tx": 234567,
  "rx_packets": 900,
  "tx_packets": 1000
}
```

Effects: services is keyed by service name; rx and tx are bytes.

#### GET `/api/net/wifi`

Response:

```json
{
  "settings": {
    "ssid": "MyWifi",
    "password_set": true,
    "from_esphome": false,
    "mode": "dhcp"
  },
  "status": {
    "mode": "wifi_uplink",
    "state": "connected",
    "error": "",
    "radio": "b",
    "cp_version": "1.0.0",
    "ssid": "MyWifi",
    "bssid": "aa:bb:cc:dd:ee:ff",
    "rssi": -55,
    "channel": 36,
    "band": "5 GHz",
    "phy": "11ax",
    "connected_s": 600,
    "connects": 1,
    "disconnects": 0,
    "ip_mode": "dhcp",
    "default_route": false,
    "ipv4": "192.168.1.60",
    "gateway": "192.168.1.1",
    "netmask": "255.255.255.0"
  }
}
```

Effects: Always available. status describes the uplink even when it is not the active role (state then reflects that). The password is never returned.

#### POST `/api/net/wifi`

Body:

```json
{
  "ssid": "MyWifi",
  "password": "wifi-password",
  "mode": "static",
  "ip": "192.168.1.60",
  "mask": "255.255.255.0",
  "gw": "192.168.1.1",
  "dns1": "192.168.1.1",
  "dns2": ""
}
```

Response:

```json
{
  "settings": {
    "ssid": "MyWifi",
    "password_set": true,
    "mode": "static"
  },
  "status": {
    "...": "..."
  }
}
```

Errors: 400 ssid missing or over 32 characters, password not 8..64 characters (empty for open), or invalid IP settings.

Effects: Stored in NVS and applied at once when the uplink runs. password left out keeps the old one. ssid: "" drops the own setting so the ESPHome Wi-Fi is used again. Sends a notification.

#### POST `/api/net/wifi/restart`

Body:

```json
{}
```

Response:

```json
{}
```

Errors: 400 hang_test_s outside 0..60.; 409 The Wi-Fi uplink does not run.

Effects: The body is optional. {"hang_test_s": 20} holds the radio in reset for that many seconds without telling the host, to test the watchdog; the host then restarts it by itself.

#### GET `/api/net/wifi/scan`

Response:

```json
[
  {
    "ssid": "MyWifi",
    "rssi": -55,
    "channel": 36,
    "open": false
  }
]
```

Errors: 409 The Wi-Fi uplink does not run.; 500 Scan failed.

Effects: Blocking scan, at most 40 networks.

#### GET `/api/net/wifi/traffic`

Response:

```json
{
  "uptime_ms": 600000,
  "services": {
    "mqtt": {
      "rx": 100,
      "tx": 200,
      "rx_packets": 3,
      "tx_packets": 4
    }
  },
  "rx": 1000,
  "tx": 2000,
  "rx_packets": 20,
  "tx_packets": 25
}
```

Effects: Same shape as /api/net/traffic; uptime_ms counts from the first uplink start and is 0 if it never started.

### MQTT and Home Assistant

| Method | Path | What it does | Permission |
|---|---|---|---|
| GET | `/api/ha/discover` | Finds Home Assistant instances on the LAN (mDNS _home-assistant._tcp). | any signed-in client |
| GET | `/api/mqtt` | MQTT client settings and connection state. | page `ha/mqtt`, read |
| POST | `/api/mqtt` | Changes MQTT settings; send only the fields to change. Reconnects the client. | page `ha/mqtt`, edit |

#### GET `/api/ha/discover`

Response:

```json
[
  {
    "name": "Home",
    "port": 8123,
    "ip": "192.168.1.10",
    "version": "2026.10.0",
    "internal_url": "http://192.168.1.10:8123",
    "location_name": "Home"
  }
]
```

Effects: Waits up to 3 s for answers; the list is empty when none answer. The web UI uses it to suggest the Matter server address.

#### GET `/api/mqtt`

Response:

```json
{
  "enabled": true,
  "uri": "mqtt://192.168.1.10:1883",
  "user": "p4gw",
  "password_set": true,
  "prefix": "homeassistant",
  "tls": "bundle",
  "ca": "",
  "skip_cn_check": false,
  "client_id": "",
  "keepalive": 60,
  "base": "",
  "discovery": true,
  "discovery_groups": [
    "roles",
    "system",
    "update",
    "sound",
    "sound_events",
    "sound_triggers",
    "sensors",
    "modbus",
    "gpio"
  ],
  "interval": 60,
  "base_topic": "p4gw/p4gw-a1b2c3",
  "connected": true,
  "last_error": "",
  "publishes": 120
}
```

Effects: The password is never returned (password_set). base_topic is the topic prefix actually used.

#### POST `/api/mqtt`

Body:

```json
{
  "enabled": true,
  "uri": "mqtt://192.168.1.10:1883",
  "user": "p4gw",
  "password": "mqtt-password",
  "prefix": "homeassistant",
  "tls": "bundle",
  "ca": "",
  "skip_cn_check": false,
  "client_id": "p4gw",
  "base": "",
  "keepalive": 60,
  "interval": 60,
  "discovery": true,
  "discovery_groups": [
    "roles",
    "system",
    "sound"
  ]
}
```

Response:

```json
{
  "enabled": true,
  "uri": "mqtt://192.168.1.10:1883",
  "connected": true,
  "...": "same as GET /api/mqtt"
}
```

Errors: 400 A field is invalid (the message says which): uri must start with mqtt://, mqtts://, ws:// or wss:// (max 120); user, password, client_id max 63; prefix 1..31 characters without wildcards, spaces and outer slashes; base topic without #, +, spaces and outer slashes (max 63); tls bundle or ca; ca a PEM certificate up to 3999 characters, required when tls is ca; keepalive 10..240; interval 10..3600; discovery_groups from roles, system, update, sound, sound_events, sound_triggers, sensors, modbus, gpio.

Effects: Body up to 8 kB. Stores to NVS, then stops and restarts the MQTT client. Turning discovery or a group off first removes those entities from Home Assistant. interval is the state publish period in seconds. Returns the new state like GET.

### I2C sensors

| Method | Path | What it does | Permission |
|---|---|---|---|
| GET | `/api/sensors` | I2C sensors on header JP1: configuration, state, values, the last scan and the supported types. | page `system/hw`, read |
| PUT | `/api/sensors` | Sets the read interval and/or replaces the list of sensors. | page `system/hw`, edit |
| POST | `/api/sensors/scan` | Scans the bus, identifies new chips and adds those recognised without doubt. | page `system/hw`, edit |

#### GET `/api/sensors`

Response:

```json
{
  "bus": {
    "ok": true,
    "sda": 7,
    "scl": 8,
    "khz": 100
  },
  "interval_s": 30,
  "max_sensors": 16,
  "sensors": [
    {
      "id": "i2c_76",
      "addr": 118,
      "type": "bme280",
      "chip": "BME280",
      "name": "Living room",
      "expose": true,
      "enabled": true,
      "state": "ok",
      "error": "",
      "quantities": [
        "temperature",
        "humidity",
        "pressure"
      ],
      "values": {
        "temperature": 21.4,
        "humidity": 45.2,
        "pressure": 1004.1
      },
      "age_s": 3.2,
      "reads": 120,
      "errors": 0
    }
  ],
  "scan": {
    "done": true,
    "time": 1793700000,
    "ago_s": 42,
    "devices": [
      {
        "addr": 24,
        "onboard": true,
        "configured": false,
        "hint": "ES8311 audio codec (on the board)",
        "candidates": []
      },
      {
        "addr": 68,
        "onboard": false,
        "configured": false,
        "hint": "",
        "candidates": [
          {
            "type": "sht4x",
            "chip": "SHT4x",
            "sure": true
          }
        ]
      },
      {
        "addr": 118,
        "onboard": false,
        "configured": true,
        "hint": "DPS310, SPL06, MS5611 (not supported)",
        "candidates": []
      }
    ]
  },
  "types": [
    {
      "type": "bme280",
      "label": "BME280",
      "vendor": "Bosch",
      "note": "GY-BME280 (0x76), Adafruit and SparkFun (0x77)",
      "addrs": [
        118,
        119
      ],
      "quantities": [
        "temperature",
        "humidity",
        "pressure"
      ]
    }
  ],
  "quantities": [
    {
      "key": "temperature",
      "name": "Temperature",
      "unit": "°C",
      "device_class": "temperature",
      "precision": 1
    }
  ]
}
```

Effects: values holds only fresh readings (none for a sensor that stopped answering; age_s is then missing). state: ok, starting, stale (no new value for three periods), error (error says why; the sensor is started again every 60 s) or off. scan.devices lists every address that answered: onboard is the board's codec, candidates the chips the identification found (sure: an ID register or ID command matched; otherwise the chip only answers like it). types lists all supported chips with their addresses; quantities the units and Home Assistant device classes. bus2 {sda, scl, khz} while a second bus is set up; every sensor and scanned device has its bus (1 or 2).

#### PUT `/api/sensors`

Body:

```json
{
  "interval_s": 30,
  "sensors": [
    {
      "addr": 118,
      "type": "bme280",
      "name": "Living room",
      "expose": true,
      "enabled": true
    }
  ]
}
```

Response:

```json
{
  "...": "same as GET /api/sensors"
}
```

Errors: 400 interval_s not 5..3600; more than 16 sensors; addr outside 0x08..0x77, 0x18 (the codec) or not an address of the type; unknown type; an address twice; name over 32 characters or with quotes, braces or backslash.

Effects: sensors replaces the whole list (send the ones to keep too). name defaults to the chip, expose and enabled to true; chip (the exact chip, e.g. BME688) is kept as sent. A sensor with the same address and type keeps running; removed ones stop, and their MQTT entities are removed. expose: the integration and MQTT (discovery group sensors) show it in Home Assistant. bus: 1 (the board's, default) or 2 (the second bus set up under System › GPIO); a sensor on bus 2 has the id i2c2_<addr>.

#### POST `/api/sensors/scan`

Response:

```json
{
  "added": [
    68
  ],
  "...": "same as GET /api/sensors"
}
```

Effects: Takes up to a few seconds (an SCD4x needs 0.5 s). Configured sensors and the codec are only probed, not identified. A chip is added when exactly one type is sure, or when exactly one type answers like it; otherwise it waits in scan.devices for a PUT with the chosen type. The gateway also scans at every start.

### Modbus (RS485 and TCP)

| Method | Path | What it does | Permission |
|---|---|---|---|
| GET | `/api/modbus` | RS485 bus settings and the Modbus devices with their state and values. | page `system/rs485`, read |
| PUT | `/api/modbus` | Sets the bus and/or replaces the list of devices. | page `system/rs485`, edit |
| GET | `/api/modbus/templates` | The device templates, built in and own. | page `system/rs485`, read |
| GET | `/api/modbus/template` | A whole template (?id=). | page `system/rs485`, read |
| PUT | `/api/modbus/template` | Stores an own template (new, or replacing the own one with the same id). | page `system/rs485`, edit |
| DELETE | `/api/modbus/template` | Deletes an own template (?id=). | page `system/rs485`, edit |
| POST | `/api/modbus/write` | Writes a writable entity of a device. | page `system/rs485`, edit |
| POST | `/api/modbus/raw` | Manual request: reads or writes registers or coils. | page `system/rs485`, edit |
| GET | `/api/modbus/probe` | State and result of the address search. | page `system/rs485`, read |
| POST | `/api/modbus/probe` | Starts a search of addresses (and baud rates), or stops it. | page `system/rs485`, edit |
| GET | `/api/modbus/scan` | State and result of the Modbus TCP search in a network. | page `system/rs485`, read |
| POST | `/api/modbus/scan` | Starts a Modbus TCP search in a network, or stops it. | page `system/rs485`, edit |
| GET | `/api/modbus/server` | Modbus TCP server: the devices offered in the network, ports, clients and counters. | page `system/rs485`, read |
| PUT | `/api/modbus/server` | Sets the Modbus TCP server: the shared port and the devices offered. | page `system/rs485`, edit |

#### GET `/api/modbus`

Response:

```json
{
  "bus": {
    "baud": 9600,
    "parity": "N",
    "stop": 1,
    "timeout_ms": 500,
    "ok": true,
    "uart": 3,
    "tx": 26,
    "rx": 27,
    "stats": {
      "requests": 1200,
      "timeouts": 3,
      "crc_errors": 0,
      "exceptions": 1
    }
  },
  "devices": [
    {
      "id": "d1",
      "name": "House meter",
      "template": "eastron_sdm630",
      "transport": "rtu",
      "slave": 1,
      "poll_s": 0,
      "expose": "integration",
      "enabled": true,
      "manufacturer": "Eastron",
      "model": "SDM630-Modbus V2",
      "template_rev": 3021911,
      "state": "ok",
      "error": "",
      "age_s": 2,
      "polls": 120,
      "errors": 0,
      "values": {
        "voltage_l1": 231.4,
        "total_power": 1835,
        "import_energy": 15234.112
      }
    },
    {
      "id": "d2",
      "name": "House meter as ABB B23",
      "template": "abb_b23",
      "transport": "virtual",
      "source": "d1",
      "poll_s": 0,
      "expose": "none",
      "enabled": true,
      "manufacturer": "ABB",
      "model": "B23 / B24 (EQ meter)",
      "template_rev": 1781302,
      "state": "ok",
      "error": "",
      "age_s": 2,
      "polls": 120,
      "errors": 0,
      "values": {
        "voltage_l1": 231.4,
        "power": 1835,
        "type": "B23 112-100"
      },
      "map": {
        "voltage_l1": "voltage_l1",
        "power": "total_power",
        "type": "fixed",
        "firmware": ""
      },
      "mapped": 29,
      "entities": 30
    }
  ],
  "max_devices": 16
}
```

Effects: values hold the entities read (key from the template): numbers already scaled, true/false for bits and coils, the text of a map entry, strings. state: ok, partial (some reads refused), offline (3 polls without an answer; tried again every 30 s), starting or off. expose: integration, mqtt, both or none. poll_s 0 means the template's interval. A virtual device (transport virtual) shows the values of its source as its template's device: map tells per entity where its value comes from (a source key, a sum like power_l1+power_l2+power_l3, a difference, "fixed" for the template's emulate value, "" for none, " x1000" when units convert), mapped how many entities have a source.

#### PUT `/api/modbus`

Body:

```json
{
  "bus": {
    "baud": 9600,
    "parity": "N",
    "stop": 1,
    "timeout_ms": 500
  },
  "devices": [
    {
      "id": "d1",
      "name": "House meter",
      "template": "eastron_sdm630",
      "transport": "rtu",
      "slave": 1,
      "expose": "integration"
    },
    {
      "name": "Inverter",
      "template": "my_inverter",
      "transport": "tcp",
      "host": "192.168.1.60",
      "port": 502,
      "slave": 1,
      "poll_s": 5,
      "expose": "both",
      "enabled": true
    },
    {
      "name": "House meter as ABB B23",
      "template": "abb_b23",
      "transport": "virtual",
      "source": "d1",
      "expose": "none"
    }
  ]
}
```

Response:

```json
{
  "...": "same as GET /api/modbus"
}
```

Errors: 400 baud not 1200-115200 of the list, parity not N/E/O, stop not 1/2, timeout_ms not 50-5000; more than 16 devices; unknown template; transport not rtu/tcp/udp/virtual; virtual without source, or source not a non-virtual device of the list; tcp or udp without host; slave not 1-247 (TCP, UDP 0-255); port, poll_s, expose or enabled invalid; name over 32 characters or with quotes, braces, backslash.

Effects: transport: rtu (RS485), tcp (Modbus TCP, port 502), virtual (emulation: source is the id of the device whose values it shows; no address; read only; values follow each poll of the source; docs/modbus.md) or udp (GoodWe Wi-Fi module: Modbus RTU over UDP, answers with AA 55, port 8899). devices replaces the whole list: send the ones to keep with their id; a device without id gets a new one. A device whose template and connection stay keeps its values. The answer comes after the gateway applied it (up to 2 s). MQTT discovery follows.

#### GET `/api/modbus/templates`

Response:

```json
{
  "templates": [
    {
      "id": "eastron_sdm630",
      "manufacturer": "Eastron",
      "model": "SDM630-Modbus V2",
      "category": "energy_meter",
      "notes": "Three phase...",
      "builtin": true,
      "entities": 37,
      "rev": 3021911,
      "baud": 9600,
      "serial": "8N1",
      "slave": 1
    }
  ],
  "max_user": 16
}
```

#### GET `/api/modbus/template`

Query: `id=eastron_sdm630` template id

Response:

```json
{
  "schema": 1,
  "id": "eastron_sdm120",
  "manufacturer": "Eastron",
  "model": "SDM120-Modbus",
  "serial": {
    "baud": 2400,
    "parity": "N",
    "stop": 1
  },
  "slave": 1,
  "timeout_ms": 500,
  "gap_ms": 50,
  "poll_s": 10,
  "max_regs": 40,
  "max_gap": 20,
  "entities": [
    {
      "key": "voltage",
      "name": "Voltage",
      "name_cs": "Napětí",
      "reg": "input",
      "addr": "0x0000",
      "type": "f32",
      "precision": 1,
      "unit": "V",
      "class": "voltage",
      "state": "measurement"
    }
  ],
  "builtin": true,
  "rev": 123
}
```

Errors: 404 No such template.

Effects: The format is described in docs/modbus.md (fields, types, word orders, writes).

#### PUT `/api/modbus/template`

Body:

```json
{
  "schema": 1,
  "id": "my_meter",
  "manufacturer": "Acme",
  "model": "M1",
  "serial": {
    "baud": 9600,
    "parity": "E",
    "stop": 1
  },
  "entities": [
    {
      "key": "power",
      "name": "Power",
      "reg": "holding",
      "addr": "0x0010",
      "type": "s32",
      "order": "CDAB",
      "scale": 0.1,
      "unit": "W",
      "class": "power",
      "state": "measurement"
    },
    {
      "key": "mode",
      "name": "Mode",
      "reg": "holding",
      "addr": "0x0020",
      "map": {
        "0": "off",
        "1": "auto"
      },
      "write": {
        "kind": "select"
      }
    }
  ]
}
```

Response:

```json
{
  "...": "the stored template, as GET /api/modbus/template"
}
```

Errors: 400 The template is invalid (the message names the field or entity).; 409 The id is a built-in template's; 16 own templates already; NVS full.

Effects: Devices using a replaced template start again with its entities.

#### DELETE `/api/modbus/template`

Query: `id=my_meter` template id

Response:

```json
{
  "deleted": "my_meter"
}
```

Errors: 404 No such template.; 409 Built in, or a device uses it.

#### POST `/api/modbus/write`

Body:

```json
{
  "device": "d2",
  "key": "alarm_threshold",
  "value": 2500
}
```

Response:

```json
{
  "written": true
}
```

Errors: 400 Unknown device or key, not writable, value outside min-max, not an option of a select.; 409 The device refused (exception) or did not answer.; 503 Modbus busy for 8 s.

Effects: value: a number (number), "ON"/"OFF" or true/false (switch), the option text (select), nothing (button). The entity is read again right after.

#### POST `/api/modbus/raw`

Body:

```json
{
  "transport": "rtu",
  "slave": 1,
  "fc": 4,
  "addr": 0,
  "count": 2
}
```

Response:

```json
{
  "ok": true,
  "result": 0,
  "error": "",
  "registers": [
    17254,
    13108
  ]
}
```

Errors: 400 fc not 1-6, 15, 16; addr, count or values invalid; tcp or udp without host.; 503 Modbus busy for 8 s.

Effects: transport rtu, tcp (port default 502) or udp (GoodWe, port default 8899). Writes take values (16-bit numbers; coils 0/1). A refused request answers 200 with ok false and the exception in result and error.

#### GET `/api/modbus/probe`

Response:

```json
{
  "running": false,
  "progress": 100,
  "baud": 0,
  "found": [
    {
      "slave": 1,
      "baud": 9600
    },
    {
      "slave": 12,
      "baud": 2400
    }
  ]
}
```

#### POST `/api/modbus/probe`

Body:

```json
{
  "transport": "rtu",
  "template": "eastron_sdm120",
  "bauds": [
    2400,
    9600
  ],
  "from": 1,
  "to": 247
}
```

Response:

```json
{}
```

Errors: 400 from/to out of range, tcp without host, unknown template, bauds not a list of 1-8.

Effects: Reads the template's first register (without a template holding register 0) at every address; an exception counts as an answer. RS485 devices are not polled while it runs (about 150 ms per address and baud rate). {"stop": true} ends it.

#### GET `/api/modbus/scan`

Response:

```json
{
  "default_net": "192.168.88.0/24",
  "running": false,
  "net": "192.168.88.0/24",
  "progress": 100,
  "addresses": 254,
  "open": 1,
  "stopped": false,
  "duration_ms": 41250,
  "found": [
    {
      "host": "192.168.88.240",
      "port": 502,
      "unit": 1,
      "answer": 2,
      "manufacturer": "Victron Energy",
      "model": "GX device",
      "device_unit": 100,
      "template": "victron_gx_system",
      "serial": "c0619a000000",
      "vebus_unit": 228
    }
  ],
  "udp": [
    {
      "host": "192.168.1.170",
      "info": "192.168.1.170,34EAE7000000,Solar-WiFi211W0000",
      "unit": 127
    }
  ]
}
```

Effects: default_net is the network of the interface carrying the traffic. answer: 0 data, else the Modbus exception code of the first unit that answered. manufacturer, model, template: a device recognised by its serial number; vebus_unit: the VE.Bus unit of a Victron GX. udp: GoodWe dongles answering the discovery port 48899 (info: ip,mac,name) or Modbus RTU on UDP 8899 (unit: the one that answered; 127 is the DT/XS family, template goodwe_xs, transport udp).

#### POST `/api/modbus/scan`

Body:

```json
{
  "net": "192.168.1.0/24",
  "ports": [
    502
  ],
  "units": [
    1,
    247,
    255
  ],
  "udp": true
}
```

Response:

```json
{}
```

Errors: 400 net not a.b.c.d/n or prefix out of 22-32, ports not a list of 1-4, units not a list of 1-8 ids 0-255.; 409 A search is running.; 503 No memory for the search task.

Effects: Connects to the ports of every address (4 at a time, 300 ms each), reads holding register 0 of the units on each open port (an exception is an answer too), then recognises Victron GX and GoodWe by their serial numbers. With udp (default) also asks GoodWe dongles on UDP 8899 and sends their discovery broadcast. Runs beside the device polling; a /24 takes about 20-60 s. Ports default [502], units [1, 247, 255]. {"stop": true} ends it.

#### GET `/api/modbus/server`

Response:

```json
{
  "enabled": true,
  "port": 502,
  "devices": [
    {
      "device": "d1",
      "unit": 10,
      "port": 1502,
      "write": false,
      "allow": [],
      "rate": 5,
      "cache_ms": 2000,
      "missing": false,
      "name": "Eastron SDM630",
      "requests": 120,
      "cached": 96,
      "passed": 24,
      "refused": 0,
      "failed": 0
    }
  ],
  "ports": [
    {
      "port": 502,
      "ok": true,
      "error": "",
      "clients": [
        {
          "address": "192.168.88.20",
          "requests": 80,
          "connected_s": 3600
        }
      ]
    },
    {
      "port": 1502,
      "device": "d1",
      "ok": true,
      "error": "",
      "clients": []
    }
  ],
  "clients": 1,
  "turned_away": 0,
  "limits": {
    "devices": 16,
    "ports": 8,
    "clients_per_port": 2,
    "clients": 8,
    "rate": 50,
    "cache_ms_min": 1500,
    "cache_ms_max": 5000
  }
}
```

Effects: devices: unit is the unit id on the shared port (0: not there), port an own port (0: none) where any unit id reaches the device. cache_ms: how long answers to clients serve the next ones (0: no cache at all). cached: reads answered from the registers the gateway polled within the device's poll interval, from answers to clients within cache_ms, or by joining the same read of another client on its way; passed: requests sent to the device; refused: over the rate (exception 6), a write not allowed (1) or an invalid request (1, 3); failed: the device off, offline or silent (10, 11). missing: the Modbus device was removed (its requests get exception 10). ports: ok false with error when the port could not be opened. turned_away: connections closed at once because the port or the gateway had its clients.

#### PUT `/api/modbus/server`

Body:

```json
{
  "enabled": true,
  "port": 502,
  "devices": [
    {
      "device": "d1",
      "unit": 10,
      "port": 1502,
      "write": false,
      "allow": [],
      "rate": 5,
      "cache_ms": 2000
    },
    {
      "device": "d2",
      "unit": 0,
      "port": 1503,
      "write": true,
      "allow": [
        "192.168.88.20"
      ],
      "rate": 2,
      "cache_ms": 2000
    }
  ]
}
```

Response:

```json
{}
```

Errors: 400 port not 1-65535 or 80, 8081, the Zigbee port; more than 16 devices; device not a configured Modbus device; unit not 0-247 or port not 0-65535; neither unit nor port; an own port equal to the shared one; a device, unit or port twice; write not a bool; allow not up to 4 IPv4 addresses; rate not 1-50; cache_ms not 0 or 1500-5000; more than 8 ports (the shared one counts).

Effects: What is not sent stays. The shared port (default 502, enabled) listens while a device has a unit id there and routes by it; unknown unit ids get exception 10. Reads of registers the gateway polls, and for cache_ms (default 2000) answers to clients, come from its cache; a write drops the blocks it touches; the same read of two clients goes to the device once. Everything else goes to the device through the Modbus task (the RS485 bus is shared with the polling; requests wait for one transaction of a poll at most), at most rate requests per second per device, else exception 6. Writes (functions 5, 6, 15, 16) only with write and, when allow lists addresses, only from them; else exception 1. Up to 2 clients per port and 8 in all, one request at a time per client; idle clients are closed after 120 s. Answers the GET.

### GPIO of header JP1

| Method | Path | What it does | Permission |
|---|---|---|---|
| GET | `/api/gpio` | Header JP1 with the state of each pin, and the configured GPIO with their values. | page `system/hw`, read |
| PUT | `/api/gpio` | Replaces the GPIO configuration; applied at once. | page `system/hw`, edit |
| POST | `/api/gpio/set` | Sets an output, a pulse, the duty of a PWM pin or clears a counter. | page `system/hw`, edit |

#### GET `/api/gpio`

Response:

```json
{
  "header": [
    {
      "pin": 1,
      "kind": "3v3",
      "label": "3.3 V"
    },
    {
      "pin": 9,
      "kind": "gpio",
      "label": "GPIO2",
      "gpio": 2,
      "free": true
    },
    {
      "pin": 13,
      "kind": "gpio",
      "label": "GPIO4",
      "gpio": 4,
      "free": false,
      "reserved": "radio A (C6): UART TX"
    },
    {
      "pin": 14,
      "kind": "gpio",
      "label": "GPIO45",
      "gpio": 45,
      "free": true,
      "note": "also switches the SD card power on the board (the gateway does not use the SD card)"
    }
  ],
  "pins": [
    {
      "gpio": 2,
      "mode": "output",
      "name": "Relay",
      "expose": "integration",
      "invert": false,
      "pull": "none",
      "open_drain": false,
      "restore": "last",
      "jp1": 9,
      "active": true,
      "level": 1,
      "pin_level": 1
    },
    {
      "gpio": 3,
      "mode": "input",
      "name": "Button",
      "expose": "both",
      "invert": true,
      "pull": "up",
      "debounce_ms": 20,
      "jp1": 11,
      "active": true,
      "level": 0
    },
    {
      "gpio": 1,
      "mode": "pwm",
      "name": "LED",
      "expose": "integration",
      "invert": false,
      "pull": "none",
      "restore": "off",
      "freq": 1000,
      "jp1": 7,
      "active": true,
      "duty": 37.5,
      "resolution_bits": 16
    },
    {
      "gpio": 45,
      "mode": "counter",
      "name": "Water meter",
      "expose": "integration",
      "invert": false,
      "pull": "down",
      "edge": "rising",
      "filter_ns": 1000,
      "jp1": 14,
      "active": true,
      "count": 1520,
      "rate": 0.5,
      "frequency": 0.5
    },
    {
      "gpio": 2,
      "mode": "uart",
      "name": "Meter",
      "rx": 3,
      "baud": 9600,
      "data_bits": 8,
      "parity": "none",
      "stop_bits": 1,
      "port": 6639,
      "jp1": 9,
      "active": true,
      "client": "192.168.1.20",
      "sessions": 1,
      "rx_bytes": 1024,
      "tx_bytes": 96
    }
  ],
  "radio_b_gpio": false
}
```

Effects: header lists all 26 pins (kind 3v3, 5v, gnd, nc, gpio, i2c, c6); a GPIO is free or reserved with the reason (radio A always; radio B while it has a role or a radio is found on it). pins: level (input debounced, output as set, both logical after invert), pin_level (an output read back on the pad), duty in % of a PWM pin, count, rate (counts per second) and frequency of a counter. active false with error: the pin is configured but off (reserved now, no PWM timer or counter left). radio_b_gpio: radio B's pins are GPIO, radio B is left alone. uart: client (address of the TCP client, empty without one), sessions, rx_bytes (UART to the client), tx_bytes. ws2812: light {state, color {r, g, b}, brightness 0-255, effect}. onewire: present (a device answered the reset), temperatures {ROM code: °C or null}.

#### PUT `/api/gpio`

Body:

```json
{
  "pins": [
    {
      "gpio": 2,
      "mode": "uart",
      "name": "Meter",
      "rx": 3,
      "baud": 9600,
      "data_bits": 8,
      "parity": "none",
      "stop_bits": 1,
      "port": 6639
    },
    {
      "gpio": 1,
      "mode": "ws2812",
      "name": "Strip",
      "leds": 60,
      "order": "grb",
      "restore": "last",
      "expose": "both"
    },
    {
      "gpio": 45,
      "mode": "onewire",
      "name": "Floor",
      "interval_s": 30
    }
  ]
}
```

Response:

```json
{
  "header": [
    {
      "pin": 1,
      "kind": "3v3",
      "label": "3.3 V"
    },
    {
      "pin": 9,
      "kind": "gpio",
      "label": "GPIO2",
      "gpio": 2,
      "free": true
    },
    {
      "pin": 13,
      "kind": "gpio",
      "label": "GPIO4",
      "gpio": 4,
      "free": false,
      "reserved": "radio A (C6): UART TX"
    },
    {
      "pin": 14,
      "kind": "gpio",
      "label": "GPIO45",
      "gpio": 45,
      "free": true,
      "note": "also switches the SD card power on the board (the gateway does not use the SD card)"
    }
  ],
  "pins": [
    {
      "gpio": 2,
      "mode": "output",
      "name": "Relay",
      "expose": "integration",
      "invert": false,
      "pull": "none",
      "open_drain": false,
      "restore": "last",
      "jp1": 9,
      "active": true,
      "level": 1,
      "pin_level": 1
    },
    {
      "gpio": 3,
      "mode": "input",
      "name": "Button",
      "expose": "both",
      "invert": true,
      "pull": "up",
      "debounce_ms": 20,
      "jp1": 11,
      "active": true,
      "level": 0
    },
    {
      "gpio": 1,
      "mode": "pwm",
      "name": "LED",
      "expose": "integration",
      "invert": false,
      "pull": "none",
      "restore": "off",
      "freq": 1000,
      "jp1": 7,
      "active": true,
      "duty": 37.5,
      "resolution_bits": 16
    },
    {
      "gpio": 45,
      "mode": "counter",
      "name": "Water meter",
      "expose": "integration",
      "invert": false,
      "pull": "down",
      "edge": "rising",
      "filter_ns": 1000,
      "jp1": 14,
      "active": true,
      "count": 1520,
      "rate": 0.5,
      "frequency": 0.5
    }
  ],
  "radio_b_gpio": false
}
```

Errors: 400 gpio not a free GPIO of JP1 (radio A's pins, radio B's while it is in use), a GPIO twice, mode not off, input, output, pwm or counter, more than 4 PWM pins or 4 counters, name over 32 characters or with quotes, backslashes or braces, debounce_ms out of 0-1000, freq out of 1-1000000, filter_ns out of 0-10000, pull, restore, edge or expose unknown. uart: rx missing, the TX pin or not free, baud out of 300-3000000, data_bits not 7 or 8, parity not none, even or odd, stop_bits not 1 or 2, port out of 1024-65535 or 6638/8081; i2c: scl missing or not free, hz not 100000 or 400000; ws2812: leds out of 1-512, order not grb, rgb, brg or rbg; onewire: interval_s out of 5-3600; more than 1 UART, 1 I2C bus, 2 strips or 2 1-Wire buses.

Effects: Pins left out are off (high impedance). restore: an output or PWM pin after a restart is off, on (100 %) or its last value (saved 5 s after the last change). An output's level is set before its driver turns on. While radio B's pins are GPIO, radio B is not started, tested, flashed or backed up (409); giving radio B a role turns them off at the next start with a notification. invert: input and output levels are logical, the pin carries the opposite; a counter counts the other edge. Modes over peripherals: uart (UART4 on gpio as TX and rx as RX, a TCP server on port, one client at a time, a new one replaces it, bytes unchanged both ways), i2c (a second I2C bus, SDA gpio, SCL scl, for the sensors: /api/sensors with bus 2), onewire (DS18B20, DS1822, DS18S20 found by a ROM search, all converted at once every interval_s; needs a 4.7 kOhm pull-up), ws2812 (RMT, one colour for every LED, brightness, effect none or rainbow, state after a restart like an output).

#### POST `/api/gpio/set`

Body:

```json
{
  "gpio": 1,
  "on": true,
  "color": [
    255,
    80,
    0
  ],
  "brightness": 180,
  "effect": "rainbow"
}
```

Response:

```json
{
  "gpio": 2,
  "mode": "output",
  "name": "Relay",
  "expose": "integration",
  "invert": false,
  "pull": "none",
  "open_drain": false,
  "restore": "last",
  "jp1": 9,
  "active": true,
  "level": 1,
  "pin_level": 1
}
```

Errors: 400 The value does not fit the mode (output: level 0/1 or pulse_ms 1-3600000; pwm: duty 0-100; counter: reset true; ws2812: on or state, color [r, g, b] or {r, g, b} 0-255, brightness 0-255, effect none or rainbow; onewire: rescan true).; 404 No active pin with this GPIO.

Effects: pulse_ms turns an output to the opposite level for that time and back (a second pulse while one runs moves its end); a level ends a running pulse. Changes go to the push channel (MQTT topic gpio, /api/ws) like every value change. A strip: a colour or brightness alone turns it on. onewire rescan: searches the bus now.

### Updates and firmware

| Method | Path | What it does | Permission |
|---|---|---|---|
| POST | `/api/ota/c6fw` | Uploads the radio firmware package (all bundles of one chip type). | page `system/firmware`, edit |
| POST | `/api/ota/gateway` | Uploads a gateway firmware image into the inactive slot and boots it. | page `system/firmware`, edit |
| POST | `/api/ota/model` | Uploads the sound recognition model into the model partition. | page `system/firmware`, edit |
| POST | `/api/update/check` | Triggers a check of the update manifest. | page `system/firmware`, edit |
| POST | `/api/update/install` | Installs the offered or a chosen release: gateway firmware or a radio package. | page `system/firmware`, edit |

#### POST `/api/ota/c6fw`

Body: the raw file (`c6fw.bin`).

Response:

```json
{
  "ok": true
}
```

Errors: 400 Not a valid package (bad directory header or format version, size mismatch) or it does not match the chip.; 409 Another upload or a radio flash is running.; 413 Not a package of a plausible size.

Effects: Raw binary body. The package is staged in PSRAM; it does not restart the gateway. A radio whose bundle changed is reflashed at the next start (restart the gateway, not a power cut, which would lose the PSRAM copy).

#### POST `/api/ota/gateway`

Body: the raw file (`p4gw.bin`).

Response:

```json
{
  "slot": "ota_1",
  "version": "v1.9",
  "restarting": true
}
```

Errors: 400 Not an ESP application image, no application description, or not a p4gw image (checked in the first block before anything is erased); write or validation error.; 409 Another upload or a radio flash is running, or the running image is not confirmed yet (retry in a minute).; 413 Image empty or larger than the slot.

Effects: Raw binary body, not JSON. Writes the other OTA slot, sets it as the boot slot and restarts the gateway after 0.8 s. If the new image does not start properly it rolls back to the previous one.

#### POST `/api/ota/model`

Body: the raw file (`yamnet_p4.espdl`).

Response:

```json
{
  "bytes": 4000000,
  "restart_required": true
}
```

Errors: 400 Not an ESP-DL model (EDL2 header and size are checked before erasing), or a write error.; 409 Model in use: turn sound recognition off and restart first.; 413 Model empty or larger than the partition.

Effects: Raw binary body. Erases and rewrites the model partition. The gateway does not restart by itself; the new model loads after a restart.

#### POST `/api/update/check`

Response:

```json
{
  "installed_version": "v1.9",
  "latest_version": "v1.10",
  "title": "P4 gateway firmware",
  "release_summary": "...",
  "in_progress": false,
  "available": true,
  "checking": true,
  "checked": 1793600000,
  "error": "",
  "url": "https://example.com/p4gw/manifest.json",
  "percent": 0,
  "releases": [
    {
      "version": "v1.9",
      "date": "2026-11-01"
    }
  ],
  "c6": {
    "offered": true,
    "available": false,
    "in_progress": false,
    "percent": 0
  },
  "c5": {
    "offered": false,
    "available": false,
    "in_progress": false,
    "percent": 0
  }
}
```

Errors: 409 No update URL set.

Effects: Returns the current update state at once; the check runs in a background task, so poll the same state (also in /api/status, update) until checking is false. Fields not shown here: layout, build, default_url, release_url.

#### POST `/api/update/install`

Body:

```json
{
  "target": "gateway",
  "version": "v1.10"
}
```

Response:

```json
{
  "installed_version": "v1.9",
  "in_progress": true,
  "percent": 0,
  "c6": {
    "in_progress": false
  },
  "c5": {
    "in_progress": false
  }
}
```

Errors: 409 Nothing offered (check first), release not offered for this gateway, radio B is not an ESP32-C5 (target c5), or an installation is already running.

Effects: The body is optional. target: gateway (default), c6 (radio package of radio A) or c5 (package of radio B); version picks one of the listed releases, else the offered one is used. Returns the update state at once and downloads in the background (percent). Gateway: writes the other slot and restarts. c6 and c5: stores the package and restarts the gateway, which then reflashes the radio.

### Settings and maintenance

| Method | Path | What it does | Permission |
|---|---|---|---|
| POST | `/api/reboot` | Restarts the gateway. | page `system/maint`, edit |
| POST | `/api/settings` | Changes basic gateway settings; send only the fields to change. | page `system/settings`, edit |
| GET | `/api/time` | Clock of the gateway: time, last SNTP sync, servers and time zone. | page `system/settings`, read |
| POST | `/api/time` | Sets the SNTP servers and the time zone of the gateway; send only the fields to change. | page `system/settings`, edit |
| GET | `/api/settings/export` | Exports the gateway settings as a JSON file. | page `system/settings`, edit |
| POST | `/api/settings/import` | Imports a settings file made by the export. | page `system/settings`, edit |

#### POST `/api/reboot`

Body:

```json
{
  "full": false
}
```

Response:

```json
{}
```

Errors: 409 full requested while radio A is being flashed.

Effects: The body is optional. Restarts after 0.5 s. full: true also resets the radio A chip just before the restart.

#### POST `/api/settings`

Body:

```json
{
  "hostname": "p4gw-livingroom",
  "matter_url": "ws://192.168.1.10:5580/ws",
  "zigbee_backup_days": 1,
  "update_url": "https://example.com/p4gw/manifest.json"
}
```

Response:

```json
{
  "restart_required": true
}
```

Errors: 400 hostname not lowercase letters, digits and inner dashes (max 31); matter_url not ws:// (max 120); zigbee_backup_days not 0, 1 or 7; update_url not http(s):// (max 200); or password given (use /api/users).; 500 Storing to NVS failed.

Effects: Writes to NVS. Empty matter_url or update_url clears it; changing update_url starts an update check. zigbee_backup_days is the automatic Zigbee backup period (0 off). A new hostname takes effect after a restart (restart_required: true); the gateway does not restart by itself. Fields before an invalid one may already be stored.

#### GET `/api/time`

Response:

```json
{
  "time": 1762336800,
  "synced": 1762336512,
  "tz": "CET-1CEST,M3.5.0,M10.5.0/3",
  "tz_name": "Europe/Prague",
  "servers": [
    {
      "name": "pool.ntp.org",
      "address": "195.113.144.201",
      "reach": 255
    }
  ]
}
```

Effects: time and synced are unix seconds, 0 while unknown. address is the server's resolved address (empty before the first poll), reach the RFC 5905 reachability register of its last eight polls (bit 0 the last). tz_name is empty for a zone set as a POSIX rule only.

#### POST `/api/time`

Body:

```json
{
  "servers": [
    "pool.ntp.org",
    "192.168.1.1"
  ],
  "tz": "CET-1CEST,M3.5.0,M10.5.0/3",
  "tz_name": "Europe/Prague"
}
```

Response:

```json
{
  "time": 1762336800,
  "synced": 1762336512,
  "tz": "CET-1CEST,M3.5.0,M10.5.0/3",
  "tz_name": "Europe/Prague",
  "servers": [
    {
      "name": "pool.ntp.org",
      "address": "195.113.144.201",
      "reach": 255
    }
  ]
}
```

Errors: 400 servers not a list of up to 3 host names or addresses; tz empty or longer than 63; tz_name longer than 47 or without tz.; 500 Storing to NVS failed.

Effects: Applies at once, no restart. An empty server list stops time sync. tz is the POSIX rule the gateway uses (Telegram quiet hours, the nightly Zigbee backup between 03:00 and 04:00); tz_name the IANA name the web UI shows times in. Sends a system notification.

#### GET `/api/settings/export`

Query: `secrets=1` 1 includes passwords, tokens and keys; without it they are left out.

Response:

```json
{
  "format": "p4gw-settings",
  "version": 1,
  "hostname": "p4gw-a1b2c3",
  "firmware": "v1.9",
  "exported": 1793610000,
  "secrets": false,
  "keys": {
    "...": "NVS keys and values"
  }
}
```

Effects: Sent as a download (Content-Disposition: <hostname>-settings.json). With secrets=1 the file holds the keys of the networks, so keep it private.

#### POST `/api/settings/import`

Body:

```json
{
  "format": "p4gw-settings",
  "version": 1,
  "keys": {
    "...": "as exported"
  }
}
```

Response:

```json
{
  "keys": 42,
  "restarting": true
}
```

Errors: 400 Not a p4gw-settings file, keys not an object, or an entry is invalid (nothing is written then).; 409 Radio A flashing in progress.; 413 Body over 64 kB.

Effects: Send the exported JSON as the body. Everything is validated first, then written to NVS (board-specific keys are skipped), and the gateway restarts after 1 s.

### Logs and diagnostics

| Method | Path | What it does | Permission |
|---|---|---|---|
| DELETE | `/api/coredump` | Erases the stored core dump. | page `system/diag`, edit |
| GET | `/api/coredump` | Downloads the stored core dump (ELF) of the last crash. | page `system/diag`, read |
| GET | `/api/logs` | Gateway log lines since a sequence number. | page `system/diag`, read |
| GET | `/api/logs/previous` | Log of the run before the last restart. | page `system/diag`, read |
| POST | `/api/tools/ping` | Pings a host from the gateway. | page `system/diag`, edit |

#### DELETE `/api/coredump`

Response:

```json
{}
```

Errors: 500 Erase failed.

#### GET `/api/coredump`

Response: application/octet-stream coredump.elf.

Errors: 404 No core dump.

#### GET `/api/logs`

Query: `since=number` Sequence returned in the previous X-Log-Seq header; 0 or missing returns the buffered log.

Response: text/plain log lines; header X-Log-Seq holds the sequence to pass as since next time (up to 16 KB per call).

#### GET `/api/logs/previous`

Response: text/plain log lines.

Errors: 404 No log from before the last restart.

#### POST `/api/tools/ping`

Body:

```json
{
  "host": "192.168.1.1",
  "count": 4
}
```

Response:

```json
{
  "target": "192.168.1.1",
  "sent": 4,
  "received": 4,
  "replies": [
    {
      "seq": 1,
      "ms": 1
    },
    {
      "seq": 2,
      "timeout": true
    }
  ]
}
```

Errors: 400 Cannot resolve host.; 500 Ping session failed.

Effects: host is a name or an IPv4/IPv6 address; count 1..10 (default 4, clamped). The request waits until the pings finish, up to count * 2 + 2 s.

### Notifications and Telegram

| Method | Path | What it does | Permission |
|---|---|---|---|
| GET | `/api/auth/telegram` | Own Telegram notification preferences and pairing state. | own account |
| POST | `/api/auth/telegram` | Changes own Telegram notification preferences. | own account |
| POST | `/api/auth/telegram/pair` | Creates a code to pair the own Telegram chat with the bot. | own account |
| POST | `/api/auth/telegram/test` | Sends a test message to the own paired Telegram chat. | own account |
| GET | `/api/auth/telegram/reports` | The own periodic Telegram reports, the devices they may cover and the limits. | own account |
| PUT | `/api/auth/telegram/reports` | Replaces the own periodic Telegram reports. | own account |
| POST | `/api/auth/telegram/reports/send` | Sends an own report now (the period so far), or returns its text. | own account |
| POST | `/api/auth/telegram/unpair` | Removes the pairing of the own Telegram chat. | own account |
| GET | `/api/notify` | Recent gateway notifications, oldest first. | any signed-in client |
| GET | `/api/telegram` | Telegram bot settings and state (admin). | administrators |
| POST | `/api/telegram` | Changes Telegram bot settings (admin); send only the fields to change. | administrators |

#### GET `/api/auth/telegram`

Response:

```json
{
  "bot_enabled": true,
  "bot": "my_p4gw_bot",
  "paired": true,
  "levels": {
    "security": 3,
    "system": 2,
    "network": 1,
    "sound": 0
  },
  "allowed": {
    "security": true,
    "system": true,
    "network": true,
    "sound": false
  },
  "quiet_from": 22,
  "quiet_to": 7,
  "lang": "cs",
  "sound_muted_s": 0
}
```

Errors: 409 Only for signed-in users (not for tokens or an open gateway).

Effects: levels per topic (security, system, network, sound): 0 off, 1 critical only, 2 warnings and critical, 3 all. A topic reaches a user only with the read right of its page (security: administrators, system: System › Information, network: System › LAN, sound: Sound › Detection log), shown in allowed. In the quiet hours (quiet_from to quiet_to, hours 0..23, -1 none) only critical notifications come; /mute pauses the sound topic.

#### POST `/api/auth/telegram`

Body:

```json
{
  "levels": {
    "security": 3,
    "system": 2,
    "network": 1,
    "sound": 0
  },
  "quiet_from": 22,
  "quiet_to": 7,
  "lang": "cs"
}
```

Response:

```json
{
  "bot_enabled": true,
  "paired": true,
  "levels": {
    "security": 3
  },
  "quiet_from": 22,
  "quiet_to": 7,
  "lang": "cs"
}
```

Errors: 409 Only for signed-in users.

Effects: Changes only the fields sent; values outside the range (levels 0..3, quiet hours -1..23, lang cs or en) are ignored. Level meaning as in GET. Returns the new state like GET.

#### POST `/api/auth/telegram/pair`

Response:

```json
{
  "code": 12345678,
  "bot": "my_p4gw_bot",
  "link": "https://t.me/my_p4gw_bot?start=12345678",
  "expires_s": 600
}
```

Errors: 409 Only for signed-in users, or the Telegram bot is not running.

Effects: The code is valid for 10 minutes; open the link or send /start <code> to the bot.

#### POST `/api/auth/telegram/test`

Response:

```json
{}
```

Errors: 409 Not paired, the bot is off, or not a signed-in user.; 503 Outgoing queue full.

#### GET `/api/auth/telegram/reports`

Response:

```json
{
  "reports": [
    {
      "id": 1,
      "name": "Daily summary",
      "devices": [
        "modbus:d1",
        "sensors:i2c_76",
        "gpio:2"
      ],
      "period_min": 1440,
      "at": 420,
      "weekday": 0,
      "enabled": true,
      "period_start": 1794380400,
      "period_end": 1794466800,
      "next_s": 52000
    }
  ],
  "sources": [
    {
      "id": "modbus",
      "name": "Modbus",
      "name_cs": "Modbus",
      "devices": [
        {
          "id": "modbus:d1",
          "name": "House meter"
        }
      ]
    }
  ],
  "limits": {
    "reports": 4,
    "devices": 8,
    "period_min": 15,
    "period_max": 10080,
    "presets": [
      15,
      30,
      60,
      360,
      720,
      1440,
      10080
    ]
  },
  "paired": true
}
```

Errors: 409 Not a signed-in user (an API token has no reports).

Effects: sources lists only the devices the user may read (Modbus: System › RS485, I2C sensors and GPIO: System › Hardware). period_start and period_end: the current period (Unix time); next_s: seconds to the next report, -1 when off. Periods run by the gateway's clock and time zone: whole days from at (minutes after midnight) of a day, 7 days from at of weekday (0 Monday), parts of a day from at + k × period, other periods in steps from at of Monday 2024-01-01.

#### PUT `/api/auth/telegram/reports`

Body:

```json
{
  "reports": [
    {
      "id": 1,
      "name": "Daily summary",
      "devices": [
        "modbus:d1",
        "sensors:i2c_76",
        "gpio:2"
      ],
      "period_min": 1440,
      "at": 420,
      "weekday": 0,
      "enabled": true
    }
  ]
}
```

Response:

```json
{
  "...": "same as GET /api/auth/telegram/reports"
}
```

Errors: 400 More than 4 reports; name not 1-40 characters; devices not 1-8 of the listed ones; period_min not 15-10080; at not 0-1439; weekday not 0-6; id not 1-255 or twice.; 409 Not a signed-in user.

Effects: Send the reports to keep with their id; a report without id gets one. One whose devices and schedule stay keeps its period and samples. Saving makes the gateway sample at once (else every 60 s). at defaults to 420 (07:00), enabled to true.

#### POST `/api/auth/telegram/reports/send`

Body:

```json
{
  "id": 1,
  "preview": false
}
```

Response:

```json
{
  "parts": [
    "📊 <b>Daily summary</b> · p4gw-xxxxxx\n11.11. 07:00 – 11.11. 08:27\n\n<b>House meter</b>\n• Voltage L1: 230.1 V <i>(min 228.4 · max 233.0 · ⌀ 230.6)</i>\n• Import energy: 1012.34 kWh <i>(+12.34 kWh in the period)</i>"
  ],
  "sent": true
}
```

Errors: 404 No such report.; 409 Not paired with Telegram (unless preview), the clock is not set, or not a signed-in user.

Effects: parts: the Telegram HTML messages (split at 4000 bytes). The period goes on. Values: current; measured ones with minimum, maximum and average since the start of the period (since a restart within it, said in the text); counters with the difference since the start of the period (start values kept in NVS). In the user's quiet hours a report comes without sound.

#### POST `/api/auth/telegram/unpair`

Response:

```json
{
  "bot_enabled": true,
  "paired": false
}
```

Errors: 409 Only for signed-in users.

Effects: Returns the preferences like GET /api/auth/telegram.

#### GET `/api/notify`

Query: `since=number` Return only notifications with a higher seq; default 0.

Response:

```json
{
  "notifications": [
    {
      "seq": 12,
      "boot": 123456,
      "topic": "system",
      "level": "info",
      "text": "Gateway restarted, v1.9",
      "time": 1793600000
    }
  ],
  "seq": 12,
  "boot": 123456
}
```

Effects: topic is security, system, network or sound; level is info, warning or critical. The gateway keeps a limited ring in memory. boot changes on every restart, which resets seq.

#### GET `/api/telegram`

Response:

```json
{
  "enabled": true,
  "token_set": true,
  "bot": "my_p4gw_bot",
  "state": "running",
  "error": "",
  "tz": "CET-1CEST,M3.5.0,M10.5.0/3",
  "paired": 2
}
```

Effects: state is off, connecting, running or error. The token is never returned.

#### POST `/api/telegram`

Body:

```json
{
  "token": "123456:ABC-DEF1234ghIkl-zyx57W2v1u123ew11",
  "enabled": true,
  "tz": "CET-1CEST,M3.5.0,M10.5.0/3"
}
```

Response:

```json
{
  "enabled": true,
  "token_set": true,
  "bot": "my_p4gw_bot",
  "state": "connecting",
  "error": "",
  "tz": "CET-1CEST,M3.5.0,M10.5.0/3",
  "paired": 0
}
```

Errors: 400 token without a colon (the BotFather format 123456:ABC...) or too long; tz empty or too long.

Effects: Stored in NVS and the bot restarts its connection. tz (optional, for older clients) sets the gateway time zone as POST /api/time does, without its IANA name. An empty token removes it. Sends a notification.
