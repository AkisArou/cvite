#ifndef CVITE_CLI_SEMANTIC_CACHE_H
#define CVITE_CLI_SEMANTIC_CACHE_H

#ifdef __cplusplus
extern "C" {
#endif

/* Internal development-server API. Ordinary C programs never call it. */
int cvite_semantic_cache_stage(
    const char *source_path,
    const char *active_ir_path,
    const char *staged_ir_path);

void cvite_semantic_cache_promote(
    const char *source_path,
    const char *active_ir_path,
    const char *staged_ir_path);

unsigned cvite_semantic_cache_report_pending(void);
void cvite_semantic_cache_clear(void);

#ifdef __cplusplus
}
#endif

#endif
