#include "test_support.hpp"

void registerDecoderTests(test::Registry& registry);
void registerCfgTests(test::Registry& registry);
void registerAnalysisTests(test::Registry& registry);
void registerLifterTests(test::Registry& registry);
void registerSirTests(test::Registry& registry);
void registerWin64JitTests(test::Registry& registry);
void registerMemoryMapTests(test::Registry& registry);
void registerSlaveTests(test::Registry& registry);
void registerVdp1FramebufferTests(test::Registry& registry);
void registerScspTimingTests(test::Registry& registry);
void registerScspSynthTests(test::Registry& registry);
void registerScspDriverTests(test::Registry& registry);
void registerScspDmaTests(test::Registry& registry);
void registerVdp1DebugTests(test::Registry& registry);
void registerLiveDebugTests(test::Registry& registry);
#ifdef SATURNBOX_TEST_M68K
void registerM68kTests(test::Registry& registry);
#endif

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
    registerVdp1FramebufferTests(registry);
    registerScspTimingTests(registry);
    registerScspSynthTests(registry);
    registerScspDriverTests(registry);
    registerScspDmaTests(registry);
    registerVdp1DebugTests(registry);
    registerLiveDebugTests(registry);
#ifdef SATURNBOX_TEST_M68K
    registerM68kTests(registry);
#endif
    return registry.run();
}
