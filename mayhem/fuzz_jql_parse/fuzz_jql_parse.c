// Fuzzes the JQL query parser, binds every placeholder the query text names, then runs the
// matcher and `apply` against a fixed JSON document. Projection needs a live `jbexec`, so it is
// fuzzed through `ejdb_exec` in fuzz_ejdb_query.
#include <ejdb2.h>
#include <iowow/iwpool.h>
#include "../jql_bind.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static const char DOC[] =
  "{\"foo\":{\"bar\":1,\"baz\":[\"a\",\"b\"]},\"n\":3,\"s\":\"hello world\",\"arr\":[1,2,3],\"t\":true}";

int LLVMFuzzerInitialize(int *argc, char ***argv) {
  (void) argc;
  (void) argv;
  if (ejdb_init() != 0) {
    abort();
  }
  return 0;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  char *query = malloc(size + 1);
  if (!query) {
    abort();
  }
  memcpy(query, data, size);
  query[size] = '\0';

  struct jql *q = 0;
  iwrc rc = jql_create2(&q, "c", query, JQL_SILENT_ON_PARSE_ERROR);
  if (rc) {
    free(query);
    return -1;
  }
  bind_placeholders(q, query, size);
  free(query);

  int64_t v;
  (void) jql_collection(q);
  (void) jql_first_anchor(q);
  (void) jql_has_apply(q);
  (void) jql_has_apply_upsert(q);
  (void) jql_has_apply_delete(q);
  (void) jql_has_projection(q);
  (void) jql_has_orderby(q);
  (void) jql_has_aggregate_count(q);
  (void) jql_get_skip(q, &v);
  (void) jql_get_limit(q, &v);
  (void) jql_estimate_allocated_size(q);

  struct jbl *jbl = 0;
  if (jbl_from_json(&jbl, DOC) == 0) {
    bool matched = false;
    (void) jql_matched(q, jbl, &matched);
    jql_reset(q, true, false);

    struct iwpool *pool = iwpool_create(1024);
    if (pool) {
      struct jbl_node *root = 0;
      if (jbn_from_json(DOC, &root, pool) == 0) {
        (void) jql_apply(q, root, pool);
      }
      iwpool_destroy(pool);
    }
    jbl_destroy(&jbl);
  }
  jql_destroy(&q);
  return 0;
}
