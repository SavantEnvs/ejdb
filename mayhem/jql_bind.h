// Binds every placeholder a JQL query's text names, the way a caller binds its own query: the
// library dereferences an unbound placeholder, so executing without binding is harness misuse.
#ifndef MAYHEM_JQL_BIND_H
#define MAYHEM_JQL_BIND_H
#include <ejdb2.h>
#include <ctype.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

static const char *const STRS[] = { "foo", "n", "s", "bar", "hello" };

// Binds one placeholder, rotating the value type by position; a projection placeholder only
// accepts a string, so a rejected type falls back to one.
static void bind_one(struct jql *q, const char *name, int index, unsigned k) {
  iwrc rc;
  switch (k % 6) {
    case 0:
      rc = jql_set_i64(q, name, index, (int64_t) k - 2);
      break;
    case 1:
      rc = jql_set_f64(q, name, index, (double) k + 0.5);
      break;
    case 2:
      rc = jql_set_bool(q, name, index, k & 1);
      break;
    case 3:
      rc = jql_set_null(q, name, index);
      break;
    case 4: {
      struct jbl *v = 0;
      rc = jbl_from_json(&v, "{\"n\":5,\"arr\":[1,2]}");
      if (!rc) {
        rc = jql_set_json_jbl(q, name, index, v);
        jbl_destroy(&v);
      }
      break;
    }
    default:
      rc = IW_ERROR_FAIL;
      break;
  }
  if (rc) {
    (void) jql_set_str(q, name, index, STRS[k % (sizeof(STRS) / sizeof(*STRS))]);
  }
}

// Every `:name` and `:?` the query text can contain, bound the way a caller binds its own query.
static void bind_placeholders(struct jql *q, const char *text, size_t size) {
  unsigned k = 0;
  int index = 0;
  for (size_t i = 0; i < size && k < 64; ++i) {
    if (text[i] != ':') {
      continue;
    }
    size_t j = i + 1;
    if (j < size && text[j] == '?') {
      bind_one(q, 0, index++, k++);
      continue;
    }
    while (j < size && j - i <= 32 && isalnum((unsigned char) text[j])) {
      ++j;
    }
    if (j > i + 1) {
      char name[40];
      memcpy(name, text + i + 1, j - i - 1);
      name[j - i - 1] = '\0';
      bind_one(q, name, 0, k++);
    }
  }
}

#endif
