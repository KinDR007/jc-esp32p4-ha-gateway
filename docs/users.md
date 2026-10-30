# Users, roles and permissions

Web UI: **System › User management** (admins only) and **My account** (the name at the bottom of the
menu). Firmware: `components/gw_core/gw_auth.c`.

## Open gateway and the first account

Without any user the gateway is open, as before a password was set. The first account created under
System › User management › Users is always an admin; from then on every page and API call needs a
sign-in. The single admin password of older versions becomes the account `admin` at the first start.
Deleting all users opens the gateway again (the last admin can be deleted only as the last user).

## Roles and permissions

| Role | Default |
|---|---|
| Admin | every page and action, user management, cannot be limited |
| User | operates the gateway; reads Home Assistant pages, system information, LAN, firmware and maintenance; no system settings |
| Viewer | reads; Thread settings (network key), Zigbee backups, MQTT, system settings and maintenance hidden |

Every page of the menu has a level per role: hidden, read or edit (System › User management › Roles
and permissions). A user can have own levels for single pages (Users › Edit › Own permissions). A
hidden page disappears from the menu, a read page shows a note and its buttons that change something
are disabled.

The server enforces the levels: each API URI belongs to a page (`AUTH_MAP` in `gw_auth.c`), `GET`
needs read, other methods edit. Requests that reveal keys or recordings need edit even when they
only read: `GET /api/thread/dataset`, `/api/c6/zigbee-snapshot`, `/api/settings/export`,
`/api/sound/clip`, `/api/sound/learn/sample`. URIs not listed are for admins only.

## Sign-in

- Session cookie `p4s` (HttpOnly, SameSite=Strict). It ends after inactivity (default 12 h); "Stay
  signed in" keeps it for 30 days, also across restarts.
- After 5 failed attempts the account locks for 15 minutes (an admin can unlock it).
- A password set by an admin (new account, reset) has to be changed at the next sign-in; until then
  the API answers `403 password_change_required`.
- Limits under System › User management › Security: inactivity, remembered sessions, lock, minimum
  password length (default 8), two-factor sign-in required per role.

## Two-factor sign-in (TOTP)

My account › Two-factor sign-in: scan the QR code with Google Authenticator (or any TOTP app,
6 digits, 30 s), confirm with a code and keep the 10 recovery codes (each signs in once instead of
the app code). Codes need the gateway clock (SNTP); without it only recovery codes work. A role can
require 2FA: its users set it up right after the next sign-in. An admin can turn 2FA off for a user
who lost the phone (Users › Edit).

## Forgotten password

"Forgot password" on the sign-in page sends a request to the admins (at most 4 per 10 minutes, the
same answer for unknown names). Admins see a badge in the menu and the list under User management ›
Reset requests, and get a notification (topic `security`). Setting a temporary password answers the
request; the user changes it at the next sign-in.

## API clients

| Client | Sign-in |
|---|---|
| Home Assistant, scripts | API token, `Authorization: Bearer p4gw_…` (User management › API tokens, shown once, with a role, no 2FA) |
| Clients of older versions | HTTP Basic with a user name and password, only for accounts without 2FA |
| Web UI | session cookie |

The push WebSocket `/api/ws` takes the same sign-in. The OTBR REST API (port 8081) and the Zigbee TCP
port (6638) are not part of it: Home Assistant and Zigbee2MQTT use them without authentication.

## Storage

NVS: one entry per user (`usr0`–`usr15`, PBKDF2-HMAC-SHA256 with 10 000 iterations and a random
salt, TOTP secret, hashes of the recovery codes), `auth_tok` (token hashes), `auth_roles`,
`auth_pol`, `auth_ses` (sessions) and `auth_req` (reset requests). A settings backup holds users and
tokens only with "Include passwords"; sessions and requests stay on the board.

## API

| Endpoint | Who |
|---|---|
| `GET /api/auth/state`, `POST /api/auth/login`, `POST /api/auth/reset-request` | anyone |
| `GET /api/auth/me`, `POST /api/auth/{logout,password}`, `POST /api/auth/totp/{setup,enable,disable}`, `GET /api/auth/sessions`, `POST /api/auth/sessions/revoke` | the signed-in user |
| `GET/POST /api/users`, `POST /api/users/delete`, `GET/POST /api/auth/{roles,policy}`, `GET /api/auth/requests`, `POST /api/auth/requests/dismiss`, `GET/POST /api/tokens`, `POST /api/tokens/delete` | admins |
