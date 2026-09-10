#pragma once

#include <functional>

#include "FloatEnvironment.h"

namespace abglow {

// Parallel execution backend. In the plug-in this maps onto the host thread
// pool (PF_Iterate8Suite1::iterate_generic) so we never oversubscribe the CPU
// while After Effects renders several frames at once.
class TaskRunner {
public:
    using Body = std::function<void(int index, int thread_index)>;

    virtual ~TaskRunner() = default;
    virtual int ThreadCount() const = 0;
    virtual void ParallelFor(int count, const Body& body) = 0;
};

// Runs everything on the calling thread.
class SerialTaskRunner final : public TaskRunner {
public:
    int ThreadCount() const override { return 1; }

    void ParallelFor(int count, const Body& body) override {
        for (int i = 0; i < count; ++i) body(i, 0);
    }
};

// Splits [0, rows) into chunks sized for the available threads and runs `body`
// over each chunk's row range.
template <typename RowRangeFn>
void ParallelRows(TaskRunner& runner, int rows, const RowRangeFn& body) {
    if (rows <= 0) return;
    const int threads = runner.ThreadCount() > 0 ? runner.ThreadCount() : 1;
    if (threads <= 1 || rows < 8) {
        ScopedFlushDenormals denormals;
        body(0, rows, 0);
        return;
    }
    const int chunks = threads * 4 < rows ? threads * 4 : rows;
    const int rows_per_chunk = (rows + chunks - 1) / chunks;
    runner.ParallelFor(chunks, [&](int index, int thread_index) {
        const int begin = index * rows_per_chunk;
        if (begin >= rows) return;
        int end = begin + rows_per_chunk;
        if (end > rows) end = rows;
        ScopedFlushDenormals denormals;
        body(begin, end, thread_index);
    });
}

}  // namespace abglow
