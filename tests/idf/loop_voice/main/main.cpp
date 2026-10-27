#include "loop_voice_target_cases.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static_assert(sizeof(void*) == 4, "target smoke must use the ESP32-S3 ABI");
static void yield_case() { vTaskDelay(1); }
extern "C" void app_main() {
    std::puts("V1_TARGET_BEGIN esp32s3 macro=1 synthetic_only=1");
    loop_target_test::run(yield_case);
    std::puts("V1_TARGET_PASS");
    std::fflush(stdout);
}
