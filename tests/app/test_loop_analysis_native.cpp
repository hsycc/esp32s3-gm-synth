#include "loop_analysis_cases.h"
#include <openssl/sha.h>
static bool hash(const std::uint8_t* p,std::size_t n,loop_analysis_test::Bank::Digest& out) noexcept {
    return SHA256(p,n,out.data())!=nullptr;
}
int main(){loop_analysis_test::run(hash);}
