// Fuzzes ejdb's document update API: the input is a JSON document, a newline, then a patch (an
// RFC 6902 JSON Patch array or an RFC 7386 merge-patch object). The document is stored in a fresh
// per-input indexed database, then ejdb_patch and ejdb_merge_or_put apply the untrusted patch
// text to it (and ejdb_merge_or_put to a missing id), and the result is read back with ejdb_get.
#include "../fuzz_db.h"
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

static void show(struct ejdb *db, int64_t id) {
  struct jbl *got = 0;
  if (ejdb_get(db, "c", id, &got) == 0) {
    char *s = 0;
    if (jbl_as_json_alloc(got, 0, &s) == 0) {
      free(s);
    }
    jbl_destroy(&got);
  }
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  char *text = malloc(size + 1);
  if (!text) {
    abort();
  }
  memcpy(text, data, size);
  text[size] = '\0';

  char *patch = strchr(text, '\n');
  if (patch) {
    *patch++ = '\0';
  } else {
    patch = text + size;
  }

  struct jbl *doc = 0;
  if (jbl_from_json(&doc, text) != 0) {
    free(text);
    return -1;
  }

  struct fuzz_db f;
  fuzz_db_open(&f);
  (void) ejdb_ensure_index(f.db, "c", "/a", EJDB_IDX_STR);
  (void) ejdb_ensure_index(f.db, "c", "/n", EJDB_IDX_I64);

  int64_t id = 0;
  if (ejdb_put_new(f.db, "c", doc, &id) == 0) {
    if (ejdb_patch(f.db, "c", patch, id) == 0) {
      show(f.db, id);
    }
    if (ejdb_merge_or_put(f.db, "c", patch, id) == 0) {
      show(f.db, id);
    }
    if (ejdb_merge_or_put(f.db, "c", patch, id + 100) == 0) {
      show(f.db, id + 100);
    }
  }
  jbl_destroy(&doc);
  fuzz_db_close(&f);
  free(text);
  return 0;
}
