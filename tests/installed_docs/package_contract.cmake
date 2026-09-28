# Additive archive/recipe contract; retain all earlier package assertions.
foreach(document README.md recipes.md inventory.json api/index.html api/rt__runtime.hpp.html)
    if(NOT EXISTS "${RTFW_DATA_DIR}/manual/${document}")
        message(FATAL_ERROR "Installed SDK manual is missing ${document}")
    endif()
endforeach()
set(RECIPE_WITH_BENCHMARK "${RTFW_TEST_BENCHMARK}")
add_subdirectory("${RTFW_DATA_DIR}/examples/recipes" installed-recipes)
