// Fuzzes ejdb's HTTP/WebSocket server (src/jbr/jbr.c: _on_http_request, _on_query and the WebSocket
// command handler, on iwnet's HTTP and WebSocket parsers) over loopback. LLVMFuzzerInitialize opens
// one database in a scratch directory under $TMPDIR with the server enabled on 127.0.0.1 and a free
// ephemeral port.
//
// Each input becomes exactly one complete exchange on a fresh TCP connection, so the server always
// finishes and closes it (iwnet serves one request per read event and stalls on pipelined or
// truncated requests until its own idle timeout, and drops a session whose read sees EOF early):
//   byte 0, bit 0 clear: one HTTP request. Byte 1 picks the method; the rest is the request target
//     on the first line, then header lines up to an empty line, then the body. Only allow-listed
//     header names are sent; the harness adds Host, Content-Length (the body's real length) and
//     `Connection: close`.
//   byte 0, bit 0 set: a WebSocket session. The harness sends the upgrade request, then each
//     following line as one masked text frame, then a close frame.
// The response is read until the server closes the connection.
// Kept across inputs on purpose: the server and the open database. After every input all
// collections are removed, so each input starts from an empty database.
#include <ejdb2.h>
#include <iowow/iwjson.h>
#include <iowow/iwxstr.h>
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <unistd.h>

static struct ejdb *db;
static struct ejdb_opts opts;
static char dir[512], path[600];
static struct sockaddr_in addr;

static int free_port(void) {
  int s = socket(AF_INET, SOCK_STREAM, 0);
  struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = 0 };
  socklen_t alen = sizeof(a);
  a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (  s < 0 || bind(s, (struct sockaddr*) &a, sizeof(a)) != 0
     || getsockname(s, (struct sockaddr*) &a, &alen) != 0) {
    abort();
  }
  close(s);
  return ntohs(a.sin_port);
}

static int send_all(int s, const void *data, size_t size) {
  const uint8_t *p = data;
  size_t off = 0;
  while (off < size) {
    ssize_t n = send(s, p + off, size - off, MSG_NOSIGNAL);
    if (n < 0 && errno == EINTR) {
      continue;
    }
    if (n <= 0) {
      return -1;  // the server closed the connection early
    }
    off += (size_t) n;
  }
  return 0;
}

static const char *const methods[] = { "GET", "HEAD", "POST", "PUT", "PATCH", "DELETE", "OPTIONS" };

// Request headers a client may send that jbr or iwnet interpret; framing headers stay the harness's.
static const char *const header_names[] = {
  "content-type", "x-hints", "x-access-token", "origin", "access-control-request-method",
  "access-control-request-headers", "accept", "cookie", "user-agent",
};

static int header_allowed(const uint8_t *line, size_t len) {
  for (size_t i = 0; i < sizeof(header_names) / sizeof(header_names[0]); ++i) {
    size_t n = strlen(header_names[i]);
    if (len > n && line[n] == ':' && strncasecmp((const char*) line, header_names[i], n) == 0) {
      return 1;
    }
  }
  return 0;
}

static const uint8_t* next_line(const uint8_t *p, const uint8_t *end, size_t *len) {
  const uint8_t *nl = memchr(p, '\n', (size_t) (end - p));
  *len = (size_t) ((nl ? nl : end) - p);
  return nl ? nl + 1 : end;
}

static int has_crlf(const uint8_t *p, size_t len) {
  return memchr(p, '\r', len) != 0;
}

static void ws_frame(int s, uint8_t opcode, const uint8_t *payload, size_t len) {
  static const uint8_t mask[4] = { 0x37, 0xfa, 0x21, 0x3d };
  uint8_t hdr[14];
  size_t h = 0;
  hdr[h++] = 0x80 | opcode;
  if (len < 126) {
    hdr[h++] = 0x80 | (uint8_t) len;
  } else if (len <= 0xffff) {
    hdr[h++] = 0x80 | 126;
    hdr[h++] = (uint8_t) (len >> 8);
    hdr[h++] = (uint8_t) len;
  } else {
    hdr[h++] = 0x80 | 127;
    for (int i = 7; i >= 0; --i) {
      hdr[h++] = (uint8_t) ((uint64_t) len >> (8 * i));
    }
  }
  memcpy(hdr + h, mask, 4);
  h += 4;
  uint8_t *buf = malloc(len ? len : 1);
  if (!buf) {
    abort();
  }
  for (size_t i = 0; i < len; ++i) {
    buf[i] = payload[i] ^ mask[i % 4];
  }
  if (send_all(s, hdr, h) == 0) {
    (void) send_all(s, buf, len);
  }
  free(buf);
}

// Reads the upgrade response up to the end of its header block; -1 if the server closed first.
static int read_http_head(int s) {
  unsigned last = 0;
  char c;
  for (;;) {
    ssize_t n = recv(s, &c, 1, 0);
    if (n < 0 && errno == EINTR) {
      continue;
    }
    if (n <= 0) {
      return -1;
    }
    last = (last << 8) | (uint8_t) c;
    if (last == 0x0d0a0d0a) {
      return 0;
    }
  }
}

// Builds one complete HTTP/1.1 request from the input (layout in the header comment), or returns 0
// when the first line cannot be a request-target (RFC 9112 3.2: no SP, CR or LF; not empty).
static struct iwxstr* http_request(const uint8_t *p, const uint8_t *end) {
  const char *method = methods[(p < end ? *p++ : 0) % (sizeof(methods) / sizeof(methods[0]))];
  size_t len;
  const uint8_t *target = p;
  p = next_line(p, end, &len);
  if (len == 0 || has_crlf(target, len) || memchr(target, ' ', len)) {
    return 0;
  }
  struct iwxstr *req = iwxstr_new();
  if (!req) {
    abort();
  }
  iwxstr_cat2(req, method);
  iwxstr_cat2(req, " ");
  iwxstr_cat(req, target, len);
  iwxstr_cat2(req, " HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n");
  while (p < end) {
    const uint8_t *line = p;
    p = next_line(p, end, &len);
    if (len == 0) {
      break;
    }
    if (header_allowed(line, len) && !has_crlf(line, len)) {
      iwxstr_cat(req, line, len);
      iwxstr_cat2(req, "\r\n");
    }
  }
  char cl[64];
  snprintf(cl, sizeof(cl), "Content-Length: %zu\r\n\r\n", (size_t) (end - p));
  iwxstr_cat2(req, cl);
  iwxstr_cat(req, p, (size_t) (end - p));
  return req;
}
int LLVMFuzzerInitialize(int *argc, char ***argv) {
  (void) argc;
  (void) argv;
  if (ejdb_init() != 0) {
    abort();
  }
  const char *tmp = getenv("TMPDIR");
  snprintf(dir, sizeof(dir), "%s/ejdbjbr-XXXXXX", tmp && *tmp ? tmp : "/tmp");
  if (!mkdtemp(dir)) {
    abort();
  }
  snprintf(path, sizeof(path), "%s/db", dir);
  int port = free_port();
  opts = (struct ejdb_opts) {
    .kv = { .path = path, .oflags = IWKV_TRUNC },
    .no_wal = true,
    .http = {
      .enabled = true,
      .bind = "127.0.0.1",
      .port = port,
      .blocking = false,
    },
  };
  if (ejdb_open(&opts, &db) != 0) {
    abort();
  }
  addr = (struct sockaddr_in) { .sin_family = AF_INET, .sin_port = htons(port) };
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  return 0;
}

// Removes every collection the input created, so the next input sees an empty database.
static void reset_db(void) {
  for (int guard = 0; guard < 1024; ++guard) {
    struct jbl *meta = 0, *name = 0;
    if (ejdb_get_meta(db, &meta) != 0) {
      abort();
    }
    int done = 1;
    if (jbl_at(meta, "/collections/0/name", &name) == 0) {
      const char *cname = jbl_get_str(name);
      if (cname && ejdb_remove_collection(db, cname) == 0) {
        done = 0;
      }
      jbl_destroy(&name);
    }
    jbl_destroy(&meta);
    if (done) {
      return;
    }
  }
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  if (size < 1) {
    return -1;
  }
  const uint8_t *p = data + 1, *end = data + size;
  struct iwxstr *req = 0;
  if (!(data[0] & 1)) {
    req = http_request(p, end);
    if (!req) {
      return -1;
    }
  }

  int s = socket(AF_INET, SOCK_STREAM, 0);
  if (s < 0) {
    abort();
  }
  int rc;
  do {
    rc = connect(s, (struct sockaddr*) &addr, sizeof(addr));
  } while (rc != 0 && errno == EINTR);
  if (rc != 0) {
    abort();  // the server is up for the whole process lifetime
  }

  if (req) {
    (void) send_all(s, iwxstr_ptr(req), iwxstr_size(req));
    iwxstr_destroy(req);
  } else {
    static const char upgrade[]
      = "GET / HTTP/1.1\r\nHost: localhost\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
        "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n\r\n";
    // Frames sent before the 101 response would sit in the HTTP request buffer unread.
    if (send_all(s, upgrade, sizeof(upgrade) - 1) == 0 && read_http_head(s) == 0) {
      while (p < end) {
        size_t len;
        const uint8_t *msg = p;
        p = next_line(p, end, &len);
        ws_frame(s, 0x1, msg, len);
      }
      ws_frame(s, 0x8, (const uint8_t*) "\x03\xe8", 2);
    }
  }

  char buf[4096];
  for (;;) {
    ssize_t n = recv(s, buf, sizeof(buf), 0);
    if (n < 0 && errno == EINTR) {
      continue;
    }
    if (n <= 0) {
      break;
    }
  }
  close(s);
  reset_db();
  return 0;
}
