#ifndef LINKPULSE_ISP_H
#define LINKPULSE_ISP_H

#include <stddef.h>

#include "linkpulse/status.h"

/* Looks up the public IP and ISP/organization name for the internet-facing
   connection via an external HTTPS lookup service. */
lp_status_t lp_isp_lookup(char *isp_out, size_t isp_cap, char *ip_out, size_t ip_cap);

#endif /* LINKPULSE_ISP_H */
