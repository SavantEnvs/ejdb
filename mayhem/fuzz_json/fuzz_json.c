// Fuzzes ejdb's document store path with an untrusted JSON document: the text is parsed, stored
// with ejdb_put_new into a fresh per-input database whose collection has string, integer and
// float indexes (so ejdb's index maintenance walks the document's fields), read back with
// ejdb_get, replaced with ejdb_put, found through an indexed JQL query and deleted.
#include "../fuzz_db.h"
#include <iowow/iwpool.h>
#include <stdint.h>
#include <string.h>

int LLVMFuzzerInitialize(int *argc, char ***argv) {
  (void) argc;
  (void) argv;
  if (ejdb_init() != 0) {
    abort();
  }
  return 0;
}

static void print_free(struct jbl *jbl) {
  char *s = 0;
  if (jbl_as_json_alloc(jbl, JBL_PRINT_PRETTY, &s) == 0) {
    free(s);
  }
}

static iwrc visitor(struct ejdb_exec *ctx, struct ejdb_doc *doc, int64_t *step) {
  (void) ctx;
  (void) step;
  print_free(doc->raw);
  return 0;
}

static void query(struct ejdb *db, const char *q) {
  struct jql *jql = 0;
  if (jql_create(&jql, "c", q) != 0) {
    abort();  // fixed, valid queries
  }
  struct iwpool *pool = iwpool_create(512);
  struct ejdb_exec ux = { .db = db, .q = jql, .visitor = visitor, .pool = pool };
  (void) ejdb_exec(&ux);
  if (pool) {
    iwpool_destroy(pool);
  }
  jql_destroy(&jql);
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  char *text = malloc(size + 1);
  if (!text) {
    abort();
  }
  memcpy(text, data, size);
  text[size] = '\0';

  struct jbl *jbl = 0;
  if (jbl_from_json(&jbl, text) != 0) {
    free(text);
    return -1;
  }

  struct fuzz_db f;
  fuzz_db_open(&f);
  (void) ejdb_ensure_index(f.db, "c", "/a", EJDB_IDX_STR);
  (void) ejdb_ensure_index(f.db, "c", "/n", EJDB_IDX_I64 | EJDB_IDX_UNIQUE);
  (void) ejdb_ensure_index(f.db, "c", "/f", EJDB_IDX_F64);
  (void) ejdb_ensure_index(f.db, "c", "/a/b", EJDB_IDX_STR);

  int64_t id = 0;
  if (ejdb_put_new(f.db, "c", jbl, &id) == 0) {
    struct jbl *got = 0;
    if (ejdb_get(f.db, "c", id, &got) == 0) {
      print_free(got);
      // Store the read-back form again, under a second id: exercises the unique-index clash too.
      int64_t id2 = 0;
      (void) ejdb_put_new(f.db, "c", got, &id2);
      (void) ejdb_put(f.db, "c", got, id);
      jbl_destroy(&got);
    }
    query(f.db, "/[a = \"x\"] or /[n > 0] or /[f < 1.5]");
    query(f.db, "/* | /{a,n,f}");
    (void) ejdb_del(f.db, "c", id);
  }
  jbl_destroy(&jbl);
  fuzz_db_close(&f);
  free(text);
  return 0;
}
