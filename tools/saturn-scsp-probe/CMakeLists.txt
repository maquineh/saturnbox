add_executable(saturn-scsp-probe main.cpp)
target_link_libraries(saturn-scsp-probe PRIVATE saturn::m68k saturn::platform)
target_compile_features(saturn-scsp-probe PRIVATE cxx_std_20)
