/******************************************************************************
** Copyright (c) 2014 Intel Corporation All Rights Reserved
**
** Licensed under the Apache License, Version 2.0 (the "License"); you may not
** use this file except in compliance with the License.
**
** You may obtain a copy of the License at
**     http://www.apache.org/licenses/LICENSE-2.0
**
** Unless required by applicable law or agreed to in writing, software
** distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
** WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
**
** See the License for the specific language governing permissions and
** limitations under the License.
**
******************************************************************************/

#ifndef _ESIF_CCB_TIMER_H_
#define _ESIF_CCB_TIMER_H_

#include "esif.h"
#include "esif_rc.h"
#include "esif_ccb.h"
#include "esif_ccb_time.h"
#include "esif_ccb_memory.h"
#include "esif_ccb_lock.h"
#include "esif_uf_ccb_thread.h"

#ifdef ESIF_ATTR_OS_WINDOWS
#ifdef ESIF_FEAT_OPT_USE_COALESCABLE_TIMERS
/*
 * Coalescable timer options
 */
#define TOLERABLE_DELAY_DIVISOR 10    /* timeout divisor */
#define TOLERABLE_DELAY_INCREMENT 50  /* ms */
#define DUE_TIME_MS_CONV_FACTOR 10000

#endif
#endif

/******************************************************************************
*   KERNEL TIMER
******************************************************************************/
#ifdef ESIF_ATTR_KERNEL

/* Timer Callback Function */
typedef void (*esif_ccb_timer_cb)(void *context);

/*
 * Timer Is Defined to be opaue here to  acoomondate different
 * OS and implementation approaches for timers
 */

#ifdef ESIF_ATTR_OS_LINUX
typedef struct {
    struct delayed_work work;
    esif_ccb_timer_cb function_ptr;
    esif_ccb_low_priority_thread_lock_t context_lock;
    u32 exit_flag;
    u32  timer_period_msec;
    u32 periodic_flag;
    void* context_ptr;
} esif_ccb_timer_t;
#endif /* ESIF_ATTR_OS_LINUX */

#ifdef ESIF_ATTR_OS_WINDOWS

typedef struct {
    esif_ccb_timer_cb function_ptr;
    esif_ccb_low_priority_thread_lock_t context_lock;
    u32 exit_flag;
    esif_ccb_time_t  timer_period_msec;
    u32 periodic_flag;
    void* context_ptr;
} esif_ccb_timer_context_t;


#ifdef ESIF_FEAT_OPT_USE_COALESCABLE_TIMERS

typedef struct esif_ccb_timer {
    KTIMER  timer;
    KDPC dpc;
    esif_ccb_timer_context_t timer_context;
} esif_ccb_timer_t;

typedef struct esif_ccb_work_item_context {
    void* ptr;
} esif_ccb_work_item_context_t;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(esif_ccb_work_item_context_t,
                   esif_ccb_get_work_item_context)
#else
typedef WDFTIMER esif_ccb_timer_t;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(esif_ccb_timer_context_t,
                   esif_ccb_get_timer_context)
#endif


#endif /* ESIF_ATTR_OS_WINDOWS */

static enum esif_rc esif_ccb_timer_set_msec(
    esif_ccb_timer_t *timer_ptr,
    esif_ccb_time_t timeout,
    esif_flags_t periodic,
    esif_ccb_timer_cb function_ptr,
    void* context_ptr
);


#ifdef ESIF_ATTR_OS_LINUX
/* Timer Callback Wrapper  Find And Fire Function */
static ESIF_INLINE void esif_ccb_timer_cb_wrapper(struct work_struct* work)
{
    esif_ccb_timer_t* timer_ptr = container_of(
            (struct delayed_work *)work,
            esif_ccb_timer_t,
            work);

    TIMER_DEBUG("%s: timer fired!!!!!\n", ESIF_FUNC);

    if (NULL == timer_ptr)
        goto exit;

    esif_ccb_low_priority_thread_read_lock(&timer_ptr->context_lock);

    if (!timer_ptr->exit_flag) {
        timer_ptr->function_ptr(timer_ptr->context_ptr);

        /* RESET The Timer */
        if (timer_ptr->periodic_flag) { 
            esif_ccb_timer_set_msec(timer_ptr,
                        timer_ptr->timer_period_msec,
                        ESIF_TRUE,
                        timer_ptr->function_ptr,
                        timer_ptr->context_ptr);
        }
    }
    esif_ccb_low_priority_thread_read_unlock(&timer_ptr->context_lock);
exit:
    ;
}


#endif /* ESIF_ATTR_OS_LINUX */

#ifdef ESIF_ATTR_OS_WINDOWS
/* Timer Callback Wrapper Find And Fire Function */

#ifdef ESIF_FEAT_OPT_USE_COALESCABLE_TIMERS

static KDEFERRED_ROUTINE esif_ccb_timer_dpc;
static EVT_WDF_WORKITEM esif_ccb_timer_cb_wrapper;

static void esif_ccb_timer_cb_wrapper(
    WDFWORKITEM work_item
    )
{
    esif_ccb_work_item_context_t *work_item_context_ptr = NULL;
    esif_ccb_timer_t *timer_ptr = NULL;
    esif_ccb_timer_context_t *timer_context_ptr = NULL;

    TIMER_DEBUG("%s: timer fired!!!!!\n", ESIF_FUNC);

    work_item_context_ptr = esif_ccb_get_work_item_context(work_item);
    if(NULL == work_item_context_ptr) {
        goto exit;
    }

    timer_ptr = (esif_ccb_timer_t *)work_item_context_ptr->ptr;
    if(NULL == timer_ptr) {
        goto exit;
    }

    timer_context_ptr = &timer_ptr->timer_context;
    if (NULL == timer_context_ptr)
        goto exit;

    esif_ccb_low_priority_thread_read_lock(
        &timer_context_ptr->context_lock);

    if (!timer_context_ptr->exit_flag) {
        timer_context_ptr->function_ptr(timer_context_ptr->context_ptr);

        /* RESET The Timer */
        if (timer_context_ptr->periodic_flag) { 
            esif_ccb_timer_set_msec(
                    timer_ptr,
                    timer_context_ptr->timer_period_msec,
                    ESIF_TRUE,
                    timer_context_ptr->function_ptr,
                    timer_context_ptr->context_ptr);
        }
    }
    esif_ccb_low_priority_thread_read_unlock(
        &timer_context_ptr->context_lock);
exit:
    WdfObjectDelete(work_item);
}

static void esif_ccb_timer_dpc (
    struct _KDPC* Dpc,
    PVOID DeferredContext,
    PVOID SystemArgument1,
    PVOID SystemArgument2
    )
{
    NTSTATUS status = STATUS_SUCCESS;
    esif_ccb_work_item_context_t *work_item_context_ptr = NULL;
    WDF_WORKITEM_CONFIG workItemConfig    = {0};
    WDF_OBJECT_ATTRIBUTES workItemAttribs = {0};
    WDFWORKITEM workItem = NULL;

    UNREFERENCED_PARAMETER(Dpc);
    UNREFERENCED_PARAMETER(SystemArgument1);
    UNREFERENCED_PARAMETER(SystemArgument2);

    WDF_WORKITEM_CONFIG_INIT(&workItemConfig, esif_ccb_timer_cb_wrapper);
    workItemConfig.AutomaticSerialization = TRUE;

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&workItemAttribs, 
                            esif_ccb_work_item_context_t);
    workItemAttribs.ParentObject = g_wdf_ipc_queue_handle;

    status = WdfWorkItemCreate(&workItemConfig, &workItemAttribs, &workItem);
    if (NT_SUCCESS(status)) {
        work_item_context_ptr = esif_ccb_get_work_item_context(workItem);
        work_item_context_ptr->ptr = DeferredContext;
        WdfWorkItemEnqueue(workItem);
    }
}



#else /* NOT ESIF_FEAT_OPT_USE_COALESCABLE_TIMERS */

EVT_WDF_TIMER esif_ccb_timer_cb_wrapper;

ESIF_INLINE void esif_ccb_timer_cb_wrapper(WDFTIMER timer)
{
    esif_ccb_timer_context_t *timer_context_ptr =
        esif_ccb_get_timer_context(timer);

    TIMER_DEBUG("%s: timer fired!!!!!\n", ESIF_FUNC);
    if (NULL == timer_context_ptr)
        goto exit;

    esif_ccb_low_priority_thread_read_lock(
        &timer_context_ptr->context_lock);
        
    if (!timer_context_ptr->exit_flag) {
        timer_context_ptr->function_ptr(timer_context_ptr->context_ptr);

        /* RESET The Timer */
        if (timer_context_ptr->periodic_flag) { 
            esif_ccb_timer_set_msec(&timer,
                timer_context_ptr->timer_period_msec,
                ESIF_TRUE,
                timer_context_ptr->function_ptr,
                timer_context_ptr->context_ptr);
        }
    }
    esif_ccb_low_priority_thread_read_unlock(
        &timer_context_ptr->context_lock);
exit:
    (0);
}
#endif /* NOT ESIF_FEAT_OPT_USE_COALESCABLE_TIMERS */
#endif /* ESIF_ATTR_OS_WINDOWS */


/* Timer Initialize */
static ESIF_INLINE
enum esif_rc esif_ccb_timer_init(esif_ccb_timer_t *timer_ptr)
{
    enum esif_rc rc = ESIF_E_UNSPECIFIED;
#ifdef ESIF_ATTR_OS_LINUX
    TIMER_DEBUG("%s: timer %p\n", ESIF_FUNC, timer_ptr);
    INIT_DELAYED_WORK(&timer_ptr->work, NULL);
    esif_ccb_low_priority_thread_lock_init(&timer_ptr->context_lock);
    timer_ptr->exit_flag = FALSE;
    rc = ESIF_OK;
#endif

#ifdef ESIF_ATTR_OS_WINDOWS
#ifdef ESIF_FEAT_OPT_USE_COALESCABLE_TIMERS

    esif_ccb_memset(timer_ptr, 0, sizeof(*timer_ptr));

    KeInitializeDpc(&timer_ptr->dpc, esif_ccb_timer_dpc, timer_ptr);
    KeInitializeTimer(&timer_ptr->timer);

    esif_ccb_low_priority_thread_lock_init(
        &timer_ptr->timer_context.context_lock);

    timer_ptr->timer_context.exit_flag = FALSE;

    TIMER_DEBUG("%s: timer %p\n", ESIF_FUNC, timer_ptr);
    rc = ESIF_OK;

#else /* NOT ESIF_FEAT_OPT_USE_COALESCABLE_TIMERS */
    NTSTATUS status;

    WDF_TIMER_CONFIG timer_config = {0};
    WDF_OBJECT_ATTRIBUTES timer_attributes      = {0};
    esif_ccb_timer_context_t *timer_context_ptr = NULL;

    TIMER_DEBUG("%s: timer %p\n", ESIF_FUNC, timer_ptr);

    WDF_TIMER_CONFIG_INIT(&timer_config, esif_ccb_timer_cb_wrapper);
    timer_config.AutomaticSerialization = TRUE;

    WDF_OBJECT_ATTRIBUTES_INIT(&timer_attributes);
    timer_attributes.ParentObject   = g_wdf_ipc_queue_handle;
    timer_attributes.ExecutionLevel = WdfExecutionLevelPassive;

    WDF_OBJECT_ATTRIBUTES_SET_CONTEXT_TYPE(&timer_attributes,
                           esif_ccb_timer_context_t);
    status = WdfTimerCreate(&timer_config, &timer_attributes, timer_ptr);
    TIMER_DEBUG("%s: timer %p status %08x\n", ESIF_FUNC, timer_ptr, status);
    if (!NT_SUCCESS(status))
        goto exit;

    timer_context_ptr = esif_ccb_get_timer_context(*timer_ptr);
    if (timer_context_ptr == NULL)
        goto exit;

    esif_ccb_low_priority_thread_lock_init(&timer_context_ptr->context_lock);
    timer_context_ptr->exit_flag = FALSE;
    rc = ESIF_OK;
exit:
#endif /* NOT ESIF_FEAT_OPT_USE_COALESCABLE_TIMERS */
#endif /* ESIF_ATTR_OS_WINDOWS */
    return rc;
}


/* Timer Set */
static ESIF_INLINE enum esif_rc esif_ccb_timer_set_msec(
    esif_ccb_timer_t *timer_ptr,
    esif_ccb_time_t timeout,
    esif_flags_t periodic,
    esif_ccb_timer_cb function_ptr,
    void* context_ptr
    )
{
    enum esif_rc rc = ESIF_E_UNSPECIFIED;
#ifdef ESIF_ATTR_OS_LINUX
    u64 x;
    u64 y;
    u8 result;

    timer_ptr->periodic_flag = periodic;
    timer_ptr->timer_period_msec = timeout;
    timer_ptr->function_ptr = function_ptr;
    timer_ptr->context_ptr  = context_ptr;

    TIMER_DEBUG("%s: timer %p timeout %u\n",
            ESIF_FUNC,
            timer_ptr,
            (int)timeout);

    PREPARE_DELAYED_WORK(&timer_ptr->work, esif_ccb_timer_cb_wrapper);

    /* Keep 32 bit Linux Happy */
    x = timeout * HZ;
    y = 1000;

    do_div(x, y);

    result = schedule_delayed_work(&timer_ptr->work, x);
    if (ESIF_TRUE == result)
        rc = ESIF_OK;
#endif

#ifdef ESIF_ATTR_OS_WINDOWS
#ifdef ESIF_FEAT_OPT_USE_COALESCABLE_TIMERS
    esif_ccb_timer_context_t *timer_context_ptr = NULL;
    LARGE_INTEGER due_time = {0LL};
    u32 tolerable_delay = 0;

    TIMER_DEBUG("%s: timer %p timeout %u\n", ESIF_FUNC, timer_ptr, timeout);

    if(NULL == timer_ptr) {
        rc = ESIF_E_PARAMETER_IS_NULL;
        goto exit;
    }

    timer_context_ptr = &timer_ptr->timer_context;
    timer_context_ptr->function_ptr = function_ptr;
    timer_context_ptr->context_ptr  = context_ptr;
    timer_context_ptr->periodic_flag = periodic;
    timer_context_ptr->timer_period_msec = timeout;
    timer_context_ptr->exit_flag    = FALSE;

    /*
     * Calculate the delay that can be tolerated, with a minimum of at
     * least 1 TOLERABLE_DELAY_INCREMENT. 
     */
    tolerable_delay = (u32) timeout / TOLERABLE_DELAY_DIVISOR;
    tolerable_delay = (tolerable_delay / TOLERABLE_DELAY_INCREMENT) + 1;
    tolerable_delay *= TOLERABLE_DELAY_INCREMENT;

    due_time.QuadPart = (LONGLONG)timeout * -10000;
    KeSetCoalescableTimer(&timer_ptr->timer,
                  due_time,
                  0,
                  tolerable_delay,
                  &timer_ptr->dpc);

    rc = ESIF_OK;
exit:

#else /* NOT ESIF_FEAT_OPT_USE_COALESCABLE_TIMERS */

    esif_ccb_timer_context_t *timer_context_ptr = NULL;
    BOOLEAN status;

    TIMER_DEBUG("%s: timer %p timeout %u\n", ESIF_FUNC, timer_ptr, timeout);

    if (NULL == timer_ptr) {
        rc = ESIF_E_PARAMETER_IS_NULL;
        goto exit;
    }

    /* Set Context */
    timer_context_ptr = esif_ccb_get_timer_context(*timer_ptr);
    if (NULL != timer_context_ptr) {
        timer_context_ptr->timer_period_msec = timeout;
        timer_context_ptr->periodic_flag = periodic;
        timer_context_ptr->function_ptr = function_ptr;
        timer_context_ptr->context_ptr  = context_ptr;
        timer_context_ptr->exit_flag    = FALSE;
    }

    /* Finally Start Timer */
    status = WdfTimerStart(*timer_ptr, (LONG)timeout * -10000);
    TIMER_DEBUG("%s: timer %p status %08x\n", ESIF_FUNC, timer_ptr, status);
    if (TRUE == status)
        rc = ESIF_OK;
exit:

#endif /* NOT ESIF_FEAT_OPT_USE_COALESCABLE_TIMERS */
#endif /* ESIF_ATTR_OS_WINDOWS */
    return rc;
}


/* Timer Stop And Destory */
static ESIF_INLINE
enum esif_rc esif_ccb_timer_kill(esif_ccb_timer_t *timer)
{
    enum esif_rc rc = ESIF_E_UNSPECIFIED;
#ifdef ESIF_ATTR_OS_LINUX
    TIMER_DEBUG("%s: timer %p\n", ESIF_FUNC, timer);

    esif_ccb_low_priority_thread_write_lock(&timer->context_lock);
    timer->exit_flag = TRUE;
    esif_ccb_low_priority_thread_write_unlock(&timer->context_lock);

    if (ESIF_TRUE == cancel_delayed_work(&timer->work))
        rc = ESIF_OK;
#endif

#ifdef ESIF_ATTR_OS_WINDOWS
#ifdef ESIF_FEAT_OPT_USE_COALESCABLE_TIMERS
    esif_ccb_timer_context_t *timer_context_ptr = NULL;

    TIMER_DEBUG("%s: timer %p\n", ESIF_FUNC, timer);

    if(NULL == timer) {
        goto exit;
    }

    timer_context_ptr = &timer->timer_context;

    if (timer_context_ptr != NULL) {
        esif_ccb_low_priority_thread_write_lock(
            &timer_context_ptr->context_lock);
        timer_context_ptr->exit_flag = TRUE;
        esif_ccb_low_priority_thread_write_unlock(
            &timer_context_ptr->context_lock);
    }

    KeCancelTimer(&timer->timer);
exit:
    rc = ESIF_OK;

#else /* NOT ESIF_FEAT_OPT_USE_COALESCABLE_TIMERS */
    esif_ccb_timer_context_t *timer_context_ptr = NULL;

    TIMER_DEBUG("%s: timer %p\n", ESIF_FUNC, timer);

    timer_context_ptr = esif_ccb_get_timer_context(*timer);
    if (timer_context_ptr != NULL) {
        esif_ccb_low_priority_thread_write_lock(
            &timer_context_ptr->context_lock);
        timer_context_ptr->exit_flag = TRUE;
        esif_ccb_low_priority_thread_write_unlock(
            &timer_context_ptr->context_lock);
    }

    /* WDF Framework Will Cleanup  */
    WdfTimerStop(*timer, FALSE);
    rc = ESIF_OK;

#endif /* NOT ESIF_FEAT_OPT_USE_COALESCABLE_TIMERS */
#endif /* ESIF_ATTR_OS_WINDOWS */
    return rc;
}


#endif /* ESIF_ATTR_KERNEL*/

/******************************************************************************
*   USER TIMER
******************************************************************************/

#ifdef ESIF_ATTR_USER

#include "esif.h"

/* OS Agnostic Callback Function */
typedef void (ESIF_CALLCONV *esif_ccb_timer_cb)
(
    const void* context_ptr
);

#pragma pack(push, 1)

/* OS Agnostic Timer Context */
typedef struct esif_ccb_timer_ctx {
    esif_ccb_timer_cb  cb_func;  /* Call Back Function */
    void* cb_context_ptr;        /* Call Back Function Context */
} esif_ccb_timer_ctx_t;

#pragma pack(pop)

#ifdef ESIF_ATTR_OS_LINUX

#include <signal.h>

#pragma pack(push,1)

/* Linux Timer */
typedef struct esif_ccb_timer {
    timer_t  timer;                       /* Linux specific timer */
    esif_ccb_timer_ctx_t* timer_ctx_ptr;  /* OS Agnostic timer context */
    esif_ccb_mutex_t context_lock;        /* Call back function lock */
} esif_ccb_timer_t;

#pragma pack(pop)

/*
 *  Each OS Expects its own CALLBACK primitive we use this
 *  wrapper function to normalize the parameters and and
 *  ultimately call our func_ptr and func_context originally
 *  provided to the timer.  By example Linux expects a union
 *  here that contains our timer context poiner which in turn
 *  contains our function and context for the function.
 */

 /* Linux Timer Behavior Note
 *  Reference : http://man7.org/linux/man-pages/man2/timer_create.2.html
 *
 *  Because of the way POSIX timer callbacks are handled, it is possible
 *  that the OS may have a pending timer notification for a timer that
 *  was cancelled, reinitialized, and reset from a user space app.  In
 *  that instance, if you are using the same timer repeatedly you may see
 *  2 callbacks for a single timer due to this pending signal issue.
 */

static ESIF_INLINE void esif_ccb_timer_cb_wrapper(
    const union sigval sv
    )
{
    esif_ccb_timer_t* timer_ptr = (esif_ccb_timer_t *)sv.sival_ptr;

    ESIF_ASSERT(timer_ptr->timer_ctx_ptr != NULL);
    ESIF_ASSERT(timer_ptr->timer_ctx_ptr->cb_func != NULL);
    ESIF_ASSERT(timer_ptr->timer_ctx_ptr->cb_context_ptr != NULL);

    esif_ccb_mutex_lock(&(timer_ptr->context_lock));

    if (NULL != timer_ptr->timer_ctx_ptr &&
        NULL != timer_ptr->timer_ctx_ptr->cb_func &&
        NULL != timer_ptr->timer_ctx_ptr->cb_context_ptr) {

        timer_ptr->timer_ctx_ptr->cb_func(
            timer_ptr->timer_ctx_ptr->cb_context_ptr);

    }

    esif_ccb_mutex_unlock(&(timer_ptr->context_lock));
}


#endif /* ESIF_ATTR_OS_LINUX */

#ifdef ESIF_ATTR_OS_WINDOWS

#include <WinBase.h>

#pragma pack(push,1)

/* Windows Timer */
typedef struct esif_ccb_timer {
    HANDLE timer;               /* Windows specific timer */
    HANDLE timer_wait_handle;
    esif_ccb_timer_ctx_t *timer_ctx_ptr;    /* OS Agnostic timer context */
} esif_ccb_timer_t;

#pragma pack(pop)

/*
 *  Each OS Expects its own CALLBACK primitive we use this
 *  wrapper function to normalize the parameters and and
 *  ultimately call our func_ptr and func_context originally
 *  provided to the timer.  By example Windows expects two
 *  parameters of which we only use one.  The other one is
 *  always true.
 */
static ESIF_INLINE void NTAPI esif_ccb_timer_cb_wrapper(
    void *context_ptr,
    BOOLEAN notUsed
    )
{
    esif_ccb_timer_ctx_t *timer_ctx_ptr =
        (esif_ccb_timer_ctx_t *)context_ptr;

    ESIF_ASSERT(timer_ctx_ptr != NULL);
    ESIF_ASSERT(timer_ctx_ptr->cb_func != NULL);
    ESIF_ASSERT(timer_ctx_ptr->cb_context_ptr != NULL);

    UNREFERENCED_PARAMETER(notUsed);

    if (NULL != timer_ctx_ptr &&
        NULL != timer_ctx_ptr->cb_func &&
        NULL != timer_ctx_ptr->cb_context_ptr) {
        timer_ctx_ptr->cb_func(timer_ctx_ptr->cb_context_ptr);
    }
}


#endif /* ESIF_ATTR_OS_WINDOWS */


static ESIF_INLINE eEsifError esif_ccb_timer_init(
    esif_ccb_timer_t* timer_ptr,        /* Our Timer */
    const esif_ccb_timer_cb function_ptr,   /* Callback when timer fires */
    void* context_ptr           /* Callback context if any */
    )       
{
    eEsifError rc = ESIF_E_UNSPECIFIED;
#ifdef ESIF_ATTR_OS_LINUX
    struct sigevent se;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
#endif

    ESIF_ASSERT(timer_ptr != NULL);
    ESIF_ASSERT(function_ptr != NULL);

    if (NULL == timer_ptr) {
        rc = ESIF_E_PARAMETER_IS_NULL;
        goto exit;
    }

    esif_ccb_memset(timer_ptr, 0, sizeof(*timer_ptr));

#ifdef ESIF_ATTR_OS_WINDOWS
    timer_ptr->timer = CreateWaitableTimer(NULL, TRUE, NULL);
    if (NULL == timer_ptr->timer)
        goto exit;

    rc = ESIF_OK;
#endif

    /* Allocate and setup new context */
    timer_ptr->timer_ctx_ptr = (esif_ccb_timer_ctx_t *)
        esif_ccb_malloc(sizeof(*timer_ptr->timer_ctx_ptr));

    if (NULL == timer_ptr->timer_ctx_ptr) {
        rc = ESIF_E_NO_MEMORY;
        goto exit;
    }

    /* Store state for timer set */
    timer_ptr->timer_ctx_ptr->cb_func        = function_ptr;
    timer_ptr->timer_ctx_ptr->cb_context_ptr = context_ptr;

#ifdef ESIF_ATTR_OS_LINUX
    /* Initialize the callback lock and exit flag */
    esif_ccb_mutex_init(&(timer_ptr->context_lock));

    se.sigev_notify = SIGEV_THREAD;
    se.sigev_notify_function   = esif_ccb_timer_cb_wrapper;
    se.sigev_value.sival_ptr   = timer_ptr;
    se.sigev_notify_attributes = &attr;

    if (0 == timer_create(CLOCK_REALTIME, &se,
                (timer_t *)&timer_ptr->timer)) {
        rc = ESIF_OK;
    } else {
       esif_ccb_free(timer_ptr->timer_ctx_ptr );
       timer_ptr->timer_ctx_ptr  = NULL;
    }
#endif
exit:
    return rc;
}


static ESIF_INLINE eEsifError esif_ccb_timer_set_msec(
    esif_ccb_timer_t* timer_ptr,    /* Our Timer */
    const esif_ccb_time_t timeout   /* Timeout in msec */
    )   
{
    eEsifError rc = ESIF_E_UNSPECIFIED;

#ifdef ESIF_ATTR_OS_LINUX
    struct itimerspec its;
    u64 freq_nanosecs = timeout * 1000 * 1000; /* convert msec to nsec */
#endif
#ifdef ESIF_ATTR_OS_WINDOWS
    u32 ret_val = 0;
    u32 wait_timeout = (u32) timeout;
#ifdef ESIF_FEAT_OPT_USE_COALESCABLE_TIMERS
    u32 tolerable_delay = 0;
    LARGE_INTEGER due_time = {0};
#endif
#endif /* ESIF_ATTR_OS_WINDOWS */

    ESIF_ASSERT(timer_ptr != NULL);

    if (NULL == timer_ptr) {
        rc = ESIF_E_NO_MEMORY;
        goto exit;
    }

#ifdef ESIF_ATTR_OS_WINDOWS

    if (NULL == timer_ptr->timer)
        goto exit;

    if (NULL != timer_ptr->timer_wait_handle) {
        UnregisterWaitEx(timer_ptr->timer_wait_handle, 
            INVALID_HANDLE_VALUE);
        timer_ptr->timer_wait_handle = NULL;
    }

#ifdef ESIF_FEAT_OPT_USE_COALESCABLE_TIMERS
    /*
     * Calculate the delay that can be tolerated, with a minimum of at
     * least 1 TOLERABLE_DELAY_INCREMENT. 
     */
    tolerable_delay = (u32) timeout / TOLERABLE_DELAY_DIVISOR;
    tolerable_delay = (tolerable_delay / TOLERABLE_DELAY_INCREMENT) + 1;
    tolerable_delay *= TOLERABLE_DELAY_INCREMENT;

    /*
     * For the coalescable timer, we use the timeout of the timer; not
     * the timeout of the waiting thread.
     */
    wait_timeout = INFINITE;

    due_time.QuadPart = -1LL * timeout * DUE_TIME_MS_CONV_FACTOR;

    ret_val = SetWaitableTimerEx(timer_ptr->timer,
                    &due_time,
                    0,
                    NULL,
                    NULL,
                    NULL,
                    tolerable_delay);
    if (!ret_val)
        goto exit;

#endif /* ESIF_FEAT_OPT_USE_COALESCABLE_TIMERS */

    ret_val = RegisterWaitForSingleObject(&timer_ptr->timer_wait_handle,
            timer_ptr->timer,
            esif_ccb_timer_cb_wrapper,
            timer_ptr->timer_ctx_ptr,
            (u32)wait_timeout,
            WT_EXECUTEONLYONCE);
    if (!ret_val) {
#ifdef ESIF_FEAT_OPT_USE_COALESCABLE_TIMERS
        CancelWaitableTimer(timer_ptr->timer);
#endif
        goto exit;
    }

    rc = ESIF_OK;
#endif /* ESIF_ATTR_OS_WINDOWS */

#ifdef ESIF_ATTR_OS_LINUX
    its.it_value.tv_sec     = freq_nanosecs / 1000000000;
    its.it_value.tv_nsec    = freq_nanosecs % 1000000000;
    its.it_interval.tv_sec  = 0;
    its.it_interval.tv_nsec = 0;

    if (0 == timer_settime(timer_ptr->timer, 0, &its, NULL))
        rc = ESIF_OK;

#endif /* ESIF_ATTR_OS_LINUX */
exit:
    return rc;
}


static ESIF_INLINE eEsifError esif_ccb_timer_kill(
    esif_ccb_timer_t *timer_ptr /* Our Timer */
    )
{
    eEsifError rc = ESIF_E_UNSPECIFIED;

    if (NULL == timer_ptr)
        goto exit;

#ifdef ESIF_ATTR_OS_WINDOWS

    if (NULL != timer_ptr->timer_wait_handle) {
        UnregisterWaitEx(timer_ptr->timer_wait_handle,
            INVALID_HANDLE_VALUE );
        timer_ptr->timer_wait_handle = NULL;
    }

    if (timer_ptr->timer != NULL) {
        CloseHandle(timer_ptr->timer);
        timer_ptr->timer = NULL;
    }

    rc = ESIF_OK;

#endif

#ifdef ESIF_ATTR_OS_LINUX
    esif_ccb_mutex_lock(&(timer_ptr->context_lock));

    if (0 == timer_delete(timer_ptr->timer))
        rc = ESIF_OK;

#endif
    if (timer_ptr->timer_ctx_ptr != NULL) {
        esif_ccb_free(timer_ptr->timer_ctx_ptr);
        timer_ptr->timer_ctx_ptr = NULL;
    }

#ifdef ESIF_ATTR_OS_LINUX
    esif_ccb_mutex_unlock(&(timer_ptr->context_lock));
#endif

exit:
    return rc;
}


#endif /* ESIF_ATTR_USER */
#endif /* _ESIF_CCB_TIMER_H_ */