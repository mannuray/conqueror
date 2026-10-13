#include "app/app.h"
#include "app/tasks.h"

namespace cq {
namespace app {

void applySettings() {
  const GlobalSettings& gs = g_app.settings.global();
  g_app.encoders_enabled = gs.encoders_enabled != 0;
  // APPLY-END
}

}  // namespace app
}  // namespace cq
