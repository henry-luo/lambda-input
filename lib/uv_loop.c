/**
 * @file uv_loop.c
 * @brief Unified libuv event loop wrapper implementation
 */

#include "uv_loop.h"
#include "memtrack.h"
#include "log.h"
#include <stdlib.h>
#include <limits.h>
#include <stdatomic.h>

struct LambdaUvLoop {
    uv_loop_t* loop;
    uv_prepare_t prepare;
    uv_check_t check;
    LambdaUvDrain microtask_drain;
    LambdaUvDrain task_drain;
    void* drain_context;
    unsigned run_depth;
    bool prepare_active;
    bool check_active;
};

static LambdaUvLoop g_default;
static uv_loop_t *g_loop = NULL;
static void (*g_microtask_drain)(void) = NULL;
static void (*g_task_drain)(void) = NULL;
static bool g_host_owned = false;
static THREAD_LOCAL unsigned g_shared_loop_depth = 0;
static atomic_uint g_host_leases = 0;
static atomic_bool g_loop_closing = false;

void lambda_uv_set_host_owned(bool owned) {
    g_host_owned = owned;
}

bool lambda_uv_is_host_owned(void) {
    return g_host_owned || atomic_load(&g_host_leases) != 0;
}

bool lambda_uv_host_lease_acquire(void) {
    if (atomic_load(&g_loop_closing)) return false;
    unsigned leases = atomic_load(&g_host_leases);
    do {
        if (leases == UINT_MAX) return false;
    } while (!atomic_compare_exchange_weak(&g_host_leases, &leases, leases + 1));
    // Teardown may have started between the first check and the increment.
    if (atomic_load(&g_loop_closing)) {
        atomic_fetch_sub(&g_host_leases, 1);
        return false;
    }
    return true;
}

bool lambda_uv_host_lease_release(void) {
    unsigned leases = atomic_load(&g_host_leases);
    do {
        if (!leases) return false;
    } while (!atomic_compare_exchange_weak(&g_host_leases, &leases, leases - 1));
    return true;
}

bool lambda_uv_has_host_leases(void) {
    return atomic_load(&g_host_leases) != 0;
}

static bool lambda_uv_begin_cleanup(void) {
    bool expected = false;
    if (!atomic_compare_exchange_strong(&g_loop_closing, &expected, true)) return false;
    if (lambda_uv_has_host_leases()) {
        atomic_store(&g_loop_closing, false);
        log_error("uv_loop: cleanup refused while a host lease remains");
        return false;
    }
    return true;
}

static void drain_task_checkpoint(LambdaUvLoop* state) {
    if (state && state->microtask_drain) state->microtask_drain(state->drain_context);
}

static void prepare_cb(uv_prepare_t *handle) {
    // drain tasks before libuv enters poll
    drain_task_checkpoint((LambdaUvLoop*)handle->data);
}

static void check_cb(uv_check_t *handle) {
    LambdaUvLoop* state = (LambdaUvLoop*)handle->data;
    // drain tasks after poll callbacks complete
    drain_task_checkpoint(state);
    // Lambda resumes are macrotasks: they run only after the JS microtask checkpoint.
    if (state->task_drain) state->task_drain(state->drain_context);
}

void lambda_uv_checkpoint(void) {
    if (g_microtask_drain) g_microtask_drain();
}

static void legacy_microtask_drain(void* context) {
    (void)context;
    if (g_microtask_drain) g_microtask_drain();
}

static void legacy_task_drain(void* context) {
    (void)context;
    if (g_task_drain) g_task_drain();
}

static int lambda_uv_state_init(LambdaUvLoop* state) {
    state->loop = (uv_loop_t*)mem_alloc(sizeof(uv_loop_t), MEM_CAT_TEMP);
    if (!state->loop) return UV_ENOMEM;
    int result = uv_loop_init(state->loop);
    if (result != 0) {
        mem_free(state->loop);
        state->loop = NULL;
        return result;
    }
    bool prepare_initialized = false;
    bool check_initialized = false;
    result = uv_prepare_init(state->loop, &state->prepare);
    if (result != 0) goto fail;
    prepare_initialized = true;
    state->prepare.data = state;
    result = uv_prepare_start(&state->prepare, prepare_cb);
    if (result != 0) goto fail;
    state->prepare_active = true;
    uv_unref((uv_handle_t*)&state->prepare);
    result = uv_check_init(state->loop, &state->check);
    if (result != 0) goto fail;
    check_initialized = true;
    state->check.data = state;
    result = uv_check_start(&state->check, check_cb);
    if (result != 0) goto fail;
    state->check_active = true;
    uv_unref((uv_handle_t*)&state->check);
    return 0;

fail:
    if (check_initialized) uv_close((uv_handle_t*)&state->check, NULL);
    if (prepare_initialized) uv_close((uv_handle_t*)&state->prepare, NULL);
    uv_run(state->loop, UV_RUN_DEFAULT);
    uv_loop_close(state->loop);
    mem_free(state->loop);
    state->loop = NULL;
    state->prepare_active = state->check_active = false;
    return result;
}

int lambda_uv_instance_create(LambdaUvLoop** out) {
    if (!out) return UV_EINVAL;
    *out = NULL;
    LambdaUvLoop* state = (LambdaUvLoop*)mem_calloc(1, sizeof(*state), MEM_CAT_TEMP);
    if (!state) return UV_ENOMEM;
    int result = lambda_uv_state_init(state);
    if (result != 0) {
        mem_free(state);
        return result;
    }
    *out = state;
    return 0;
}

uv_loop_t* lambda_uv_instance_loop(LambdaUvLoop* instance) {
    return instance ? instance->loop : NULL;
}

void lambda_uv_instance_set_drains(LambdaUvLoop* instance,
        LambdaUvDrain microtask, LambdaUvDrain task, void* context) {
    if (!instance) return;
    instance->microtask_drain = microtask;
    instance->task_drain = task;
    instance->drain_context = context;
}

void lambda_uv_instance_checkpoint(LambdaUvLoop* instance) {
    if (!instance) return;
    instance->run_depth++;
    drain_task_checkpoint(instance);
    instance->run_depth--;
}

int lambda_uv_instance_run(LambdaUvLoop* instance, uv_run_mode mode) {
    if (!instance || !instance->loop) return UV_EINVAL;
    if (instance->run_depth) return UV_EBUSY;
    instance->run_depth++;
    int result = uv_run(instance->loop, mode);
    instance->run_depth--;
    return result;
}

void lambda_uv_instance_stop(LambdaUvLoop* instance) {
    if (instance && instance->loop) uv_stop(instance->loop);
}

typedef struct LambdaUvWalk {
    LambdaUvLoop* instance;
    bool has_external_handles;
} LambdaUvWalk;

static void lambda_uv_find_external(uv_handle_t* handle, void* data) {
    LambdaUvWalk* walk = (LambdaUvWalk*)data;
    if (handle != (uv_handle_t*)&walk->instance->prepare &&
            handle != (uv_handle_t*)&walk->instance->check) {
        walk->has_external_handles = true;
    }
}

int lambda_uv_instance_destroy(LambdaUvLoop* instance) {
    if (!instance || !instance->loop) return UV_EINVAL;
    if (instance->run_depth) return UV_EBUSY;
    LambdaUvWalk walk = {instance, false};
    uv_walk(instance->loop, lambda_uv_find_external, &walk);
    // active requests and unreferenced user handles both prevent destruction.
    if (walk.has_external_handles || uv_loop_alive(instance->loop)) return UV_EBUSY;
    if (instance->prepare_active) {
        uv_prepare_stop(&instance->prepare);
        instance->prepare_active = false;
        uv_close((uv_handle_t*)&instance->prepare, NULL);
    }
    if (instance->check_active) {
        uv_check_stop(&instance->check);
        instance->check_active = false;
        uv_close((uv_handle_t*)&instance->check, NULL);
    }
    uv_run(instance->loop, UV_RUN_NOWAIT);
    int result = uv_loop_close(instance->loop);
    if (result != 0) return result;
    mem_free(instance->loop);
    mem_free(instance);
    return 0;
}

uv_loop_t* lambda_uv_loop(void) {
    return g_loop;
}

int lambda_uv_init(void) {
    if (atomic_load(&g_loop_closing)) return -1;
    if (g_loop) return 0; // already initialized
    int r = lambda_uv_state_init(&g_default);
    if (r != 0) {
        log_error("uv_loop: initialization failed: %s", uv_strerror(r));
        return r;
    }
    g_default.microtask_drain = legacy_microtask_drain;
    g_default.task_drain = legacy_task_drain;
    g_loop = g_default.loop;

    log_debug("uv_loop: initialized");
    return 0;
}

int lambda_uv_run(void) {
    if (!g_loop) return -1;
    return lambda_uv_run_loop(g_loop, UV_RUN_DEFAULT);
}

int lambda_uv_run_loop(uv_loop_t* loop, uv_run_mode mode) {
    if (!loop) return -1;
    bool shared = loop == g_loop;
    if (shared) g_shared_loop_depth++;
    int result = uv_run(loop, mode);
    if (shared) g_shared_loop_depth--;
    return result;
}

bool lambda_uv_is_running(void) {
    return g_shared_loop_depth != 0;
}

void lambda_uv_stop(void) {
    if (g_loop) uv_stop(g_loop);
}

static void close_walk_cb(uv_handle_t *handle, void *arg) {
    if (!uv_is_closing(handle)) {
        uv_close(handle, NULL);
    }
}

void lambda_uv_cleanup(void) {
    if (!g_loop) return;
    if (lambda_uv_is_running()) {
        log_error("uv_loop: cleanup refused inside a shared-loop callback");
        return;
    }
    if (!lambda_uv_begin_cleanup()) return;

    if (g_default.prepare_active) {
        uv_prepare_stop(&g_default.prepare);
        g_default.prepare_active = false;
    }
    if (g_default.check_active) {
        uv_check_stop(&g_default.check);
        g_default.check_active = false;
    }

    // close all remaining handles
    uv_walk(g_loop, close_walk_cb, NULL);
    lambda_uv_run_loop(g_loop, UV_RUN_DEFAULT);

    uv_loop_close(g_loop);
    mem_free(g_loop);
    g_loop = NULL;
    g_default.loop = NULL;
    g_microtask_drain = NULL;
    g_task_drain = NULL;
    atomic_store(&g_loop_closing, false);

    log_debug("uv_loop: cleaned up");
}

void lambda_uv_abandon(void) {
    if (!g_loop) return;
    if (!lambda_uv_begin_cleanup()) return;

    // A signal-aborted JS run can leave libuv queues half-mutated; walking or
    // closing handles would re-enter corrupted queue links during recovery.
    mem_free(g_loop);
    g_loop = NULL;
    g_default.loop = NULL;
    g_default.prepare_active = false;
    g_default.check_active = false;
    g_microtask_drain = NULL;
    g_task_drain = NULL;
    atomic_store(&g_loop_closing, false);
    log_error("uv_loop: abandoned unsafe loop without closing handles");
}

void lambda_uv_set_microtask_drain(void (*drain_fn)(void)) {
    g_microtask_drain = drain_fn;
}

void lambda_uv_set_task_drain(void (*drain_fn)(void)) {
    g_task_drain = drain_fn;
}
