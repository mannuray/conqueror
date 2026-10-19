// Tank Logo over HTTP (Plan N3, Tank Logo LLD §20.4): POST /logo/check, GET / PUT / DELETE /logo/programs[/<name>].
// The web server task only hands each request to the logo worker (async), so /ws and STOP are never held up.
// Program text never goes through the link task.
#pragma once

namespace cq {
namespace net {

void logoHttpRegister();  // after httpServerStart(), logoWorkerBegin() and programStoreBegin()

}  // namespace net
}  // namespace cq
