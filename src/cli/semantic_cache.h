#ifndef CVITE_CLI_SEMANTIC_CACHE_H
#define CVITE_CLI_SEMANTIC_CACHE_H

#ifdef __cplusplus
extern "C" {
#endif

/* Internal development-server API. Ordinary C programs never call it. */
#ifdef CVITE_SEMANTIC_CACHE_ENABLED
int cvite_semantic_cache_activate(
    const char *source_path,
    const char *active_ir_path);

int cvite_semantic_cache_stage(
    const char *source_path,
    const char *active_ir_path,
    const char *staged_ir_path);

void cvite_semantic_cache_promote(
    const char *source_path,
    const char *active_ir_path,
    const char *staged_ir_path);

void cvite_semantic_cache_discard(
    const char *source_path,
    const char *staged_ir_path);

unsigned cvite_semantic_cache_report_pending(void);
void cvite_semantic_cache_clear(void);
#else
static inline int cvite_semantic_cache_activate(
    const char *source_path,
    const char *active_ir_path)
{
    (void)source_path;
    (void)active_ir_path;
    return 0;
}

static inline int cvite_semantic_cache_stage(
    const char *source_path,
    const char *active_ir_path,
    const char *staged_ir_path)
{
    (void)source_path;
    (void)active_ir_path;
    (void)staged_ir_path;
    return 0;
}

static inline void cvite_semantic_cache_promote(
    const char *source_path,
    const char *active_ir_path,
    const char *staged_ir_path)
{
    (void)source_path;
    (void)active_ir_path;
    (void)staged_ir_path;
}

static inline void cvite_semantic_cache_discard(
    const char *source_path,
    const char *staged_ir_path)
{
    (void)source_path;
    (void)staged_ir_path;
}

static inline unsigned cvite_semantic_cache_report_pending(void)
{
    return 0U;
}

static inline void cvite_semantic_cache_clear(void)
{
}
#endif

#ifdef __cplusplus
}
#endif

#endif
