#include "cvite/status.h"

const char *cvite_status_string(cvite_status status)
{
    switch (status) {
    case CVITE_STATUS_OK:
        return "ok";
    case CVITE_STATUS_INVALID_ARGUMENT:
        return "invalid argument";
    case CVITE_STATUS_OUT_OF_MEMORY:
        return "out of memory";
    case CVITE_STATUS_DUPLICATE_SYMBOL:
        return "duplicate symbol";
    case CVITE_STATUS_UNKNOWN_SYMBOL:
        return "unknown symbol";
    case CVITE_STATUS_ABI_MISMATCH:
        return "ABI mismatch";
    case CVITE_STATUS_LAYOUT_MISMATCH:
        return "layout mismatch";
    case CVITE_STATUS_STALE_GENERATION:
        return "stale generation";
    case CVITE_STATUS_INVALID_STATE:
        return "invalid state";
    case CVITE_STATUS_INVALID_PACKET:
        return "invalid packet";
    case CVITE_STATUS_UNSUPPORTED_PROTOCOL:
        return "unsupported protocol";
    case CVITE_STATUS_CHECKSUM_MISMATCH:
        return "checksum mismatch";
    case CVITE_STATUS_IO_ERROR:
        return "I/O error";
    case CVITE_STATUS_LINK_ERROR:
        return "link error";
    case CVITE_STATUS_NOT_IMPLEMENTED:
        return "not implemented";
    }

    return "unknown status";
}
