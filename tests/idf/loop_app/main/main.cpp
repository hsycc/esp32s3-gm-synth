#include "loop_app_target_cases.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "psa/crypto.h"

static_assert(sizeof(void*)==4,"ESP32-S3 ABI required");
static bool hash(const std::uint8_t* data,std::size_t n,loop_app_target::Bank::Digest& out) noexcept {
    std::size_t produced=0;
    return psa_hash_compute(PSA_ALG_SHA_256,data,n,out.data(),out.size(),&produced)==PSA_SUCCESS && produced==out.size();
}
static void yield(){vTaskDelay(1);}
struct Job {void(*fn)(void*)=nullptr;void* arg=nullptr;TaskHandle_t parent=nullptr;};
static Job job;
static std::atomic<bool> complete{false};
static void entry(void* p){
    auto* j=static_cast<Job*>(p);j->fn(j->arg);
    complete.store(true,std::memory_order_release);
    xTaskNotifyGive(j->parent); // completion, no further access to fixture/argument
    vTaskDelete(nullptr);
}
static void start(void(*fn)(void*),void* arg){
    complete.store(false,std::memory_order_relaxed);
    job={fn,arg,xTaskGetCurrentTaskHandle()};
    loop_app_target::require(xTaskCreatePinnedToCore(entry,"loop-test-worker",16384,&job,5,nullptr,1)==pdPASS,__LINE__);
}
static void join(){
    loop_app_target::require(ulTaskNotifyTake(pdTRUE,pdMS_TO_TICKS(15000))==1,__LINE__);
    loop_app_target::require(complete.load(std::memory_order_acquire),__LINE__);
}
extern "C" void app_main(){
    std::puts("APP_TARGET_BEGIN esp32s3 macro=1 embedded_sf2=1");
    loop_app_target::require(psa_crypto_init()==PSA_SUCCESS,__LINE__);
    loop_app_target::run(hash,{start,join,yield});
    std::puts("APP_TARGET_END");std::fflush(stdout);
}
