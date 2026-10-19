// The Tank Logo worker (Plan N3, Tank Logo LLD §20.1-§20.2): its own task on core 0 below the web server. The web
// server only accepts a /logo/* request and hands it over (esp_http_server's async requests), so the WebSocket -
// STOP included - is never held up by a check, a 40 KB reply or a flash write. One job at a time. The worker also
// owns the running program and the Vm that the link task's LogoController uses.
#pragma once

#include <esp_http_server.h>
#include <stddef.h>
#include <stdint.h>

#include "app/logo_controller.h"
#include "logo/check_job.h"

namespace cq {
namespace net {

using LogoJob = void (*)(httpd_req_t* req, const char* name);  // runs on the worker task

bool logoWorkerBegin();  // at boot, before linkTaskStart: memory, the task

// Web server task.
bool logoJobTake();  // false: a job is running (answer 503 busy)
// After httpd_req_async_handler_begin: the worker runs job(req, name), then completes the request and lets the
// next job in. False: not posted (the caller answers, completes and calls logoJobGiveUp).
bool logoJobPost(LogoJob job, httpd_req_t* async_req, const char* name);
void logoJobGiveUp();

// Worker task (inside a job).
char* logoInbox();  // ≤ 8192 bytes of program text
uint32_t logoCheck(uint16_t len, const char* name, bool& runnable);  // check + CheckBox; its number
const logo::CheckSpace& logoSpace();

// Link task: the checked program for logo_run, and the running program's storage.
app::LogoController::Took logoTake(uint32_t prog, logo::Program& out, char* name, size_t cap);
bool logoRunReady();  // false only when not even 36 KB were free at boot
logo::Program& logoRunningProgram();
logo::Vm& logoVm();

}  // namespace net
}  // namespace cq
