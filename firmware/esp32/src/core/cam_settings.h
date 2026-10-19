// Camera settings and stats (bridge LLD R13, R15.3): value types shared by net/camera_service and app/.
#pragma once

#include <stdint.h>

namespace cq {
namespace core {

// Plan O (bridge LLD R13): settings changed at run time, saved in NVS cq_cam. size 0 QVGA, 1 VGA, 2 SVGA, 3 HD;
// fps_cap 0 = no limit. The camera starts at HD so its frame buffers fit every size.
struct CamSettings {
  uint8_t size = 1;
  uint8_t quality = 12;
  uint8_t fps_cap = 0;
  int8_t brightness = 0;
  int8_t contrast = 0;
  bool low_light = false;
  bool vflip = true;
  bool hmirror = false;
};
struct CamStats {
  uint16_t fps_x10 = 0;
  uint16_t kbps = 0;
};

}  // namespace core
}  // namespace cq
