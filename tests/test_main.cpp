#include "test_support.hpp"

void registerDecoderTests(test::Registry& registry);
void registerCfgTests(test::Registry& registry);
void registerAnalysisTests(test::Registry& registry);
void registerLifterTests(test::Registry& registry);
void registerSirTests(test::Registry& registry);
void registerWin64JitTests(test::Registry& registry);
void registerMemoryMapTests(test::Registry& registry);
void registerSlaveTests(test::Registry& registry);

int main() {
    test::Registry registry;
    registerDecoderTests(registry);
    registerCfgTests(registry);
    registerAnalysisTests(registry);
    registerLifterTests(registry);
    registerSirTests(registry);
    registerWin64JitTests(registry);
    registerMemoryMapTests(registry);
    registerSlaveTests(registry);
    return registry.run();
}
