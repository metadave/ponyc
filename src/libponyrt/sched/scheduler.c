#define PONY_WANT_ATOMIC_DEFS

#include "scheduler.h"
#include "cpu.h"
#include "mpmcq.h"
#include "../actor/actor.h"
#include "../gc/cycle.h"
#include "../asio/asio.h"
#include "../mem/pool.h"
#include "ponyassert.h"
#include <dtrace.h>
#include <string.h>
#include "mutemap.h"

#define PONY_SCHED_BATCH 100

static DECLARE_THREAD_FN(run_thread);

typedef enum
{
  SCHED_BLOCK,
  SCHED_UNBLOCK,
  SCHED_CNF,
  SCHED_ACK,
  SCHED_TERMINATE,
  SCHED_UNMUTE_ACTOR,
  SCHED_NOISY_ASIO,
  SCHED_UNNOISY_ASIO
} sched_msg_t;

// Scheduler global data.
static uint32_t scheduler_count;
static scheduler_t* scheduler;
static PONY_ATOMIC(bool) detect_quiescence;
static bool use_yield;
static mpmcq_t inject;
static __pony_thread_local scheduler_t* this_scheduler;

/**
 * Gets the next actor from the scheduler queue.
 */
static pony_actor_t* pop(scheduler_t* sched)
{
  return (pony_actor_t*)ponyint_mpmcq_pop(&sched->q);
}

/**
 * Puts an actor on the scheduler queue.
 */
static void push(scheduler_t* sched, pony_actor_t* actor)
{
  ponyint_mpmcq_push_single(&sched->q, actor);
}

/**
 * Handles the global queue and then pops from the local queue
 */
static pony_actor_t* pop_global(scheduler_t* sched)
{
  pony_actor_t* actor = (pony_actor_t*)ponyint_mpmcq_pop(&inject);

  if(actor != NULL)
    return actor;

  return pop(sched);
}

/**
 * Sends a message to a thread.
 */

static void send_msg(uint32_t to, sched_msg_t msg, intptr_t arg)
{
  pony_msgi_t* m = (pony_msgi_t*)pony_alloc_msg(
    POOL_INDEX(sizeof(pony_msgi_t)), msg);

  m->i = arg;
  ponyint_messageq_push(&scheduler[to].mq, &m->msg, &m->msg);
}

static void send_msg_all(sched_msg_t msg, intptr_t arg)
{
  for(uint32_t i = 0; i < scheduler_count; i++)
    send_msg(i, msg, arg);
}

static bool read_msg(scheduler_t* sched)
{
  pony_msgi_t* m;

  bool run_queue_changed = false;

  while((m = (pony_msgi_t*)ponyint_messageq_pop(&sched->mq)) != NULL)
  {
    switch(m->msg.id)
    {
      case SCHED_BLOCK:
      {
        sched->block_count++;

        if(atomic_load_explicit(&detect_quiescence, memory_order_relaxed) &&
          (sched->block_count == scheduler_count))
        {
          // If we think all threads are blocked, send CNF(token) to everyone.
          send_msg_all(SCHED_CNF, sched->ack_token);
        }
        break;
      }

      case SCHED_UNBLOCK:
      {
        // if the ASIO thread has already been stopped
        if (sched->asio_stopped)
        {
          // restart the ASIO thread
          bool asio_started = ponyint_asio_start();
          pony_assert(asio_started);
          (void) asio_started;
          sched->asio_stopped = false;
        }

        // make sure asio hasn't already been stopped or else runtime is in
        // an invalid state without the ASIO thread running
        pony_assert(!sched->asio_stopped);

        // Cancel all acks and increment the ack token, so that any pending
        // acks in the queue will be dropped when they are received.
        sched->block_count--;
        sched->ack_token++;
        sched->ack_count = 0;
        break;
      }

      case SCHED_CNF:
      {
        // Echo the token back as ACK(token).
        send_msg(0, SCHED_ACK, m->i);
        break;
      }

      case SCHED_ACK:
      {
        // If it's the current token, increment the ack count.
        if(m->i == sched->ack_token)
          sched->ack_count++;
        break;
      }

      case SCHED_TERMINATE:
      {
        sched->terminate = true;
        break;
      }

      case SCHED_UNMUTE_ACTOR:
      {
        if (ponyint_sched_unmute_senders(&sched->ctx, (pony_actor_t*)m->i))
          run_queue_changed = true;

        break;
      }

      case SCHED_NOISY_ASIO:
      {
        // mark asio as being noisy
        sched->asio_noisy = true;
        break;
      }

      case SCHED_UNNOISY_ASIO:
      {
        // mark asio as not being noisy
        sched->asio_noisy = false;
        break;
      }

      default: {}
    }
  }

  return run_queue_changed;
}


/**
 * If we can terminate, return true. If all schedulers are waiting, one of
 * them will stop the ASIO back end and tell the cycle detector to try to
 * terminate.
 */

static bool quiescent(scheduler_t* sched, uint64_t tsc, uint64_t tsc2)
{
  if(sched->terminate)
    return true;

  if(sched->ack_count == scheduler_count)
  {
    if(sched->asio_stopped)
    {
      send_msg_all(SCHED_TERMINATE, 0);

      sched->ack_token++;
      sched->ack_count = 0;
    } else if(ponyint_asio_stop()) {
      sched->asio_stopped = true;
      sched->ack_token++;
      sched->ack_count = 0;

      // Run another CNF/ACK cycle.
      send_msg_all(SCHED_CNF, sched->ack_token);
    }
  }

  ponyint_cpu_core_pause(tsc, tsc2, use_yield);
  return false;
}



static scheduler_t* choose_victim(scheduler_t* sched)
{
  scheduler_t* victim = sched->last_victim;

  while(true)
  {
    // Schedulers are laid out sequentially in memory

    // Back up one.
    victim--;

    if(victim < scheduler)
      // victim is before the first scheduler location
      // wrap around to the end.
      victim = &scheduler[scheduler_count - 1];

    if(victim == sched->last_victim)
    {
      // If we have tried all possible victims, return no victim. Set our last
      // victim to ourself to indicate we've started over.
      sched->last_victim = sched;
      break;
    }

    // Don't try to steal from ourself.
    if(victim == sched)
      continue;

    // Record that this is our victim and return it.
    sched->last_victim = victim;
    return victim;
  }

  return NULL;
}


/**
 * Use mpmcqs to allow stealing directly from a victim, without waiting for a
 * response.
 */
static pony_actor_t* steal(scheduler_t* sched)
{
  bool block_sent = false;
  uint32_t steal_attempts = 0;

  uint64_t tsc = ponyint_cpu_tick();
  pony_actor_t* actor;

  while(true)
  {
    scheduler_t* victim = choose_victim(sched);

    if(victim == NULL)
      actor = (pony_actor_t*)ponyint_mpmcq_pop(&inject);
    else
      actor = pop_global(victim);

    if(actor != NULL)
    {
      DTRACE3(WORK_STEAL_SUCCESSFUL, (uintptr_t)sched, (uintptr_t)victim, (uintptr_t)actor);
      break;
    }

    uint64_t tsc2 = ponyint_cpu_tick();

    if(read_msg(sched))
    {
      // An actor was unmuted and added to our run queue. Pop it and return.
      // Effectively, we are "stealing" from ourselves. We need to verify that
      // popping succeeded (actor != NULL) as some other scheduler might have
      // stolen the newly scheduled actor from us already. Schedulers, what a
      // bunch of thieving bastards!
      actor = pop_global(sched);
      if(actor != NULL)
        break;
    }

    if(quiescent(sched, tsc, tsc2))
    {
      DTRACE2(WORK_STEAL_FAILURE, (uintptr_t)sched, (uintptr_t)victim);
      return NULL;
    }

    // Determine if we are blocked.
    //
    // Note, "blocked" means we have no more work to do and we believe that we
    // should check to see if we can terminate the program.
    //
    // To be blocked, we have to:
    //
    // 1. Not have any noisy actors registered with the ASIO thread/subsystem.
    //    If we have any noisy actors then, while we might not have any work
    //    to do, we aren't blocked. Blocked means we can't make forward
    //    progress and the program might be ready to terminate. Noisy actors
    //    means that no, the program isn't ready to terminate becuase one of
    //    noisy actors could receive a message from an external source (timer,
    //    network, etc).
    // 2. Not have any muted actors. If we are holding any muted actors then,
    //    while we might not have any work to do, we aren't blocked. Blocked
    //    means we can't make forward progress and the program might be ready
    //    to terminate. Muted actors means that no, the program isn't ready
    //    to terminate.
    // 3. We have attempted to steal from every other scheduler and failed to
    //    get any work. In the process of stealing from every other scheduler,
    //    we will have also tried getting work off the ASIO inject queue
    //    multiple times
    // 4. We've been trying to steal for at least 1 million cycles.
    //    In many work stealing scenarios, we immediately get steal an actor.
    //    Sending a block/unblock pair in that scenario is very wasteful.
    //    Same applies to other "quick" steal scenarios.
    //    1 million cycles is roughly 1 millisecond, depending on clock speed.
    //    By waiting 1 millisecond before sending a block message, we are going to
    //    delay quiescence by a small amount of time but also optimize work
    //    stealing for generating far fewer block/unblock messages.
    if (!block_sent)
    {
      if (steal_attempts < scheduler_count)
      {
        steal_attempts++;
      }
      else if ((!sched->asio_noisy) &&
        ((tsc2 - tsc) > 1000000) &&
        (ponyint_mutemap_size(&sched->mute_mapping) == 0))
      {
        send_msg(0, SCHED_BLOCK, 0);
        block_sent = true;
      }
    }
  }

  if(block_sent)
  {
    // Only send unblock message if a corresponding block message was sent
    send_msg(0, SCHED_UNBLOCK, 0);
  }
  return actor;
}

/**
 * Run a scheduler thread until termination.
 */
static void run(scheduler_t* sched)
{
  pony_actor_t* actor = pop_global(sched);
  if (DTRACE_ENABLED(ACTOR_SCHEDULED) && actor != NULL) {
    DTRACE2(ACTOR_SCHEDULED, (uintptr_t)sched, (uintptr_t)actor);
  }

  while(true)
  {
    // In response to reading a message, we might have unmuted an actor and
    // added it back to our queue. if we don't have an actor to run, we want
    // to pop from our queue to check for a recently unmuted actor
    if(read_msg(sched) && actor == NULL)
    {
      actor = pop_global(sched);
    }

    if(actor == NULL)
    {
      // We had an empty queue and no rescheduled actor.
      actor = steal(sched);

      if(actor == NULL)
      {
        // Termination.
        pony_assert(pop(sched) == NULL);
        return;
      }
      DTRACE2(ACTOR_SCHEDULED, (uintptr_t)sched, (uintptr_t)actor);
    }

    // Run the current actor and get the next actor.
    bool reschedule = ponyint_actor_run(&sched->ctx, actor, PONY_SCHED_BATCH);
    pony_actor_t* next = pop_global(sched);

    if(reschedule)
    {
      if(next != NULL)
      {
        // If we have a next actor, we go on the back of the queue. Otherwise,
        // we continue to run this actor.
        push(sched, actor);
        DTRACE2(ACTOR_DESCHEDULED, (uintptr_t)sched, (uintptr_t)actor);
        actor = next;
        DTRACE2(ACTOR_SCHEDULED, (uintptr_t)sched, (uintptr_t)actor);
      }
    } else {
      // We aren't rescheduling, so run the next actor. This may be NULL if our
      // queue was empty.
      DTRACE2(ACTOR_DESCHEDULED, (uintptr_t)sched, (uintptr_t)actor);
      actor = next;
      if (DTRACE_ENABLED(ACTOR_SCHEDULED) && actor != NULL) {
        DTRACE2(ACTOR_SCHEDULED, (uintptr_t)sched, (uintptr_t)actor);
      }
    }
  }
}

static DECLARE_THREAD_FN(run_thread)
{
  scheduler_t* sched = (scheduler_t*) arg;
  this_scheduler = sched;
  ponyint_cpu_affinity(sched->cpu);
  run(sched);
  ponyint_pool_thread_cleanup();

  return 0;
}

static void ponyint_sched_shutdown()
{
  uint32_t start;

  start = 0;

  for(uint32_t i = start; i < scheduler_count; i++)
    ponyint_thread_join(scheduler[i].tid);

  DTRACE0(RT_END);
  ponyint_cycle_terminate(&scheduler[0].ctx);

  for(uint32_t i = 0; i < scheduler_count; i++)
  {
    while(ponyint_messageq_pop(&scheduler[i].mq) != NULL);
    ponyint_messageq_destroy(&scheduler[i].mq);
    ponyint_mpmcq_destroy(&scheduler[i].q);
  }

  ponyint_pool_free_size(scheduler_count * sizeof(scheduler_t), scheduler);
  scheduler = NULL;
  scheduler_count = 0;

  ponyint_mpmcq_destroy(&inject);
}

pony_ctx_t* ponyint_sched_init(uint32_t threads, bool noyield, bool nopin,
  bool pinasio)
{
  pony_register_thread();

  use_yield = !noyield;

  // If no thread count is specified, use the available physical core count.
  if(threads == 0)
    threads = ponyint_cpu_count();

  scheduler_count = threads;
  scheduler = (scheduler_t*)ponyint_pool_alloc_size(
    scheduler_count * sizeof(scheduler_t));
  memset(scheduler, 0, scheduler_count * sizeof(scheduler_t));

  uint32_t asio_cpu = ponyint_cpu_assign(scheduler_count, scheduler, nopin,
    pinasio);

  for(uint32_t i = 0; i < scheduler_count; i++)
  {
    scheduler[i].ctx.scheduler = &scheduler[i];
    scheduler[i].last_victim = &scheduler[i];
    scheduler[i].asio_noisy = false;
    ponyint_messageq_init(&scheduler[i].mq);
    ponyint_mpmcq_init(&scheduler[i].q);
  }

  ponyint_mpmcq_init(&inject);
  ponyint_asio_init(asio_cpu);

  return pony_ctx();
}

bool ponyint_sched_start(bool library)
{
  pony_register_thread();

  if(!ponyint_asio_start())
    return false;

  atomic_store_explicit(&detect_quiescence, !library, memory_order_relaxed);

  DTRACE0(RT_START);
  uint32_t start = 0;

  for(uint32_t i = start; i < scheduler_count; i++)
  {
    if(!ponyint_thread_create(&scheduler[i].tid, run_thread, scheduler[i].cpu,
      &scheduler[i]))
      return false;
  }

  if(!library)
  {
    ponyint_sched_shutdown();
  }

  return true;
}

void ponyint_sched_stop()
{
  atomic_store_explicit(&detect_quiescence, true, memory_order_relaxed);
  ponyint_sched_shutdown();
}

void ponyint_sched_add(pony_ctx_t* ctx, pony_actor_t* actor)
{
  if(ctx->scheduler != NULL)
  {
    // Add to the current scheduler thread.
    push(ctx->scheduler, actor);
  } else {
    // Put on the shared mpmcq.
    ponyint_mpmcq_push(&inject, actor);
  }
}

uint32_t ponyint_sched_cores()
{
  return scheduler_count;
}

PONY_API void pony_register_thread()
{
  if(this_scheduler != NULL)
    return;

  // Create a scheduler_t, even though we will only use the pony_ctx_t.
  this_scheduler = POOL_ALLOC(scheduler_t);
  memset(this_scheduler, 0, sizeof(scheduler_t));
  this_scheduler->tid = ponyint_thread_self();
}

PONY_API void pony_unregister_thread()
{
  if(this_scheduler == NULL)
    return;

  POOL_FREE(scheduler_t, this_scheduler);
  this_scheduler = NULL;

  ponyint_pool_thread_cleanup();
}

PONY_API pony_ctx_t* pony_ctx()
{
  pony_assert(this_scheduler != NULL);
  return &this_scheduler->ctx;
}

// Tell all scheduler threads that asio is noisy
void ponyint_sched_noisy_asio()
{
  send_msg_all(SCHED_NOISY_ASIO, 0);
}

// Tell all scheduler threads that asio is not noisy
void ponyint_sched_unnoisy_asio()
{
  send_msg_all(SCHED_UNNOISY_ASIO, 0);
}

// Manage a scheduler's mute map
//
// When an actor attempts to send to an overloaded actor, it will be added
// to the mute map for this scheduler. The mute map is in the form of:
//
// overloaded receiving actor => [sending actors]
//
// - A given actor will only existing as a sending actor in the map for
//   a given scheduler.
// - Receiving actors can exist as a mute map key in the mute map of more
//   than one scheduler
//
// Because muted sending actors only exist in a single scheduler's mute map
// and because they aren't scheduled when they are muted, any manipulation
// that we do on their state (for example incrementing or decrementing their
// mute count) is thread safe as only a single scheduler thread will be
// accessing the information.

void ponyint_sched_mute(pony_ctx_t* ctx, pony_actor_t* sender, pony_actor_t* recv)
{
  pony_assert(sender != recv);
  scheduler_t* sched = ctx->scheduler;
  size_t index = HASHMAP_UNKNOWN;
  muteref_t key;
  key.key = recv;

  muteref_t* mref = ponyint_mutemap_get(&sched->mute_mapping, &key, &index);
  if(mref == NULL)
  {
    mref = ponyint_muteref_alloc(recv);
    ponyint_mutemap_putindex(&sched->mute_mapping, mref, index);
  }

  size_t index2 = HASHMAP_UNKNOWN;
  pony_actor_t* r = ponyint_muteset_get(&mref->value, sender, &index2);
  if(r == NULL)
  {
    // This is safe because an actor can only ever be in a single scheduler's
    // mutemap
    ponyint_muteset_putindex(&mref->value, sender, index2);
    uint64_t muted = atomic_load_explicit(&sender->muted, memory_order_relaxed);
    muted++;
    atomic_store_explicit(&sender->muted, muted, memory_order_relaxed);
  }
}

void ponyint_sched_start_global_unmute(pony_actor_t* actor)
{
  send_msg_all(SCHED_UNMUTE_ACTOR, (intptr_t)actor);
}

DECLARE_STACK(ponyint_actorstack, actorstack_t, pony_actor_t);
DEFINE_STACK(ponyint_actorstack, actorstack_t, pony_actor_t);

bool ponyint_sched_unmute_senders(pony_ctx_t* ctx, pony_actor_t* actor)
{
  size_t actors_rescheduled = 0;
  scheduler_t* sched = ctx->scheduler;
  size_t index = HASHMAP_UNKNOWN;
  muteref_t key;
  key.key = actor;

  muteref_t* mref = ponyint_mutemap_get(&sched->mute_mapping, &key, &index);

  if(mref != NULL)
  {
    size_t i = HASHMAP_UNKNOWN;
    pony_actor_t* muted = NULL;
    actorstack_t* needs_unmuting = NULL;

    // Find and collect any actors that need to be unmuted
    while((muted = ponyint_muteset_next(&mref->value, &i)) != NULL)
    {
      // This is safe because an actor can only ever be in a single scheduler's
      // mutemap
      uint64_t muted_count = atomic_load_explicit(&muted->muted, memory_order_relaxed);
      pony_assert(muted_count > 0);
      muted_count--;
      atomic_store_explicit(&muted->muted, muted_count, memory_order_relaxed);

      if (muted_count == 0)
      {
        needs_unmuting = ponyint_actorstack_push(needs_unmuting, muted);
      }
    }

    ponyint_mutemap_removeindex(&sched->mute_mapping, index);
    ponyint_muteref_free(mref);

    // Unmute any actors that need to be unmuted
    pony_actor_t* to_unmute;

    while(needs_unmuting != NULL)
    {
      needs_unmuting = ponyint_actorstack_pop(needs_unmuting, &to_unmute);

      if(!has_flag(to_unmute, FLAG_UNSCHEDULED))
      {
        ponyint_unmute_actor(to_unmute);
        // TODO: we don't want to reschedule if our queue is empty.
        // That's wasteful.
        ponyint_sched_add(ctx, to_unmute);
        DTRACE2(ACTOR_SCHEDULED, (uintptr_t)sched, (uintptr_t)to_unmute);
        actors_rescheduled++;
      }

      ponyint_sched_start_global_unmute(to_unmute);
    }
  }

  return actors_rescheduled > 0;
}

