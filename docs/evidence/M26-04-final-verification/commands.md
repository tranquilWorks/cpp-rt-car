# Reproduction

    ./scripts/agent-verify.sh full
    ctest --test-dir build/agent-quick -R host_adapter_golden_xdma --output-on-failure
    python3 tests/golden_xdma/verify_package.py --work-directory build/m26-04-package
    clang++-14 --analyze -Xanalyzer -analyzer-output=text -std=c++20 -Wall -Wextra -Werror -Irt/include -Icore/include -Iinclude samples/golden_xdma/main.cpp tests/golden_xdma/test_xdma.cpp

Extract retained-logs.tar.gz to inspect every original log and runner;
retained-artifacts.json binds each uncompressed file by size and SHA256.
To reproduce the library caches, configure CMake with build type Debug,
ENABLE_TESTS=OFF, RTFW_BUILD_EXAMPLES=ON, RTFW_BUILD_EXPERIMENTAL=OFF,
RTFW_BUILD_BENCHMARKS=OFF, RTFW_ENABLE_CUDA=OFF, RTFW_ENABLE_XDMA=OFF and
SIM_WERROR=ON. Set SIM_SANITIZERS to the CMake list address;undefined for
build/m26-04r-asan and thread for build/m26-04r-tsan. The retained build-binding
JSON records exact compiler/configuration, library and fixture binary hashes.
The retained sanitizer runner builds Debug/O0 Runtime/CUDA/XDMA libraries with
ASan+UBSan or TSan and the new inline fixture with matching full instrumentation
and-O1. It executes all25 bounded CTest cases, including full1024 owner replay;
only diagnosed before-main mapping failures retry. Functional failures stop.
Commands, flags, exits and all attempts are retained with reviewed source hashes.
No physical or independent-human acceptance follows from these commands.
