#include "sntp.h"
#include "arch.h"
#include "event.h"
#include "log.h"
#include "util.h"

#define SNTP_TIME_OFFSET 2208988800U  // (1970 - 1900) in seconds
#define SNTP_MAX_FRAC 4294967295.0    // 2 ** 32 - 1

static uint64_t s_boot_timestamp = 0;  // Updated by SNTP

uint64_t mg_now(void) {
  return mg_millis() + s_boot_timestamp;
}

static int64_t gettimestamp(const uint32_t *data) {
  uint32_t sec = mg_ntohl(data[0]), frac = mg_ntohl(data[1]);
  if (sec) sec -= SNTP_TIME_OFFSET;
  return ((int64_t) sec) * 1000 + (int64_t) (frac / SNTP_MAX_FRAC * 1000.0);
}

int64_t mg_sntp_parse(const unsigned char *buf, size_t len) {
  int64_t epoch_milliseconds = -1;
  int mode = len > 0 ? buf[0] & 7 : 0;
  int version = len > 0 ? (buf[0] >> 3) & 7 : 0;
  if (len < 48) {
    MG_ERROR(("%s", "corrupt packet"));
  } else if (mode != 4 && mode != 5) {
    MG_ERROR(("%s", "not a server reply"));
  } else if (buf[1] == 0) {
    MG_ERROR(("%s", "server sent a kiss of death"));
  } else if (version == 4 || version == 3) {
    // int64_t ref = gettimestamp((uint32_t *) &buf[16]);
    int64_t origin_time = gettimestamp((uint32_t *) &buf[24]);
    int64_t receive_time = gettimestamp((uint32_t *) &buf[32]);
    int64_t transmit_time = gettimestamp((uint32_t *) &buf[40]);
    int64_t now = (int64_t) mg_millis();
    int64_t latency = (now - origin_time) - (transmit_time - receive_time);
    epoch_milliseconds = transmit_time + latency / 2;
    s_boot_timestamp = (uint64_t) (epoch_milliseconds - now);
  } else {
    MG_ERROR(("unexpected version: %d", version));
  }
  return epoch_milliseconds;
}

static void sntp_cb(struct mg_connection *c, int ev, void *ev_data) {
  uint64_t *expiration_time = (uint64_t *) c->data;
  if (ev == MG_EV_OPEN) {
    *expiration_time = mg_millis() + 3000;  // Store expiration time in 3s
  } else if (ev == MG_EV_CONNECT) {
    mg_sntp_request(c);
  } else if (ev == MG_EV_READ) {
    int64_t milliseconds = mg_sntp_parse(c->recv.buf, c->recv.len);
    if (milliseconds > 0) {
      s_boot_timestamp = (uint64_t) milliseconds - mg_millis();
      mg_call(c, MG_EV_SNTP_TIME, (uint64_t *) &milliseconds);
      MG_DEBUG(("%lu got time: %lld ms from epoch", c->id, milliseconds));
    }
    // mg_iobuf_del(&c->recv, 0, c->recv.len);  // Free receive buffer
    c->is_closing = 1;
  } else if (ev == MG_EV_POLL) {
    if (mg_millis() > *expiration_time) c->is_closing = 1;
  } else if (ev == MG_EV_CLOSE) {
  }
  (void) ev_data;
}

void mg_sntp_request(struct mg_connection *c) {
  if (c->is_resolving) {
    MG_ERROR(("%lu wait until resolved", c->id));
  } else {
    int64_t now = (int64_t) mg_millis();  // Use int64_t, for vc98
    uint8_t buf[48] = {0};
    uint32_t *t = (uint32_t *) &buf[40];
    double frac = ((double) (now % 1000)) / 1000.0 * SNTP_MAX_FRAC;
    buf[0] = (0 << 6) | (4 << 3) | 3;
    t[0] = mg_htonl((uint32_t) (now / 1000) + SNTP_TIME_OFFSET);
    t[1] = mg_htonl((uint32_t) frac);
    mg_send(c, buf, sizeof(buf));
  }
}

struct mg_connection *mg_sntp_connect(struct mg_mgr *mgr, const char *url,
                                      mg_event_handler_t fn, void *fn_data) {
  if (url == NULL) url = "udp://time.google.com:123";
  return mg_connect_svc(mgr, url, fn, fn_data, sntp_cb, NULL);
}
