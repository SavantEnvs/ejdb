// Fuzzes the EJDB engine end to end: a fresh database file in $TMPDIR is filled with JSON
// documents and a JQL query runs against it. The input is the query on the first line, then one
// JSON document per line.
#include <ejdb2.h>
#include <iowow/iwpool.h>
#include <iowow/iwxstr.h>
#include "../jql_bind.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int LLVMFuzzerInitialize(int *argc, char ***argv) {
  (void) argc;
  (void) argv;
  if (ejdb_init() != 0) {
    abort();
  }
  return 0;
}

static iwrc visitor(struct ejdb_exec *ctx, struct ejdb_doc *doc, int64_t *step) {
  (void) ctx;
  (void) step;
  char *json = 0;
  if (doc->node) {
    if (jbn_as_json_alloc(doc->node, 0, &json) == 0) {
      free(json);
    }
  } else if (jbl_as_json_alloc(doc->raw, 0, &json) == 0) {
    free(json);
  }
  return 0;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  char *text = malloc(size + 1);
  if (!text) {
    abort();
  }
  memcpy(text, data, size);
  text[size] = '\0';

  int ret = -1;
  struct jql *q = 0;
  struct ejdb *db = 0;
  struct iwpool *pool = 0;
  char dir[512], path[600];
  const char *tmp = getenv("TMPDIR");
  snprintf(dir, sizeof(dir), "%s/ejdbfuzz-XXXXXX", tmp && *tmp ? tmp : "/tmp");
  if (!mkdtemp(dir)) {
    abort();
  }
  snprintf(path, sizeof(path), "%s/db", dir);

  char *docs = strchr(text, '\n');
  if (docs) {
    *docs++ = '\0';
  } else {
    docs = text + size;
  }

  if (jql_create2(&q, "c", text, JQL_SILENT_ON_PARSE_ERROR) != 0) {
    goto cleanup;
  }
  bind_placeholders(q, text, strlen(text));
  ret = 0;

  struct ejdb_opts opts = {
    .kv = { .path = path, .oflags = IWKV_TRUNC },
    .no_wal = true
  };
  if (ejdb_open(&opts, &db) != 0) {
    abort();
  }
  (void) ejdb_ensure_index(db, "c", "/foo", EJDB_IDX_STR);
  (void) ejdb_ensure_index(db, "c", "/n", EJDB_IDX_I64);

  while (*docs) {
    char *nl = strchr(docs, '\n');
    if (nl) {
      *nl = '\0';
    }
    struct jbl *jbl = 0;
    if (jbl_from_json(&jbl, docs) == 0) {
      int64_t id;
      (void) ejdb_put_new(db, "c", jbl, &id);
      jbl_destroy(&jbl);
    }
    if (!nl) {
      break;
    }
    docs = nl + 1;
  }

  pool = iwpool_create(1024);
  struct ejdb_exec ux = { .db = db, .q = q, .visitor = visitor, .pool = pool };
  (void) ejdb_exec(&ux);

  struct iwxstr *log = iwxstr_new();
  if (log) {
    jql_reset(q, true, false);
    struct ejdb_exec ux2 = { .db = db, .q = q, .log = log };
    (void) ejdb_exec(&ux2);
    iwxstr_destroy(log);
  }

cleanup:
  if (pool) {
    iwpool_destroy(pool);
  }
  jql_destroy(&q);
  if (db) {
    (void) ejdb_close(&db);
  }
  unlink(path);
  rmdir(dir);
  free(text);
  return ret;
}
