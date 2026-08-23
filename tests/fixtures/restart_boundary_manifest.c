#include "cvite/candidate.h"

const cvite_candidate_manifest __cvite_candidate_manifest = {
    CVITE_CANDIDATE_MANIFEST_SCHEMA,
    CVITE_CANDIDATE_FLAG_RESTART_TLS |
        CVITE_CANDIDATE_FLAG_RESTART_CONSTRUCTORS |
        CVITE_CANDIDATE_FLAG_RESTART_DESTRUCTORS,
    0U,
    0,
    0U,
    0,
};
