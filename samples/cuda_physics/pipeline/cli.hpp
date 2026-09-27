#pragma once
#include "scenario.hpp"
#include <charconv>
#include <iostream>

namespace rtfw::cuda_physics::pipeline {
inline int parse(int argc,char** argv,Options& o,bool real=false) {
    if (argc==2 && std::string_view(argv[1])=="--help") {
        std::cout<<"usage: sample_cuda_pipeline"<<(real?"_real":"")
          <<" [--dispatch kernel|graph] [--schedule frame|active] [--count 1..4096] [--steps 1..1024] [--seed UINT32] [--workers 1..2]\n";
        return 3;
    }
    unsigned seen{};
    for (int i=1;i<argc;i+=2) {
        if (i+1==argc) return 2;
        const std::string_view flag(argv[i]),v(argv[i+1]); unsigned bit{};
        if (flag=="--dispatch") {
            if (v!="kernel" && v!="graph") return 2;
            o.graph=v=="graph"; bit=16;
        } else if (flag=="--schedule") {
            if (v!="frame" && v!="active") return 2;
            o.active=v=="active"; bit=32;
        } else {
            std::uint32_t n{}; const auto r=std::from_chars(v.data(),v.data()+v.size(),n);
            if (r.ec!=std::errc{} || r.ptr!=v.data()+v.size()) return 2;
            if (flag=="--count") { o.count=n; bit=1; }
            else if (flag=="--steps") { o.steps=n; bit=2; }
            else if (flag=="--seed") { o.seed=n; bit=4; }
            else if (flag=="--workers") { o.workers=n; bit=8; }
            else return 2;
        }
        if (seen&bit) return 2;
        seen|=bit;
    }
    return o.valid()?0:2;
}
inline void summary(const Options& o,std::uint64_t completed,std::string_view evidence) {
    std::cout<<"model=constant-acceleration-v1 dispatch="<<(o.graph?"graph":"kernel")
      <<" schedule="<<(o.active?"active":"frame")<<" lanes="<<lanes(o)<<" seed="<<o.seed
      <<" count="<<o.count<<" steps="<<o.steps<<" workers="<<o.workers
      <<" completed="<<completed<<" validation=pass evidence="<<evidence<<'\n';
}
}
