#pragma once
#include "provider.hpp"
namespace golden::showcase {
bool variant_run(std::string_view variant, std::string_view dispatch, Options,
                 const std::filesystem::path&, Invocation&);
}
