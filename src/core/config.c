#include "linkpulse/config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void lp_config_defaults(lp_config_t *config)
{
    config->mode = LP_IFACE_SELECT_AUTO;
    config->iface_name[0] = '\0';
    config->include_virtual = false;
    config->use_bits = false;
    config->interval_ms = 1000;
}

static const char *mode_name(lp_iface_select_mode_t mode)
{
    switch (mode) {
    case LP_IFACE_SELECT_MANUAL:
        return "manual";
    case LP_IFACE_SELECT_ALL:
        return "all";
    case LP_IFACE_SELECT_AUTO:
    default:
        return "auto";
    }
}

static void trim(char *s)
{
    char *start = s;
    while (*start == ' ' || *start == '\t') {
        ++start;
    }
    if (start != s) {
        memmove(s, start, strlen(start) + 1);
    }
    size_t len = strlen(s);
    while (len > 0 &&
           (s[len - 1] == ' ' || s[len - 1] == '\t' || s[len - 1] == '\r' || s[len - 1] == '\n')) {
        s[--len] = '\0';
    }
}

static void apply_line(lp_config_t *config, char *line)
{
    trim(line);
    if (line[0] == '\0' || line[0] == '#' || line[0] == ';') {
        return;
    }

    char *eq = strchr(line, '=');
    if (eq == NULL) {
        return;
    }
    *eq = '\0';
    char *key = line;
    char *value = eq + 1;
    trim(key);
    trim(value);

    if (strcmp(key, "mode") == 0) {
        if (strcmp(value, "manual") == 0) {
            config->mode = LP_IFACE_SELECT_MANUAL;
        } else if (strcmp(value, "all") == 0) {
            config->mode = LP_IFACE_SELECT_ALL;
        } else if (strcmp(value, "auto") == 0) {
            config->mode = LP_IFACE_SELECT_AUTO;
        }
    } else if (strcmp(key, "iface") == 0) {
        snprintf(config->iface_name, sizeof(config->iface_name), "%s", value);
    } else if (strcmp(key, "include_virtual") == 0) {
        if (strcmp(value, "1") == 0) {
            config->include_virtual = true;
        } else if (strcmp(value, "0") == 0) {
            config->include_virtual = false;
        }
    } else if (strcmp(key, "use_bits") == 0) {
        if (strcmp(value, "1") == 0) {
            config->use_bits = true;
        } else if (strcmp(value, "0") == 0) {
            config->use_bits = false;
        }
    } else if (strcmp(key, "interval_ms") == 0) {
        char *end = NULL;
        const unsigned long parsed = strtoul(value, &end, 10);
        const unsigned as_u = (unsigned)parsed;
        if (end != value && *end == '\0' && parsed > 0 && (unsigned long)as_u == parsed) {
            config->interval_ms = as_u;
        }
    }
}

void lp_config_parse(lp_config_t *config, const char *text)
{
    if (config == NULL || text == NULL) {
        return;
    }

    char line[512];
    size_t line_len = 0;
    for (const char *p = text;; ++p) {
        const char c = *p;
        if (c == '\n' || c == '\0') {
            line[line_len] = '\0';
            apply_line(config, line);
            line_len = 0;
            if (c == '\0') {
                break;
            }
        } else if (line_len + 1 < sizeof(line)) {
            line[line_len++] = c;
        }
    }
}

size_t lp_config_serialize(const lp_config_t *config, char *out, size_t cap)
{
    if (out == NULL || cap == 0) {
        return 0;
    }
    if (config == NULL) {
        out[0] = '\0';
        return 0;
    }

    const int written =
        snprintf(out, cap,
                 "mode=%s\n"
                 "iface=%s\n"
                 "include_virtual=%d\n"
                 "use_bits=%d\n"
                 "interval_ms=%u\n",
                 mode_name(config->mode), config->iface_name, config->include_virtual ? 1 : 0,
                 config->use_bits ? 1 : 0, config->interval_ms);

    if (written < 0) {
        out[0] = '\0';
        return 0;
    }
    return ((size_t)written < cap) ? (size_t)written : cap - 1;
}
