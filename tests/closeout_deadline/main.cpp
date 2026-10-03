// Reuse every assertion in the original full-frame probe without editing it.
#define main original_native_main
#include "../golden_xdma_native/test_native.cpp"
#undef main
#include "trace.hpp"
int main(int argc, char** argv) {
    if (argc != 2 || (std::string_view(argv[1]) != "--frame" &&
                      std::string_view(argv[1]) != "--delay-submit")) return 2;
    deadline_trace::delay_submit = std::string_view(argv[1]) == "--delay-submit";
    char name[] = "unchanged-native-probe", frame[] = "--frame";
    char* arguments[] = {name, frame};
    const int result = original_native_main(2, arguments);
    // Original local Sessions have now performed checked destruction/joins.
    const bool complete = deadline_trace::dump();
    std::printf("ORIGINAL_EXIT %d\n", result);
    return complete ? result : 3;
}
