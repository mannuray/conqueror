#include "app/app.h"
#include "app/tasks.h"
#include "app/motion_api.h"
#include "app/camera_api.h"

namespace cq {
namespace app {

void applySettings() {
  const GlobalSettings& gs = g_app.settings.global();
  g_app.encoders_enabled = gs.encoders_enabled != 0;
  const int16_t gyro_bias[3] = {gs.gyro_bias_x, gs.gyro_bias_y, gs.gyro_bias_z};
  g_app.gyro_zero.seed(gyro_bias);  // restarts only if the stored zero changed (LLD §11.8)
  configureMotion();
  configureCamera();
  // APPLY-END
}

}  // namespace app
}  // namespace cq
