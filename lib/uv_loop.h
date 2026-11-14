/**
 * @file uv_loop.h
 * @brief Unified libuv event loop wrapper for Lambda
 *
 * Provides context-owned loops and the legacy shared-loop interface.
 */

#ifndef LAMBDA_UV_LOOP_H
#define LAMBDA_UV_LOOP_H

#include <uv.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct LambdaUvLoop LambdaUvLoop;
typedef void (*LambdaUvDrain)(void* context);

// Each instance is driven and modified by its owning thread. Drive it only
// through lambda_uv_instance_run so callback-time lifecycle checks apply.
// User handles and requests must be closed or completed before destroy;
// UV_EBUSY leaves the instance live and usable for draining or retry.
int lambda_uv_instance_create(LambdaUvLoop** out);
uv_loop_t* lambda_uv_instance_loop(LambdaUvLoop* instance);
// The caller keeps the callback context alive until it is replaced or the
// instance is destroyed, including any host-managed roots it requires.
void lambda_uv_instance_set_drains(LambdaUvLoop* instance,
    LambdaUvDrain microtask, LambdaUvDrain task, void* context);
void lambda_uv_instance_checkpoint(LambdaUvLoop* instance);
int lambda_uv_instance_run(LambdaUvLoop* instance, uv_run_mode mode);
void lambda_uv_instance_stop(LambdaUvLoop* instance);
int lambda_uv_instance_destroy(LambdaUvLoop* instance);

// global event loop (one per process)
uv_loop_t* lambda_uv_loop(void);

// lifecycle
int  lambda_uv_init(void);       // create loop, return 0 on success
int  lambda_uv_run(void);        // run until no active handles/requests
int  lambda_uv_run_loop(uv_loop_t* loop, uv_run_mode mode);
bool lambda_uv_is_running(void);
void lambda_uv_stop(void);       // stop loop (from signal handler etc.)
void lambda_uv_cleanup(void);    // close all handles, free loop
void lambda_uv_abandon(void);    // free an unsafe loop without walking handles
void lambda_uv_set_host_owned(bool owned);
bool lambda_uv_is_host_owned(void);
// Long-lived owners keep the shared loop through nested runtime teardown.
bool lambda_uv_host_lease_acquire(void);
bool lambda_uv_host_lease_release(void);
bool lambda_uv_has_host_leases(void);

// JS task integration — called at event-loop phase checkpoints
void lambda_uv_set_microtask_drain(void (*drain_fn)(void));
void lambda_uv_set_task_drain(void (*drain_fn)(void));
// Run the microtask checkpoint now. uv_run skips its check phase when no
// handle keeps the loop alive, so an idle drive calls this before giving up.
void lambda_uv_checkpoint(void);

#ifdef __cplusplus
}
#endif

#endif // LAMBDA_UV_LOOP_H
