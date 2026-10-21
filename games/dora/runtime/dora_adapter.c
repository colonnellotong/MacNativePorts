/* The one entry point the shared runtime calls for this game's own answers.
 *
 * runtime/services.c asks runtime_game_override first, before any shared table, so everything that is
 * particular to this engine collects here instead of being scattered through shared files. Each part
 * is a separate file with one subject:
 *
 *   dora_fs_ext.c     the nn::fs calls Unity makes and the shared host does not implement
 *   dora_ro.c         the nn::ro module registration Unity does, which the host loader replaced
 *   dora_nvn_trace.c  the NVN API recorder (also answers the shared graphics layer's weak hooks)
 *   dora_exc.c        opt-in: why an exception found no handler (TOTK_TRACE_THROW)
 *
 * Adding a third subject means adding a call below, not touching the shared runtime again.
 */
#include <stddef.h>

void *runtime_dora_fs_override(const char *name);
void *runtime_dora_ro_override(const char *name);
void *runtime_dora_exc_override(const char *name);

void *runtime_game_override(const char *name) {
    void *p = runtime_dora_fs_override(name);
    if (p) return p;
    p = runtime_dora_ro_override(name);
    if (p) return p;
    p = runtime_dora_exc_override(name);
    if (p) return p;
    return NULL;
}
