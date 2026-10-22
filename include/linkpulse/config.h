#ifndef LINKPULSE_CONFIG_H
#define LINKPULSE_CONFIG_H

#include <stdbool.h>
#include <stddef.h>

#include "linkpulse/sampler.h"
#include "linkpulse/status.h"

typedef struct {
    lp_iface_select_mode_t mode;
    char iface_name[LP_IFNAME_MAX];
    bool include_virtual;
    bool use_bits;
    unsigned interval_ms;
} lp_config_t;

/* Accepted poll interval range. Faster polling only burns CPU; slower polling
   makes the tray look frozen. */
#define LP_CONFIG_INTERVAL_MIN_MS 100u
#define LP_CONFIG_INTERVAL_MAX_MS 60000u

/* Initializes every configuration field to the application default. */
void lp_config_defaults(lp_config_t *config);

/* Parses a decimal poll interval in milliseconds. Rejects signs, whitespace,
   trailing characters, overflow, and values outside
   [LP_CONFIG_INTERVAL_MIN_MS, LP_CONFIG_INTERVAL_MAX_MS]. Returns false and
   leaves *out_ms untouched on rejection. */
bool lp_config_parse_interval(const char *text, unsigned *out_ms);

/* Parses "key=value" lines from an in-memory buffer (testable without real
   files). Blank lines and lines starting with '#' or ';' are ignored. Unknown
   keys and malformed values are ignored, leaving whatever *config already
   held for that field -- so callers should apply lp_config_defaults() first.
   A leading UTF-8 byte order mark is skipped. */
void lp_config_parse(lp_config_t *config, const char *text);

/* Serializes `config` as "key=value" lines into `out` (size `cap`), always
   NUL-terminated. Returns the number of bytes written, excluding the NUL. */
size_t lp_config_serialize(const lp_config_t *config, char *out, size_t cap);

/* Loads from the platform's config file, applying defaults first. Returns
   LP_OK even if the file does not exist yet (defaults are used); only real
   I/O errors on an existing file are reported. */
lp_status_t lp_config_load(lp_config_t *config);

/* Saves to the platform's config file, creating its directory if needed. */
lp_status_t lp_config_save(const lp_config_t *config);

#endif /* LINKPULSE_CONFIG_H */
