/* nn::fs adapter for Unity (Doraemon Story of Seasons).
 *
 * The shared host (runtime/fs_host.c, runtime/romfs.c) answers 26 of the 42 nn::fs symbols this
 * game imports. The 16 left are the part of nn::fs that a self-written engine does not reach but
 * Unity does: its own allocator, the global file-data cache, free-space queries, directory
 * create/delete/rename on storage, the add-on-content mount path, and the two-argument Commit.
 *
 * Nothing here reimplements a shared function. Where the shared host already knows the answer
 * (path rule, mount table, handle tables, commit, exact read) it is asked through the accessors
 * declared in fs_host.h. What is genuinely new is only the part that had no host side at all.
 *
 * Result codes match nn::fs: 0 is success, otherwise the 32-bit Result the game checks.
 */
#include "runtime.h"
#include "fs_host.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <unistd.h>

#define R_PATH_NOT_FOUND    0x202u
#define R_ALREADY_EXISTS    0x402u
#define R_NOT_EMPTY         0x1002u
#define R_TARGET_NOT_FOUND  0x7d402u
#define R_INVALID_ARGUMENT  0x2ee202u
#define R_NOT_SUPPORTED     0x6402u

/* ---------- diagnostics ----------
 * Every entry below is one the game wrote down by importing it, so it will be reached early.
 * DORA_FS_LOG=1 prints the ones that are refused or ignored, which is how a gap in the adapter
 * shows up as a line in the log instead of a wrong path later. */
static int log_on(void) {
    static int on = -1;
    if (on < 0) on = getenv("DORA_FS_LOG") != NULL;
    return on;
}
static void note(const char *what, const char *detail) {
    if (log_on()) fprintf(stderr, "[dora-fs] %s%s%s\n", what, detail ? ": " : "", detail ? detail : "");
}

/* ---------- the allocator the game installs (SetAllocator) ----------
 * nn::fs hands the host a pair of functions to allocate the RomFS cache it asked for. This host
 * serves RomFS from a pre-verified image and reports a cache size of 0, so the allocator is not
 * needed for the mount; keeping it means a heap-backed mount path can use the game's own
 * allocator later instead of the process heap. */
static void *(*game_alloc)(size_t);
static void (*game_free)(void *, size_t);

static void set_allocator(void *(*alloc_fn)(size_t), void (*free_fn)(void *, size_t)) {
    game_alloc = alloc_fn;
    game_free = free_fn;
    note("SetAllocator", alloc_fn ? "stored" : "cleared");
}

/* EnableGlobalFileDataCache: nn::fs keeps recently read file data in a buffer the game provides.
 * The host reads straight from the mmap'd RomFS image, so a second cache in front of it only
 * duplicates memory. Accepted and ignored rather than refused: nothing downstream can observe
 * the difference, and a refusal would look like a broken filesystem to the engine. */
static unsigned enable_global_cache(void *buffer, size_t size) {
    (void)buffer;
    note("EnableGlobalFileDataCache", "ignored, RomFS reads are served from the verified image");
    (void)size;
    return 0;
}

/* GetFileOpenMode: the mode the handle was opened with. Unity asks this before deciding whether a
 * package can be written back, so answering wrongly would silently divert save data. */
static int file_open_mode(uint64_t h) {
    int mode = runtime_fs_host_file_mode(h);
    if (mode < 0) {
        /* A RomFS handle: read-only by construction. */
        if (runtime_romfs_handle(h)) return 1;
        note("GetFileOpenMode", "not a host file");
        return 1;
    }
    return mode;
}

/* ---------- directory operations on writable storage ----------
 * CreateDirectory and DeleteDirectoryRecursively already exist; these are the plain and the
 * rename forms that Unity uses when it maintains its own subdirectories under save/. */
static unsigned delete_directory(const char *path) {
    char p[1100];
    unsigned r = runtime_fs_host_path(path, p, sizeof p, NULL);
    if (r) return r;
    if (rmdir(p) == 0) return 0;
    return errno == ENOENT ? R_PATH_NOT_FOUND : errno == ENOTEMPTY ? R_NOT_EMPTY : R_INVALID_ARGUMENT;
}
static unsigned rename_directory(const char *from, const char *to) {
    char a[1100], b[1100];
    unsigned r = runtime_fs_host_path(from, a, sizeof a, NULL);
    if (!r) r = runtime_fs_host_path(to, b, sizeof b, NULL);
    if (r) return r;
    struct stat st;
    if (!stat(b, &st)) return R_ALREADY_EXISTS;
    return rename(a, b) ? (errno == ENOENT ? R_PATH_NOT_FOUND : R_INVALID_ARGUMENT) : 0;
}
/* CleanDirectoryRecursively: empty a directory but keep it. The shared host has a recursive
 * delete that removes the directory as well, so this walks one level instead. */
static unsigned clean_directory_recursive(const char *path) {
    char p[1100];
    unsigned r = runtime_fs_host_path(path, p, sizeof p, NULL);
    if (r) return r;
    DIR *d = opendir(p);
    if (!d) return R_PATH_NOT_FOUND;
    struct dirent *e;
    int failed = 0;
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char child[2200];
        if (snprintf(child, sizeof child, "%s/%s", p, e->d_name) >= (int)sizeof child) { failed = 1; continue; }
        if (unlink(child) && errno != ENOENT) failed = 1;
    }
    closedir(d);
    return failed ? R_INVALID_ARGUMENT : 0;
}

/* GetFreeSpaceSize: Unity sizes its write budget from this. Reporting the volume's real free
 * space is right; reporting a fictional figure would let a save start that cannot finish. */
static unsigned free_space(int64_t *out, const char *path) {
    if (!out) return R_INVALID_ARGUMENT;
    char p[1100];
    unsigned r = runtime_fs_host_path(path, p, sizeof p, NULL);
    if (r) return r;
    struct statfs fs;
    if (statfs(p, &fs)) return R_PATH_NOT_FOUND;
    *out = (int64_t)fs.f_bavail * (int64_t)fs.f_bsize;
    return 0;
}

static unsigned dir_entry_count(int64_t *out, uint64_t h) {
    return runtime_fs_host_dir_count(h, out);
}

static int is_mounted(const char *name) { return runtime_fs_host_is_mounted(name) ? 1 : 0; }

/* Commit(const char * const *names, int count): the batch form. RomFS is read-only and has
   nothing to flush, so it is silently skipped the way the system fs process would. */
static unsigned commit_many(const char *const *names, int count) {
    if (count < 0 || (!names && count)) return R_INVALID_ARGUMENT;
    unsigned first = 0;
    for (int i = 0; i < count; i++) {
        unsigned r = runtime_fs_host_commit(names[i]);
        if (r == R_PATH_NOT_FOUND && runtime_fs_host_is_mounted(names[i])) continue;
        if (r && !first) first = r;
    }
    return first;
}

/* ReadFile(handle, offset, buffer, size, ReadOption): the option only carries an offset-adjust
 * flag, which the host already applies, so this is the exact read. */
static unsigned read_file_opt(uint64_t h, int64_t offset, void *buffer, uint64_t size, const void *option) {
    (void)option;
    return runtime_fs_host_read_exact(h, offset, buffer, size);
}

/* IsSaveDataExisting(uid): the single-account form. The shared host creates the profile's save
   storage on first start, so the local account always has one; any other account has none. */
static unsigned char is_save_existing(const uint64_t *uid) {
    if (!uid) return 0;
    uint64_t me[2];
    runtime_account_uid(me);
    return uid[0] == me[0] && uid[1] == me[1];
}

/* ---------- mounts this build has no content for ----------
 * RamDisk and add-on content are the two mount kinds with nothing behind them yet: the base game
 * has no DLC installed and Unity's RamDisk use would need a backing store the host does not have.
 * Both answer honestly instead of pretending, so the first attempt shows up in the log at the
 * exact call the engine makes rather than as a missing file much later.
 *
 * AOC is mounted only after the update and DLC stages (see games/dora/README); until then
 * QueryMountAddOnContentCacheSize reports 0 and MountAddOnContent refuses. */
static unsigned query_aoc_cache_size(uint64_t *out, int index) {
    if (!out) return R_INVALID_ARGUMENT;
    *out = 0;
    note("QueryMountAddOnContentCacheSize", "no add-on content in this build");
    (void)index;
    return 0;
}
static unsigned mount_add_on_content(const char *name, int index, void *cache, uint64_t size) {
    (void)name; (void)cache; (void)size; (void)index;
    note("MountAddOnContent", "refused, this build mounts the base RomFS only");
    return R_TARGET_NOT_FOUND;
}
static uint64_t ram_disk_buffer_size(uint64_t index) {
    note("GetRamDiskBufferSize", "no ram disk");
    (void)index;
    return 0;
}
static unsigned mount_ram_disk(const char *name, void *buffer, uint64_t size) {
    (void)buffer; (void)size;
    note("MountRamDisk", name ? name : "(null)");
    return R_NOT_SUPPORTED;
}

/* ---------- registration ----------
 * Reached through runtime_game_override (dora_adapter.c), so this file only has to answer for
 * the names it knows. */
void *runtime_dora_fs_override(const char *name) {
    static const struct { const char *name; void *fn; } map[] = {
        {"_ZN2nn2fs12SetAllocatorEPFPvmEPFvS1_mE", set_allocator},
        {"_ZN2nn2fs25EnableGlobalFileDataCacheEPvm", enable_global_cache},
        {"_ZN2nn2fs15GetFileOpenModeENS0_10FileHandleE", file_open_mode},
        {"_ZN2nn2fs15DeleteDirectoryEPKc", delete_directory},
        {"_ZN2nn2fs15RenameDirectoryEPKcS2_", rename_directory},
        {"_ZN2nn2fs25CleanDirectoryRecursivelyEPKc", clean_directory_recursive},
        {"_ZN2nn2fs16GetFreeSpaceSizeEPlPKc", free_space},
        {"_ZN2nn2fs22GetDirectoryEntryCountEPlNS0_15DirectoryHandleE", dir_entry_count},
        {"_ZN2nn2fs9IsMountedEPKc", is_mounted},
        {"_ZN2nn2fs6CommitEPKPKci", commit_many},
        {"_ZN2nn2fs8ReadFileENS0_10FileHandleElPvmRKNS0_10ReadOptionE", read_file_opt},
        {"_ZN2nn2fs18IsSaveDataExistingERKNS_7account3UidE", is_save_existing},
        {"_ZN2nn2fs31QueryMountAddOnContentCacheSizeEPmi", query_aoc_cache_size},
        {"_ZN2nn2fs17MountAddOnContentEPKciPvm", mount_add_on_content},
        {"_ZN2nn2fs20GetRamDiskBufferSizeEm", ram_disk_buffer_size},
        {"_ZN2nn2fs12MountRamDiskEPKcPvm", mount_ram_disk},
    };
    for (size_t i = 0; i < sizeof map / sizeof *map; i++) if (!strcmp(name, map[i].name)) return map[i].fn;
    return NULL;
}

/* Unused on purpose when the game never asks; kept so the allocator pair is not optimised away. */
void runtime_dora_fs_allocator(void **alloc_fn, void **free_fn) {
    if (alloc_fn) *alloc_fn = (void *)game_alloc;
    if (free_fn) *free_fn = (void *)game_free;
}
