#include "net/logo_http.h"

#include <esp_http_server.h>
#include <stdio.h>
#include <string.h>

#include "logo/check_json.h"
#include "logo/program_names.h"
#include "net/http_server.h"
#include "net/link_task.h"
#include "net/logo_worker.h"
#include "net/program_store.h"

namespace cq {
namespace net {
namespace {

esp_err_t sendJson(httpd_req_t* req, const char* status, const char* json) {
  httpd_resp_set_status(req, status);
  httpd_resp_set_type(req, "application/json");
  return httpd_resp_sendstr(req, json);
}

esp_err_t busy(httpd_req_t* req) { return sendJson(req, "503 Service Unavailable", "{\"ok\":false,\"why\":\"BUSY\"}"); }

// The body into buf (cap includes the NUL); false when the client went away.
bool readBody(httpd_req_t* req, char* buf, size_t len) {
  size_t got = 0;
  while (got < len) {
    const int n = httpd_req_recv(req, buf + got, len - got);
    if (n == HTTPD_SOCK_ERR_TIMEOUT) continue;
    if (n <= 0) return false;
    got += static_cast<size_t>(n);
  }
  buf[len] = '\0';
  return true;
}

// "/logo/programs/<name>[?…]" → name (≤ 24 + NUL).
void nameOf(const char* uri, char* out, size_t cap) {
  const char* p = uri + strlen("/logo/programs/");
  size_t n = 0;
  while (p[n] != '\0' && p[n] != '?' && n + 1 < cap) {
    out[n] = p[n];
    ++n;
  }
  out[n] = '\0';
}

bool chunk(void* ctx, const char* data, size_t len) {
  return httpd_resp_send_chunk(static_cast<httpd_req_t*>(ctx), data, static_cast<ssize_t>(len)) == ESP_OK;
}

// ---- the jobs: on the worker task, the request is an async copy (the web server is free meanwhile) ----

void doCheck(httpd_req_t* req, const char* name) {
  const size_t len = req->content_len;
  if (len < logo::kSourceInbox && !readBody(req, logoInbox(), len)) return;  // the client went away
  bool runnable = false;
  const uint32_t prog = logoCheck(len < logo::kSourceInbox ? static_cast<uint16_t>(len) : logo::kSourceInbox, name,
                                  runnable);
  httpd_resp_set_type(req, "application/json");
  logo::JsonOut out(chunk, req);
  logo::writeCheckReply(logoSpace(), logoInbox(), runnable, prog, out);
  httpd_resp_send_chunk(req, nullptr, 0);
}

void doList(httpd_req_t* req, const char*) {
  static ProgramInfo list[logo::kMaxPrograms + 3];
  const uint8_t n = programList(list, sizeof list / sizeof list[0]);
  httpd_resp_set_type(req, "application/json");
  httpd_resp_send_chunk(req, "{\"programs\":[", HTTPD_RESP_USE_STRLEN);
  for (uint8_t i = 0; i < n; ++i) {
    char item[112];
    snprintf(item, sizeof item, "%s{\"name\":\"%.24s\",\"bytes\":%u,\"example\":%s}", i > 0 ? "," : "", list[i].name,
             list[i].bytes, list[i].example ? "true" : "false");
    httpd_resp_send_chunk(req, item, HTTPD_RESP_USE_STRLEN);
  }
  httpd_resp_send_chunk(req, "]}", HTTPD_RESP_USE_STRLEN);
  httpd_resp_send_chunk(req, nullptr, 0);
}

void doLoad(httpd_req_t* req, const char* name) {
  const int n = programLoad(name, logoInbox(), logo::kSourceInbox);
  if (n < 0) {
    sendJson(req, "404 Not Found", "{\"ok\":false,\"why\":\"NOT_FOUND\"}");
    return;
  }
  httpd_resp_set_type(req, "text/plain; charset=utf-8");
  httpd_resp_send(req, logoInbox(), n);
}

void storeReply(httpd_req_t* req, StoreStatus s) {
  if (s == StoreStatus::kOk) {
    sendJson(req, "200 OK", "{\"ok\":true}");
    return;
  }
  char json[48];
  snprintf(json, sizeof json, "{\"ok\":false,\"why\":\"%s\"}", storeStatusName(s));
  sendJson(req, s == StoreStatus::kNotFound ? "404 Not Found" : "409 Conflict", json);
}

bool running(httpd_req_t* req) {  // no saving / deleting while a program runs (flash writes)
  if (!linkStatus().logo_running) return false;
  sendJson(req, "409 Conflict", "{\"ok\":false,\"why\":\"RUNNING\"}");
  return true;
}

void doSave(httpd_req_t* req, const char* name) {
  if (running(req)) return;
  if (req->content_len >= logo::kSourceInbox) return storeReply(req, StoreStatus::kTooBig);
  if (!readBody(req, logoInbox(), req->content_len)) return;
  storeReply(req, programSave(name, logoInbox(), req->content_len));
}

void doDelete(httpd_req_t* req, const char* name) {
  if (running(req)) return;
  storeReply(req, programRemove(name));
}

// ---- the handlers: on the web server task, they only hand the request over ----

esp_err_t accept(httpd_req_t* req, LogoJob job, const char* name) {
  if (!logoJobTake()) return busy(req);
  httpd_req_t* copy = nullptr;
  if (httpd_req_async_handler_begin(req, &copy) != ESP_OK) {
    logoJobGiveUp();
    return busy(req);
  }
  if (!logoJobPost(job, copy, name)) {
    busy(copy);
    httpd_req_async_handler_complete(copy);
    logoJobGiveUp();
  }
  return ESP_OK;
}

esp_err_t onCheck(httpd_req_t* req) {
  char query[48] = "", name[25] = "";
  if (httpd_req_get_url_query_str(req, query, sizeof query) == ESP_OK) {
    httpd_query_key_value(query, "name", name, sizeof name);
  }
  return accept(req, doCheck, name);
}

esp_err_t onList(httpd_req_t* req) { return accept(req, doList, ""); }

esp_err_t onProgram(httpd_req_t* req, LogoJob job) {
  char name[25];
  nameOf(req->uri, name, sizeof name);
  return accept(req, job, name);
}

esp_err_t onLoad(httpd_req_t* req) { return onProgram(req, doLoad); }
esp_err_t onSave(httpd_req_t* req) { return onProgram(req, doSave); }
esp_err_t onDelete(httpd_req_t* req) { return onProgram(req, doDelete); }

void add(const char* uri, httpd_method_t method, esp_err_t (*handler)(httpd_req_t*)) {
  httpd_uri_t u{};
  u.uri = uri;
  u.method = method;
  u.handler = handler;
  httpd_register_uri_handler(httpServer(), &u);
}

}  // namespace

void logoHttpRegister() {
  add("/logo/check", HTTP_POST, onCheck);
  add("/logo/programs", HTTP_GET, onList);
  add("/logo/programs/*", HTTP_GET, onLoad);
  add("/logo/programs/*", HTTP_PUT, onSave);
  add("/logo/programs/*", HTTP_DELETE, onDelete);
}

}  // namespace net
}  // namespace cq
