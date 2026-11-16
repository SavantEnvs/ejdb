// Per-input scratch database for the harnesses: a fresh directory under $TMPDIR (fallback /tmp)
// holding a truncated, WAL-less ejdb file, removed again after the input.
#pragma once
#include <ejdb2.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

struct fuzz_db {
  struct ejdb *db;
  char dir[512];
  char path[600];
};

static void fuzz_db_open(struct fuzz_db *f) {
  const char *tmp = getenv("TMPDIR");
  snprintf(f->dir, sizeof(f->dir), "%s/ejdbfuzz-XXXXXX", tmp && *tmp ? tmp : "/tmp");
  if (!mkdtemp(f->dir)) {
    abort();
  }
  snprintf(f->path, sizeof(f->path), "%s/db", f->dir);
  struct ejdb_opts opts = {
    .kv = { .path = f->path, .oflags = IWKV_TRUNC },
    .no_wal = true
  };
  f->db = 0;
  if (ejdb_open(&opts, &f->db) != 0) {
    abort();
  }
}

static void fuzz_db_close(struct fuzz_db *f) {
  if (f->db) {
    (void) ejdb_close(&f->db);
  }
  unlink(f->path);
  rmdir(f->dir);
}
