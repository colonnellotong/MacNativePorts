/* Which P/Invoke library is asked for, named at the moment it is asked for.
 *
 * IL2CPP resolves a [DllImport] library through a replaceable callback kept in main's data:
 *
 *   main+0x3600c0   the loader.  adrp x8,#0x6676000 / ldr x21,[x8,#0xa48] fetches the callback and
 *                   blr x21 is handed the library name; if it returns that same pointer the caller reads
 *                   it as "not found" and the generated wrapper raises DllNotFoundException.
 *   main+0x360194   the setter: adrp x8 / str x0,[x8,#0xa48] / ret.
 *
 * Reading the name out of a dead stack instead does not work: by the time the SDK reports the break the
 * frames are unwound, and the assembled message is a compile-time constant of the metadata, so finding its
 * text in memory proves nothing about whether it was thrown. Hooking here answers it directly.
 *
 * Unity installs its own callback during IL2CPP start-up, which runs well after this hook is placed, so the
 * slot is re-asserted on the way through every SVC: whichever side writes last, our wrapper stays in place
 * and the previous callback is remembered and chained to. TOTK_PINVOKE=0 switches it off.
 */
#include "runtime.h"
#include <libkern/OSCacheControl.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#define PINVOKE_SLOT_OFFSET 0x6676a48ULL   /* main: il2cpp PlatformInvoke's load-library callback slot */

static void **pinvoke_slot;
static void *pinvoke_original;
static char *(*pinvoke_chained)(const char *);
static int pinvoke_enabled = -1;

static char *pinvoke_logged(const char *name) {
    /* With no previous callback installed, the loader's own "not found" signal is the name pointer itself;
       returning NULL would be dereferenced by the caller (`ldrb w8,[x0]` right after the call). */
    char *result = pinvoke_chained ? pinvoke_chained(name) : (char *)name;
    if (!name) return result;
    if (!result || result == name) {
        /* Returning the name itself is how the loader says "no such module". */
        fprintf(stderr, "[pinvoke] \"%s\" -> not found\n", name);
        fflush(stderr);
    } else {
        static char seen[32][72];
        static unsigned count;
        for (unsigned i = 0; i < count; i++) if (!strcmp(seen[i], name)) return result;
        if (count < 32) snprintf(seen[count++], sizeof seen[0], "%s", name);
        fprintf(stderr, "[pinvoke] \"%s\" -> %s\n", name, result);
        fflush(stderr);
    }
    return result;
}

/* main may not be mapped yet; the address is resolved on first use. */
static void **pinvoke_slot_addr(void) {
    if (pinvoke_slot) return pinvoke_slot;
    uint8_t *base = runtime_module_base("main");
    if (!base) return NULL;
    return pinvoke_slot = (void **)(void *)(base + PINVOKE_SLOT_OFFSET);
}

void runtime_dora_pinvoke_poll(void) {
    if (pinvoke_enabled < 0) {
        const char *e = getenv("TOTK_PINVOKE");
        pinvoke_enabled = !(e && !strcmp(e, "0"));
    }
    if (!pinvoke_enabled) return;
    static unsigned counter;
    if (counter++ & 0x3f) return;                  /* ~1 in 64 SVCs is plenty: the callback is set once */
    void **slot = pinvoke_slot_addr();
    if (!slot) return;
    void *current = __atomic_load_n(slot, __ATOMIC_RELAXED);
    if (current == (void *)pinvoke_logged) return;
    if (current) { pinvoke_original = current; pinvoke_chained = (char *(*)(const char *))current; }
    __atomic_store_n(slot, (void *)pinvoke_logged, __ATOMIC_RELAXED);
}

/* ---- the Win32 surface ---------------------------------------------------
 *
 * A Switch binary still carries the desktop declarations its managed assemblies were compiled with:
 * kernel32.dll and advapi32.dll sit in the metadata next to the BCL's marshaling and SafeWaitHandle
 * code, and user32.dll next to GetKeyboardLayout. Those declarations are normally never reached, so
 * the library simply is not there and the generated wrapper raises DllNotFoundException.
 *
 * This game reaches one of them: a worker thread created right after the render window appears calls
 * user32!GetKeyboardLayout(0). The resolver is a single choke point, `main+0x3936c0`, reached through
 * the thunk at `main+0x3e9080` (a bare `b 0x3936c0` followed by three `udf #0`). The 16 bytes there
 * take an absolute jump, so the answer for one Win32 entry can be supplied here while everything else
 * goes on to the original resolver:
 *
 *   0x3e9080  ldr x16, #8      ; the address in the next eight bytes
 *   0x3e9084  br  x16
 *   0x3e9088  .quad address of pinvoke_answered
 *
 * The request the wrappers build on their stack is {char* lib, int64 libLen, char* entry,
 * int64 entryLen, uint32 pad, uint32 kind, uint32 pad2}.
 */
#define PINVOKE_RESOLVE_OFFSET 0x3936c0ULL   /* main: the P/Invoke resolver itself */
#define PINVOKE_THUNK_OFFSET   0x3e9080ULL   /* main: `b 0x3936c0` + three `udf #0` */

typedef struct {
    const char *lib; int64_t lib_len;
    const char *entry; int64_t entry_len;
    uint32_t zero, kind, tail;
} PInvokeRequest;

/* user32!GetKeyboardLayout(DWORD idThread) -> HKL. A layout handle is opaque and there is none here, so
   the answer is the null handle: "no layout", which is what a console has. */
static void *win32_get_keyboard_layout(uint32_t id_thread) {
    (void)id_thread;
    return NULL;
}

/* Called instead of the resolver, for every P/Invoke in the game. Each distinct (library, entry) pair is
   logged once — the wrapper caches the answer, so a second resolve of the same pair is worth seeing too and
   is logged with a count. Anything not answered here is handed to the original so its own error paths keep
   working. */
static void *pinvoke_answered(PInvokeRequest *req) {
    if (req) {
        static struct { char key[112]; unsigned hits; } seen[48];
        static unsigned used;
        char key[112];
        snprintf(key, sizeof key, "%.*s!%.*s",
                 (int)(req->lib_len < 55 ? req->lib_len : 55), req->lib ? req->lib : "?",
                 (int)(req->entry_len < 55 ? req->entry_len : 55), req->entry ? req->entry : "?");
        unsigned i = 0;
        while (i < used && strcmp(seen[i].key, key)) i++;
        if (i == used && used < 48) snprintf(seen[used++].key, sizeof seen[0].key, "%s", key);
        if (i < used) {
            unsigned hits = ++seen[i].hits;
            if (hits <= 2 || hits % 64 == 0) fprintf(stderr, "[pinvoke] resolve %s (call %u)\n", key, hits);
            else hits = 0;
            fflush(stderr);
            (void)hits;
        }
        if (!strcmp(key, "user32.dll!GetKeyboardLayout")) {
            fprintf(stderr, "[pinvoke] user32.dll!GetKeyboardLayout answered locally (null layout)\n");
            fflush(stderr);
            return (void *)win32_get_keyboard_layout;
        }
    }
    void *(*original)(void *) = (void *(*)(void *))(void *)
        (runtime_module_base("main") + PINVOKE_RESOLVE_OFFSET);
    return original(req);
}

static uint64_t pinvoke_text_page(uint64_t address, int prot) {
    uint64_t page = address & ~(uint64_t)0x3fff;
    return mprotect((void *)(uintptr_t)page, 0x4000, prot) == 0 ? 0 : 1;
}

/* Replace the resolver thunk with an absolute jump to our answer. Must run while main's text can still
   be written: once `seal()` has made it r-x a write would need the page taken out of execution, which
   cannot be done safely once guest threads are running. */
int runtime_dora_pinvoke_install(void) {
    const char *off = getenv("TOTK_PINVOKE_SHIM");
    if (off && !strcmp(off, "0")) return 0;
    uint8_t *base = runtime_module_base("main");
    if (!base) return 1;
    uint64_t thunk = (uintptr_t)(base + PINVOKE_THUNK_OFFSET);
    if (pinvoke_text_page(thunk, PROT_READ | PROT_WRITE)) return 1;
    uint32_t *w = (uint32_t *)(uintptr_t)thunk;
    w[0] = 0x58000050u;                     /* ldr x16, #8 */
    w[1] = 0xd61f0200u;                     /* br  x16 */
    memcpy(w + 2, &(uint64_t){(uint64_t)(uintptr_t)pinvoke_answered}, 8);
    sys_icache_invalidate((void *)(uintptr_t)thunk, 16);
    if (pinvoke_text_page(thunk, PROT_READ | PROT_EXEC)) return 1;
    return 0;
}

