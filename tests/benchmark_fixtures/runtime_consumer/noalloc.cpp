#include "runtime_provider.hpp"
#include <atomic>
#include <cstdlib>
#include <new>
#include <iostream>
#if defined(_WIN32)
#include <malloc.h>
#endif
namespace {
std::atomic<bool> tracking{false};
std::atomic<std::size_t> allocations{0};
void count() noexcept { if(tracking.load(std::memory_order_relaxed)) ++allocations; }
void* ordinary(std::size_t n) { count();if(auto* p=std::malloc(n?n:1)) return p;throw std::bad_alloc(); }
void* aligned(std::size_t n,std::size_t a) {
    count();
#if defined(_WIN32)
    if(auto* p=_aligned_malloc(n?n:1,a)) return p;
#else
    void* p=nullptr;if(posix_memalign(&p,a,n?n:1)==0) return p;
#endif
    throw std::bad_alloc();
}
void free_aligned(void* p) noexcept {
#if defined(_WIN32)
    _aligned_free(p);
#else
    std::free(p);
#endif
}
}
void* operator new(std::size_t n){return ordinary(n);}
void* operator new[](std::size_t n){return ordinary(n);}
void* operator new(std::size_t n,std::align_val_t a){return aligned(n,static_cast<std::size_t>(a));}
void* operator new[](std::size_t n,std::align_val_t a){return aligned(n,static_cast<std::size_t>(a));}
void operator delete(void* p) noexcept{std::free(p);}
void operator delete[](void* p) noexcept{std::free(p);}
void operator delete(void* p,std::size_t) noexcept{std::free(p);}
void operator delete[](void* p,std::size_t) noexcept{std::free(p);}
void operator delete(void* p,std::align_val_t) noexcept{free_aligned(p);}
void operator delete[](void* p,std::align_val_t) noexcept{free_aligned(p);}
void operator delete(void* p,std::size_t,std::align_val_t) noexcept{free_aligned(p);}
void operator delete[](void* p,std::size_t,std::align_val_t) noexcept{free_aligned(p);}
int main() {
    namespace b=rtfw::benchmark;
    for(const auto& c:b::runtime::cases()) {
        if(!c.allocation_free) continue;
        b::runtime::Provider p;if(p.prepare(c.id)!=b::Status::ok) return 1;
        auto table=p.table();b::Observation observation;observation.counters.reserve(b::max_counters);
        allocations=0;
        for(std::uint64_t ordinal=0;ordinal<7;++ordinal) {
            // Includes the full public Runtime step on all prestarted workers.
            // Host vector capacity is caller-provided before the guarded region.
            tracking=true;
            const auto status=table.invoke(table.user,c.id,ordinal,observation);
            tracking=false;
            if(status!=b::Status::ok || !observation.correct || allocations!=0) {
                std::cerr<<c.id<<" allocation or correctness failure\n";return 1;
            }
        }
        if(p.finish()!=b::Status::ok) return 1;
    }
    std::cout<<"Runtime steady-state allocation checks passed\n";
}
