#include "../protocol/worker_supervision.hpp"

#include <cassert>
#include <cstdint>
#include <iostream>

int main() {
    using nxpctl::protocol::MaxWorkerRestarts;
    using nxpctl::protocol::WorkerRestartBudget;

    static_assert(MaxWorkerRestarts == 3);

    WorkerRestartBudget budget{};
    assert(budget.restart_count == 0);
    assert(budget.TryConsume());
    assert(budget.restart_count == 1);
    assert(budget.TryConsume());
    assert(budget.restart_count == 2);
    assert(budget.TryConsume());
    assert(budget.restart_count == 3);
    assert(!budget.TryConsume());
    assert(!budget.TryConsume());
    assert(budget.restart_count == MaxWorkerRestarts);

    std::cout << "worker restart budget OK; max_restarts=" << MaxWorkerRestarts << '\n';
}
