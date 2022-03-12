/* libunwind - a platform-independent unwind library
   Copyright (C) 2010, 2011 by FERMI NATIONAL ACCELERATOR LABORATORY

This file is part of libunwind.

Permission is hereby granted, free of charge, to any person obtaining
a copy of this software and associated documentation files (the
"Software"), to deal in the Software without restriction, including
without limitation the rights to use, copy, modify, merge, publish,
distribute, sublicense, and/or sell copies of the Software, and to
permit persons to whom the Software is furnished to do so, subject to
the following conditions:

The above copyright notice and this permission notice shall be
included in all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE
LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION
OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION
WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.  */

#include "libunwind_i.h"
#include "unwind_i.h"
#include "ucontext_i.h"
#include <signal.h>
#include <limits.h>

#pragma weak pthread_once
#pragma weak pthread_key_create
#pragma weak pthread_getspecific
#pragma weak pthread_setspecific

/* Initial hash table size. Table expands by 2 bits (times four). */
#define HASH_MIN_BITS 14

typedef struct
{
  unw_tdep_frame_t *frames;
  size_t log_size;
  size_t used;
  size_t dtor_count;  /* Counts how many times our destructor has already
                         been called. */
} unw_trace_cache_t;

static const unw_tdep_frame_t empty_frame = { 0, UNW_X86_64_FRAME_OTHER, -1, -1, 0, -1, -1 };
static define_lock (trace_init_lock);
static pthread_once_t trace_cache_once = PTHREAD_ONCE_INIT;
static sig_atomic_t trace_cache_once_happen;
static pthread_key_t trace_cache_key;
static struct mempool trace_cache_pool;
static _Thread_local  unw_trace_cache_t *tls_cache;
static _Thread_local  int tls_cache_destroyed;

/* Free memory for a thread's trace cache. */
static void
trace_cache_free (void *arg)
{
  unw_trace_cache_t *cache = arg;
  if (++cache->dtor_count < PTHREAD_DESTRUCTOR_ITERATIONS)
  {
    /* Not yet our turn to get destroyed. Re-install ourselves into the key. */
    pthread_setspecific(trace_cache_key, cache);
    Debug(5, "delayed freeing cache %p (%zx to go)\n", cache,
          PTHREAD_DESTRUCTOR_ITERATIONS - cache->dtor_count);
    return;
  }
  tls_cache_destroyed = 1;
  tls_cache = NULL;
  munmap (cache->frames, (1u << cache->log_size) * sizeof(unw_tdep_frame_t));
  mempool_free (&trace_cache_pool, cache);
  Debug(5, "freed cache %p\n", cache);
}

/* Initialise frame tracing for threaded use. */
static void
trace_cache_init_once (void)
{
  pthread_key_create (&trace_cache_key, &trace_cache_free);
  mempool_init (&trace_cache_pool, sizeof (unw_trace_cache_t), 0);
  trace_cache_once_happen = 1;
}

static unw_tdep_frame_t *
trace_cache_buckets (size_t n)
{
  unw_tdep_frame_t *frames;
  size_t i;

  GET_MEMORY(frames, n * sizeof (unw_tdep_frame_t));
  if (likely(frames != NULL))
    for (i = 0; i < n; ++i)
      frames[i] = empty_frame;

  return frames;
}

/* Allocate and initialise hash table for frame cache lookups.
   Returns the cache initialised with (1u << HASH_LOW_BITS) hash
   buckets, or NULL if there was a memory allocation problem. */
static unw_trace_cache_t *
trace_cache_create (void)
{
  unw_trace_cache_t *cache;

  if (tls_cache_destroyed)
  {
    /* The current thread is in the process of exiting. Don't recreate
       cache, as we wouldn't have another chance to free it. */
    Debug(5, "refusing to reallocate cache: "
             "thread-locals are being deallocated\n");
    return NULL;
  }

  if (! (cache = mempool_alloc(&trace_cache_pool)))
  {
    Debug(5, "failed to allocate cache\n");
    return NULL;
  }

  if (! (cache->frames = trace_cache_buckets(1u << HASH_MIN_BITS)))
  {
    Debug(5, "failed to allocate buckets\n");
    mempool_free(&trace_cache_pool, cache);
    return NULL;
  }

  cache->log_size = HASH_MIN_BITS;
  cache->used = 0;
  cache->dtor_count = 0;
  tls_cache_destroyed = 0;  /* Paranoia: should already be 0. */
  Debug(5, "allocated cache %p\n", cache);
  return cache;
}

/* Expand the hash table in the frame cache if possible. This always
   quadruples the hash size, and clears all previous frame entries. */
static int
trace_cache_expand (unw_trace_cache_t *cache)
{
  size_t old_size = (1u << cache->log_size);
  size_t new_log_size = cache->log_size + 2;
  unw_tdep_frame_t *new_frames = trace_cache_buckets (1u << new_log_size);

  if (unlikely(! new_frames))
  {
    Debug(5, "failed to expand cache to 2^%lu buckets\n", new_log_size);
    return -UNW_ENOMEM;
  }

  Debug(5, "expanded cache from 2^%lu to 2^%lu buckets\n", cache->log_size, new_log_size);
  munmap(cache->frames, old_size * sizeof(unw_tdep_frame_t));
  cache->frames = new_frames;
  cache->log_size = new_log_size;
  cache->used = 0;
  return 0;
}

static unw_trace_cache_t *
trace_cache_get_unthreaded (void)
{
  unw_trace_cache_t *cache;
  intrmask_t saved_mask;
  static unw_trace_cache_t *global_cache = NULL;
  lock_acquire (&trace_init_lock, saved_mask);
  if (! global_cache)
  {
    mempool_init (&trace_cache_pool, sizeof (unw_trace_cache_t), 0);
    global_cache = trace_cache_create ();
  }
  cache = global_cache;
  lock_release (&trace_init_lock, saved_mask);
  Debug(5, "using cache %p\n", cache);
  return cache;
}

/* Get the frame cache for the current thread. Create it if there is none. */
static unw_trace_cache_t *
trace_cache_get (void)
{
  unw_trace_cache_t *cache;
  if (likely (pthread_once != NULL))
  {
    pthread_once(&trace_cache_once, &trace_cache_init_once);
    if (!trace_cache_once_happen)
    {
      return trace_cache_get_unthreaded();
    }
    if (! (cache = tls_cache))
    {
      cache = trace_cache_create();
      pthread_setspecific(trace_cache_key, cache);
      tls_cache = cache;
    }
    Debug(5, "using cache %p\n", cache);
    return cache;
  }
  else
  {
    return trace_cache_get_unthreaded();
  }
}

/* Initialise frame properties for address cache slot F at address
   RIP using current CFA, RBP and RSP values.  Modifies CURSOR to
   that location, performs one unw_step(), and fills F with what
   was discovered about the location.  Returns F.
*/
static unw_tdep_frame_t *
trace_init_addr (unw_tdep_frame_t *f,
                 unw_cursor_t *cursor,
                 unw_word_t cfa,
                 unw_word_t rip,
                 unw_word_t rbp,
                 unw_word_t rsp)
{
  struct cursor *c = (struct cursor *) cursor;
  struct dwarf_cursor *d = &c->dwarf;
  int ret = -UNW_EINVAL;

  /* Initialise frame properties: unknown, not last. */
  f->virtual_address = rip;
  f->frame_type = UNW_X86_64_FRAME_OTHER;
  f->last_frame = 0;
  f->cfa_reg_rsp = -1;
  f->cfa_reg_offset = 0;
  f->rbp_cfa_offset = -1;
  f->rsp_cfa_offset = -1;

  /* Reinitialise cursor to this instruction - but undo next/prev RIP
     adjustment because unw_step will redo it - and force RIP, RBP
     RSP into register locations (=~ ucontext we keep), then set
     their desired values. Then perform the step. */
  d->ip = rip + d->use_prev_instr;
  d->cfa = cfa;
  for(int i = 0; i < DWARF_NUM_PRESERVED_REGS; i++) {
    d->loc[i] = DWARF_NULL_LOC;
  }
  d->loc[UNW_X86_64_RIP] = DWARF_REG_LOC (d, UNW_X86_64_RIP);
  d->loc[UNW_X86_64_RBP] = DWARF_REG_LOC (d, UNW_X86_64_RBP);
  d->loc[UNW_X86_64_RSP] = DWARF_REG_LOC (d, UNW_X86_64_RSP);
  c->frame_info = *f;

  if (likely(dwarf_put (d, d->loc[UNW_X86_64_RIP], rip) >= 0)
      && likely(dwarf_put (d, d->loc[UNW_X86_64_RBP], rbp) >= 0)
      && likely(dwarf_put (d, d->loc[UNW_X86_64_RSP], rsp) >= 0)
      && likely((ret = unw_step (cursor)) >= 0))
    *f = c->frame_info;

  /* If unw_step() stopped voluntarily, remember that, even if it
     otherwise could not determine anything useful.  This avoids
     failing trace if we hit frames without unwind info, which is
     common for the outermost frame (CRT stuff) on many systems.
     This avoids failing trace in very common circumstances; failing
     to unw_step() loop wouldn't produce any better result. */
  if (ret == 0)
    f->last_frame = -1;

  Debug (3, "frame va %lx type %d last %d cfa %s+%d rbp @ cfa%+d rsp @ cfa%+d\n",
         f->virtual_address, f->frame_type, f->last_frame,
         f->cfa_reg_rsp ? "rsp" : "rbp", f->cfa_reg_offset,
         f->rbp_cfa_offset, f->rsp_cfa_offset);

  return f;
}

/* Look up and if necessary fill in frame attributes for address RIP
   in CACHE using current CFA, RBP and RSP values.  Uses CURSOR to
   perform any unwind steps necessary to fill the cache.  Returns the
   frame cache slot which describes RIP. */
static unw_tdep_frame_t *
trace_lookup (unw_cursor_t *cursor,
              unw_trace_cache_t *cache,
              unw_word_t cfa,
              unw_word_t rip,
              unw_word_t rbp,
              unw_word_t rsp)
{
  /* First look up for previously cached information using cache as
     linear probing hash table with probe step of 1.  Majority of
     lookups should be completed within few steps, but it is very
     important the hash table does not fill up, or performance falls
     off the cliff. */
  uint64_t i, addr;
  uint64_t cache_size = 1u << cache->log_size;
  uint64_t slot = ((rip * 0x9e3779b97f4a7c16) >> 43) & (cache_size-1);
  unw_tdep_frame_t *frame;

  for (i = 0; i < 16; ++i)
  {
    frame = &cache->frames[slot];
    addr = frame->virtual_address;

    /* Return if we found the address. */
    if (likely(addr == rip))
    {
      Debug (4, "found address after %ld steps\n", i);
      return frame;
    }

    /* If slot is empty, reuse it. */
    if (likely(! addr))
      break;

    /* Linear probe to next slot candidate, step = 1. */
    if (++slot >= cache_size)
      slot -= cache_size;
  }

  /* If we collided after 16 steps, or if the hash is more than half
     full, force the hash to expand. Fill the selected slot, whether
     it's free or collides. Note that hash expansion drops previous
     contents; further lookups will refill the hash. */
  Debug (4, "updating slot %lu after %ld steps, replacing 0x%lx\n", slot, i, addr);
  if (unlikely(addr || cache->used >= cache_size / 2))
  {
    if (unlikely(trace_cache_expand (cache) < 0))
      return NULL;

    cache_size = 1u << cache->log_size;
    slot = ((rip * 0x9e3779b97f4a7c16) >> 43) & (cache_size-1);
    frame = &cache->frames[slot];
    addr = frame->virtual_address;
  }

  if (! addr)
    ++cache->used;

  return trace_init_addr (frame, cursor, cfa, rip, rbp, rsp);
}

/* Fast stack backtrace for x86-64.

   This is used by backtrace() implementation to accelerate frequent
   queries for current stack, without any desire to unwind. It fills
   BUFFER with the call tree from CURSOR upwards for at most SIZE
   stack levels. The first frame, backtrace itself, is omitted. When
   called, SIZE should give the maximum number of entries that can be
   stored into BUFFER. Uses an internal thread-specific cache to
   accelerate queries.

   The caller should fall back to a unw_step() loop if this function
   fails by returning -UNW_ESTOPUNWIND, meaning the routine hit a
   stack frame that is too complex to be traced in the fast path.

   This function is tuned for clients which only need to walk the
   stack to get the call tree as fast as possible but without any
   other details, for example profilers sampling the stack thousands
   to millions of times per second.  The routine handles the most
   common x86-64 ABI stack layouts: CFA is RBP or RSP plus/minus
   constant offset, return address is at CFA-8, and RBP and RSP are
   either unchanged or saved on stack at constant offset from the CFA;
   the signal return frame; and frames without unwind info provided
   they are at the outermost (final) frame or can conservatively be
   assumed to be frame-pointer based.

   Any other stack layout will cause the routine to give up. There
   are only a handful of relatively rarely used functions which do
   not have a stack in the standard form: vfork, longjmp, setcontext
   and _dl_runtime_profile on common linux systems for example.

   On success BUFFER and *SIZE reflect the trace progress up to *SIZE
   stack levels or the outermost frame, which ever is less.  It may
   stop short of outermost frame if unw_step() loop would also do so,
   e.g. if there is no more unwind information; this is not reported
   as an error.

   The function returns a negative value for errors, -UNW_ESTOPUNWIND
   if tracing stopped because of an unusual frame unwind info.  The
   BUFFER and *SIZE reflect tracing progress up to the error frame.

   Callers of this function would normally look like this:

     unw_cursor_t     cur;
     unw_context_t    ctx;
     void             addrs[128];
     int              depth = 128;
     int              ret;

     unw_getcontext(&ctx);
     unw_init_local(&cur, &ctx);
     if ((ret = unw_tdep_trace(&cur, addrs, &depth)) < 0)
     {
       depth = 0;
       unw_getcontext(&ctx);
       unw_init_local(&cur, &ctx);
       while ((ret = unw_step(&cur)) > 0 && depth < 128)
       {
         unw_word_t ip;
         unw_get_reg(&cur, UNW_REG_IP, &ip);
         addresses[depth++] = (void *) ip;
       }
     }
*/
HIDDEN int
tdep_trace (unw_cursor_t *cursor, void **buffer, int *size)
{
  struct cursor *c = (struct cursor *) cursor;
  struct dwarf_cursor *d = &c->dwarf;
  unw_trace_cache_t *cache;
  unw_word_t rbp, rsp, rip, cfa;
  int maxdepth = 0;
  int depth = 0;
  int ret;
  int validate = 0;

  /* Check input parametres. */
  if (unlikely(! cursor || ! buffer || ! size || (maxdepth = *size) <= 0))
    return -UNW_EINVAL;

  Debug (1, "begin ip 0x%lx cfa 0x%lx\n", d->ip, d->cfa);

  /* Tell core dwarf routines to call back to us. */
  d->stash_frames = 1;

  /* Determine initial register values. These are direct access safe
     because we know they come from the initial machine context. */
  rip = d->ip;
  rsp = cfa = d->cfa;
  ACCESS_MEM_FAST(ret, 0, d, DWARF_GET_LOC(d->loc[UNW_X86_64_RBP]), rbp);
  assert(ret == 0);

  /* Get frame cache. */
  if (unlikely(! (cache = trace_cache_get())))
  {
    Debug (1, "returning %d, cannot get trace cache\n", -UNW_ENOMEM);
    *size = 0;
    d->stash_frames = 0;
    return -UNW_ENOMEM;
  }

  /* Trace the stack upwards, starting from current RIP.  Adjust
     the RIP address for previous/next instruction as the main
     unwinding logic would also do.  We undo this before calling
     back into unw_step(). */
  while (depth < maxdepth)
  {
    rip -= d->use_prev_instr;
    Debug (2, "depth %d cfa 0x%lx rip 0x%lx rsp 0x%lx rbp 0x%lx\n",
           depth, cfa, rip, rsp, rbp);

    /* See if we have this address cached.  If not, evaluate enough of
       the dwarf unwind information to fill the cache line data, or to
       decide this frame cannot be handled in fast trace mode.  We
       cache negative results too to prevent unnecessary dwarf parsing
       for common failures. */
    unw_tdep_frame_t *f = trace_lookup (cursor, cache, cfa, rip, rbp, rsp);

    /* If we don't have information for this frame, give up. */
    if (unlikely(! f))
    {
      ret = -UNW_ENOINFO;
      break;
    }

    Debug (3, "frame va %lx type %d last %d cfa %s+%d rbp @ cfa%+d rsp @ cfa%+d\n",
           f->virtual_address, f->frame_type, f->last_frame,
           f->cfa_reg_rsp ? "rsp" : "rbp", f->cfa_reg_offset,
           f->rbp_cfa_offset, f->rsp_cfa_offset);

    assert (f->virtual_address == rip);

    /* Stop if this was the last frame.  In particular don't evaluate
       new register values as it may not be safe - we don't normally
       run with full validation on, and do not want to - and there's
       enough bad unwind info floating around that we need to trust
       what unw_step() previously said, in potentially bogus frames. */
    if (f->last_frame)
      break;

    /* Evaluate CFA and registers for the next frame. */
    switch (f->frame_type)
    {
    case UNW_X86_64_FRAME_GUESSED:
      /* Fall thru to standard processing after forcing validation. */
      if (d->as == unw_local_addr_space)
        dwarf_set_validate(d, 1);

    case UNW_X86_64_FRAME_STANDARD:
      /* Advance standard traceable frame. */
      cfa = (f->cfa_reg_rsp ? rsp : rbp) + f->cfa_reg_offset;
      if (d->as == unw_local_addr_space)
        validate = dwarf_get_validate(d);
      ACCESS_MEM_FAST(ret, validate, d, cfa - 8, rip);
      if (likely(ret >= 0) && likely(f->rbp_cfa_offset != -1))
        ACCESS_MEM_FAST(ret, validate, d, cfa + f->rbp_cfa_offset, rbp);

      /* Don't bother reading RSP from DWARF, CFA becomes new RSP. */
      rsp = cfa;

      /* Next frame needs to back up for unwind info lookup. */
      d->use_prev_instr = 1;
      break;

    case UNW_X86_64_FRAME_SIGRETURN:
      cfa = cfa + f->cfa_reg_offset; /* cfa now points to ucontext_t.  */

      if (d->as == unw_local_addr_space)
        validate = dwarf_get_validate(d);
      ACCESS_MEM_FAST(ret, validate, d, cfa + UC_MCONTEXT_GREGS_RIP, rip);
      if (likely(ret >= 0))
        ACCESS_MEM_FAST(ret, validate, d, cfa + UC_MCONTEXT_GREGS_RBP, rbp);
      if (likely(ret >= 0))
        ACCESS_MEM_FAST(ret, validate, d, cfa + UC_MCONTEXT_GREGS_RSP, rsp);

      /* Resume stack at signal restoration point. The stack is not
         necessarily continuous here, especially with sigaltstack(). */
      cfa = rsp;

      /* Next frame should not back up. */
      d->use_prev_instr = 0;
      break;

    case UNW_X86_64_FRAME_ALIGNED:
      /* Address of RIP was pushed on the stack via a simple
       * def_cfa_expr - result stack offset stored in cfa_reg_offset */
      cfa = (f->cfa_reg_rsp ? rsp : rbp) + f->cfa_reg_offset;
      if (d->as == unw_local_addr_space)
        validate = dwarf_get_validate(d);
      ACCESS_MEM_FAST(ret, validate, d, cfa, cfa);
      if (likely(ret >= 0))
        ACCESS_MEM_FAST(ret, validate, d, cfa - 8, rip);
      if (likely(ret >= 0))
        ACCESS_MEM_FAST(ret, validate, d, rbp, rbp);

      /* Don't bother reading RSP from DWARF, CFA becomes new RSP. */
      rsp = cfa;

      /* Next frame needs to back up for unwind info lookup. */
      d->use_prev_instr = 1;

      break;

    default:
      /* We cannot trace through this frame, give up and tell the
         caller we had to stop.  Data collected so far may still be
         useful to the caller, so let it know how far we got.  */
      ret = -UNW_ESTOPUNWIND;
      break;
    }

    Debug (4, "new cfa 0x%lx rip 0x%lx rsp 0x%lx rbp 0x%lx\n",
           cfa, rip, rsp, rbp);

    /* If we failed or ended up somewhere bogus, stop. */
    if (unlikely(ret < 0 || rip < 0x4000))
      break;

    /* Record this address in stack trace. We skipped the first address. */
    buffer[depth++] = (void *) rip;
  }

#if UNW_DEBUG
  Debug (1, "returning %d, depth %d\n", ret, depth);
#endif
  *size = depth;
  return ret;
}
