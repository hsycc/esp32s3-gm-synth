#include "loop_app_target_cases.h"
#include <openssl/sha.h>
#include <thread>
static std::thread thread;
static bool hash(const std::uint8_t* data,std::size_t n,loop_app_target::Bank::Digest& out) noexcept {
    return SHA256(data,n,out.data())!=nullptr;
}
static void start(void(*fn)(void*),void* arg){thread=std::thread(fn,arg);}
static void join(){thread.join();}
static void yield(){std::this_thread::yield();}
int main(){loop_app_target::run(hash,{start,join,yield});}
