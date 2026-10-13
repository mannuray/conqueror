#include "net/web_routes.h"

#include <string.h>

#include "net/http_server.h"
#include "net/log.h"
#include "web_assets.h"

namespace cq {
namespace net {
namespace {

esp_err_t serveAsset(httpd_req_t* req) {
  const web::Asset* a = static_cast<const web::Asset*>(req->user_ctx);
  httpd_resp_set_type(req, a->type);
  if (a->gzip) httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
  httpd_resp_set_hdr(req, "Cache-Control", "no-cache");  // an OTA update shows up at once
  return httpd_resp_send(req, reinterpret_cast<const char*>(a->data),
                         static_cast<ssize_t>(a->len));
}

}  // namespace

void webRoutesRegister() {
  for (size_t i = 0; i < web::kAssetCount; ++i) {
    httpd_uri_t u{};
    u.uri = web::kAssets[i].uri;
    u.method = HTTP_GET;
    u.handler = serveAsset;
    u.user_ctx = const_cast<web::Asset*>(&web::kAssets[i]);
    httpd_register_uri_handler(httpServer(), &u);
  }
  logf("http: %u static assets (page %u B gzipped)", static_cast<unsigned>(web::kAssetCount),
       static_cast<unsigned>(web::kAssets[0].len));
}

}  // namespace net
}  // namespace cq
