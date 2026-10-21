/* Reading a thrown exception object (TOTK_TRACE_THROW=1), and the reason this cannot also be a test of whether the
 * exception was catchable.
 *
 * A managed exception in this game is raised as C++ Il2CppExceptionWrapper: __cxa_throw hands it to the SDK's own
 * unwinder, which either reaches a personality routine and a catch clause or returns _URC_END_OF_STACK (5) and lets
 * __cxa_throw call std::terminate.
 *
 * Both hooks here change that outcome, so a run with either of them enabled always ends in the abort:
 *
 *   __cxa_throw           the wrapper needs a frame of its own, and an unwind that starts at a host frame has no FDE
 *                         to describe the guest's caller
 *   _Unwind_RaiseException  worse -- it is the function that walks the stack, so replacing it replaces the walk itself;
 *                         the reason code it returns describes the host's stack, not the game's, which is how an
 *                         earlier investigation came to "phase one found no handler" for exceptions that in fact
 *                         unwind correctly. Enable TOTK_TRACE_THROW only to read the exception's own fields.
 *
 * __cxa_throw is handed the wrapper object itself, whose single member is the Il2CppException*; every string in that
 * is an Il2CppString: { Il2CppObject object; int32_t length; Il2CppChar chars[]; }, chars at +0x14.
 */
#include <stdio.h>
#include <stdlib.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include "runtime.h"

static void print_managed_string(const char *label, uint64_t p) {
    if (!p) return;
    int32_t len = 0; mach_vm_size_t got = 0;
    if (mach_vm_read_overwrite(mach_task_self(), p + 0x10, 4, (uintptr_t)&len, &got) != KERN_SUCCESS || got != 4) return;
    if (len <= 0 || len > 4096) return;
    char raw[8192];
    if ((uint64_t)len * 2 > sizeof raw) return;
    if (mach_vm_read_overwrite(mach_task_self(), p + 0x14, (mach_vm_size_t)len * 2, (uintptr_t)raw, &got) != KERN_SUCCESS) return;
    if (got != (mach_vm_size_t)len * 2) return;
    char out[8192]; size_t w = 0;
    for (int32_t i = 0; i < len && w + 1 < sizeof out; i++) {
        unsigned c = (unsigned char)raw[i * 2] | ((unsigned)(unsigned char)raw[i * 2 + 1] << 8);
        out[w++] = (c >= 0x20 && c < 0x7f) ? (char)c : (c == '\n' || c == '\t' ? ' ' : '?');
    }
    out[w] = 0;
    fprintf(stderr, "[dora-exc]   %s: %s\n", label, out);
}

/* The exception object's own field offsets are not worth guessing: which ones hold the message, the
 * file name and the trace depends on the IL2CPP version and on how deep the exception's class sits.
 * Every one of them is a pointer to an Il2CppString, though, so walk the object's pointer slots and
 * print the ones that actually read back as a string. A slot that is not one fails the length and
 * character test and is skipped. */
static void hunt_strings(uint64_t ex) {
    uint64_t w[24]; mach_vm_size_t got = 0;
    if (mach_vm_read_overwrite(mach_task_self(), (uintptr_t)ex, sizeof w, (uintptr_t)w, &got) != KERN_SUCCESS || got != sizeof w) return;
    for (unsigned i = 1; i < 24; i++) {
        uint64_t p = w[i];
        if (p < 0x1000) continue;
        int32_t len = 0;
        if (mach_vm_read_overwrite(mach_task_self(), (uintptr_t)p + 0x10, 4, (uintptr_t)&len, &got) != KERN_SUCCESS || got != 4) continue;
        if (len < 1 || len > 2048) continue;
        char raw[4097 * 2];
        if (mach_vm_read_overwrite(mach_task_self(), (uintptr_t)p + 0x14, (mach_vm_size_t)len * 2, (uintptr_t)raw, &got) != KERN_SUCCESS) continue;
        if (got != (mach_vm_size_t)len * 2) continue;
        char out[4097]; size_t k = 0; unsigned printable = 0;
        for (int32_t c = 0; c < len; c++) {
            unsigned u = (unsigned char)raw[c * 2] | ((unsigned)(unsigned char)raw[c * 2 + 1] << 8);
            if (u == '\n' || u == '\t') u = ' ';
            if (u >= 0x20 && u < 0x7f) printable++;
            out[k++] = (u >= 0x20 && u < 0x7f) ? (char)u : '?';
        }
        out[k] = 0;
        if (printable * 10 < (unsigned)len * 9) continue;    /* a string, not a table of pointers read as UTF-16 */
        fprintf(stderr, "[dora-exc]   [%#04x] %s\n", i * 8, out);
    }
}

/* Does the guest's own unwinder see guest frames at all? _Unwind_Backtrace walks the stack with the same FDE search
 * that _Unwind_RaiseException phase one uses, so it answers the question the throw itself cannot: if the walk stops at
 * the first guest frame, the loaded images' .eh_frame is registered with nothing, phase one never reaches a personality
 * routine, and every throw ends in std::terminate no matter who was ready to catch it. */
static unsigned long (*unw_getip)(void *);
static int unw_n;
static int unw_cb(void *ctx, void *arg) {
    (void)arg;
    extern void runtime_describe_address(uint64_t a, char *out, size_t n);
    uint64_t ip = unw_getip ? unw_getip(ctx) : 0;
    char d[160];
    runtime_describe_address(ip, d, sizeof d);
    fprintf(stderr, "[dora-exc]   bt[%02d] %s\n", unw_n++, d);
    return unw_n > 24 ? 5 : 0;                       /* 5 = _URC_END_OF_STACK, 0 = _URC_NO_REASON */
}
static void unwind_probe(void) {
    int (*backtrace)(int (*)(void *, void *), void *) = runtime_symbol("_Unwind_Backtrace");
    unw_getip = runtime_symbol("_Unwind_GetIP");
    unw_n = 0;
    /* The decisive question is not how far a walk gets but whether any image's FDE is known at all: _Unwind_Find_FDE
       takes a pc and answers with the entry that covers it. A null answer for a guest text address means the loaded
       images' tables were never registered, and then phase one of a throw can never reach a personality routine. */
    extern uint8_t *runtime_module_base(const char *name);
    void *(*find_fde)(void *, void *) = runtime_symbol("_Unwind_Find_FDE");
    if (find_fde) {
        uint64_t bases[3] = {0, 0, 0};
        for (unsigned i = 0; i < 3; i++) {
            const char *names[3] = {"main", "subsdk0", "sdk"};
            uint8_t *b = runtime_module_base(names[i]);
            if (!b) continue;
            void *fde = find_fde(b + 0x10000, bases);
            fprintf(stderr, "[dora-exc]   Find_FDE(%s+0x10000) = %p\n", names[i], fde);
        }
    } else fprintf(stderr, "[dora-exc]   Find_FDE missing\n");
    if (!backtrace || !unw_getip) { fprintf(stderr, "[dora-exc]   unwind probe: symbols missing\n"); return; }
    int rc = backtrace(unw_cb, NULL);
    fprintf(stderr, "[dora-exc]   unwind probe: %d frames, returned %d\n", unw_n, rc);
}

static void traced_throw(void *wrapper, void *tinfo, void (*dest)(void *)) {
    const char *name = tinfo ? *(const char *const *)((const char *)tinfo + 8) : NULL;
    fprintf(stderr, "[dora-exc] __cxa_throw wrapper=%p type=%s\n", wrapper, name ? name : "?");
    fprintf(stderr, "[dora-exc]   env throw=%s unwind=%s\n",
            getenv("TOTK_TRACE_THROW") ? getenv("TOTK_TRACE_THROW") : "(unset)",
            getenv("TOTK_TRACE_UNWIND") ? getenv("TOTK_TRACE_UNWIND") : "(unset)");
    /* The exception object itself carries no text for this one, so the only evidence left is where it came from.
       __cxa_throw is exempt from the host-stack wrapper (it must not return), and a PLT stub has no frame of its
       own, so frame 1 is the guest function that threw: its return address is the throw site's caller, and its
       frame pointer starts the guest chain. */
    {
        extern void runtime_describe_address(uint64_t a, char *out, size_t n);
        char here[160];
        runtime_describe_address((uint64_t)__builtin_return_address(0), here, sizeof here);
        fprintf(stderr, "[dora-exc]   thrown at %s\n", here);
        runtime_trace_guest((uint64_t)__builtin_frame_address(1), (uint64_t)__builtin_return_address(1));
        if (getenv("TOTK_TRACE_UNWIND")) unwind_probe();
    }
    if (wrapper) {
        uint64_t ex = 0; mach_vm_size_t got = 0;
        if (mach_vm_read_overwrite(mach_task_self(), (uintptr_t)wrapper, 8, (uintptr_t)&ex, &got) == KERN_SUCCESS && got == 8 && ex) {
            fprintf(stderr, "[dora-exc]   Il2CppException=%#llx\n", (unsigned long long)ex);
            uint64_t w[12]; mach_vm_size_t wgot = 0;
            if (mach_vm_read_overwrite(mach_task_self(), (uintptr_t)ex, sizeof w, (uintptr_t)w, &wgot) == KERN_SUCCESS && wgot == sizeof w) {
                /* Il2CppClass: image, gc_desc, name, namespaze — the managed type of the exception. */
                char nm[128] = {0}, ns[128] = {0};
                uint64_t pname = 0, pns = 0;
                mach_vm_read_overwrite(mach_task_self(), w[0] + 0x10, 8, (uintptr_t)&pname, &wgot);
                mach_vm_read_overwrite(mach_task_self(), w[0] + 0x18, 8, (uintptr_t)&pns, &wgot);
                if (pns) mach_vm_read_overwrite(mach_task_self(), pns, sizeof ns - 1, (uintptr_t)ns, &wgot);
                if (pname) mach_vm_read_overwrite(mach_task_self(), pname, sizeof nm - 1, (uintptr_t)nm, &wgot);
                fprintf(stderr, "[dora-exc]   managed type: %s%s%s\n", ns[0] ? ns : "", ns[0] ? "." : "", nm);
                /* The object's own layout is what tells the message from the trace: System.Exception puts
                   _className and _message first, so a null there means the exception was built with no text and
                   only the throw site identifies it. */
                for (unsigned i = 1; i < 8; i++) fprintf(stderr, "[dora-exc]   [+%#04x] %#llx\n", i * 8, (unsigned long long)w[i]);
                print_managed_string("class", w[5]);
                print_managed_string("message", w[3]);
                print_managed_string("trace", w[6]);
                hunt_strings(ex);
            }
        }
    }
    /* The guest's own __cxa_throw, not the host's: the host one hands a guest exception object to the host unwinder,
       which knows nothing about the loaded images. */
    static void (*real)(void *, void *, void (*)(void *));
    if (!real) real = (void (*)(void *, void *, void (*)(void *)))runtime_guest_symbol_value("__cxa_throw");
    if (!real) real = runtime_symbol("__cxa_throw");
    if (!real) { fprintf(stderr, "[dora-exc] __cxa_throw not found\n"); abort(); }
    real(wrapper, tinfo, dest);
    abort();                            /* __cxa_throw does not return */
}

/* nn::ro::detail::GetExceptionInfo (sdk+0x392610) is where a guest exception either finds the image it
 * was thrown from or gives up, and it is the unwinder's primary source of FDEs: sdk+0x55c134 calls it
 * for the pc of each frame and only falls back to the __unw_add_dynamic_fde table when it says no.
 *
 * It never simply returns false on a port problem, either. Every step that disagrees calls AbortImpl with
 * an empty message, so a wrong answer here kills the unwinder instead of failing the search -- which is
 * exactly what "C++ exception aborts in GetExceptionInfo" looks like from outside. The decision procedure
 * is the part that depends on the port, because all of it is read from svcQueryMemory:
 *
 *   1. the pc page must be permission 5 (read+execute) and state 3 (Code) or 8 (ModuleCode)
 *   2. walk down while pages keep that state and permission -- the last base is the module start
 *   3. walk up over the Code pages, then over the CodeData pages (4, or 9 after ModuleCode), to its end
 *   4. read MOD0 at base + *(u32 *)(base + 4) and take its .eh_frame_hdr offset and size
 *
 * A loader whose QueryMemory claims every unknown host page is Code+RX makes step 2 run past the image
 * forever, and every throw then aborts here. This reports each lookup and replays the procedure against
 * the same answers, so a failure names the step that rejected the pc. */
static int32_t (*real_get_exception_info)(void *, uint64_t);
static unsigned ro_lookups;
static int guest_read(uint64_t address, void *out, size_t n) {
    mach_vm_size_t got = 0;
    if (!address) return 0;
    return mach_vm_read_overwrite(mach_task_self(), (uintptr_t)address, n, (uintptr_t)out, &got) == KERN_SUCCESS && got == n;
}

static void trace_ro_lookup(uint64_t pc) {
    MemInfo m;
    runtime_query_memory(pc, &m);
    fprintf(stderr, "[dora-ro] #%u pc=%#llx -> base=%#llx size=%#llx state=%u perm=%u\n",
            ++ro_lookups, (unsigned long long)pc, (unsigned long long)m.base,
            (unsigned long long)m.size, m.state, m.perm);
    unsigned state = m.state;
    if (m.perm != 5 || (state != 3 && state != 8)) {
        fprintf(stderr, "[dora-ro]   no module: needs perm 5 and state 3 or 8, got perm %u state %u\n", m.perm, state);
        return;
    }
    uint64_t addr = pc, start = 0, end = 0, edge = 0;
    int matched = 0;
    for (int i = 0; i < 0x40000; i++) {
        runtime_query_memory(addr, &m);
        if (m.state != state || m.perm != 5) break;
        start = m.base; matched = 1;
        if (!m.base || m.base - 1 > pc) break;
        addr = m.base - 1;
    }
    if (!matched) { fprintf(stderr, "[dora-ro]   the downward walk matched nothing\n"); return; }
    matched = 0;
    addr = pc;
    for (int i = 0; i < 0x40000; i++) {
        runtime_query_memory(addr, &m);
        if (m.state != state) break;
        edge = m.base + m.size; end = edge - 1; matched = 1;
        if (!m.size || edge < pc) break;
        addr = edge;
    }
    if (!matched) { fprintf(stderr, "[dora-ro]   the Code walk matched nothing\n"); return; }
    unsigned next = state == 3 ? 4 : 9;
    addr = end + 1;
    for (int i = 0; i < 0x40000; i++) {
        runtime_query_memory(addr, &m);
        if (m.state != next) break;
        edge = m.base + m.size; end = edge - 1;
        if (!m.size || edge < addr) break;
        addr = edge;
    }
    uint32_t mod0_off = 0;
    if (!guest_read(start + 4, &mod0_off, 4)) { fprintf(stderr, "[dora-ro]   [base+4] unreadable\n"); return; }
    uint64_t mod0 = start + mod0_off;
    char magic[5] = {0};
    if (!guest_read(mod0, magic, 4)) { fprintf(stderr, "[dora-ro]   MOD0 at %#llx unreadable\n", (unsigned long long)mod0); return; }
    uint32_t eh[2] = {0, 0};
    if (!__builtin_memcmp(magic, "MOD0", 4)) guest_read(mod0 + 0x10, eh, 8);
    fprintf(stderr, "[dora-ro]   start=%#llx size=%#llx mod0=+%#x magic=%.4s eh_hdr=+%#x size=%#x -> %#llx\n",
            (unsigned long long)start, (unsigned long long)(end - start + 1), mod0_off, magic,
            mod0_off + eh[0], eh[1] - eh[0], (unsigned long long)(start + mod0_off + eh[0]));
}

static int32_t traced_get_exception_info(void *out, uint64_t pc) {
    if (!real_get_exception_info)
        real_get_exception_info = (int32_t (*)(void *, uint64_t))runtime_guest_symbol_value(
            "_ZN2nn2ro6detail16GetExceptionInfoEPNS1_13ExceptionInfoEm");
    if (ro_lookups < 64) trace_ro_lookup(pc);
    if (!real_get_exception_info) { fprintf(stderr, "[dora-ro]   GetExceptionInfo not found in the guest images\n"); return 0; }
    int32_t r = real_get_exception_info(out, pc);
    if (ro_lookups <= 64) {
        uint64_t o[4] = {0, 0, 0, 0};
        if (out && guest_read((uintptr_t)out, o, sizeof o))
            fprintf(stderr, "[dora-ro]   -> %d base=%#llx size=%#llx eh=+%#llx/%#llx\n", r,
                    (unsigned long long)o[0], (unsigned long long)o[1],
                    (unsigned long long)o[2], (unsigned long long)o[3]);
    }
    return r;
}

void *runtime_dora_exc_override(const char *name) {
    if (getenv("TOTK_TRACE_RO") &&
        !__builtin_strcmp(name, "_ZN2nn2ro6detail16GetExceptionInfoEPNS1_13ExceptionInfoEm"))
        return (void *)traced_get_exception_info;
    if (!getenv("TOTK_TRACE_THROW")) return NULL;
    if (!__builtin_strcmp(name, "__cxa_throw")) return (void *)traced_throw;
    return NULL;
}
