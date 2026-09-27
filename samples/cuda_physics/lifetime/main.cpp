#include "conformance.hpp"
#include <string_view>
int main(int argc,char** argv) {
    if(argc==2 && std::string_view(argv[1])=="--help") {
        std::cout<<"Usage: cuda_lifetime [--help]\nFinite portable Runtime/CUDA failure and lifetime conformance; no hardware probe.\n";
        return 0;
    }
    if(argc!=1) { std::cerr<<"Usage: cuda_lifetime [--help]\n"; return 2; }
    return rtfw::cuda_physics::lifetime::suite()?0:1;
}
