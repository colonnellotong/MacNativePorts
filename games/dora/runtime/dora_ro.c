/* nn::ro adapter (Doraemon Story of Seasons).
 *
 * nn::ro is the in-process face of Horizon's dynamic loader: it keeps a list of RoModule entries
 * and forwards module registration to the system loader over IPC on the "ldr:ro" port. This host
 * replaced that loader — every module is already mapped, relocated and sealed before nnMain runs —
 * so the service has nothing to answer. What the game imports from nn::ro is eight entry points,
 * and they are answered here directly.
 *
 * This is the same shape as the nn::fs adapter: Unity goes through nn::sf for system services that
 * do their work in another process, and this process is where that work already happened. Without
 * these, the first nn::ro::Initialize call opens a session to "ldr:ro" and stops the runtime on a
 * service no one was ever going to implement.
 *
 * Result codes match the ones the SDK itself returns: 0 is success, 0xA16 is "symbol not found"
 * (the value nn::ro::LookupSymbol leaves in w0 when both module lists are exhausted), 0x816 is the
 * NRO header check in GetBufferSize, 0xC16 the NRR header check in RegisterModuleInfo.
 */
#include "runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define R_NOT_AN_NRO        0x816u   /* GetBufferSize: header magic is not NRO0 */
#define R_SYMBOL_NOT_FOUND  0xa16u
#define R_NOT_AN_NRR        0xc16u   /* RegisterModuleInfo: header magic is not NRR0 */

/* ---------- diagnostics ----------
 * DORA_RO_LOG=1 prints every call. The default is silent: these are all success paths, and a line
 * per call would drown the boot log. */
static int log_on(void) {
    static int on = -1;
    if (on < 0) on = getenv("DORA_RO_LOG") != NULL;
    return on;
}
static void note(const char *what, const char *detail) {
    if (log_on()) fprintf(stderr, "[dora-ro] %s%s%s\n", what, detail ? ": " : "", detail ? detail : "");
}

/* ---------- Initialize ----------
 * The guest version is a tail call into InitializeWithPortName("ldr:ro"), which resolves the port,
 * asks it for the process module list and stores the resulting interface in a global. The list it
 * would receive is the one this host built while loading, so the call becomes a no-op. */
static uint32_t ro_initialize(void) {
    note("Initialize", "modules were registered by the host loader");
    return 0;
}

/* ---------- LookupSymbol ----------
 * The guest version walks the auto-load list and then the manual-load list, comparing each module's
 * symbol table, and returns value + module base. runtime_symbol does the same search over the
 * modules this host loaded, including the case where the symbol lives in another module. */
static uint32_t ro_lookup_symbol(uintptr_t *out, const char *name) {
    void *p = name ? runtime_symbol(name) : NULL;
    if (!p) {
        note("LookupSymbol not found", name ? name : "(null)");
        return R_SYMBOL_NOT_FOUND;
    }
    if (out) *out = (uintptr_t)p;
    note("LookupSymbol", name);
    return 0;
}

/* ---------- module registration ----------
 * Every module announces itself to nn::ro during its initializers, and the guest implementation
 * forwards the image to ldr:ro. The host already knows these images, so the entry is only recorded
 * so that UnregisterModuleInfo can be answered for the same pointer. */
typedef struct { const void *image; void *info; } Registration;
static Registration registrations[8];
static unsigned nregistrations;

static uint32_t ro_register_module_info(void *info, const void *image) {
    if (!image || memcmp(image, "NRR0", 4)) return R_NOT_AN_NRR;
    if (nregistrations == sizeof registrations / sizeof *registrations)
        runtime_fail("nn::ro registration table full");
    registrations[nregistrations++] = (Registration){image, info};
    note("RegisterModuleInfo", "recorded");
    return 0;
}

static uint32_t ro_unregister_module_info(void *info) {
    for (unsigned i = 0; i < nregistrations; i++)
        if (registrations[i].info == info) {
            registrations[i] = registrations[--nregistrations];
            note("UnregisterModuleInfo", "dropped");
            return 0;
        }
    note("UnregisterModuleInfo for an entry that was never registered", NULL);
    return 0;
}

/* ---------- GetBufferSize ----------
 * The guest version checks the NRO header and reads the size the module needs for its own
 * bookkeeping at +0x38. Same check, same field: the answer stays meaningful even though no buffer
 * is ever allocated, because LoadModule below refuses instead of pretending. */
static uint32_t ro_get_buffer_size(size_t *out, const void *image) {
    if (!image || memcmp(image, "NRO0", 4)) return R_NOT_AN_NRO;
    if (out) memcpy(out, (const uint8_t *)image + 0x38, sizeof *out);
    note("GetBufferSize", "NRO header size field");
    return 0;
}

/* ---------- dynamic modules ----------
 * LoadModule/UnloadModule/LookupModuleSymbol load an NRR image the game supplies at run time and
 * relocate it inside the process. This host has no guest-visible relocation service, and a symbol
 * lookup that answers "not found" for a module that was never loaded would send the game down a
 * NULL path, so the load is refused where it starts. */
static uint32_t ro_load_module(void *out, const void *image, void *buffer, size_t size, int flag) {
    (void)out; (void)image; (void)buffer; (void)size; (void)flag;
    runtime_fail("nn::ro::LoadModule: run-time NRR loading is not supported by this host");
}
static uint32_t ro_unload_module(void *module) {
    (void)module;
    runtime_fail("nn::ro::UnloadModule: run-time NRR loading is not supported by this host");
}
static uint32_t ro_lookup_module_symbol(uintptr_t *out, const void *module, const char *name) {
    (void)out; (void)module; (void)name;
    runtime_fail("nn::ro::LookupModuleSymbol: run-time NRR loading is not supported by this host");
}

/* ---------- dispatch ---------- */
void *runtime_dora_ro_override(const char *name) {
    static const struct { const char *name; void *fn; } map[] = {
        {"_ZN2nn2ro10InitializeEv", (void *)ro_initialize},
        {"_ZN2nn2ro12LookupSymbolEPmPKc", (void *)ro_lookup_symbol},
        {"_ZN2nn2ro18RegisterModuleInfoEPNS0_16RegistrationInfoEPKv", (void *)ro_register_module_info},
        {"_ZN2nn2ro20UnregisterModuleInfoEPNS0_16RegistrationInfoE", (void *)ro_unregister_module_info},
        {"_ZN2nn2ro13GetBufferSizeEPmPKv", (void *)ro_get_buffer_size},
        {"_ZN2nn2ro10LoadModuleEPNS0_6ModuleEPKvPvmi", (void *)ro_load_module},
        {"_ZN2nn2ro12UnloadModuleEPNS0_6ModuleE", (void *)ro_unload_module},
        {"_ZN2nn2ro18LookupModuleSymbolEPmPKNS0_6ModuleEPKc", (void *)ro_lookup_module_symbol},
    };
    for (size_t i = 0; i < sizeof map / sizeof *map; i++)
        if (!strcmp(name, map[i].name)) return map[i].fn;
    return NULL;
}
