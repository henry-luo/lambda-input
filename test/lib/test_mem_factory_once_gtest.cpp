#include <gtest/gtest.h>
#include <atomic>
#include <set>
#include <thread>
#include <vector>
#include "lib/mem_factory.h"
#include "lib/mem_context.h"
#include "lambda/core/mem_factory_core.h"

// this executable has no earlier factory user, so these threads race first use.
TEST(MemFactoryOnceTest, ConcurrentFirstUseInstallsAllReleaseHooks) {
    constexpr int thread_count = 8;
    constexpr int repetitions = 64;
    MemContext* root = mem_context_root();
    ASSERT_NE(root, nullptr);
    std::atomic<int> ready{0};
    std::atomic<bool> start{false};
    std::atomic<bool> failed{false};
    unsigned ids[thread_count][repetitions]{};
    std::vector<std::thread> workers;
    for (int i = 0; i < thread_count; i++) {
        workers.emplace_back([&, i] {
            ready.fetch_add(1);
            while (!start.load()) std::this_thread::yield();
            for (int j = 0; j < repetitions; j++) {
                Pool* pool = mem_pool_create(root, MEM_ROLE_INPUT, "once.pool");
                Arena* arena = mem_arena_create(root, MEM_ROLE_INPUT, "once.arena");
                NamePool* names = pool
                    ? mem_name_pool_create(root, pool, nullptr, MEM_ROLE_INPUT, "once.names")
                    : nullptr;
                if (!pool || !arena || !pool_get_mem_node(pool) || !arena_get_mem_node(arena))
                    failed.store(true);
                if (pool) ids[i][j] = pool_get_id(pool);
                if (!names || !names->mem_node) failed.store(true);
                if (arena) {
                    ScratchArena scratch{};
                    mem_scratch_init(root, &scratch, arena, MEM_ROLE_TEMP, "once.scratch");
                    if (!scratch.mem_node || !scratch_alloc(&scratch, 32)) failed.store(true);
                    scratch_release(&scratch);
                    if (scratch.mem_node || arena_active_scope_count(arena) != 0) failed.store(true);
                }
                if (names) name_pool_release(names);
                if (arena) mem_arena_destroy(arena);
                if (pool) mem_pool_destroy(pool);
            }
        });
    }
    while (ready.load() != thread_count) std::this_thread::yield();
    start.store(true);
    for (auto& worker : workers) worker.join();
    EXPECT_FALSE(failed.load());
    std::set<unsigned> observed;
    for (const auto& row : ids) {
        for (unsigned id : row) EXPECT_TRUE(observed.insert(id).second);
    }
    EXPECT_EQ(mem_context_live_count(root), 0u);
    mem_context_shutdown();
}
