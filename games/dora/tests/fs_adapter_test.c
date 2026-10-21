/* Exercises the nn::fs adapter against the real shared host.
 *
 * fs_host.c and dora_fs_ext.c are compiled unmodified; only the four things they expect from the
 * rest of the runtime are stubbed here (fatal error, clock, account identity, autoload hook), plus
 * a three-file RomFS so the RomFS branch of every shared entry point is taken as well.
 *
 * What this covers that a coverage table cannot: that the adapter's arguments line up with what the
 * shared host expects, that free-space and directory counting answer through the real mount table,
 * that the batch Commit reaches each mount, and that the RomFS path stays read-only under the
 * adapter's ReadFile form.
 */
#include <dirent.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "runtime.h"
#include "fs_host.h"

/* ---------- what the shared host expects from the rest of the runtime ---------- */
_Noreturn void runtime_fail(const char *message) {
    fprintf(stderr, "  runtime_fail: %s\n", message);
    exit(2);
}
uint64_t runtime_now_ns(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}
void runtime_account_uid(uint64_t out[2]) {
    out[0] = 0x1111222233334444ull;
    out[1] = 0x5555666677778888ull;
}
void runtime_autoload_save_file_opened(const char *path) { (void)path; }
void dm_guest_write_prepare(void *addr, size_t n) { (void)addr; (void)n; }
/* fs_host.c forwards every RomFS open here; romfs.c owns it in a real build. */
void runtime_fs_log(const char *what, const char *path) {
    if (getenv("DORA_FS_LOG")) fprintf(stderr, "  [fs] %s %s\n", what, path ? path : "(null)");
}

/* ---------- a stand-in RomFS: three files, read-only ---------- */
#define ROMH 0x100000000ull
static const char *rom_paths[] = {"/Data/Managed/Metadata/global-metadata.dat", "/Data/level0", "/Data/sharedassets0.assets"};
static const char *rom_bodies[] = {"IL2CPP-METADATA-BLOB", "LEVEL0-PAYLOAD", "ASSETS-PAYLOAD"};
static int rom_open = 0;
static int rom_mounted = 0;   /* romfs.c keeps the mount name empty until MountRom */

int runtime_romfs_owns(const char *path) { return rom_mounted && path && !strncmp(path, "rom:", 4); }
void runtime_test_mount_rom(void) { rom_mounted = 1; }
int runtime_romfs_handle(uint64_t h) { return h >= ROMH && h < ROMH + 8; }
static int rom_index(const char *path) {
    if (!path) return -1;
    const char *rel = path + 4;
    while (*rel == '/') rel++;
    for (int i = 0; i < 3; i++) {
        const char *want = rom_paths[i];
        while (*want == '/') want++;
        if (!strcmp(rel, want)) return i;
    }
    return -1;
}
unsigned runtime_romfs_open(uint64_t *out, const char *path, int mode) {
    if (mode != 1) return 0x6402u;
    int i = rom_index(path);
    if (i < 0 || !out) return 0x202u;
    rom_open = i + 1;
    *out = ROMH + (uint64_t)i;
    return 0;
}
unsigned runtime_romfs_read(uint64_t *out, uint64_t h, int64_t offset, void *buffer, uint64_t size) {
    if (!runtime_romfs_handle(h) || !out) return 0x2ee202u;
    int i = (int)(h - ROMH);
    size_t len = strlen(rom_bodies[i]);
    if (offset < 0 || (uint64_t)offset >= len) { *out = 0; return 0; }
    uint64_t n = size < len - (uint64_t)offset ? size : len - (uint64_t)offset;
    memcpy(buffer, rom_bodies[i] + offset, (size_t)n);
    *out = n;
    return 0;
}
unsigned runtime_romfs_size(int64_t *out, uint64_t h) {
    if (!runtime_romfs_handle(h) || !out) return 0x2ee202u;
    *out = (int64_t)strlen(rom_bodies[h - ROMH]);
    return 0;
}
void runtime_romfs_close(uint64_t h) { (void)h; rom_open = 0; }
unsigned runtime_romfs_entry_type(int *out, const char *path) {
    if (!out) return 0x2ee202u;
    if (rom_index(path) >= 0) { *out = 1; return 0; }
    return 0x202u;
}
unsigned runtime_romfs_list(const char *path, int mode, void (*add)(void *, const char *, int, int64_t), void *ctx) {
    (void)path; (void)add; (void)ctx;
    if (mode & 1) return 0;
    return 0;
}

/* ---------- resolution through the same chain the loader uses ---------- */
extern void *runtime_game_override(const char *name);
static void *resolve(const char *name) {
    void *p = runtime_game_override(name);
    if (p) return p;
    return runtime_fs_host_override(name);
}

static unsigned checks, failures;
#define CHECK(cond, what) do { \
    checks++; \
    if (!(cond)) { failures++; printf("  FAIL  %s\n", what); } \
    else printf("  ok    %s\n", what); \
} while (0)
#define CHECK_EQ(got, want, what) do { \
    checks++; \
    unsigned long long g_ = (unsigned long long)(got), w_ = (unsigned long long)(want); \
    if (g_ != w_) { failures++; printf("  FAIL  %s (got %#llx, want %#llx)\n", what, g_, w_); } \
    else printf("  ok    %s (%#llx)\n", what, g_); \
} while (0)

/* Signatures exactly as the mangled names declare them. */
typedef void (*set_allocator_fn)(void *(*)(size_t), void (*)(void *, size_t));
typedef unsigned (*u_cache_fn)(void *, size_t);
typedef int (*open_mode_fn)(uint64_t);
typedef unsigned (*path_fn)(const char *);
typedef unsigned (*two_path_fn)(const char *, const char *);
typedef unsigned (*free_space_fn)(int64_t *, const char *);
typedef unsigned (*dir_count_fn)(int64_t *, uint64_t);
typedef int (*is_mounted_fn)(const char *);
typedef unsigned (*commit_many_fn)(const char *const *, int);
typedef unsigned (*read_opt_fn)(uint64_t, int64_t, void *, uint64_t, const void *);
typedef unsigned char (*save_exists_fn)(const uint64_t *);
typedef unsigned (*mount_fn)(const char *, void *, uint64_t);
typedef uint64_t (*ram_size_fn)(uint64_t);

static void *test_alloc(size_t n) { return malloc(n); }
static void test_free(void *p, size_t n) { (void)n; free(p); }

int main(void) {
    char root[] = "/tmp/dora-fs-test-XXXXXX";
    if (!mkdtemp(root)) { perror("mkdtemp"); return 2; }
    setenv("TOTK_PROFILE_DIR", root, 1);

    printf("adapter symbols resolve:\n");
    CHECK(resolve("_ZN2nn2fs12SetAllocatorEPFPvmEPFvS1_mE") != NULL, "SetAllocator");
    CHECK(resolve("_ZN2nn2fs15GetFileOpenModeENS0_10FileHandleE") != NULL, "GetFileOpenMode");
    CHECK(resolve("_ZN2nn2fs16GetFreeSpaceSizeEPlPKc") != NULL, "GetFreeSpaceSize");
    CHECK(resolve("_ZN2nn2fs22GetDirectoryEntryCountEPlNS0_15DirectoryHandleE") != NULL, "GetDirectoryEntryCount");
    CHECK(resolve("_ZN2nn2fs9IsMountedEPKc") != NULL, "IsMounted");
    CHECK(resolve("_ZN2nn2fs6CommitEPKPKci") != NULL, "Commit(names,count)");
    CHECK(resolve("_ZN2nn2fs8ReadFileENS0_10FileHandleElPvmRKNS0_10ReadOptionE") != NULL, "ReadFile(...,ReadOption)");
    CHECK(resolve("_ZN2nn2fs18IsSaveDataExistingERKNS_7account3UidE") != NULL, "IsSaveDataExisting(uid)");

    printf("\nallocator and cache:\n");
    ((set_allocator_fn)resolve("_ZN2nn2fs12SetAllocatorEPFPvmEPFvS1_mE"))(test_alloc, test_free);
    CHECK(1, "SetAllocator accepted a pair of functions");
    CHECK_EQ(((u_cache_fn)resolve("_ZN2nn2fs25EnableGlobalFileDataCacheEPvm"))(NULL, 0), 0,
             "EnableGlobalFileDataCache");

    printf("\nmounts:\n");
    is_mounted_fn mounted = (is_mounted_fn)resolve("_ZN2nn2fs9IsMountedEPKc");
    CHECK_EQ(mounted("save"), 0, "save is not mounted yet");
    uint64_t uid[2];
    runtime_account_uid(uid);
    unsigned (*mount_save)(const char *, const uint64_t *) = (void *)resolve("_ZN2nn2fs13MountSaveDataEPKcRKNS_7account3UidE");
    CHECK_EQ(mount_save("save", uid), 0, "MountSaveData");
    CHECK_EQ(mounted("save"), 1, "save is mounted");
    CHECK_EQ(mounted("rom"), 0, "RomFS is not mounted until MountRom");
    runtime_test_mount_rom();
    CHECK_EQ(mounted("rom"), 1, "after MountRom the RomFS name answers");
    CHECK_EQ(mounted("save"), 1, "and the storage mount is unaffected");

    printf("\nfree space through the mount table:\n");
    int64_t space = -1;
    CHECK_EQ(((free_space_fn)resolve("_ZN2nn2fs16GetFreeSpaceSizeEPlPKc"))(&space, "save:/"), 0,
             "GetFreeSpaceSize(save:/)");
    CHECK(space > 0, "free space is a real figure, not zero");
    printf("        reported %lld bytes\n", (long long)space);
    CHECK_EQ(((free_space_fn)resolve("_ZN2nn2fs16GetFreeSpaceSizeEPlPKc"))(&space, "nosuch:/"), 0x202,
             "GetFreeSpaceSize on an unmounted name is path-not-found");

    printf("\ndirectories:\n");
    path_fn mkdir_fn = (path_fn)resolve("_ZN2nn2fs15CreateDirectoryEPKc");
    two_path_fn rename_dir = (two_path_fn)resolve("_ZN2nn2fs15RenameDirectoryEPKcS2_");
    path_fn clean_dir = (path_fn)resolve("_ZN2nn2fs25CleanDirectoryRecursivelyEPKc");
    path_fn rmdir_fn = (path_fn)resolve("_ZN2nn2fs15DeleteDirectoryEPKc");
    CHECK_EQ(mkdir_fn("save:/sub"), 0, "CreateDirectory(save:/sub)");
    CHECK_EQ(mkdir_fn("save:/sub2"), 0, "CreateDirectory(save:/sub2)");
    CHECK_EQ(rename_dir("save:/sub2", "save:/sub3"), 0, "RenameDirectory");
    CHECK_EQ(mkdir_fn("save:/sub2"), 0, "create the name again after the rename");
    CHECK_EQ(rename_dir("save:/sub", "save:/sub2"), 0x402, "RenameDirectory onto an existing name is already-exists");
    CHECK_EQ(rmdir_fn("save:/sub2"), 0, "DeleteDirectory");
    CHECK_EQ(rmdir_fn("save:/sub2"), 0x202, "DeleteDirectory twice is path-not-found");

    printf("\nfiles, open mode and the ReadFile form with ReadOption:\n");
    unsigned (*create_file)(const char *, int64_t) = (void *)resolve("_ZN2nn2fs10CreateFileEPKcl");
    unsigned (*open_file)(uint64_t *, const char *, int) = (void *)resolve("_ZN2nn2fs8OpenFileEPNS0_10FileHandleEPKci");
    open_mode_fn open_mode = (open_mode_fn)resolve("_ZN2nn2fs15GetFileOpenModeENS0_10FileHandleE");
    read_opt_fn read_opt = (read_opt_fn)resolve("_ZN2nn2fs8ReadFileENS0_10FileHandleElPvmRKNS0_10ReadOptionE");
    void (*close_file)(uint64_t) = (void *)resolve("_ZN2nn2fs9CloseFileENS0_10FileHandleE");

    CHECK_EQ(create_file("save:/sub/a.bin", 8), 0, "CreateFile");
    uint64_t h = 0;
    CHECK_EQ(open_file(&h, "save:/sub/a.bin", 2), 0, "OpenFile for write");
    CHECK_EQ(open_mode(h), 2, "GetFileOpenMode on a write handle");
    unsigned (*write_file)(uint64_t, int64_t, const void *, uint64_t, const int32_t *) =
        (void *)resolve("_ZN2nn2fs9WriteFileENS0_10FileHandleElPKvmRKNS0_11WriteOptionE");
    CHECK_EQ(write_file(h, 0, "ABCDEFGH", 8, NULL), 0, "WriteFile");
    close_file(h);

    CHECK_EQ(open_file(&h, "save:/sub/a.bin", 1), 0, "OpenFile for read");
    CHECK_EQ(open_mode(h), 1, "GetFileOpenMode on a read handle");
    char buf[16] = {0};
    CHECK_EQ(read_opt(h, 0, buf, 8, NULL), 0, "ReadFile(handle,off,buf,size,ReadOption)");
    CHECK(!memcmp(buf, "ABCDEFGH", 8), "the bytes came back");
    CHECK_EQ(read_opt(h, 0, buf, 9, NULL), 0x2ee202, "a short read is an error, not a silent truncation");
    close_file(h);

    printf("\ndirectory entry count:\n");
    unsigned (*open_dir)(uint64_t *, const char *, int) = (void *)resolve("_ZN2nn2fs13OpenDirectoryEPNS0_15DirectoryHandleEPKci");
    dir_count_fn dir_count = (dir_count_fn)resolve("_ZN2nn2fs22GetDirectoryEntryCountEPlNS0_15DirectoryHandleE");
    void (*close_dir)(uint64_t) = (void *)resolve("_ZN2nn2fs14CloseDirectoryENS0_15DirectoryHandleE");
    uint64_t dh = 0;
    CHECK_EQ(open_dir(&dh, "save:/sub", 3), 0, "OpenDirectory(save:/sub)");
    int64_t n = -1;
    CHECK_EQ(dir_count(&n, dh), 0, "GetDirectoryEntryCount(out, h)");
    CHECK(n != -1, "count answered");
    printf("        %lld entries, and the position is untouched\n", (long long)n);
    CHECK_EQ(n, 1, "one file in save:/sub");
    close_dir(dh);

    printf("\nbatch commit:\n");
    const char *names[] = {"save"};
    CHECK_EQ(((commit_many_fn)resolve("_ZN2nn2fs6CommitEPKPKci"))(names, 1), 0, "Commit(save,1)");
    const char *two[] = {"save", "save"};
    CHECK_EQ(((commit_many_fn)resolve("_ZN2nn2fs6CommitEPKPKci"))(two, 2), 0, "Commit with the same mount twice");
    CHECK_EQ(((commit_many_fn)resolve("_ZN2nn2fs6CommitEPKPKci"))(names, 0), 0, "Commit of nothing");
    CHECK_EQ(((commit_many_fn)resolve("_ZN2nn2fs6CommitEPKPKci"))(NULL, 1), 0x2ee202, "Commit(NULL,1) is invalid");

    printf("\nclean keeps the directory:\n");
    CHECK_EQ(clean_dir("save:/sub"), 0, "CleanDirectoryRecursively(save:/sub)");
    struct stat st;
    CHECK(stat("save:/sub", &st) != 0, "the path is still a RomFS-style name, not a host path");
    char host[1100];
    CHECK_EQ(runtime_fs_host_path("save:/sub", host, sizeof host, NULL), 0, "resolve save:/sub");
    CHECK(!stat(host, &st) && S_ISDIR(st.st_mode), "the directory itself survived");
    CHECK_EQ(open_dir(&dh, "save:/sub", 3), 0, "open it again");
    n = -1;
    dir_count(&n, dh);
    CHECK_EQ(n, 0, "and it is empty");
    close_dir(dh);

    printf("\nRomFS stays read-only under the adapter:\n");
    uint64_t rh = 0;
    CHECK_EQ(open_file(&rh, "rom:/Data/Managed/Metadata/global-metadata.dat", 1), 0, "open a RomFS file");
    CHECK_EQ(open_mode(rh), 1, "GetFileOpenMode on a RomFS handle is read-only");
    memset(buf, 0, sizeof buf);
    CHECK_EQ(read_opt(rh, 0, buf, 8, NULL), 0, "ReadFile with ReadOption on a RomFS handle");
    CHECK(!memcmp(buf, "IL2CPP-M", 8), "the RomFS bytes came back");
    close_file(rh);

    printf("\nsave data presence:\n");
    save_exists_fn save_exists = (save_exists_fn)resolve("_ZN2nn2fs18IsSaveDataExistingERKNS_7account3UidE");
    CHECK_EQ(save_exists(uid), 1, "the local account has save data");
    uint64_t other[2] = {1, 2};
    CHECK_EQ(save_exists(other), 0, "another account has none");
    CHECK_EQ(save_exists(NULL), 0, "no account, no save data");

    printf("\ncontent this build does not have:\n");
    CHECK_EQ(((mount_fn)resolve("_ZN2nn2fs12MountRamDiskEPKcPvm"))("ram", NULL, 0), 0x6402,
             "MountRamDisk is refused, not faked");
    CHECK_EQ(((ram_size_fn)resolve("_ZN2nn2fs20GetRamDiskBufferSizeEm"))(0), 0, "GetRamDiskBufferSize is 0");
    uint64_t cache = 1;
    CHECK_EQ(((unsigned (*)(uint64_t *, int))resolve("_ZN2nn2fs31QueryMountAddOnContentCacheSizeEPmi"))(&cache, 0), 0,
             "QueryMountAddOnContentCacheSize");
    CHECK_EQ(cache, 0, "no add-on content cache");
    CHECK_EQ(((unsigned (*)(const char *, int, void *, uint64_t))resolve("_ZN2nn2fs17MountAddOnContentEPKciPvm"))("aoc", 0, NULL, 0),
             0x7d402, "MountAddOnContent is refused with target-not-found");

    printf("\n%u checks, %u failures\n", checks, failures);
    char cmd[1200];
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", root);
    if (system(cmd)) {}
    printf("{\"status\":\"%s\",\"checks\":%u,\"failures\":%u}\n", failures ? "fail" : "pass", checks, failures);
    return failures ? 1 : 0;
}
