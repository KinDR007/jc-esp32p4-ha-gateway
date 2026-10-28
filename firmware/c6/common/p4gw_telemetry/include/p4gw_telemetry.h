#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Runtime telemetry of the C6 for the P4 gateway, one JSON object:
 *   {"fw":"ot_rcp","ver":"...","temp_c":41.5,"heap_free":..,"heap_min":..,"uptime_s":..,"reset":"poweron","rev":1}
 * Shared by all C6 firmwares; each one delivers it over its own link to the P4.
 *
 * A task of its own refreshes the JSON every 10 s, so the link code (for the RCP the OpenThread
 * task with its small stack) only copies a finished string and never touches drivers.
 */
#define P4GW_TELEMETRY_MAX 192

/* Starts the refresh task; fw names the firmware in the JSON (static string). */
void p4gw_telemetry_start(const char *fw);
/* Copies the latest JSON into buf (with terminator), returns its length; 0 before the first refresh. */
size_t p4gw_telemetry_get(char *buf, size_t size);

/* Spinel vendor property the OpenThread RCP answers with the telemetry JSON (see the openthread patch). */
#define P4GW_SPINEL_PROP_TELEMETRY 0x3cf0

#ifdef __cplusplus
}
#endif
