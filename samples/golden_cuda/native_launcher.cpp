// The optional native host links the real Driver API. Keep its loader
// dependency behind this control-plane launcher so absent drivers and --help
// are usable.
#include <iostream>
#include <string_view>
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <process.h>
#include <windows.h>
#else
#include <dlfcn.h>
#include <unistd.h>
#endif
int main(int argc, char **argv) {
  if (argc == 2 && std::string_view(argv[1]) == "--help") {
    std::cout
        << "golden_cuda_real [--dispatch kernel|graph] [--mode native|host] "
           "[--count 1..256] [--ticks 1..1024] [--workers 1..3] "
           "[--grain 1|4|16|64] [--output DIRECTORY]\n"
           "Native replay unsupported; absent driver/device exits3 NOT_RUN.\n";
    return 0;
  }
#if defined(_WIN32)
  const auto driver = LoadLibraryA("nvcuda.dll");
#else
  auto *driver = dlopen("libcuda.so.1", RTLD_NOW | RTLD_LOCAL);
#endif
  if (!driver) {
    std::cout << "status=NOT_RUN reason=CUDA_driver_library_unavailable\n";
    return 3;
  }
#if defined(_WIN32)
  if (!FreeLibrary(driver))
    return 1;
  const auto result = _spawnv(_P_WAIT, RTFW_GOLDEN_CUDA_DRIVER_PATH,
                              const_cast<const char *const *>(argv));
  return result >= 0 && result <= 3 ? static_cast<int>(result) : 1;
#else
  if (dlclose(driver) != 0)
    return 1;
  execv(RTFW_GOLDEN_CUDA_DRIVER_PATH, argv);
  std::cerr << "Cannot execute the native CUDA host\n";
  return 1;
#endif
}
