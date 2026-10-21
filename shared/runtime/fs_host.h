/* Unified nn::fs host layer: RomFS (romfs.c) plus writable save, temporary and cache storage. */
#pragma once
#include <stddef.h>
#include <stdint.h>

void *runtime_fs_host_override(const char *name);

/* romfs.c */
int runtime_romfs_owns(const char *path);
int runtime_romfs_handle(uint64_t h);
unsigned runtime_romfs_open(uint64_t *out, const char *path, int mode);
unsigned runtime_romfs_read(uint64_t *out, uint64_t h, int64_t offset, void *buffer, uint64_t size);
unsigned runtime_romfs_size(int64_t *out, uint64_t h);
void runtime_romfs_close(uint64_t h);
unsigned runtime_romfs_entry_type(int *out, const char *path);
/* The immediate children of a RomFS folder (mode: 1 folders, 2 files); add is called once per child. */
unsigned runtime_romfs_list(const char *path, int mode, void (*add)(void *ctx, const char *name, int is_dir, int64_t size), void *ctx);

/* account.c: the local user identity */
void runtime_account_uid(uint64_t out[2]);

/* ---------- shared accessors for a per-engine adapter layer ----------
   A game whose engine uses more of nn::fs than this host implements gets its own adapter
   (games/<id>/runtime/...). Those functions need the mount table, the handle table and the
   path rule without copying them, so the few accessors below are exported. They are read-only
   views onto state that already exists; no behaviour of this file depends on them. */

/* "name:/a/b" -> host path. 0 on success; *kind gets the mount kind (0 free, 1 save, 2 temp,
   3 cache). A RomFS path is not in the mount table and answers 0x202 here. */
unsigned runtime_fs_host_path(const char *path, char *out, size_t cap, int *kind);
/* A storage mount, or the RomFS mount, carries this name. */
int runtime_fs_host_is_mounted(const char *name);
/* Commit of one storage mount, the same work Commit(name) does. 0x202 if unknown. */
unsigned runtime_fs_host_commit(const char *name);
/* Open mode of a file handle, -1 when the handle is not a host file. */
int runtime_fs_host_file_mode(uint64_t h);
/* Read exactly size bytes; short reads are an error. RomFS handles work here too. */
unsigned runtime_fs_host_read_exact(uint64_t h, int64_t offset, void *buffer, uint64_t size);
/* Bytes left to read in an open directory, without touching its position. */
unsigned runtime_fs_host_dir_count(uint64_t h, int64_t *out);
