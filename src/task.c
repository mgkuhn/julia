// This file is a part of Julia. License is MIT: https://julialang.org/license

/*
  task.c
  lightweight processes (symmetric coroutines)
*/

// need this to get the real definition of ucontext_t,
// if we're going to use the ucontext_t implementation there
//#if defined(__APPLE__) && defined(JL_HAVE_UCONTEXT)
//#pragma push_macro("_XOPEN_SOURCE")
//#define _XOPEN_SOURCE
//#include <ucontext.h>
//#pragma pop_macro("_XOPEN_SOURCE")
//#endif

// this is needed for !COPY_STACKS to work on linux
#ifdef _FORTIFY_SOURCE
// disable __longjmp_chk validation so that we can jump between stacks
// (which would normally be invalid to do with setjmp / longjmp)
#pragma push_macro("_FORTIFY_SOURCE")
#undef _FORTIFY_SOURCE
#include <setjmp.h>
#pragma pop_macro("_FORTIFY_SOURCE")
#endif

#include "platform.h"

#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <errno.h>
#include <inttypes.h>
#include "julia.h"
#include "julia_internal.h"
#include "threading.h"
#include "julia_assert.h"

#ifdef __cplusplus
extern "C" {
#endif

#if defined(JL_ASAN_ENABLED)
static inline void sanitizer_start_switch_fiber(const void* bottom, size_t size) {
    __sanitizer_start_switch_fiber(NULL, bottom, size);
}
static inline void sanitizer_finish_switch_fiber(void) {
    __sanitizer_finish_switch_fiber(NULL, NULL, NULL);
}
#else
static inline void sanitizer_start_switch_fiber(const void* bottom, size_t size) {}
static inline void sanitizer_finish_switch_fiber(void) {}
#endif

#if defined(JL_TSAN_ENABLED)
static inline void tsan_destroy_ctx(jl_ptls_t ptls, void *state) {
    if (state != &ptls->root_task->state) {
        __tsan_destroy_fiber(ctx->state);
    }
    ctx->state = NULL;
}
static inline void tsan_switch_to_ctx(void *state)  {
    __tsan_switch_to_fiber(state, 0);
}
#endif

// empirically, jl_finish_task needs about 64k stack space to infer/run
// and additionally, gc-stack reserves 64k for the guard pages
#if defined(MINSIGSTKSZ) && MINSIGSTKSZ > 131072
#define MINSTKSZ MINSIGSTKSZ
#else
#define MINSTKSZ 131072
#endif

#define ROOT_TASK_STACK_ADJUSTMENT 3000000

#ifdef JL_HAVE_ASYNCIFY
// Switching logic is implemented in JavaScript
#define STATIC_OR_JS JL_DLLEXPORT
#else
#define STATIC_OR_JS static
#endif

extern size_t jl_page_size;
static char *jl_alloc_fiber(jl_ucontext_t *t, size_t *ssize, jl_task_t *owner) JL_NOTSAFEPOINT;
STATIC_OR_JS void jl_set_fiber(jl_ucontext_t *t);
STATIC_OR_JS void jl_swap_fiber(jl_ucontext_t *lastt, jl_ucontext_t *t);
STATIC_OR_JS void jl_start_fiber_swap(jl_ucontext_t *savet, jl_ucontext_t *t);
STATIC_OR_JS void jl_start_fiber_set(jl_ucontext_t *t);

#ifdef ALWAYS_COPY_STACKS
# ifndef COPY_STACKS
# error "ALWAYS_COPY_STACKS requires COPY_STACKS"
# endif
static int always_copy_stacks = 1;
#else
static int always_copy_stacks = 0;
#endif

#ifdef COPY_STACKS
static void memcpy_a16(uint64_t *to, uint64_t *from, size_t nb)
{
    memcpy((char*)jl_assume_aligned(to, 16), (char*)jl_assume_aligned(from, 16), nb);
    //uint64_t *end = (uint64_t*)((char*)from + nb);
    //while (from < end)
    //    *(to++) = *(from++);
}

static void NOINLINE save_stack(jl_ptls_t ptls, jl_task_t *lastt, jl_task_t **pt)
{
    char *frame_addr = (char*)((uintptr_t)jl_get_frame_addr() & ~15);
    char *stackbase = (char*)ptls->stackbase;
    assert(stackbase > frame_addr);
    size_t nb = stackbase - frame_addr;
    void *buf;
    if (lastt->bufsz < nb) {
        buf = (void*)jl_gc_alloc_buf(ptls, nb);
        lastt->stkbuf = buf;
        lastt->bufsz = nb;
    }
    else {
        buf = lastt->stkbuf;
    }
    *pt = NULL; // clear the gc-root for the target task before copying the stack for saving
    lastt->copy_stack = nb;
    lastt->sticky = 1;
    memcpy_a16((uint64_t*)buf, (uint64_t*)frame_addr, nb);
    // this task's stack could have been modified after
    // it was marked by an incremental collection
    // move the barrier back instead of walking it again here
    jl_gc_wb_back(lastt);
}

static void NOINLINE JL_NORETURN restore_stack(jl_task_t *t, jl_ptls_t ptls, char *p)
{
    size_t nb = t->copy_stack;
    char *_x = (char*)ptls->stackbase - nb;
    if (!p) {
        // switch to a stackframe that's beyond the bounds of the last switch
        p = _x;
        if ((char*)&_x > _x) {
            p = (char*)alloca((char*)&_x - _x);
        }
        restore_stack(t, ptls, p); // pass p to ensure the compiler can't tailcall this or avoid the alloca
    }
    void *_y = t->stkbuf;
    assert(_x != NULL && _y != NULL);
    memcpy_a16((uint64_t*)_x, (uint64_t*)_y, nb); // destroys all but the current stackframe

    sanitizer_start_switch_fiber(t->stkbuf, t->bufsz);
#if defined(_OS_WINDOWS_)
    jl_setcontext(&t->ctx);
#else
    jl_longjmp(t->copy_stack_ctx.uc_mcontext, 1);
#endif
    abort(); // unreachable
}

static void restore_stack2(jl_task_t *t, jl_ptls_t ptls, jl_task_t *lastt)
{
    assert(t->copy_stack && !lastt->copy_stack);
    size_t nb = t->copy_stack;
    char *_x = (char*)ptls->stackbase - nb;
    void *_y = t->stkbuf;
    assert(_x != NULL && _y != NULL);
    memcpy_a16((uint64_t*)_x, (uint64_t*)_y, nb); // destroys all but the current stackframe
#if defined(JL_HAVE_UNW_CONTEXT)
    volatile int returns = 0;
    int r = unw_getcontext(&lastt->ctx);
    if (++returns == 2) // r is garbage after the first return
        return;
    if (r != 0 || returns != 1)
        abort();
#elif defined(JL_HAVE_ASM) || defined(JL_HAVE_SIGALTSTACK) || defined(_OS_WINDOWS_)
    if (jl_setjmp(lastt->copy_stack_ctx.uc_mcontext, 0))
        return;
#else
#error COPY_STACKS is incompatible with this platform
#endif
    sanitizer_start_switch_fiber(t->stkbuf, t->bufsz);
#if defined(_OS_WINDOWS_)
    jl_setcontext(&t->ctx);
#else
    jl_longjmp(t->copy_stack_ctx.uc_mcontext, 1);
#endif
}
#endif

/* Rooted by the base module */
static jl_function_t *task_done_hook_func JL_GLOBALLY_ROOTED = NULL;

void JL_NORETURN jl_finish_task(jl_task_t *t)
{
    jl_ptls_t ptls = jl_get_ptls_states();
    JL_SIGATOMIC_BEGIN();
    if (t->_isexception)
        jl_atomic_store_release(&t->_state, JL_TASK_STATE_FAILED);
    else
        jl_atomic_store_release(&t->_state, JL_TASK_STATE_DONE);
    if (t->copy_stack) // early free of stkbuf
        t->stkbuf = NULL;
    // ensure that state is cleared
    ptls->in_finalizer = 0;
    ptls->in_pure_callback = 0;
    jl_get_ptls_states()->world_age = jl_world_counter;
    // let the runtime know this task is dead and find a new task to run
    jl_function_t *done = jl_atomic_load_relaxed(&task_done_hook_func);
    if (done == NULL) {
        done = (jl_function_t*)jl_get_global(jl_base_module, jl_symbol("task_done_hook"));
        if (done != NULL)
            jl_atomic_store_release(&task_done_hook_func, done);
    }
    if (done != NULL) {
        jl_value_t *args[2] = {done, (jl_value_t*)t};
        JL_TRY {
            jl_apply(args, 2);
        }
        JL_CATCH {
            jl_no_exc_handler(jl_current_exception());
        }
    }
    gc_debug_critical_error();
    abort();
}

JL_DLLEXPORT void *jl_task_stack_buffer(jl_task_t *task, size_t *size, int *tid)
{
    size_t off = 0;
#ifndef _OS_WINDOWS_
    if (jl_all_tls_states[0]->root_task == task) {
        // See jl_init_root_task(). The root task of the main thread
        // has its buffer enlarged by an artificial 3000000 bytes, but
        // that means that the start of the buffer usually points to
        // inaccessible memory. We need to correct for this.
        off = ROOT_TASK_STACK_ADJUSTMENT;
    }
#endif
    *tid = -1;
    for (int i = 0; i < jl_n_threads; i++) {
        jl_ptls_t ptls = jl_all_tls_states[i];
        if (ptls->current_task == task) {
            *tid = i;
#ifdef COPY_STACKS
            if (task->copy_stack) {
                *size = ptls->stacksize;
                return (char *)ptls->stackbase - *size;
            }
#endif
            break; // continue with normal return
        }
    }
    *size = task->bufsz - off;
    return (void *)((char *)task->stkbuf + off);
}

JL_DLLEXPORT void jl_active_task_stack(jl_task_t *task,
                                       char **active_start, char **active_end,
                                       char **total_start, char **total_end)
{
    if (!task->started) {
        *total_start = *active_start = 0;
        *total_end = *active_end = 0;
        return;
    }

    int16_t tid = task->tid;
    jl_ptls_t ptls2 = (tid != -1) ? jl_all_tls_states[tid] : 0;

    if (task->copy_stack && ptls2 && task == ptls2->current_task) {
        *total_start = *active_start = (char*)ptls2->stackbase - ptls2->stacksize;
        *total_end = *active_end = (char*)ptls2->stackbase;
    }
    else if (task->stkbuf) {
        *total_start = *active_start = (char*)task->stkbuf;
#ifndef _OS_WINDOWS_
        if (jl_all_tls_states[0]->root_task == task) {
            // See jl_init_root_task(). The root task of the main thread
            // has its buffer enlarged by an artificial 3000000 bytes, but
            // that means that the start of the buffer usually points to
            // inaccessible memory. We need to correct for this.
            *active_start += ROOT_TASK_STACK_ADJUSTMENT;
            *total_start += ROOT_TASK_STACK_ADJUSTMENT;
        }
#endif

        *total_end = *active_end = (char*)task->stkbuf + task->bufsz;
#ifdef COPY_STACKS
        // save_stack stores the stack of an inactive task in stkbuf, and the
        // actual number of used bytes in copy_stack.
        if (task->copy_stack > 1)
            *active_end = (char*)task->stkbuf + task->copy_stack;
#endif
    }
    else {
        // no stack allocated yet
        *total_start = *active_start = 0;
        *total_end = *active_end = 0;
        return;
    }

    if (task == jl_current_task) {
        // scan up to current `sp` for current thread and task
        *active_start = (char*)jl_get_frame_addr();
    }
}

// Marked noinline so we can consistently skip the associated frame.
// `skip` is number of additional frames to skip.
NOINLINE static void record_backtrace(jl_ptls_t ptls, int skip) JL_NOTSAFEPOINT
{
    // storing bt_size in ptls ensures roots in bt_data will be found
    ptls->bt_size = rec_backtrace(ptls->bt_data, JL_MAX_BT_SIZE, skip + 1);
}

JL_DLLEXPORT void julia_init(JL_IMAGE_SEARCH rel)
{
    _julia_init(rel);
}

JL_DLLEXPORT void jl_set_next_task(jl_task_t *task) JL_NOTSAFEPOINT
{
    jl_get_ptls_states()->next_task = task;
}

JL_DLLEXPORT jl_task_t *jl_get_next_task(void) JL_NOTSAFEPOINT
{
    jl_ptls_t ptls = jl_get_ptls_states();
    if (ptls->next_task)
        return ptls->next_task;
    return ptls->current_task;
}

#ifdef JL_TSAN_ENABLED
const char tsan_state_corruption[] = "TSAN state corrupted. Exiting HARD!\n";
#endif

static void ctx_switch(jl_ptls_t ptls)
{
    jl_task_t **pt = &ptls->next_task;
    jl_task_t *t = *pt;
    assert(t != ptls->current_task);
    jl_task_t *lastt = ptls->current_task;
    // none of these locks should be held across a task switch
    assert(ptls->locks.len == 0);

#ifdef JL_TSAN_ENABLED
    if (lastt->ctx.tsan_state != __tsan_get_current_fiber()) {
        // Something went really wrong - don't even assume that we can
        // use assert/abort which involve lots of signal handling that
        // looks at the tsan state.
        write(STDERR_FILENO, tsan_state_corruption, sizeof(tsan_state_corruption) - 1);
        _exit(1);
    }
#endif

    int killed = lastt->_state != JL_TASK_STATE_RUNNABLE;
    if (!t->started && !t->copy_stack) {
        // may need to allocate the stack
        if (t->stkbuf == NULL) {
            t->stkbuf = jl_alloc_fiber(&t->ctx, &t->bufsz, t);
            if (t->stkbuf == NULL) {
#ifdef COPY_STACKS
                // fall back to stack copying if mmap fails
                t->copy_stack = 1;
                t->sticky = 1;
                t->bufsz = 0;
                if (always_copy_stacks)
                    memcpy(&t->copy_stack_ctx, &ptls->copy_stack_ctx, sizeof(t->copy_stack_ctx));
                else
                    memcpy(&t->ctx, &ptls->base_ctx, sizeof(t->ctx));
#else
                jl_throw(jl_memory_exception);
#endif
            }
        }
    }

    if (killed) {
        *pt = NULL; // can't fail after here: clear the gc-root for the target task now
        lastt->gcstack = NULL;
        if (!lastt->copy_stack && lastt->stkbuf) {
            // early free of stkbuf back to the pool
            jl_release_task_stack(ptls, lastt);
        }
    }
    else {
#ifdef COPY_STACKS
        if (lastt->copy_stack) { // save the old copy-stack
            save_stack(ptls, lastt, pt); // allocates (gc-safepoint, and can also fail)
            if (jl_setjmp(lastt->copy_stack_ctx.uc_mcontext, 0)) {
                sanitizer_finish_switch_fiber();
                // TODO: mutex unlock the thread we just switched from
                return;
            }
        }
        else
#endif
        *pt = NULL; // can't fail after here: clear the gc-root for the target task now
        lastt->gcstack = ptls->pgcstack;
    }

    // set up global state for new task
    ptls->pgcstack = t->gcstack;
    ptls->world_age = 0;
    t->gcstack = NULL;
#ifdef MIGRATE_TASKS
    ptls->previous_task = lastt;
#endif
    ptls->current_task = t;

#if defined(JL_TSAN_ENABLED)
    tsan_switch_to_ctx(&t->tsan_state);
    if (killed)
        tsan_destroy_ctx(ptls, &lastt->tsan_state);
#endif
    if (t->started) {
#ifdef COPY_STACKS
        if (t->copy_stack) {
            if (!killed && !lastt->copy_stack)
                restore_stack2(t, ptls, lastt);
            else if (lastt->copy_stack) {
                restore_stack(t, ptls, NULL); // (doesn't return)
            }
            else {
                restore_stack(t, ptls, (char*)1); // (doesn't return)
            }
        }
        else
#endif
        {
            sanitizer_start_switch_fiber(t->stkbuf, t->bufsz);
            if (killed) {
                jl_set_fiber(&t->ctx); // (doesn't return)
                abort(); // unreachable
            }
            else {
                if (lastt->copy_stack) {
                    // Resume at the jl_setjmp earlier in this function,
                    // don't do a full task swap
                    jl_set_fiber(&t->ctx); // (doesn't return)
                }
                else {
                    jl_swap_fiber(&lastt->ctx, &t->ctx);
                }
            }
        }
    }
    else {
        sanitizer_start_switch_fiber(t->stkbuf, t->bufsz);
        if (t->copy_stack && always_copy_stacks) {
#ifdef COPY_STACKS
#if defined(_OS_WINDOWS_)
            jl_setcontext(&t->ctx);
#else
            jl_longjmp(t->copy_stack_ctx.uc_mcontext, 1);
#endif
#endif
            abort(); // unreachable
        }
        else {
            if (killed) {
                jl_start_fiber_set(&t->ctx); // (doesn't return)
                abort();
            }
            else if (lastt->copy_stack) {
                // Resume at the jl_setjmp earlier in this function
                jl_start_fiber_set(&t->ctx); // (doesn't return)
                abort();
            }
            else {
                jl_start_fiber_swap(&lastt->ctx, &t->ctx);
            }
        }
    }
    sanitizer_finish_switch_fiber();
}

static jl_ptls_t NOINLINE refetch_ptls(void)
{
    return jl_get_ptls_states();
}

JL_DLLEXPORT void jl_switch(void)
{
    jl_ptls_t ptls = jl_get_ptls_states();
    jl_task_t *t = ptls->next_task;
    jl_task_t *ct = ptls->current_task;
    if (t == ct) {
        return;
    }
    if (t->_state != JL_TASK_STATE_RUNNABLE || (t->started && t->stkbuf == NULL)) {
        ct->_isexception = t->_isexception;
        ct->result = t->result;
        jl_gc_wb(ct, ct->result);
        return;
    }
    if (ptls->in_finalizer)
        jl_error("task switch not allowed from inside gc finalizer");
    if (ptls->in_pure_callback)
        jl_error("task switch not allowed from inside staged nor pure functions");
    if (t->sticky && jl_atomic_load_acquire(&t->tid) == -1) {
        // manually yielding to a task
        if (jl_atomic_compare_exchange(&t->tid, -1, ptls->tid) != -1)
            jl_error("cannot switch to task running on another thread");
    }
    else if (t->tid != ptls->tid) {
        jl_error("cannot switch to task running on another thread");
    }

    // Store old values on the stack and reset
    sig_atomic_t defer_signal = ptls->defer_signal;
    int8_t gc_state = jl_gc_unsafe_enter(ptls);
    size_t world_age = ptls->world_age;
    int finalizers_inhibited = ptls->finalizers_inhibited;
    ptls->world_age = 0;
    ptls->finalizers_inhibited = 0;

#ifdef ENABLE_TIMINGS
    jl_timing_block_t *blk = ptls->timing_stack;
    if (blk)
        jl_timing_block_stop(blk);
    ptls->timing_stack = NULL;
#endif

    ctx_switch(ptls);

#ifdef MIGRATE_TASKS
    ptls = refetch_ptls();
    t = ptls->previous_task;
    assert(t->tid == ptls->tid);
    if (!t->sticky && !t->copy_stack)
        t->tid = -1;
#elif defined(NDEBUG)
    (void)refetch_ptls();
#else
    assert(ptls == refetch_ptls());
#endif

    // Pop old values back off the stack
    assert(ct == ptls->current_task &&
           0 == ptls->world_age &&
           0 == ptls->finalizers_inhibited);
    ptls->world_age = world_age;
    ptls->finalizers_inhibited = finalizers_inhibited;

#ifdef ENABLE_TIMINGS
    assert(ptls->timing_stack == NULL);
    ptls->timing_stack = blk;
    if (blk)
        jl_timing_block_start(blk);
#else
    (void)ct;
#endif

    jl_gc_unsafe_leave(ptls, gc_state);
    sig_atomic_t other_defer_signal = ptls->defer_signal;
    ptls->defer_signal = defer_signal;
    if (other_defer_signal && !defer_signal)
        jl_sigint_safepoint(ptls);
}

JL_DLLEXPORT void jl_switchto(jl_task_t **pt)
{
    jl_set_next_task(*pt);
    jl_switch();
}

JL_DLLEXPORT JL_NORETURN void jl_no_exc_handler(jl_value_t *e)
{
    // NULL exception objects are used when rethrowing. we don't have a handler to process
    // the exception stack, so at least report the exception at the top of the stack.
    if (!e)
        e = jl_current_exception();

    jl_printf((JL_STREAM*)STDERR_FILENO, "fatal: error thrown and no exception handler available.\n");
    jl_static_show((JL_STREAM*)STDERR_FILENO, e);
    jl_printf((JL_STREAM*)STDERR_FILENO, "\n");
    jlbacktrace(); // written to STDERR_FILENO
    jl_exit(1);
}

// yield to exception handler
static void JL_NORETURN throw_internal(jl_value_t *exception JL_MAYBE_UNROOTED)
{
    jl_ptls_t ptls = jl_get_ptls_states();
    ptls->io_wait = 0;
    // @time needs its compile timer disabled on error,
    // and cannot use a try-finally as it would break scope for assignments
    jl_measure_compile_time[ptls->tid] = 0;
    if (ptls->safe_restore)
        jl_longjmp(*ptls->safe_restore, 1);
    // During startup
    if (!ptls->current_task)
        jl_no_exc_handler(exception);
    JL_GC_PUSH1(&exception);
    jl_gc_unsafe_enter(ptls);
    if (exception) {
        // The temporary ptls->bt_data is rooted by special purpose code in the
        // GC. This exists only for the purpose of preserving bt_data until we
        // set ptls->bt_size=0 below.
        assert(ptls->current_task);
        jl_push_excstack(&ptls->current_task->excstack, exception,
                          ptls->bt_data, ptls->bt_size);
        ptls->bt_size = 0;
    }
    assert(ptls->current_task->excstack && ptls->current_task->excstack->top);
    jl_handler_t *eh = ptls->current_task->eh;
    if (eh != NULL) {
#ifdef ENABLE_TIMINGS
        jl_timing_block_t *cur_block = ptls->timing_stack;
        while (cur_block && eh->timing_stack != cur_block) {
            cur_block = jl_pop_timing_block(cur_block);
        }
        assert(cur_block == eh->timing_stack);
#endif
        jl_longjmp(eh->eh_ctx, 1);
    }
    else {
        jl_no_exc_handler(exception);
    }
    assert(0);
}

// record backtrace and raise an error
JL_DLLEXPORT void jl_throw(jl_value_t *e JL_MAYBE_UNROOTED)
{
    jl_ptls_t ptls = jl_get_ptls_states();
    assert(e != NULL);
    if (ptls->safe_restore)
        throw_internal(NULL);
    record_backtrace(ptls, 1);
    throw_internal(e);
}

// rethrow with current excstack state
JL_DLLEXPORT void jl_rethrow(void)
{
    jl_excstack_t *excstack = jl_get_ptls_states()->current_task->excstack;
    if (!excstack || excstack->top == 0)
        jl_error("rethrow() not allowed outside a catch block");
    throw_internal(NULL);
}

// Special case throw for errors detected inside signal handlers.  This is not
// (cannot be) called directly in the signal handler itself, but is returned to
// after the signal handler exits.
JL_DLLEXPORT void jl_sig_throw(void)
{
    jl_ptls_t ptls = jl_get_ptls_states();
    jl_value_t *e = ptls->sig_exception;
    ptls->sig_exception = NULL;
    throw_internal(e);
}

JL_DLLEXPORT void jl_rethrow_other(jl_value_t *e JL_MAYBE_UNROOTED)
{
    // TODO: Should uses of `rethrow(exc)` be replaced with a normal throw, now
    // that exception stacks allow root cause analysis?
    jl_excstack_t *excstack = jl_get_ptls_states()->current_task->excstack;
    if (!excstack || excstack->top == 0)
        jl_error("rethrow(exc) not allowed outside a catch block");
    // overwrite exception on top of stack. see jl_excstack_exception
    jl_excstack_raw(excstack)[excstack->top-1].jlvalue = e;
    JL_GC_PROMISE_ROOTED(e);
    throw_internal(NULL);
}

JL_DLLEXPORT jl_task_t *jl_new_task(jl_function_t *start, jl_value_t *completion_future, size_t ssize)
{
    jl_ptls_t ptls = jl_get_ptls_states();
    jl_task_t *t = (jl_task_t*)jl_gc_alloc(ptls, sizeof(jl_task_t), jl_task_type);
    t->copy_stack = 0;
    if (ssize == 0) {
        // stack size unspecified; use default
        if (always_copy_stacks) {
            t->copy_stack = 1;
            t->bufsz = 0;
        }
        else {
            t->bufsz = JL_STACK_SIZE;
        }
        t->stkbuf = NULL;
    }
    else {
        // user requested dedicated stack of a certain size
        if (ssize < MINSTKSZ)
            ssize = MINSTKSZ;
        t->bufsz = ssize;
        t->stkbuf = jl_alloc_fiber(&t->ctx, &t->bufsz, t);
        if (t->stkbuf == NULL)
            jl_throw(jl_memory_exception);
    }
    t->next = jl_nothing;
    t->queue = jl_nothing;
    t->tls = jl_nothing;
    t->_state = JL_TASK_STATE_RUNNABLE;
    t->start = start;
    t->result = jl_nothing;
    t->donenotify = completion_future;
    t->_isexception = 0;
    // Inherit logger state from parent task
    t->logstate = ptls->current_task->logstate;
    // there is no active exception handler available on this stack yet
    t->eh = NULL;
    t->sticky = 1;
    t->gcstack = NULL;
    t->excstack = NULL;
    t->started = 0;
    t->prio = -1;
    t->tid = -1;

#if defined(JL_DEBUG_BUILD)
    if (!t->copy_stack)
        memset(&t->ctx, 0, sizeof(t->ctx));
#endif
#ifdef COPY_STACKS
    if (always_copy_stacks)
        memcpy(&t->copy_stack_ctx, &ptls->copy_stack_ctx, sizeof(t->copy_stack_ctx));
    else if (t->copy_stack)
        memcpy(&t->ctx, &ptls->base_ctx, sizeof(t->ctx));
#endif
#ifdef JL_TSAN_ENABLED
    t->tsan_state = __tsan_create_fiber(0);
#endif
    return t;
}

JL_DLLEXPORT jl_value_t *jl_get_current_task(void)
{
    jl_ptls_t ptls = jl_get_ptls_states();
    return (jl_value_t*)ptls->current_task;
}

JL_DLLEXPORT jl_jmp_buf *jl_get_safe_restore(void)
{
    jl_ptls_t ptls = jl_get_ptls_states();
    return ptls->safe_restore;
}

JL_DLLEXPORT void jl_set_safe_restore(jl_jmp_buf *sr)
{
    jl_ptls_t ptls = jl_get_ptls_states();
    ptls->safe_restore = sr;
}

#ifdef JL_HAVE_ASYNCIFY
JL_DLLEXPORT jl_ucontext_t *task_ctx_ptr(jl_task_t *t)
{
    return &t->ctx;
}

JL_DLLEXPORT jl_value_t *jl_get_root_task(void)
{
    jl_ptls_t ptls = jl_get_ptls_states();
    return (jl_value_t*)ptls->root_task;
}

JL_DLLEXPORT void jl_task_wait()
{
    static jl_function_t *wait_func = NULL;
    if (!wait_func) {
        wait_func = (jl_function_t*)jl_get_global(jl_base_module, jl_symbol("wait"));
    }
    size_t last_age = jl_get_ptls_states()->world_age;
    jl_get_ptls_states()->world_age = jl_get_world_counter();
    jl_apply(&wait_func, 1);
    jl_get_ptls_states()->world_age = last_age;
}

JL_DLLEXPORT void jl_schedule_task(jl_task_t *task)
{
    static jl_function_t *sched_func = NULL;
    if (!sched_func) {
        sched_func = (jl_function_t*)jl_get_global(jl_base_module, jl_symbol("schedule"));
    }
    size_t last_age = jl_get_ptls_states()->world_age;
    jl_get_ptls_states()->world_age = jl_get_world_counter();
    jl_value_t *args[] = {(jl_value_t*)sched_func, (jl_value_t*)task};
    jl_apply(args, 2);
    jl_get_ptls_states()->world_age = last_age;
}
#endif

// Do one-time initializations for task system
void jl_init_tasks(void) JL_GC_DISABLED
{
    char *acs = getenv("JULIA_COPY_STACKS");
    if (acs) {
        if (!strcmp(acs, "1") || !strcmp(acs, "yes"))
            always_copy_stacks = 1;
        else if (!strcmp(acs, "0") || !strcmp(acs, "no"))
            always_copy_stacks = 0;
        else {
            jl_safe_printf("invalid JULIA_COPY_STACKS value: %s\n", acs);
            exit(1);
        }
    }
#ifndef COPY_STACKS
    if (always_copy_stacks) {
        jl_safe_printf("Julia built without COPY_STACKS support");
        exit(1);
    }
#endif
}

STATIC_OR_JS void NOINLINE JL_NORETURN start_task(void)
{
#ifdef _OS_WINDOWS_
#if defined(_CPU_X86_64_)
    // install the unhandled exception handler at the top of our stack
    // to call directly into our personality handler
    asm volatile ("\t.seh_handler __julia_personality, @except\n\t.text");
#endif
#else
    // wipe out the call-stack unwind capability beyond this function
    // (we are noreturn, so it is not a total lie)
#if defined(_CPU_X86_64_)
    // per nongnu libunwind: "x86_64 ABI specifies that end of call-chain is marked with a NULL RBP or undefined return address"
    // so we do all 3, to be extra certain of it
    asm volatile ("\t.cfi_undefined rip");
    asm volatile ("\t.cfi_undefined rbp");
    asm volatile ("\t.cfi_return_column rbp");
#else
    // per nongnu libunwind: "DWARF spec says undefined return address location means end of stack"
    // we use whatever happens to be register 1 on this platform for this
    asm volatile ("\t.cfi_undefined 1");
    asm volatile ("\t.cfi_return_column 1");
#endif
#endif

    // this runs the first time we switch to a task
    sanitizer_finish_switch_fiber();
    jl_ptls_t ptls = jl_get_ptls_states();
    jl_task_t *t = ptls->current_task;
    jl_value_t *res;
    assert(ptls->finalizers_inhibited == 0);

#ifdef MIGRATE_TASKS
    jl_task_t *pt = ptls->previous_task;
    if (!pt->sticky && !pt->copy_stack)
        pt->tid = -1;
#endif

    t->started = 1;
    if (t->_isexception) {
        record_backtrace(ptls, 0);
        jl_push_excstack(&t->excstack, t->result,
                         ptls->bt_data, ptls->bt_size);
        res = t->result;
    }
    else {
        JL_TRY {
            if (ptls->defer_signal) {
                ptls->defer_signal = 0;
                jl_sigint_safepoint(ptls);
            }
            JL_TIMING(ROOT);
            ptls->world_age = jl_world_counter;
            res = jl_apply(&t->start, 1);
        }
        JL_CATCH {
            res = jl_current_exception();
            t->_isexception = 1;
            goto skip_pop_exception;
        }
skip_pop_exception:;
    }
    t->result = res;
    jl_gc_wb(t, t->result);
    jl_finish_task(t);
    gc_debug_critical_error();
    abort();
}


#if defined(JL_HAVE_UCONTEXT)
#ifdef _OS_WINDOWS_
#define setcontext jl_setcontext
#define swapcontext jl_swapcontext
#define makecontext jl_makecontext
#endif
static char *jl_alloc_fiber(jl_ucontext_t *t, size_t *ssize, jl_task_t *owner) JL_NOTSAFEPOINT
{
#ifndef _OS_WINDOWS_
    int r = getcontext(t);
    if (r != 0)
        jl_error("getcontext failed");
#endif
    void *stk = jl_malloc_stack(ssize, owner);
    if (stk == NULL)
        return NULL;
    t->uc_stack.ss_sp = stk;
    t->uc_stack.ss_size = *ssize;
#ifdef _OS_WINDOWS_
    makecontext(t, &start_task);
#else
    t->uc_link = NULL;
    makecontext(t, &start_task, 0);
#endif
    return (char*)stk;
}
static void jl_start_fiber_set(jl_ucontext_t *t)
{
    setcontext(t);
}
static void jl_start_fiber_swap(jl_ucontext_t *lastt, jl_ucontext_t *t)
{
    assert(lastt);
    swapcontext(lastt, t);
}
static void jl_swap_fiber(jl_ucontext_t *lastt, jl_ucontext_t *t)
{
    swapcontext(lastt, t);
}
static void jl_set_fiber(jl_ucontext_t *t)
{
    setcontext(t);
}
#endif

#if defined(JL_HAVE_UNW_CONTEXT) || defined(JL_HAVE_ASM)
static char *jl_alloc_fiber(jl_ucontext_t *t, size_t *ssize, jl_task_t *owner)
{
    char *stkbuf = (char*)jl_malloc_stack(ssize, owner);
    if (stkbuf == NULL)
        return NULL;
#ifndef __clang_analyzer__
    ((char**)t)[0] = stkbuf; // stash the stack pointer somewhere for start_fiber
    ((size_t*)t)[1] = *ssize; // stash the stack size somewhere for start_fiber
#endif
    return stkbuf;
}
#endif

#if defined(JL_HAVE_UNW_CONTEXT)
static inline void jl_unw_swapcontext(unw_context_t *old, unw_cursor_t *c)
{
    volatile int returns = 0;
    int r = unw_getcontext(old);
    if (++returns == 2) // r is garbage after the first return
        return;
    if (r != 0 || returns != 1)
        abort();
    unw_resume(c);
}
static void jl_swap_fiber(jl_ucontext_t *lastt, jl_ucontext_t *t)
{
    unw_cursor_t c;
    int r = unw_init_local(&c, t);
    if (r < 0)
        abort();
    jl_unw_swapcontext(lastt, &c);
}
static void jl_set_fiber(unw_context_t *t)
{
    unw_cursor_t c;
    int r = unw_init_local(&c, t);
    if (r < 0)
        abort();
    unw_resume(&c);
}
#elif defined(JL_HAVE_ASM)
static void jl_swap_fiber(jl_ucontext_t *lastt, jl_ucontext_t *t)
{
    if (jl_setjmp(lastt->uc_mcontext, 0))
        return;
    jl_set_fiber(t); // doesn't return
}
static void jl_set_fiber(jl_ucontext_t *t)
{
    jl_longjmp(t->uc_mcontext, 1);
}
#endif

#if defined(JL_HAVE_UNW_CONTEXT) && !defined(JL_HAVE_ASM)
#if defined(_CPU_X86_) || defined(_CPU_X86_64_)
#define PUSH_RET(ctx, stk) \
    do { \
        stk -= sizeof(uintptr_t); \
        *(uintptr_t*)stk = 0; /* push null RIP/EIP onto the stack */ \
    } while (0)
#elif defined(_CPU_ARM_)
#define PUSH_RET(ctx, stk) \
    if (unw_set_reg(ctx, UNW_ARM_R14, 0)) /* put NULL into the LR */ \
        abort();
#else
#error please define how to simulate a CALL on this platform
#endif
static void jl_start_fiber_set(unw_context_t *t)
{
    unw_cursor_t c;
    char *stk = ((char**)t)[0];
    size_t ssize = ((size_t*)t)[1];
    uintptr_t fn = (uintptr_t)&start_task;
    stk += ssize;
    int r = unw_getcontext(t);
    if (r)
        abort();
    if (unw_init_local(&c, t))
        abort();
    PUSH_RET(&c, stk);
#if defined __linux__
#error savannah nongnu libunwind is not capable of setting UNW_REG_SP, as required
#endif
    if (unw_set_reg(&c, UNW_REG_SP, (uintptr_t)stk))
        abort();
    if (unw_set_reg(&c, UNW_REG_IP, fn))
        abort();
    unw_resume(&c); // (doesn't return)
}
static void jl_start_fiber_swap(unw_context_t *lastt, unw_context_t *t)
{
    assert(lastt);
    unw_cursor_t c;
    char *stk = ((char**)t)[0];
    size_t ssize = ((size_t*)t)[1];
    uintptr_t fn = (uintptr_t)&start_task;
    stk += ssize;
    volatile int returns = 0;
    int r = unw_getcontext(lastt);
    if (++returns == 2) // r is garbage after the first return
        return;
    if (r != 0 || returns != 1)
        abort();
    int r = unw_getcontext(t);
    if (r != 0)
        abort();
    if (unw_init_local(&c, t))
        abort();
    PUSH_RET(&c, stk);
    if (unw_set_reg(&c, UNW_REG_SP, (uintptr_t)stk))
        abort();
    if (unw_set_reg(&c, UNW_REG_IP, fn))
        abort();
    jl_unw_swapcontext(lastt, &c);
}
#endif

#if defined(JL_HAVE_ASM)
static void jl_start_fiber_swap(jl_ucontext_t *lastt, jl_ucontext_t *t)
{
    assert(lastt);
#ifdef JL_HAVE_UNW_CONTEXT
    volatile int returns = 0;
    int r = unw_getcontext(lastt);
    if (++returns == 2) // r is garbage after the first return
        return;
    if (r != 0 || returns != 1)
        abort();
#else
    if (jl_setjmp(lastt->uc_mcontext, 0))
        return;
#endif
    jl_start_fiber_set(t); // doesn't return
}
static void jl_start_fiber_set(jl_ucontext_t *t)
{
    char *stk = ((char**)t)[0];
    size_t ssize = ((size_t*)t)[1];
    uintptr_t fn = (uintptr_t)&start_task;
    stk += ssize;
#ifdef _CPU_X86_64_
    asm volatile (
        " movq %0, %%rsp;\n"
        " movq %1, %%rax;\n"
        " xorq %%rbp, %%rbp;\n"
        " push %%rbp;\n" // instead of RSP
        " jmpq *%%rax;\n" // call `fn` with fake stack frame
        " ud2"
        : : "r"(stk), "r"(fn) : "memory" );
#elif defined(_CPU_X86_)
    asm volatile (
        " movl %0, %%esp;\n"
        " movl %1, %%eax;\n"
        " xorl %%ebp, %%ebp;\n"
        " push %%ebp;\n" // instead of ESP
        " jmpl *%%eax;\n" // call `fn` with fake stack frame
        " ud2"
        : : "r"(stk), "r"(fn) : "memory" );
#elif defined(_CPU_AARCH64_)
    asm volatile(
        " mov sp, %0;\n"
        " mov x29, xzr;\n" // Clear link register (x29) and frame pointer
        " mov x30, xzr;\n" // (x30) to terminate unwinder.
        " br %1;\n" // call `fn` with fake stack frame
        " brk #0x1" // abort
        : : "r" (stk), "r"(fn) : "memory" );
#elif defined(_CPU_ARM_)
    // A "i" constraint on `&start_task` works only on clang and not on GCC.
    asm(" mov sp, %0;\n"
        " mov lr, #0;\n" // Clear link register (lr) and frame pointer
        " mov fp, #0;\n" // (fp) to terminate unwinder.
        " bx %1;\n" // call `fn` with fake stack frame.  While `bx` can change
                    // the processor mode to thumb, this will never happen
                    // because all our addresses are word-aligned.
        " udf #0" // abort
        : : "r" (stk), "r"(fn) : "memory" );
#elif defined(_CPU_PPC64_)
    // N.B.: There is two iterations of the PPC64 ABI.
    // v2 is current and used here. Make sure you have the
    // correct version of the ABI reference when working on this code.
    asm volatile(
        // Move stack (-0x30 for initial stack frame) to stack pointer
        " addi 1, %0, -0x30;\n"
        // Build stack frame
        // Skip local variable save area
        " std 2, 0x28(1);\n" // Save TOC
        // Clear link editor/compiler words
        " std 0, 0x20(1);\n"
        " std 0, 0x18(1);\n"
        // Clear LR/CR save area
        " std 0, 0x10(1);\n"
        " std 0, 0x8(1);\n"
        " std 0, 0x0(1); \n" // Clear back link to terminate unwinder
        " mtlr 0; \n"        // Clear link register
        " mr 12, %1; \n"     // Set up target global entry point
        " mtctr 12; \n"      // Move jump target to counter register
        " bctr; \n"          // branch to counter (lr update disabled)
        " trap; \n"
        : : "r"(stk), "r"(fn) : "memory");
#else
#error JL_HAVE_ASM defined but not implemented for this CPU type
#endif
    __builtin_unreachable();
}
#endif

#if defined(JL_HAVE_SIGALTSTACK)
#if defined(JL_TSAN_ENABLED)
#error TSAN support not currently implemented for this tasking model
#endif

static void start_basefiber(int sig)
{
    jl_ptls_t ptls = jl_get_ptls_states();
    if (jl_setjmp(ptls->base_ctx.uc_mcontext, 0))
        start_task(); // sanitizer_finish_switch_fiber is part of start_task
}
static char *jl_alloc_fiber(jl_ucontext_t *t, size_t *ssize, jl_task_t *owner)
{
    stack_t uc_stack, osigstk;
    struct sigaction sa, osa;
    sigset_t set, oset;
    void *stk = jl_malloc_stack(ssize, owner);
    if (stk == NULL)
        return NULL;
    // setup
    jl_ptls_t ptls = jl_get_ptls_states();
    jl_ucontext_t base_ctx;
    memcpy(&base_ctx, &ptls->base_ctx, sizeof(ptls->base_ctx));
    sigfillset(&set);
    if (sigprocmask(SIG_BLOCK, &set, &oset) != 0) {
       jl_free_stack(stk, *ssize);
       jl_error("sigprocmask failed");
    }
    uc_stack.ss_sp = stk;
    uc_stack.ss_size = *ssize;
    uc_stack.ss_flags = 0;
    if (sigaltstack(&uc_stack, &osigstk) != 0) {
       jl_free_stack(stk, *ssize);
       jl_error("sigaltstack failed");
    }
    memset(&sa, 0, sizeof(sa));
    sigemptyset(&sa.sa_mask);
    sa.sa_handler = start_basefiber;
    sa.sa_flags = SA_ONSTACK;
    if (sigaction(SIGUSR2, &sa, &osa) != 0) {
       jl_free_stack(stk, *ssize);
       jl_error("sigaction failed");
    }
    // emit signal
    pthread_kill(pthread_self(), SIGUSR2); // initializes jl_basectx
    sigdelset(&set, SIGUSR2);
    sigsuspend(&set);
    // cleanup
    if (sigaction(SIGUSR2, &osa, NULL) != 0) {
       jl_free_stack(stk, *ssize);
       jl_error("sigaction failed");
    }
    if (osigstk.ss_size < MINSTKSZ && (osigstk.ss_flags | SS_DISABLE))
       osigstk.ss_size = MINSTKSZ;
    if (sigaltstack(&osigstk, NULL) != 0) {
       jl_free_stack(stk, *ssize);
       jl_error("sigaltstack failed");
    }
    if (sigprocmask(SIG_SETMASK, &oset, NULL) != 0) {
       jl_free_stack(stk, *ssize);
       jl_error("sigprocmask failed");
    }
    if (&ptls->base_ctx != t) {
        memcpy(t, &ptls->base_ctx, sizeof(ptls->base_ctx));
        memcpy(&ptls->base_ctx, &base_ctx, sizeof(ptls->base_ctx)); // restore COPY_STACKS context
    }
    return (char*)stk;
}
static void jl_start_fiber_set(jl_ucontext_t *t) {
    jl_longjmp(t->uc_mcontext, 1); // (doesn't return)
}
static void jl_start_fiber_swap(jl_ucontext_t *lastt, jl_ucontext_t *t)
{
    assert(lastt);
    if (lastt && jl_setjmp(lastt->uc_mcontext, 0))
        return;
    jl_start_fiber_set(t);
}
static void jl_swap_fiber(jl_ucontext_t *lastt, jl_ucontext_t *t)
{
    if (jl_setjmp(lastt->uc_mcontext, 0))
        return;
    jl_start_fiber_set(t); // doesn't return
}
static void jl_set_fiber(jl_ucontext_t *t)
{
    jl_longjmp(t->uc_mcontext, 1);
}
#endif

#if defined(JL_HAVE_ASYNCIFY)
#if defined(JL_TSAN_ENABLED)
#error TSAN support not currently implemented for this tasking model
#endif

static char *jl_alloc_fiber(jl_ucontext_t *t, size_t *ssize, jl_task_t *owner) JL_NOTSAFEPOINT
{
    void *stk = jl_malloc_stack(ssize, owner);
    if (stk == NULL)
        return NULL;
    t->stackbottom = stk;
    t->stacktop = ((char*)stk) + *ssize;
    return (char*)stk;
}
// jl_*_fiber implemented in js
#endif

// Initialize a root task using the given stack.
void jl_init_root_task(void *stack_lo, void *stack_hi)
{
    jl_ptls_t ptls = jl_get_ptls_states();
    if (ptls->root_task == NULL) {
        ptls->root_task = (jl_task_t*)jl_gc_alloc(ptls, sizeof(jl_task_t), jl_task_type);
        memset(ptls->root_task, 0, sizeof(jl_task_t));
        ptls->root_task->tls = jl_nothing;
    }
    ptls->current_task = ptls->root_task;
    void *stack = stack_lo;
    size_t ssize = (char*)stack_hi - (char*)stack_lo;
#ifndef _OS_WINDOWS_
    if (ptls->tid == 0) {
        stack = (void*)((char*)stack - ROOT_TASK_STACK_ADJUSTMENT); // offset our guess of the address of the bottom of stack to cover the guard pages too
        ssize += ROOT_TASK_STACK_ADJUSTMENT; // sizeof stack is known exactly, but not where we are in that stack
    }
#endif
    if (always_copy_stacks) {
        ptls->current_task->copy_stack = 1;
        ptls->current_task->stkbuf = NULL;
        ptls->current_task->bufsz = 0;
    }
    else {
        ptls->current_task->copy_stack = 0;
        ptls->current_task->stkbuf = stack;
        ptls->current_task->bufsz = ssize;
    }
    ptls->current_task->started = 1;
    ptls->current_task->next = jl_nothing;
    ptls->current_task->queue = jl_nothing;
    ptls->current_task->_state = JL_TASK_STATE_RUNNABLE;
    ptls->current_task->start = NULL;
    ptls->current_task->result = jl_nothing;
    ptls->current_task->donenotify = jl_nothing;
    ptls->current_task->_isexception = 0;
    ptls->current_task->logstate = jl_nothing;
    ptls->current_task->eh = NULL;
    ptls->current_task->gcstack = NULL;
    ptls->current_task->excstack = NULL;
    ptls->current_task->tid = ptls->tid;
    ptls->current_task->sticky = 1;

#ifdef JL_TSAN_ENABLED
    ptls->current_task->tsan_state = __tsan_get_current_fiber();
#endif

#ifdef COPY_STACKS
    // initialize the base_ctx from which all future copy_stacks will be copies
    if (always_copy_stacks) {
        // when this is set, we will attempt to corrupt the process stack to switch tasks,
        // although this is unreliable, and thus not recommended
        ptls->stackbase = stack_hi;
        ptls->stacksize = ssize;
#ifdef _OS_WINDOWS_
        ptls->copy_stack_ctx.uc_stack.ss_sp = stack_hi;
        ptls->copy_stack_ctx.uc_stack.ss_size = ssize;
#endif
        if (jl_setjmp(ptls->copy_stack_ctx.uc_mcontext, 0))
            start_task(); // sanitizer_finish_switch_fiber is part of start_task
        return;
    }
    ssize = JL_STACK_SIZE;
    char *stkbuf = jl_alloc_fiber(&ptls->base_ctx, &ssize, NULL);
    ptls->stackbase = stkbuf + ssize;
    ptls->stacksize = ssize;
#endif
}

JL_DLLEXPORT int jl_is_task_started(jl_task_t *t) JL_NOTSAFEPOINT
{
    return t->started;
}

JL_DLLEXPORT int16_t jl_get_task_tid(jl_task_t *t) JL_NOTSAFEPOINT
{
    return t->tid;
}


#ifdef _OS_WINDOWS_
#if defined(_CPU_X86_)
extern DWORD32 __readgsdword(int);
extern DWORD32 __readgs(void);
#endif
JL_DLLEXPORT void jl_gdb_dump_threadinfo(void)
{
#if defined(_CPU_X86_64_)
    DWORD64 gs0 = __readgsqword(0x0);
    DWORD64 gs8 = __readgsqword(0x8);
    DWORD64 gs16 = __readgsqword(0x10);
    jl_safe_printf("ThreadId: %u, Stack: %p -- %p to %p, SEH: %p\n",
                   (unsigned)GetCurrentThreadId(),
                   jl_get_frame_addr(),
                   (void*)gs8, (void*)gs16, (void*)gs0);
#elif defined(_CPU_X86_)
    DWORD32 fs0 = __readfsdword(0x0);
    DWORD32 fs4 = __readfsdword(0x4);
    DWORD32 fs8 = __readfsdword(0x8);
    jl_safe_printf("ThreadId: %u, Stack: %p -- %p to %p, SEH: %p\n",
                   (unsigned)GetCurrentThreadId(),
                   jl_get_frame_addr(),
                   (void*)fs4, (void*)fs8, (void*)fs0);
    if (__readgs()) { // WoW64 if GS is non-zero
        DWORD32 gs0 = __readgsdword(0x0);
        DWORD32 gs4 = __readgsdword(0x4);
        DWORD32 gs8 = __readgsdword(0x8);
        DWORD32 gs12 = __readgsdword(0xc);
        DWORD32 gs16 = __readgsdword(0x10);
        DWORD32 gs20 = __readgsdword(0x14);
        jl_safe_printf("Stack64: %p%p to %p%p, SEH64: %p%p\n",
                       (void*)gs12, (void*)gs8,
                       (void*)gs20, (void*)gs16,
                       (void*)gs4, (void*)gs0);
    }
#else
    jl_safe_printf("ThreadId: %u, Stack: %p\n",
                   (unsigned)GetCurrentThreadId(),
                   jl_get_frame_addr());
#endif
}
#endif


#ifdef __cplusplus
}
#endif
