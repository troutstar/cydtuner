#include "touch.h"
#include "xpt2046.h"

esp_err_t touch_init(void) { return xpt2046_init(); }
bool touch_read(int *x, int *y) { int p; return xpt2046_read(x, y, &p); }
