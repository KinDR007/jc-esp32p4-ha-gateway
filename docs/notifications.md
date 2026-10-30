# Notifications and the Telegram bot

Firmware: `components/gw_core/gw_notify.c` (notifications), `gw_telegram.c` (bot).

## Notifications

| Topic | Examples | Level |
|---|---|---|
| security | failed sign-ins and account lock, password reset request, users, 2FA, API tokens, bot settings, commands run from Telegram | info, warning |
| system | start and its reason, crash (panic, watchdog), brownout, maintenance mode, C6 radio lost, update available, role switch | info, warning, critical |
| network | LAN link was down, internet lost or back, Zigbee2MQTT connected or gone, LAN settings changed or reverted | info, warning |
| sound | a sound event detected (alarms, sirens and breaking glass are critical) | info, critical |

Every notification goes to the log and as `{"seq", "boot", "topic", "level", "text", "time"}` to the
push channel (`/api/ws`, MQTT topic `<base>/notify`). The gateway keeps the last 32 in RAM:
`GET /api/notify?since=<seq>`; `boot` changes with every start of the gateway. Texts are English.

## Home Assistant

The P4 Gateway integration turns each notification into the **Notification** event entity
(`event_type` = level, attributes `topic`, `text`, `time`) and, from the level chosen per topic in the
integration options, into a persistent notification. Details:
[home-assistant.md](home-assistant.md#p4-gateway-integration-native-without-mqtt).

## Telegram bot

Admin, System › Notifications:

1. In Telegram open @BotFather, send `/newbot`, pick a name and a user name ending with `bot`.
2. Paste the token into the gateway, turn the bot on, choose the time zone (for quiet hours).

Each user, My account › Telegram and notifications:

- **Pair**: opens `https://t.me/<bot>?start=<code>` (or send `/start <code>` to the bot); the code
  is valid 10 minutes. A chat belongs to one account; pairing it again moves it.
- Level per topic: off, critical only, warnings and critical, all. Security notifications go only to
  admins; the other topics need read access to System › Information, System › LAN or Sound › Detection
  log.
- Quiet hours: only critical notifications in that time. `/mute 1h` (`30m`, `1d`, `off`) mutes sound
  notifications for a while.
- Language of the bot: Czech or English.

### Commands

| Command | Right (page, level) | Confirmation |
|---|---|---|
| `/help` | – | – |
| `/status` | Overview, read | – |
| `/lan` | System › LAN, read | – |
| `/thread`, `/zigbee` | Thread / Zigbee › Information, read | – |
| `/noise` | Sound › Live, read | – |
| `/events` | Sound › Detection log, read | – |
| `/update` (gateway and C6 versions, build, release notes) | System › Firmware, read | – |
| `/releases` (releases this gateway can install) | System › Firmware, read | – |
| `/install`, `/install c6` (gateway firmware, C6 package), `/install v1.6`, `/install c6 v1.6` (a listed release, rollback) | System › Firmware, edit | yes |
| `/restart` (C6 and P4) | System › Maintenance, edit | yes |
| `/identify` | Overview, edit | yes |
| `/role <name>` | Role, edit | yes |
| `/sound on`, `/sound off` (recognition) | Sound › Live, edit | yes |
| `/mute <time>` | – | – |

A command that changes something shows Confirm / Cancel buttons (valid 2 minutes); for an account
with two-factor sign-in the bot then asks for the authenticator code. Commands run from Telegram are
security notifications.

### How it runs

One task long-polls `getUpdates` (5 s) over a kept TLS connection to `api.telegram.org` and sends
what is queued between the polls: a notification leaves within about 5 s and only one TLS session
(about 40 kB of internal RAM) is open. The bot token is a secret setting (`tg_token`), exported only
in a settings backup with passwords.

API: `GET/POST /api/telegram` (admins: `token`, `enabled`, `tz`), `GET/POST /api/auth/telegram`
(own levels, quiet hours, language), `POST /api/auth/telegram/{pair,unpair,test}`.
