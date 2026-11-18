// Pure part of settings: the NVS key format.
#include <string.h>

#include "settings.h"

namespace mp {

bool settings_key(char* out, const char* tool, const char* param) {
  size_t a = strlen(tool), b = strlen(param);
  if (a + 1 + b > 15) return false;
  memcpy(out, tool, a);
  out[a] = '.';
  memcpy(out + a + 1, param, b);
  out[a + 1 + b] = 0;
  return true;
}

}  // namespace mp
