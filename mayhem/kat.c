// Known-answer probe for mayhem/test.sh: runs fixed documents, queries and patches through the
// public ejdb/iowow API and prints the results, which test.sh compares against expected text.
#include <ejdb2.h>
#include <iowow/iwjson.h>
#include <iowow/iwpool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static iwrc visitor(struct ejdb_exec *ctx, struct ejdb_doc *doc, int64_t *step) {
  (void) ctx;
  (void) step;
  char *json = 0;
  iwrc rc = doc->node ? jbn_as_json_alloc(doc->node, 0, &json) : jbl_as_json_alloc(doc->raw, 0, &json);
  if (rc == 0) {
    printf("  %lld %s\n", (long long) doc->id, json);
    free(json);
  }
  return rc;
}

static void query(struct ejdb *db, const char *q) {
  struct jql *jql = 0;
  printf("query %s\n", q);
  iwrc rc = jql_create(&jql, 0, q);
  if (rc) {
    printf("  parse error\n");
    return;
  }
  struct iwpool *pool = iwpool_create(1024);
  struct ejdb_exec ux = { .db = db, .q = jql, .visitor = visitor, .pool = pool };
  rc = ejdb_exec(&ux);
  printf("  rc=%d cnt=%lld\n", rc ? 1 : 0, (long long) ux.cnt);
  iwpool_destroy(pool);
  jql_destroy(&jql);
}

static void put(struct ejdb *db, const char *json) {
  struct jbl *jbl = 0;
  int64_t id = 0;
  if (jbl_from_json(&jbl, json) != 0 || ejdb_put_new(db, "c", jbl, &id) != 0) {
    printf("put failed: %s\n", json);
    exit(1);
  }
  jbl_destroy(&jbl);
  printf("put %lld\n", (long long) id);
}

static void show(struct jbl *jbl) {
  char *json = 0;
  if (jbl_as_json_alloc(jbl, 0, &json) == 0) {
    printf("  %s\n", json);
    free(json);
  } else {
    printf("  print failed\n");
  }
}

int main(int argc, char **argv) {
  if (argc < 2 || ejdb_init() != 0) {
    return 2;
  }
  struct ejdb_opts opts = { .kv = { .path = argv[1], .oflags = IWKV_TRUNC }, .no_wal = true };
  struct ejdb *db = 0;
  if (ejdb_open(&opts, &db) != 0) {
    return 2;
  }

  put(db, "{\"foo\":\"bar\",\"n\":3}");
  put(db, "{\"foo\":\"baz\",\"n\":1}");
  put(db, "{\"foo\":\"bar\",\"n\":2}");
  put(db, "{\"foo\":\"qux\",\"n\":5,\"tags\":[\"a\",\"b\"]}");

  query(db, "@c/[foo = \"bar\"] | asc /n");
  query(db, "@c/[n > 1] | /{foo,n} | desc /n limit 2");
  query(db, "@c/tags/[** = \"b\"]");
  query(db, "@c/* | count");
  query(db, "@c/[foo = \"baz\"] | apply {\"n\":10}");
  query(db, "@c/[foo = \"baz\"]");
  query(db, "@c/[foo = \"qux\"] | del");
  query(db, "@c/* | count");
  query(db, "@c/[foo ==== ");

  ejdb_close(&db);

  struct jbl *jbl = 0;
  printf("merge-patch\n");
  jbl_from_json(&jbl, "{\"a\":1,\"b\":{\"c\":2}}");
  if (jbl_merge_patch(jbl, "{\"b\":{\"c\":null},\"d\":3}") == 0) {
    show(jbl);
  }
  jbl_destroy(&jbl);

  printf("json-patch\n");
  jbl_from_json(&jbl, "{\"a\":[1,2,3],\"b\":\"x\"}");
  if (jbl_patch_from_json(jbl, "[{\"op\":\"add\",\"path\":\"/a/1\",\"value\":9},{\"op\":\"remove\",\"path\":\"/b\"}]") == 0) {
    show(jbl);
  }
  jbl_destroy(&jbl);

  printf("binary-roundtrip\n");
  jbl_from_json(&jbl, "{\"k\":[true,null,1.5,\"s\"],\"o\":{\"z\":-7}}");
  size_t sz = jbl_serialized_size(jbl);
  uint8_t *buf = malloc(sz);
  struct jbl *copy = 0;
  if (jbl_serialize_into(jbl, buf, sz) == 0 && jbl_create_from_buf(&copy, buf, sz, false) == 0) {
    show(copy);
    jbl_destroy(&copy);
  }
  free(buf);
  jbl_destroy(&jbl);

  printf("invalid-json\n");
  printf("  %s\n", jbl_from_json(&jbl, "{\"a\":") == 0 ? "accepted" : "rejected");
  return 0;
}
