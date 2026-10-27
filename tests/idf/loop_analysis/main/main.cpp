#include "loop_analysis_cases.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "psa/crypto.h"
static_assert(sizeof(void*)==4,"ESP32-S3 ABI required");
static bool hash(const std::uint8_t* p,std::size_t n,loop_analysis_test::Bank::Digest& out) noexcept {
    std::size_t got=0;
    return psa_hash_compute(PSA_ALG_SHA_256,p,n,out.data(),out.size(),&got)==PSA_SUCCESS && got==out.size();
}
static void yield(){vTaskDelay(1);}
extern "C" void app_main(){
    std::puts("ANALYSIS_BEGIN esp32s3 macro=1 resident_analysis=1");
    loop_analysis_test::require(psa_crypto_init()==PSA_SUCCESS,__LINE__);
    loop_analysis_test::run(hash,yield);
    std::puts("ANALYSIS_END");std::fflush(stdout);
}
