#include "linkpulse/status.h"

const char *lp_status_str(lp_status_t status)
{
    switch (status) {
    case LP_OK:
        return "ok";
    case LP_ERR_INVALID_ARG:
        return "invalid argument";
    case LP_ERR_NO_MEMORY:
        return "out of memory";
    case LP_ERR_IO:
        return "I/O error";
    case LP_ERR_NOT_FOUND:
        return "not found";
    case LP_ERR_UNSUPPORTED:
        return "unsupported on this platform";
    }
    return "unknown error";
}
