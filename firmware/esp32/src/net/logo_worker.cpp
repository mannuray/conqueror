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
  logo::ThingNames things;  // the check's own copy of the AI helper's things (~11 KB; taken when a check starts)
};
struct Run {  // the link task's: PSRAM, else internal RAM (≈ 46 KB with LAUNCH's Vm) - the LogoController always exists
  logo::Program running;
  logo::Vm vm;
  logo::Vm launched;  // LAUNCH's part (Tank Logo LLD §26.4)
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
SemaphoreHandle_t g_things_mx = nullptr;  // the SkillHub's things list: link task (replaces it) and worker (copies it)
const core::SkillHub* g_hub = nullptr;
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
  // Bridge LLD R26.1: the AI helper's things, copied whole under the lock the link task also takes to replace them,
  // so a skill_things during the check changes the hub's list but never this copy.
  c.ai = g_hub != nullptr && core::copyForCheck(*g_hub, g->things, logoThingsLock());
  if (c.ai) c.things = &g->things;
  c.ai_learning = g_hub != nullptr && g_hub->learning();  // Plan Z2b-1: a teach is running (7056)
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
  g_things_mx = xSemaphoreCreateMutex();
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

void logoBindSkillHub(const core::SkillHub* hub) { g_hub = hub; }

const core::LockPort& logoThingsLock() {
  static const core::LockPort port{
      [](void*, uint32_t wait_ms) {
        if (g_things_mx == nullptr) return false;
        const TickType_t t = wait_ms == core::LockPort::kForever ? portMAX_DELAY : pdMS_TO_TICKS(wait_ms);
        return xSemaphoreTake(g_things_mx, t) == pdTRUE;
      },
      [](void*) {
        if (g_things_mx != nullptr) xSemaphoreGive(g_things_mx);
      },
      nullptr};
  return port;
}

bool logoRunReady() { return g_run != nullptr; }
logo::Program& logoRunningProgram() { return g_run->running; }
logo::Vm& logoVm() { return g_run->vm; }
logo::Vm& logoLaunchedVm() { return g_run->launched; }

}  // namespace net
}  // namespace cq
