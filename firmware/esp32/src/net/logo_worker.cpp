#include "net/logo_worker.h"

#include <Arduino.h>
#include <esp_heap_caps.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>

#include <new>

#include "cq_protocol.h"
#include "logo/check_box.h"
#include "net/link_task.h"
#include "net/log.h"

namespace cq {
namespace net {
namespace {

struct Shared {  // PSRAM only, allocated once at boot (≈ 330 KB): without it every job answers busy
  logo::CheckSpace space;
  logo::Program checked;
  char inbox[logo::kSourceInbox];
};
struct Run {  // the link task's: PSRAM, else internal RAM (≈ 36 KB) - the LogoController always exists
  logo::Program running;
  logo::Vm vm;
};
struct Job {
  LogoJob run;
  httpd_req_t* req;
  char name[25];
};

Shared* g = nullptr;
Run* g_run = nullptr;
logo::CheckBox* g_box = nullptr;
SemaphoreHandle_t g_busy = nullptr;    // one job at a time
SemaphoreHandle_t g_box_mx = nullptr;  // the CheckBox: worker (put) and link task (take, never waits)
QueueHandle_t g_jobs = nullptr;

template <class T>
T* make(uint32_t caps) {
  void* mem = heap_caps_malloc(sizeof(T), caps);
  return mem != nullptr ? new (mem) T() : nullptr;
}

logo::Caps caps() {
  logo::Caps c;
  const LinkStatus s = linkStatus();
  c.encoders = ((s.hello.caps >> static_cast<unsigned>(proto::CapBit::kEncoders)) & 1u) != 0;
  return c;
}

void workerTask(void*) {
  Job job;
  for (;;) {
    xQueueReceive(g_jobs, &job, portMAX_DELAY);
    job.run(job.req, job.name);
    httpd_req_async_handler_complete(job.req);
    xSemaphoreGive(g_busy);
  }
}

}  // namespace

bool logoWorkerBegin() {
  g_run = make<Run>(MALLOC_CAP_SPIRAM);
  if (g_run == nullptr) g_run = make<Run>(MALLOC_CAP_8BIT);
  g = make<Shared>(MALLOC_CAP_SPIRAM);
  if (g == nullptr) {
    logf("logo: no PSRAM for checks (%u bytes) - /logo answers busy", static_cast<unsigned>(sizeof(Shared)));
    return false;
  }
  static logo::CheckBox box(g->checked);
  g_box = &box;
  g_busy = xSemaphoreCreateBinary();
  xSemaphoreGive(g_busy);
  g_box_mx = xSemaphoreCreateMutex();
  g_jobs = xQueueCreate(1, sizeof(Job));
  xTaskCreatePinnedToCore(workerTask, "logo", 16384, nullptr, 2, nullptr, 0);
  logf("logo: worker ready (%u KB PSRAM)", static_cast<unsigned>(sizeof(Shared) / 1024));
  return true;
}

bool logoJobTake() { return g != nullptr && xSemaphoreTake(g_busy, 0) == pdTRUE; }
void logoJobGiveUp() { xSemaphoreGive(g_busy); }

bool logoJobPost(LogoJob job, httpd_req_t* async_req, const char* name) {
  Job j{job, async_req, ""};
  snprintf(j.name, sizeof j.name, "%s", name);
  return xQueueSend(g_jobs, &j, 0) == pdTRUE;
}

char* logoInbox() { return g->inbox; }
const logo::CheckSpace& logoSpace() { return g->space; }

uint32_t logoCheck(uint16_t len, const char* name, bool& runnable) {
  const uint32_t t0 = millis();
  runnable = logo::checkProgram(g->inbox, len, caps(), g->space);
  xSemaphoreTake(g_box_mx, portMAX_DELAY);
  const uint32_t prog = g_box->put(runnable ? &g->space.program : nullptr, name);
  xSemaphoreGive(g_box_mx);
  logf("logo: check %u: %u bytes in %lu ms%s", static_cast<unsigned>(prog), static_cast<unsigned>(len),
       static_cast<unsigned long>(millis() - t0), runnable ? "" : " (not runnable)");
  return prog;
}

app::LogoController::Took logoTake(uint32_t prog, logo::Program& out, char* name, size_t cap) {
  if (g_box == nullptr || xSemaphoreTake(g_box_mx, 0) != pdTRUE) return app::LogoController::Took::kBusy;
  const logo::CheckBox::Take t = g_box->take(prog, out, name, cap);
  xSemaphoreGive(g_box_mx);
  return t == logo::CheckBox::Take::kOk ? app::LogoController::Took::kOk : app::LogoController::Took::kCheckAgain;
}

bool logoRunReady() { return g_run != nullptr; }
logo::Program& logoRunningProgram() { return g_run->running; }
logo::Vm& logoVm() { return g_run->vm; }

}  // namespace net
}  // namespace cq
