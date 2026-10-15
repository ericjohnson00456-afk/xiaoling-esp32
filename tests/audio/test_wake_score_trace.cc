#include "wake_score_trace.h"

#include <cassert>

using micro_wake_word::WakeScoreTrace;

int main() {
    WakeScoreTrace trace;
    assert(!trace.Begin());
    for (int i = 1; i <= 45; ++i) assert(!trace.Push(static_cast<float>(i)));
    assert(trace.Begin());
    assert(trace.BeforeCount() == 20);
    assert(trace.Trigger() == 45.0f);
    for (int i = 0; i < 20; ++i) assert(trace.Before()[i] == 25.0f + i);
    assert(!trace.Begin());
    for (int i = 46; i < 65; ++i) assert(!trace.Push(static_cast<float>(i)));
    assert(trace.Collecting());
    assert(trace.Push(65.0f));
    assert(!trace.Collecting());
    for (int i = 0; i < 20; ++i) assert(trace.After()[i] == 46.0f + i);

    trace.Reset();
    assert(!trace.Begin());
    assert(!trace.Push(0.1f));
    assert(!trace.Push(0.9f));
    assert(trace.Begin());
    assert(trace.Id() == 2);
    assert(trace.BeforeCount() == 1);
    assert(trace.Before()[0] == 0.1f);
    assert(trace.Trigger() == 0.9f);
}
