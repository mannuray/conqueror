#include "app/app.h"
#include "app/tasks.h"
#include "app/motion_api.h"

namespace cq {
namespace app {

void applySettings() {
  const GlobalSettings& gs = g_app.settings.global();
  g_app.encoders_enabled = gs.encoders_enabled != 0;
  configureMotion();
  // APPLY-END
}

}  // namespace app
}  // namespace cq
