#include <gtest/gtest.h>
#include "lib/uv_loop.h"
#include <atomic>
#include <thread>
#include <vector>

struct LoopProbe {
    LambdaUvLoop* instance = nullptr;
    uv_timer_t timer{};
    int microtasks = 0;
    int tasks = 0;
    int timers = 0;
    int nested_run = 0;
    int nested_destroy = 0;
    std::vector<char> order;
};

static void count_microtask(void* data) {
    LoopProbe* probe = static_cast<LoopProbe*>(data);
    probe->microtasks++;
    probe->order.push_back('m');
}

static void count_task(void* data) {
    LoopProbe* probe = static_cast<LoopProbe*>(data);
    probe->tasks++;
    probe->order.push_back('t');
}

static void close_timer(uv_timer_t* timer) {
    LoopProbe* probe = static_cast<LoopProbe*>(timer->data);
    probe->timers++;
    probe->nested_run = lambda_uv_instance_run(probe->instance, UV_RUN_NOWAIT);
    probe->nested_destroy = lambda_uv_instance_destroy(probe->instance);
    uv_close(reinterpret_cast<uv_handle_t*>(timer), nullptr);
}

TEST(UvLoopInstanceTest, ConcurrentOwnersKeepCallbacksAndLoopsSeparate) {
    constexpr int owners = 4;
    constexpr int repetitions = 12;
    std::atomic<int> ready{0};
    std::atomic<bool> start{false};
    std::vector<std::thread> threads;
    for (int i = 0; i < owners; i++) {
        threads.emplace_back([&] {
            for (int iteration = 0; iteration < repetitions; iteration++) {
                if (iteration == 0) {
                    ready.fetch_add(1);
                    while (!start.load()) std::this_thread::yield();
                }
                LoopProbe probe;
                ASSERT_EQ(lambda_uv_instance_create(&probe.instance), 0);
                ASSERT_NE(lambda_uv_instance_loop(probe.instance), nullptr);
                lambda_uv_instance_set_drains(probe.instance, count_microtask, count_task, &probe);
                lambda_uv_instance_checkpoint(probe.instance);
                EXPECT_EQ(probe.microtasks, 1);
                ASSERT_EQ(uv_timer_init(lambda_uv_instance_loop(probe.instance), &probe.timer), 0);
                probe.timer.data = &probe;
                ASSERT_EQ(uv_timer_start(&probe.timer, close_timer, 1, 0), 0);
                EXPECT_EQ(lambda_uv_instance_run(probe.instance, UV_RUN_DEFAULT), 0);
                EXPECT_EQ(probe.timers, 1);
                EXPECT_GT(probe.microtasks, 1);
                EXPECT_GT(probe.tasks, 0);
                for (size_t event = 0; event < probe.order.size(); event++) {
                    if (probe.order[event] == 't') {
                        ASSERT_GT(event, 0u);
                        EXPECT_EQ(probe.order[event - 1], 'm');
                    }
                }
                EXPECT_EQ(probe.nested_run, UV_EBUSY);
                EXPECT_EQ(probe.nested_destroy, UV_EBUSY);
                EXPECT_EQ(lambda_uv_instance_destroy(probe.instance), 0);
            }
        });
    }
    while (ready.load() != owners) std::this_thread::yield();
    start.store(true);
    for (auto& thread : threads) thread.join();
}

TEST(UvLoopInstanceTest, DestroyRefusesUnreferencedExternalHandle) {
    LambdaUvLoop* instance = nullptr;
    ASSERT_EQ(lambda_uv_instance_create(&instance), 0);
    uv_timer_t timer{};
    ASSERT_EQ(uv_timer_init(lambda_uv_instance_loop(instance), &timer), 0);
    uv_unref(reinterpret_cast<uv_handle_t*>(&timer));
    EXPECT_EQ(lambda_uv_instance_destroy(instance), UV_EBUSY);
    uv_close(reinterpret_cast<uv_handle_t*>(&timer), nullptr);
    EXPECT_EQ(lambda_uv_instance_run(instance, UV_RUN_DEFAULT), 0);
    EXPECT_EQ(lambda_uv_instance_destroy(instance), 0);
}

static void destroy_from_checkpoint(void* data) {
    LoopProbe* probe = static_cast<LoopProbe*>(data);
    probe->nested_destroy = lambda_uv_instance_destroy(probe->instance);
}

TEST(UvLoopInstanceTest, DestroyRefusesInsideExplicitCheckpoint) {
    LoopProbe probe;
    ASSERT_EQ(lambda_uv_instance_create(&probe.instance), 0);
    lambda_uv_instance_set_drains(probe.instance, destroy_from_checkpoint, nullptr, &probe);
    lambda_uv_instance_checkpoint(probe.instance);
    EXPECT_EQ(probe.nested_destroy, UV_EBUSY);
    EXPECT_EQ(lambda_uv_instance_destroy(probe.instance), 0);
}

struct WorkProbe {
    uv_work_t request{};
    std::atomic<bool> ran{false};
    bool finished = false;
};

TEST(UvLoopInstanceTest, DestroyRefusesPendingWork) {
    LambdaUvLoop* instance = nullptr;
    ASSERT_EQ(lambda_uv_instance_create(&instance), 0);
    WorkProbe probe;
    probe.request.data = &probe;
    ASSERT_EQ(uv_queue_work(lambda_uv_instance_loop(instance), &probe.request,
        [](uv_work_t* request) { static_cast<WorkProbe*>(request->data)->ran.store(true); },
        [](uv_work_t* request, int status) {
            EXPECT_EQ(status, 0);
            static_cast<WorkProbe*>(request->data)->finished = true;
        }), 0);
    EXPECT_EQ(lambda_uv_instance_destroy(instance), UV_EBUSY);
    EXPECT_EQ(lambda_uv_instance_run(instance, UV_RUN_DEFAULT), 0);
    EXPECT_TRUE(probe.ran.load());
    EXPECT_TRUE(probe.finished);
    EXPECT_EQ(lambda_uv_instance_destroy(instance), 0);
}

static int legacy_checkpoints;
static void legacy_drain() { legacy_checkpoints++; }

TEST(UvLoopInstanceTest, LegacyCheckpointAndLoopStillWork) {
    lambda_uv_cleanup();
    legacy_checkpoints = 0;
    lambda_uv_set_microtask_drain(legacy_drain);
    lambda_uv_checkpoint();
    EXPECT_EQ(legacy_checkpoints, 1);
    ASSERT_EQ(lambda_uv_init(), 0);
    LoopProbe separate;
    ASSERT_EQ(lambda_uv_instance_create(&separate.instance), 0);
    EXPECT_NE(lambda_uv_instance_loop(separate.instance), lambda_uv_loop());
    lambda_uv_instance_set_drains(separate.instance, count_microtask, count_task, &separate);
    lambda_uv_instance_checkpoint(separate.instance);
    EXPECT_EQ(separate.microtasks, 1);
    EXPECT_EQ(legacy_checkpoints, 1);
    uv_timer_t timer{};
    ASSERT_EQ(uv_timer_init(lambda_uv_loop(), &timer), 0);
    ASSERT_EQ(uv_timer_start(&timer, [](uv_timer_t* handle) {
        uv_close(reinterpret_cast<uv_handle_t*>(handle), nullptr);
    }, 1, 0), 0);
    EXPECT_EQ(lambda_uv_run(), 0);
    EXPECT_GT(legacy_checkpoints, 1);
    EXPECT_EQ(separate.microtasks, 1);
    EXPECT_EQ(lambda_uv_instance_destroy(separate.instance), 0);
    lambda_uv_cleanup();
    EXPECT_EQ(lambda_uv_loop(), nullptr);
}
