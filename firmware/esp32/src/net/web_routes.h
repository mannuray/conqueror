// Static web assets from build/web_assets.h on port 80: / (Basic Drive page),
// /manifest.webmanifest, /icon-192.png and, from Task 9 on, /help.
#pragma once

namespace cq {
namespace net {

void webRoutesRegister();  // after httpServerStart()

}  // namespace net
}  // namespace cq
