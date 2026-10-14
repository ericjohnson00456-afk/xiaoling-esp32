#include "decode_yield_policy.h"
#include <cassert>
#include <cstdio>

int main() {
    DecodeYieldPolicy policy;
    policy.Reset(100000);
    assert(!policy.ShouldYield(100001, 1152, false));
    assert(policy.ShouldYield(100001, 0, false));
    assert(!policy.ShouldYield(149999, 1152, false));
    assert(policy.ShouldYield(150000, 1152, false));
    assert(!policy.ShouldYield(200000, 0, true));
    assert(!policy.ShouldYield(200000, 1152, true));
    policy.Reset(210000);
    assert(!policy.ShouldYield(210001, 1152, false));
    std::puts("decode yield: empty PCM, deadline, EOF and reset passed");
}
