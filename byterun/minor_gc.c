/***********************************************************************/
/*                                                                     */
/*                                OCaml                                */
/*                                                                     */
/*             Damien Doligez, projet Para, INRIA Rocquencourt         */
/*                                                                     */
/*  Copyright 1996 Institut National de Recherche en Informatique et   */
/*  en Automatique.  All rights reserved.  This file is distributed    */
/*  under the terms of the GNU Library General Public License, with    */
/*  the special exception on linking described in file ../LICENSE.     */
/*                                                                     */
/***********************************************************************/

#include <string.h>
#include "caml/config.h"
#include "caml/fail.h"
#include "caml/finalise.h"
#include "caml/gc.h"
#include "caml/gc_ctrl.h"
#include "caml/major_gc.h"
#include "caml/memory.h"
#include "caml/minor_gc.h"
#include "caml/misc.h"
#include "caml/mlvalues.h"
#include "caml/roots.h"
#include "caml/signals.h"
#include "caml/weak.h"
#include "caml/domain.h"
#include "caml/shared_heap.h"
#include "caml/addrmap.h"
#include "caml/fiber.h"
#include "caml/eventlog.h"
#include "caml/platform.h"

static void alloc_table (struct caml_ref_table *tbl, asize_t sz, asize_t rsv)
{
  tbl->size = sz;
  tbl->reserve = rsv;
  if (tbl->base != NULL) caml_stat_free (tbl->base);
  tbl->base = (value**) caml_stat_alloc ((tbl->size + tbl->reserve)
                                         * sizeof (value*));
  tbl->ptr = tbl->base;
  tbl->threshold = tbl->base + tbl->size;
  tbl->limit = tbl->threshold;
  tbl->end = tbl->base + tbl->size + tbl->reserve;
}

static void reset_table (struct caml_ref_table *tbl)
{
  tbl->size = 0;
  tbl->reserve = 0;
  if (tbl->base != NULL) caml_stat_free (tbl->base);
  tbl->base = tbl->ptr = tbl->threshold = tbl->limit = tbl->end = NULL;
}

static void clear_table (struct caml_ref_table *tbl)
{
    tbl->ptr = tbl->base;
    tbl->limit = tbl->threshold;
}

struct caml_remembered_set* caml_alloc_remembered_set()
{
  struct caml_remembered_set* r =
    caml_stat_alloc(sizeof(struct caml_remembered_set));
  memset(r, 0, sizeof(*r));
  return r;
}

void caml_free_remembered_set(struct caml_remembered_set* r)
{
  Assert(r->major_ref.ptr == r->major_ref.base);
  Assert(r->minor_ref.ptr == r->minor_ref.base);
  Assert(r->fiber_ref.ptr == r->fiber_ref.base + 1);
  Assert((value)*r->fiber_ref.base == Caml_state->current_stack);
  reset_table(&r->major_ref);
  reset_table(&r->minor_ref);
  reset_table(&r->fiber_ref);
  caml_stat_free(r);
}

/* size in bytes */
void caml_set_minor_heap_size (asize_t size)
{
  caml_domain_state* domain_state = Caml_state;
  if (domain_state->young_ptr != domain_state->young_end) caml_minor_collection ();

  caml_reallocate_minor_heap(size);

  reset_table (&domain_state->remembered_set->major_ref);
  reset_table (&domain_state->remembered_set->minor_ref);
}

//*****************************************************************************

struct oldify_state {
  value todo_list;
  uintnat live_bytes;
  int should_promote_stacks;
  struct domain* promote_domain;
  value oldest_promoted;
};

static value alloc_shared(mlsize_t wosize, tag_t tag)
{
  void* mem = caml_shared_try_alloc(Caml_state->shared_heap, wosize, tag,
                                    0 /* not pinned */);
  Caml_state->allocated_words += Whsize_wosize(wosize);
  if (mem == NULL) {
    caml_fatal_error("allocation failure during minor GC");
  }
  return Val_hp(mem);
}

/* Note that the tests on the tag depend on the fact that Infix_tag,
   Forward_tag, and No_scan_tag are contiguous. */

static void oldify_one (void* st_v, value v, value *p)
{
  struct oldify_state* st = st_v;
  value result;
  header_t hd;
  mlsize_t sz, i;
  mlsize_t infix_offset;
  tag_t tag;
  int stack_used;
  caml_domain_state* domain_state =
    st->promote_domain ? st->promote_domain->state : Caml_state;
  struct caml_remembered_set *remembered_set = domain_state->remembered_set;
  char* young_ptr = domain_state->young_ptr;
  char* young_end = domain_state->young_end;
  atomic_uintnat* header_word;
  Assert (domain_state->young_start <= domain_state->young_ptr &&
          domain_state->young_ptr <= domain_state->young_end);

 tail_call:
  if (!Is_block(v) || !Is_minor(v)) {
    /* not a minor block */
    *p = v;
    return;
  }

  infix_offset = 0;
  SPIN_WAIT {
    header_word = (atomic_uintnat*)&Hd_val(v);
    hd = atomic_load_acq(header_word);
    if (hd == 0) {
      /* already forwarded, forward pointer is first field. */
      *p = Op_val(v)[0] + infix_offset;
      return;
    } else if (hd == 1) {
      /* another thread is forwarding, keep spinning */
      continue;
    }
    tag = Tag_hd (hd);
    if (tag == Infix_tag) {
      /* Infix header, retry with the real block */
      Assert (infix_offset == 0);
      infix_offset = Infix_offset_hd (hd);
      Assert(infix_offset > 0);
      v -= infix_offset;
      continue;
    } else if (atomic_cas(header_word, hd, 1)) {
      Assert (Wosize_hd(hd) <= Max_young_wosize);
      /* we've locked this header */
      break;
    }
  }
  Assert (hd != 0 && hd != 1);
  Assert (tag != Infix_tag);

  if (((value)Hp_val(v)) > st->oldest_promoted) {
    st->oldest_promoted = (value)Hp_val(v);
  }

  if (tag < Infix_tag){
    value field0;

    if (tag == Stack_tag && !st->should_promote_stacks) {
      /* Stacks are not promoted unless explicitly requested. */
      Ref_table_add(&remembered_set->major_ref, p);
    } else {
      sz = Wosize_hd (hd);
      st->live_bytes += Bhsize_hd(hd);
      result = alloc_shared (sz, tag);
      // caml_gc_log ("promoting object %p (referred from %p) tag=%d size=%lu to %p", (value*)v, p, tag, sz, (value*)result);
      *p = result + infix_offset;
      if (tag == Stack_tag) {
        /* Ensure that the stack remains 16-byte aligned. Note: Stack_high
         * always returns 16-byte aligned down address. */
        value* stack_high = ((value*)(((value)(Op_val(v) + Wosize_hd(hd))) & (-1uLL << 4)));
        Assert(infix_offset == 0);
        stack_used = -Stack_sp(v);
        memcpy((void*)result, (void*)v, sizeof(value) * Stack_ctx_words);
        memcpy(Stack_high(result) - stack_used, stack_high - stack_used,
               stack_used * sizeof(value));

        Op_val(v)[0] = result;
        atomic_store_rel(header_word, 0);
        Op_val(v)[1] = st->todo_list;
        st->todo_list = v;
      } else {
        field0 = Op_val(v)[0];
        Assert (!Is_debug_tag(field0));
        Op_val(v)[0] = result;     /*  and forward pointer. */
        atomic_store_rel (header_word, 0);            /* Set forward flag */

        if (sz > 1){
          Op_val (result)[0] = field0;
          Op_val (result)[1] = st->todo_list;    /* Add this block */
          st->todo_list = v;                     /*  to the "to do" list. */
        }else{
          Assert (sz == 1);
          p = Op_val(result);
          v = field0;
          goto tail_call;
        }
      }
    }
  } else if (tag >= No_scan_tag) {
    sz = Wosize_hd (hd);
    st->live_bytes += Bhsize_hd(hd);
    result = alloc_shared(sz, tag);
    for (i = 0; i < sz; i++) {
      value curr = Op_val(v)[i];
      /* FIXME: this is wrong, as Debug_tag(N) is a valid value.
         However, it's a useful debugging aid for now */
      //Assert(!Is_debug_tag(curr));
      Op_val (result)[i] = curr;
    }
    Op_val (v)[0] = result;    /*  and forward pointer. */
    atomic_store_rel(header_word, 0);
    // caml_gc_log ("promoting object %p (referred from %p) tag=%d size=%lu to %p", (value*)v, p, tag, sz, (value*)result);
    Assert (infix_offset == 0);
    *p = result;
  } else {
    Assert (tag == Forward_tag);
    Assert (infix_offset == 0);

    value f = Forward_val (v);
    tag_t ft = 0;

    if (Is_block (f)) {
      /* FIXME race */
      ft = Tag_val (Hd_val (f) == 0 ? Op_val (f)[0] : f);
    }

    if (1 /* FIXME */ || ft == Forward_tag || ft == Lazy_tag || ft == Double_tag) {
      /* Do not short-circuit the pointer.  Copy as a normal block. */
      Assert (Wosize_hd (hd) == 1);
      st->live_bytes += Bhsize_hd(hd);
      result = alloc_shared (1, Forward_tag);
      // caml_gc_log ("promoting object %p (referred from %p) tag=%d size=%lu to %p",
      //             (value*)v, p, tag, (value)1, (value*)result);
      *p = result;
      Op_val (v)[0] = result;      /*  and forward pointer. */
      atomic_store_rel(header_word, 0);
      p = Op_val (result);
      v = f;
      goto tail_call;
    } else {
      v = f;                        /* Follow the forwarding */
      goto tail_call;               /*  then oldify. */
    }
  }
}

/* Finish the work that was put off by [oldify_one].
   Note that [oldify_one] itself is called by oldify_mopup, so we
   have to be careful to remove the first entry from the list before
   oldifying its fields. */
static void oldify_mopup (struct oldify_state* st)
{
  value v, new_v, f;
  mlsize_t i;
  caml_domain_state* domain_state =
    st->promote_domain ? st->promote_domain->state : Caml_state;
  char* young_ptr = domain_state->young_ptr;
  char* young_end = domain_state->young_end;

  while (st->todo_list != 0){
    v = st->todo_list;                 /* Get the head. */
    Assert (Hd_val (v) == 0);             /* It must be forwarded. */
    new_v = Op_val (v)[0];                /* Follow forward pointer. */
    if (Tag_val(new_v) == Stack_tag) {
      st->todo_list = Op_val (v)[1];   /* Remove from list (stack) */
      //caml_gc_log ("oldify_mopup: caml_scan_stack start old=%p new=%p",
      //             (value*)v, (value*)new_v);
      caml_scan_stack(oldify_one, st, new_v);
      //caml_gc_log ("oldify_mopup: caml_scan_stack end old=%p new=%p",
      //             (value*)v, (value*)new_v);
    } else {
      st->todo_list = Op_val (new_v)[1]; /* Remove from list (non-stack) */

      f = Op_val (new_v)[0];
      Assert (!Is_debug_tag(f));
      if (Is_block (f) && Is_young(v)) {
        oldify_one (st, f, Op_val (new_v));
      }
      for (i = 1; i < Wosize_val (new_v); i++){
        f = Op_val (v)[i];
        Assert (!Is_debug_tag(f));
        if (Is_block (f) && Is_young(v)) {
          oldify_one (st, f, Op_val (new_v) + i);
        } else {
          Op_val (new_v)[i] = f;
        }
      }
    }

    Assert (Wosize_val(new_v));
  }
}

//*****************************************************************************

void forward_pointer (void* state, value v, value *p) {
  header_t hd;
  mlsize_t offset;
  value fwd;
  struct domain* promote_domain = state;
  caml_domain_state* domain_state =
    promote_domain ? promote_domain->state : Caml_state;
  char* young_ptr = domain_state->young_ptr;
  char* young_end = domain_state->young_end;

  Assert(0);
  if (Is_block (v) && young_ptr <= Hp_val(v) && Hp_val(v) < young_end) {
    hd = Hd_val(v);
    if (hd == 0) {
      // caml_gc_log ("forward_pointer: p=%p old=%p new=%p", p, (value*)v, (value*)Op_val(v)[0]);
      *p = Op_val(v)[0];
      Assert (Is_block(*p) && !Is_minor(*p));
    } else if (Tag_hd(hd) == Infix_tag) {
      offset = Infix_offset_hd(hd);
      fwd = 0;
      forward_pointer (state, v - offset, &fwd);
      if (fwd) *p = fwd + offset;
    }
  }
}

static value next_minor_block(caml_domain_state* domain_state, value curr_hp)
{
  mlsize_t wsz;
  header_t hd;
  value curr_val;
  Assert ((value)domain_state->young_ptr <= curr_hp);
  Assert (curr_hp < (value)domain_state->young_end);
  hd = Hd_hp(curr_hp);
  curr_val = Val_hp(curr_hp);
  if (hd == 0) {
    /* Forwarded object, find the promoted version */
    curr_val = Op_val(curr_val)[0];
  }
  Assert (Is_block(curr_val) && Hd_val(curr_val) != 0 && Tag_val(curr_val) != Infix_tag);
  wsz = Wosize_val(curr_val);
  Assert (wsz <= Max_young_wosize);
  return curr_hp + Bsize_wsize(Whsize_wosize(wsz));
}

CAMLexport value caml_promote(struct domain* domain, value root)
{
  CAMLparam1(root);
  if (Is_minor(root)) {
    caml_gc_log("caml_promote: forcing minor GC");
    caml_empty_minor_heap();
  }
  Assert(!Is_minor(root));
  CAMLreturn (root);
}

//*****************************************************************************

/* Make sure the minor heap is empty by performing a minor collection
   if needed.
*/

void caml_empty_minor_heap_domain (struct domain* domain, void* unused)
{
  caml_domain_state* domain_state = domain->state;
  struct caml_remembered_set *remembered_set = domain_state->remembered_set;
  unsigned rewritten = 0;
  int saved_stack = 0;
  value young_ptr = (value)domain_state->young_ptr;
  value young_end = (value)domain_state->young_end;
  uintnat minor_allocated_bytes = young_end - young_ptr;
  struct oldify_state st = {0};
  value **r;

  if (!caml_stack_is_saved()) {
    saved_stack = 1;
    caml_save_stack_gc();
  }

  st.promote_domain = domain;
  st.should_promote_stacks = 1;

  if (1 || minor_allocated_bytes != 0) {
    uintnat prev_alloc_words = domain_state->allocated_words;
    caml_gc_log ("Minor collection of domain %d starting", domain->state->id);
    caml_ev_msg("Start minor");
    caml_ev_start_gc();
    caml_ev_msg("Start minor");
    caml_do_local_roots(&oldify_one, &st, domain);

    for (r = remembered_set->fiber_ref.base; r < remembered_set->fiber_ref.ptr; r++) {
      caml_scan_dirty_stack_domain (&oldify_one, &st, (value)*r, domain);
    }

    for (r = remembered_set->major_ref.base; r < remembered_set->major_ref.ptr; r++) { value x = **r;
      oldify_one (&st, x, &x);
    }

    oldify_mopup (&st);

    for (r = remembered_set->major_ref.base; r < remembered_set->major_ref.ptr; r++) {
      value v = **r;
      if (Is_block (v) && Is_minor(v)) {
        //Assert (Hp_val (v) >= domain_state->young_ptr);
        value vnew;
        header_t hd = Hd_val(v);
        int offset = 0;
        if (Tag_hd(hd) == Infix_tag) {
          offset = Infix_offset_hd(hd);
          v -= offset;
        }
        Assert (Hd_val(v) == 0);
        vnew = Op_val(v)[0] + offset;
        Assert (Is_block(vnew) && !Is_minor(vnew));
        Assert (Hd_val(vnew));
        if (Tag_hd(hd) == Infix_tag) { Assert(Tag_val(vnew) == Infix_tag); }
        if (__sync_bool_compare_and_swap (*r,v,vnew)) ++rewritten;
        caml_darken(0, vnew,0);
      }
    }

    clear_table (&remembered_set->major_ref);
    clear_table (&remembered_set->minor_ref);
    domain_state->young_ptr = domain_state->young_end;
    domain_state->stat_minor_words += Wsize_bsize (minor_allocated_bytes);
    domain_state->stat_minor_collections++;
    domain_state->stat_promoted_words += domain_state->allocated_words - prev_alloc_words;

    caml_ev_msg("End minor");
    caml_ev_end_gc();
    caml_ev_msg("End minor");
    caml_gc_log ("Minor collection of domain %d completed: %2.0f%% of %u KB live, %u pointers rewritten",
                 domain->state->id,
                 100.0 * (double)st.live_bytes / (double)minor_allocated_bytes,
                 (unsigned)(minor_allocated_bytes + 512)/1024, rewritten);
  }
  else {
    caml_gc_log ("Minor collection of domain %d: skipping", domain->state->id);
  }

  for (r = remembered_set->fiber_ref.base; r < remembered_set->fiber_ref.ptr; r++) {
    caml_scan_dirty_stack_domain (&caml_darken, 0, (value)*r, domain);
    caml_clean_stack_domain ((value)*r, domain);
  }
  clear_table (&remembered_set->fiber_ref);

  if (saved_stack) {
    caml_restore_stack_gc();
  }

  caml_global_barrier();
  
  caml_ev_msg("Minor heap empty");
  domain_state->promoted_in_current_cycle = 0;

#ifdef DEBUG
  {
    value *p;
    for (p = (value *) domain_state->young_start;
         p < (value *) domain_state->young_end; ++p){
      *p = Debug_free_minor;
    }
  }
#endif
}

void caml_empty_minor_heap ()
{
  while (Caml_state->young_ptr != Caml_state->young_end) {
    caml_try_run_on_all_domains(&caml_empty_minor_heap_domain, 0);
  }
}

/* Do a minor collection and a slice of major collection, call finalisation
   functions, etc.
   Leave the minor heap empty.
*/
CAMLexport void caml_minor_collection (void)
{
  caml_ev_pause(EV_PAUSE_GC);

  caml_handle_incoming_interrupts ();
  caml_empty_minor_heap ();
  caml_handle_incoming_interrupts ();
  caml_major_collection_slice (0);

  /* FIXME: run finalisers.
     If finalisers run, need to rerun caml_empty_minor_heap.
   */

  Assert (Caml_state->young_end == Caml_state->young_ptr);

  caml_ev_resume();

  /* If the major slice triggered a STW, do that now */
  caml_handle_gc_interrupt();
}

CAMLexport value caml_check_urgent_gc (value extra_root)
{
  CAMLparam1 (extra_root);
  caml_handle_gc_interrupt();
  CAMLreturn (extra_root);
}

void caml_realloc_ref_table (struct caml_ref_table *tbl)
{                                           Assert (tbl->ptr == tbl->limit);
                                            Assert (tbl->limit <= tbl->end);
                                      Assert (tbl->limit >= tbl->threshold);

  if (tbl->base == NULL){
    alloc_table (tbl, Caml_state->minor_heap_size / sizeof (value) / 8, 256);
  }else if (tbl->limit == tbl->threshold){
    caml_gc_log ("ref_table threshold crossed");
    tbl->limit = tbl->end;
    caml_urge_major_slice ();
  }else{ /* This will almost never happen with the bytecode interpreter. */
    asize_t sz;
    asize_t cur_ptr = tbl->ptr - tbl->base;

    tbl->size *= 2;
    sz = (tbl->size + tbl->reserve) * sizeof (value*);
    caml_gc_log ("Growing ref_table to %"
                 ARCH_INTNAT_PRINTF_FORMAT "dk bytes\n",
                     (intnat) sz/1024);
    tbl->base = (value**) caml_stat_resize ((char *) tbl->base, sz);
    if (tbl->base == NULL){
      caml_fatal_error ("Fatal error: ref_table overflow\n");
    }
    tbl->end = tbl->base + tbl->size + tbl->reserve;
    tbl->threshold = tbl->base + tbl->size;
    tbl->ptr = tbl->base + cur_ptr;
    tbl->limit = tbl->end;
  }
}
