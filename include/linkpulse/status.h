#ifndef LINKPULSE_STATUS_H
#define LINKPULSE_STATUS_H

typedef enum {
    LP_OK = 0,
    LP_ERR_INVALID_ARG,
    LP_ERR_NO_MEMORY,
    LP_ERR_IO,
    LP_ERR_NOT_FOUND,
    LP_ERR_UNSUPPORTED
} lp_status_t;

const char *lp_status_str(lp_status_t status);

#endif /* LINKPULSE_STATUS_H */
