#ifndef LINKPULSE_ISP_H
#define LINKPULSE_ISP_H

#include "linkpulse/status.h"

#define LP_ISP_IP_MAX 46
#define LP_ISP_NAME_MAX 128
#define LP_ISP_ASN_MAX 16
#define LP_ISP_HOSTNAME_MAX 256
#define LP_ISP_PLACE_MAX 128
#define LP_ISP_COUNTRY_MAX 8
#define LP_ISP_POSTAL_MAX 16
#define LP_ISP_TIMEZONE_MAX 64
#define LP_ISP_LOC_MAX 32

/* Every field the lookup service can report for the internet-facing connection. */
typedef struct {
    char public_ip[LP_ISP_IP_MAX];
    char isp[LP_ISP_NAME_MAX];     /* organization name, without the leading ASxxxx */
    char asn[LP_ISP_ASN_MAX];      /* e.g. "AS3209" */
    char hostname[LP_ISP_HOSTNAME_MAX];
    char city[LP_ISP_PLACE_MAX];
    char region[LP_ISP_PLACE_MAX];
    char country[LP_ISP_COUNTRY_MAX];
    char postal[LP_ISP_POSTAL_MAX];
    char timezone[LP_ISP_TIMEZONE_MAX];
    char loc[LP_ISP_LOC_MAX]; /* "latitude,longitude" */
} lp_isp_info_t;

/* Looks up all available public IP/ISP/location fields via an external HTTPS
   lookup service. Populates as many fields as the response provides; missing
   fields are left as empty strings. */
lp_status_t lp_isp_lookup(lp_isp_info_t *out);

#endif /* LINKPULSE_ISP_H */
