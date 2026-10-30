#pragma once

#include <stdbool.h>
#include <stdint.h>

/* Between the user accounts (gw_auth.c) and the Telegram bot (gw_telegram.c). */

enum { GW_TG_SECURITY, GW_TG_SYSTEM, GW_TG_NETWORK, GW_TG_SOUND, GW_TG_TOPICS };

typedef struct {
    int64_t chat;                /* paired Telegram chat, 0 none */
    uint8_t levels[GW_TG_TOPICS]; /* 0 off, 1 critical, 2 warnings and critical, 3 all */
    int8_t quiet_from, quiet_to; /* hours of local time with critical notifications only, -1 none */
    char lang[3];                /* "cs" or "en" */
} gw_tg_prefs_t;

void gw_tg_prefs_default(gw_tg_prefs_t *p);
void gw_auth_lock(void);
void gw_auth_unlock(void);
int gw_auth_users_max(void);
bool gw_auth_user_valid(int i);
const char *gw_auth_user_name(int i);
bool gw_auth_user_admin(int i);
bool gw_auth_user_totp(int i);
/* a current TOTP code of user i (not a recovery code) */
bool gw_auth_user_code(int i, const char *code);
int gw_auth_user_level(int i, const char *page);
gw_tg_prefs_t *gw_auth_user_tg(int i);
void gw_auth_user_save(int i);
int gw_auth_user_by_chat(int64_t chat);
/* user of the HTTP request being handled, -1 for tokens and the open gateway */
int gw_auth_current_user(void);
