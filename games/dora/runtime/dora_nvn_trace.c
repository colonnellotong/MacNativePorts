/* Records the NVN API surface this engine asks for.
 *
 * The game imports exactly one NVN symbol, nvnBootstrapLoader. Every other entry point is requested
 * by name at runtime, so nothing static tells us which part of NVN Unity uses. The shared layer's
 * name table (nvn_thunks.S, 523 names) was dumped from Tears of the Kingdom and does not describe
 * this game.
 *
 * This file answers the two hooks the shared layer offers:
 *   runtime_nvn_request  every name the engine asks for, with whether this build implements it.
 *   runtime_nvn_unknown  a stand-in for names it does not, so one run reaches the end of the
 *                        bootstrap instead of stopping at the first unknown entry.
 *
 * Each request is also written to stderr as it happens, so a crash right after still leaves the
 * list behind. At exit the whole table goes to DORA_NVN_TRACE (a TSV) when that variable is set.
 *
 * The stand-in returns 0. That is fine for collecting names: the engine asks for a batch of entry
 * points during start-up and only calls them afterwards, and the first call that cannot work is
 * exactly the point the list has already paid for.
 */
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { char *name; int hit; unsigned requests; } Entry;
static Entry *entries;
static size_t nentries, cap;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static _Atomic unsigned stand_in_calls;

static unsigned long long (*stand_in)(void);

/* One shared stand-in for every unimplemented name: only the count matters, and per-name call
   counts arrive on a later run through nvn_slot[].cnt once the name is in the table. */
static unsigned long long stand_in_body(void) {
    unsigned n = atomic_fetch_add(&stand_in_calls, 1) + 1;
    if (n <= 16 || (n % 1000) == 0) fprintf(stderr, "[dora-nvn] stand-in call #%u\n", n);
    return 0;
}

void *runtime_nvn_unknown(const char *name) {
    (void)name;
    if (!stand_in) stand_in = stand_in_body;
    return (void *)stand_in;
}

void runtime_nvn_request(const char *name, int hit) {
    if (!name) return;
    fprintf(stderr, "[dora-nvn] bootstrap %s hit=%d\n", name, hit);
    pthread_mutex_lock(&lock);
    for (size_t i = 0; i < nentries; i++)
        if (!strcmp(entries[i].name, name)) { entries[i].requests++; pthread_mutex_unlock(&lock); return; }
    if (nentries == cap) {
        size_t next = cap ? cap * 2 : 128;
        Entry *grown = realloc(entries, next * sizeof *grown);
        if (!grown) { pthread_mutex_unlock(&lock); return; }
        entries = grown;
        cap = next;
    }
    char *copy = strdup(name);
    if (copy) {
        entries[nentries++] = (Entry){copy, hit, 1};
    }
    pthread_mutex_unlock(&lock);
}

static int by_name(const void *a, const void *b) {
    return strcmp(((const Entry *)a)->name, ((const Entry *)b)->name);
}

static void dump(void) {
    FILE *f = NULL;
    const char *path = getenv("DORA_NVN_TRACE");
    if (path && *path) f = fopen(path, "w");
    pthread_mutex_lock(&lock);
    if (nentries) qsort(entries, nentries, sizeof *entries, by_name);
    unsigned table = 0, extra = 0, unknown = 0;
    if (f) fprintf(f, "name\thit\trequests\n");
    for (size_t i = 0; i < nentries; i++) {
        const Entry *e = &entries[i];
        if (e->hit == 1) table++;
        else if (e->hit == 2) extra++;
        else unknown++;
        if (f) fprintf(f, "%s\t%d\t%u\n", e->name, e->hit, e->requests);
    }
    pthread_mutex_unlock(&lock);
    fprintf(stderr, "[dora-nvn] %zu names asked for: %u in the shared table, %u in NVN_EXTRA, %u unknown; "
                    "stand-in called %u times\n", nentries, table, extra, unknown, atomic_load(&stand_in_calls));
    if (f) {
        fclose(f);
        fprintf(stderr, "[dora-nvn] trace written to %s\n", path);
    }
    (void)stand_in_calls;
}

__attribute__((constructor)) static void install_dump(void) {
    atexit(dump);
}
