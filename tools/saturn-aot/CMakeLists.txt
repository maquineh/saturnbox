add_executable(saturn-aot
    src/main.cpp
)

target_link_libraries(saturn-aot PRIVATE saturn::core saturn::sh2 saturn::sir)
target_compile_features(saturn-aot PRIVATE cxx_std_20)

if(MSVC)
    target_compile_options(saturn-aot PRIVATE /W4 /permissive-)
else()
    target_compile_options(saturn-aot PRIVATE -Wall -Wextra -Wpedantic)
endif()
