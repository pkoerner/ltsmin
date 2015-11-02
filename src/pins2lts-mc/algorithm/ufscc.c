/**
 *
 */

#include <hre/config.h>

#include <pins2lts-mc/algorithm/algorithm.h>
#include <pins2lts-mc/algorithm/util.h>
#include <pins2lts-mc/parallel/permute.h>
#include <pins2lts-mc/parallel/state-info.h>
#include <pins2lts-mc/parallel/worker.h>
#include <ltsmin-lib/ltsmin-standard.h>
#include <mc-lib/unionfind.h>
#include <pins-lib/pins-util.h>
#include <pins-lib/pins.h>
#include <util-lib/fast_set.h>
#include <pins2lts-mc/algorithm/ltl.h>


#if SEARCH_COMPLETE_GRAPH
#include <mc-lib/dlopen_extra.h>
#endif

#if HAVE_PROFILER
#include <gperftools/profiler.h>
#endif

//#define LTL_CHECK 1
//#define PRINT_GRAPH 1

#if PRINT_GRAPH
int time_tick;
int MAX_TIME_TICKS = 10000;
#endif

// TODO: move +1's and -1's inside the UF structure!

/**
 * local counters
 */
typedef struct counter_s {
    uint32_t            scc_count;
    uint32_t            unique_states;
    uint32_t            unique_trans;
    uint32_t            selfloop;
    uint32_t            claimdead;
    uint32_t            claimfound;
    uint32_t            claimsuccess;
#if LTL_CHECK
    uint32_t            accepting;
#endif
} counter_t;

/**
 * local SCC information (for each worker)
 */
struct alg_local_s {
    dfs_stack_t         search_stack;         // search stack (D)
    dfs_stack_t         roots_stack;          // roots stack (R)
    counter_t           cnt;
    state_info_t       *target;               // auxiliary state
    state_info_t       *root;                 // auxiliary state
    wctx_t             *rctx;                 // reachability for trace construction
};


/**
 * shared SCC information (between workers)
 */
typedef struct uf_alg_shared_s {
    uf_t               *uf;             // shared union-find structure
    ref_t               lasso_acc;      // SCC root for trace construction
    ref_t               lasso_end;      // last on lasso (to iterate backwards from)
    ref_t               lasso_root;     // last on lasso (to iterate backwards from)
    run_t              *reach_run;      // parallel reachability object
    bool                ltl;            // LTL property present?
} uf_alg_shared_t;

extern void report_lasso (wctx_t *ctx, ref_t accepting);
extern int reach_scc_seen (void *ext_ctx, transition_info_t *ti,
                           ref_t ref, int seen);

void
ufscc_global_init (run_t *run, wctx_t *ctx)
{
    (void) run; (void) ctx;
}


void
ufscc_global_deinit (run_t *run, wctx_t *ctx)
{
    (void) run; (void) ctx;
}


void
ufscc_local_init (run_t *run, wctx_t *ctx)
{
    ctx->local = RTmallocZero (sizeof (alg_local_t) );
    uf_alg_shared_t    *shared = (uf_alg_shared_t*) ctx->run->shared;

    ctx->local->target = state_info_create ();
    ctx->local->root   = state_info_create ();

    size_t len               = state_info_serialize_int_size (ctx->state);
    ctx->local->search_stack = dfs_stack_create (len);
    ctx->local->roots_stack  = dfs_stack_create (len);

    ctx->local->cnt.scc_count               = 0;
    ctx->local->cnt.unique_states           = 0;
    ctx->local->cnt.unique_trans            = 0;
    ctx->local->cnt.selfloop                = 0;
    ctx->local->cnt.claimdead               = 0;
    ctx->local->cnt.claimfound              = 0;
    ctx->local->cnt.claimsuccess            = 0;
#if LTL_CHECK
    ctx->local->cnt.accepting               = 0;
#endif

    shared->ltl = GBgetAcceptingStateLabelIndex(ctx->model) != -1;

    if (shared->ltl && trc_output) {
        ctx->local->rctx = run_init (shared->reach_run, ctx->model);
        ctx->local->rctx->parent = ctx;
    }

#if SEARCH_COMPLETE_GRAPH
    dlopen_setup (files[0]);
#endif

#if PRINT_GRAPH
    time_tick = 0;
#endif

    (void) run; 
}


void
ufscc_local_deinit   (run_t *run, wctx_t *ctx)
{
    alg_local_t        *loc = ctx->local;

    dfs_stack_destroy (loc->search_stack);
    dfs_stack_destroy (loc->roots_stack);
    RTfree (loc);
    (void) run;
}


static void
ufscc_handle (void *arg, state_info_t *successor, transition_info_t *ti,
              int seen)
{
    wctx_t             *ctx       = (wctx_t *) arg;
    alg_local_t        *loc       = ctx->local;
    raw_data_t          stack_loc;

    ctx->counters->trans++;

    // self-loop
    if (ctx->state->ref == successor->ref) {
        loc->cnt.selfloop ++;
#if LTL_CHECK
        if ( GBbuchiIsAccepting(ctx->model, state_info_state(successor) ) ) {
            ndfs_report_cycle (ctx->run, ctx->model, loc->search_stack, successor);
        }
#endif
        return;
    } else if (EXPECT_FALSE(trc_output && !seen && ti != &GB_NO_TRANSITION)) {
        // use parent_ref from reachability (used in CE reconstuction)
        ref_t *succ_parent = get_parent_ref(loc->rctx, successor->ref);
        atomic_write (succ_parent, ctx->state->ref);
    }

    stack_loc = dfs_stack_push (loc->search_stack, NULL);
    state_info_serialize (successor, stack_loc);

    (void) ti;
}


#if SEARCH_COMPLETE_GRAPH
/**
 * bypasses pins to directly handle the successor
 * assumes that we only require state->ref
 */
static inline void
permute_complete (void *arg, transition_info_t *ti, state_data_t dst, int *cpy)
{
    wctx_t             *ctx        = (wctx_t *) arg;
    alg_local_t        *loc        = ctx->local;

    loc->target->ref = (ref_t) dst[0];
    ufscc_handle (ctx, loc->target, ti, 0);

    (void) cpy;
}
#endif


/**
 * make a stackframe on search_stack and handle the successors of ctx->state
 * also pushes ctx->state on the roots stack
 */
static inline size_t
explore_state (wctx_t *ctx)
{
    alg_local_t        *loc       = ctx->local;
    raw_data_t          stack_loc;
    size_t              trans;

    // push the state on the roots stack
    state_info_set (loc->root,  ctx->state->ref, LM_NULL_LATTICE);
    stack_loc = dfs_stack_push (loc->roots_stack, NULL);
    state_info_serialize (loc->root, stack_loc);

    increase_level (ctx->counters);
    dfs_stack_enter (loc->search_stack);

#if SEARCH_COMPLETE_GRAPH
    // bypass the pins interface by directly handling the successors
    int                 ref_arr[2];
    ref_arr[0] =  (int) ctx->state->ref;
    trans = dlopen_next_state (NULL, 0, ref_arr, permute_complete, ctx);
#else
    trans = permute_trans (ctx->permute, ctx->state, ufscc_handle, ctx);
#endif

    ctx->counters->explored ++;
    run_maybe_report1 (ctx->run, ctx->counters, "");

    return trans;
}


static void
ufscc_init  (wctx_t *ctx)
{
    alg_local_t        *loc    = ctx->local;
    uf_alg_shared_t    *shared = (uf_alg_shared_t*) ctx->run->shared;
    transition_info_t   ti     = GB_NO_TRANSITION;
    char                claim;
    size_t              transitions;
    raw_data_t          state_data;

#if SEARCH_COMPLETE_GRAPH
    ufscc_handle (ctx, loc->target, &ti, 0);
    claim = uf_make_claim (shared->uf, loc->target->ref + 1, ctx->id);
#else
    ufscc_handle (ctx, ctx->initial, &ti, 0);
    claim = uf_make_claim (shared->uf, ctx->initial->ref + 1, ctx->id);
#endif
    
    // explore the initial state
    state_data = dfs_stack_top (loc->search_stack);
    state_info_deserialize (ctx->state, state_data); // search_stack TOP
    transitions = explore_state(ctx);

    loc->cnt.claimsuccess ++;
    if (claim == CLAIM_FIRST) {
        loc->cnt.unique_states ++;
        loc->cnt.unique_trans += transitions;
#if PRINT_GRAPH
    if (time_tick < MAX_TIME_TICKS) {
        time_tick ++;
        printf("NODE,%zu,%zu,%d\n",
               uf_find(shared->uf,ctx->state->ref+1), ctx->id, time_tick);
    }
#endif

#if LTL_CHECK
        if ( GBbuchiIsAccepting(ctx->model, state_info_state(ctx->state) ) ) {
            loc->cnt.accepting ++;
        }
#endif
    }
}


/**
 * uses the top state from search_stack (== ctx->state) and explores it
 */
static inline void
successor (wctx_t *ctx)
{
    alg_local_t        *loc        = ctx->local;
    uf_alg_shared_t    *shared     = (uf_alg_shared_t*) ctx->run->shared;
    raw_data_t          state_data, root_data;
    char                claim;
    size_t              trans;

    // get the parent state from the search_stack
    state_data = dfs_stack_peek_top (loc->search_stack, 1);
    state_info_deserialize (loc->target, state_data);

    // edge : FROM -> TO
    // FROM = parent    = loc->target
    // TO   = successor = ctx->state

    if (loc->target->ref == ctx->state->ref) { // self loop

        loc->cnt.claimfound ++; // implies CLAIM_FOUND
        if (shared->ltl && GBbuchiIsAccepting(ctx->model, state_info_state(ctx->state))) {
            report_lasso (ctx, ctx->state->ref);
        }

        dfs_stack_pop (loc->search_stack);
        return;
    }

    // early backtrack if parent is explored ==> all its children are explored
    if ( !uf_is_in_list (shared->uf, loc->target->ref + 1) ) {
        dfs_stack_pop (loc->search_stack);
        return;
    }

    // make claim:
    // - CLAIM_FIRST   (initialized)
    // - CLAIM_SUCCESS (LIVE state and we have NOT yet visited its SCC)
    // - CLAIM_FOUND   (LIVE state and we have visited its SCC before)
    // - CLAIM_DEAD    (DEAD state)
    claim = uf_make_claim (shared->uf, ctx->state->ref + 1, ctx->id);

#if PRINT_GRAPH
    if (time_tick < MAX_TIME_TICKS) {
        time_tick ++;
        if (claim == CLAIM_FIRST) {
            printf("NODE,%zu,%zu,%d\n",
                    uf_find(shared->uf,ctx->state->ref+1), ctx->id, time_tick);
        }
        printf("EDGE,%zu,%zu,%zu,%d\n",
                uf_find(shared->uf,loc->target->ref+1),
                uf_find(shared->uf,ctx->state->ref+1),
                ctx->id,
                time_tick);
    }
#endif

    if (claim == CLAIM_DEAD) {
        // (TO == DEAD) ==> get next successor
        loc->cnt.claimdead ++;
        dfs_stack_pop (loc->search_stack);
        return;
    }

    else if (claim == CLAIM_SUCCESS || claim == CLAIM_FIRST) {
        // (TO == 'new' state) ==> 'recursively' explore
        loc->cnt.claimsuccess ++;

        trans = explore_state (ctx);

        if (claim == CLAIM_FIRST) {
            loc->cnt.unique_states ++;
            loc->cnt.unique_trans += trans;
#if LTL_CHECK
            if ( GBbuchiIsAccepting(ctx->model, state_info_state(ctx->state) ) ) {
                loc->cnt.accepting ++;
            }
#endif
        }
        return;
    }

    else  { // result == CLAIM_FOUND
        // (TO == state in previously visited SCC) ==> cycle found
        loc->cnt.claimfound ++;

        Debug ("cycle: %zu  --> %zu", loc->target->ref, ctx->state->ref);

        if (uf_sameset (shared->uf, loc->target->ref + 1, ctx->state->ref + 1)) {
            dfs_stack_pop (loc->search_stack);
            return; // also no chance of new accepting cycle
        }

        // we have:  .. -> TO  -> .. -> FROM -> TO
        // where D = .. -> TO  -> .. -> FROM
        // and   R = .. -> TO* -> .. -> FROM  (may have fewer states than D)
        //                                    (TO* is a state in SCC(TO))
        // ==> unite and pop states from R until sameset (R.TOP, TO)

        // while ( not sameset (FROM, TO) )
        //   Union (R.POP(), FROM)
        // R.PUSH (TO')
        ref_t               accepting = DUMMY_IDX;
        do {
            root_data = dfs_stack_pop (loc->roots_stack); // UF Stack POP
            state_info_deserialize (loc->root, root_data); // roots_stack TOP

            if (shared->ltl && GBbuchiIsAccepting(ctx->model, state_info_state(loc->root))) {
                accepting = loc->root->ref;
            }
            Debug ("Uniting: %zu and %zu", loc->root->ref, ctx->state->ref);
#if PRINT_GRAPH
            ref_t old_a = uf_find(shared->uf, loc->root->ref + 1);
            ref_t old_b = uf_find(shared->uf, loc->target->ref + 1);
#endif
            uf_union (shared->uf, loc->root->ref + 1, loc->target->ref + 1);
#if PRINT_GRAPH
            if (time_tick < MAX_TIME_TICKS) {
                time_tick ++;
                printf("UNION,%zu,%zu,%zu,%zu,%d\n",
                        uf_find(shared->uf,loc->root->ref + 1),
                        old_a,
                        old_b,
                        ctx->id,
                        time_tick);
            }
#endif
        } while ( !uf_sameset (shared->uf, loc->target->ref + 1, ctx->state->ref + 1) );
        dfs_stack_push (loc->roots_stack, root_data);

        // after uniting SCC, report lasso
        if (accepting != DUMMY_IDX) {
            report_lasso (ctx, accepting);
        }

        // cycle is now merged (and DFS stack is unchanged)
        dfs_stack_pop (loc->search_stack);
        return;
    }
}

/**
 * there are no states on the current stackframe. We leave the stackframe and
 * check if the previous state (parent) is part of the same SCC. If this is not
 * the case, then we use pick_from_list to find a new state in the same SCC. If
 * we find one, it gets pushed on the search_stack (note that because of this,
 * the search_stack does not preserve the exact search paths) and it will be
 * addressed later. If no LIVE state is found in pick_from_list, then we have
 * that the SCC is complete (and marked DEAD). We then pop states from the
 * roots stack to ensure that the SCC is completely removed from the stacks.
 */
static inline void
backtrack (wctx_t *ctx)
{
    alg_local_t        *loc           = ctx->local;
    uf_alg_shared_t    *shared        = (uf_alg_shared_t*) ctx->run->shared;
    raw_data_t          state_data;
    bool                is_last_state;
    ref_t               state_picked;
    char                pick;
    raw_data_t          root_data;

    // leave the stackframe
    dfs_stack_leave (loc->search_stack);
    ctx->counters->level_cur--;

    // get the new stack top (ctx->state), which is now fully explored
    state_data = dfs_stack_top (loc->search_stack);
    state_info_deserialize (ctx->state, state_data);

    // remove the fully explored state from the search_stack
    dfs_stack_pop (loc->search_stack);

    // remove ctx->state from the list
    // (no other workers have to explore this state anymore)
    uf_remove_from_list (shared->uf, ctx->state->ref + 1);

    // store the previous state (from the removed one) in loc->target
    is_last_state = (0 == dfs_stack_nframes (loc->search_stack) );
    if (!is_last_state) {
        state_data = dfs_stack_peek_top (loc->search_stack, 1);
        state_info_deserialize (loc->target, state_data);
    }

    // check if previous state is part of the same SCC
    // - if so: standard backtrack (we don't need to report an SCC)
    // - else:  use pick_from_list to determine if the SCC is completed
    if ( !is_last_state
         && uf_sameset (shared->uf, loc->target->ref + 1, ctx->state->ref + 1) ) {
        return; // backtrack in same set
    }

    // ctx->state is the last KNOWN state in its SCC (according to this worker)
    // ==> check if we can find another one with pick_from_list
    pick = uf_pick_from_list (shared->uf, ctx->state->ref + 1, &state_picked);

    if (pick != PICK_SUCCESS) {
        // list is empty ==> SCC is completely explored

        // were we the one that marked it dead?
        if (pick == PICK_MARK_DEAD) {
            loc->cnt.scc_count ++;
        }

        // the SCC of the initial state has been marked DEAD ==> we are done!
        if (is_last_state) {
            return;
        }

        // we may still have states on the stack of this SCC
        if ( uf_sameset (shared->uf, loc->target->ref + 1, ctx->state->ref + 1) ) {
            return; // backtrack in same set 
            // (the state got marked dead AFTER the previous sameset check)
        }

        // pop states from Roots until !sameset (v, Roots.TOP)
        //  since Roots.TOP might not be the actual root of the SCC
        root_data = dfs_stack_top (loc->roots_stack);
        state_info_deserialize (loc->root, root_data); // R.TOP
        // pop from Roots
        while (uf_sameset (shared->uf, ctx->state->ref + 1, loc->root->ref + 1) ) {
            dfs_stack_pop (loc->roots_stack); // R.POP
            root_data = dfs_stack_top (loc->roots_stack);
            state_info_deserialize (loc->root, root_data); // R.TOP
        }
    }
    else {
        // Found w in List(v) ==> push w on stack and search its successors
        state_info_set (ctx->state, state_picked - 1, LM_NULL_LATTICE);
        state_data = dfs_stack_push (loc->search_stack, NULL);
        state_info_serialize (ctx->state, state_data);
        explore_state (ctx);
    }
}


void
ufscc_run  (run_t *run, wctx_t *ctx)
{
    alg_local_t            *loc = ctx->local;
    raw_data_t              state_data;

#if HAVE_PROFILER
    if (ctx->id == 0)
        Warning (info, "Using the profiler");
    ProfilerStart ("ufscc.perf");
#endif

#if SEARCH_COMPLETE_GRAPH
    int init_state = dlopen_get_worker_initial_state (ctx->id, W);
    int inits = 0;
    while (1)
    {
        inits ++;
        loc->target->ref = init_state;
#endif

    ufscc_init (ctx);

    // continue until we are done exploring the graph or interrupted
    while ( !run_is_stopped(run) ) {

        state_data = dfs_stack_top (loc->search_stack);

        if (state_data != NULL) {
            // there is a state on the current stackframe ==> explore it

            // store state in ctx->state
            state_info_deserialize (ctx->state, state_data);

            successor (ctx);
        }
        else {
            // there is no state on the current stackframe ==> backtrack

            // we are done if we backtrack from the initial state
            if (0 == dfs_stack_nframes (loc->search_stack))
                break;

            backtrack (ctx);
        }
    }

#if SEARCH_COMPLETE_GRAPH
        init_state = dlopen_get_new_initial_state (init_state);
        if (init_state == -1) {
            Warning(info, "Number of inits : %d", inits);
            break;
        }
    }
#endif

#if HAVE_PROFILER
    if (ctx->id == 0)
        Warning(info, "Done profiling");
    ProfilerStop();
#endif

    if (trc_output && run_is_stopped(run) &&
            global->exit_status != LTSMIN_EXIT_COUNTER_EXAMPLE) {
        report_lasso (ctx, DUMMY_IDX); // aid other thread in counter-example reconstruction
    }

    (void) run;
}


void
ufscc_reduce (run_t *run, wctx_t *ctx)
{
    if (run->reduced == NULL) {
        run->reduced = RTmallocZero (sizeof (counter_t));
    }
    counter_t          *reduced = (counter_t *) run->reduced;
    counter_t          *cnt     = &ctx->local->cnt;

    reduced->unique_trans           += cnt->unique_trans;
    reduced->unique_states          += cnt->unique_states;
    reduced->scc_count              += cnt->scc_count;
    reduced->selfloop               += cnt->selfloop;
    reduced->claimdead              += cnt->claimdead;
    reduced->claimfound             += cnt->claimfound;
    reduced->claimsuccess           += cnt->claimsuccess;
#if LTL_CHECK
    reduced->accepting              += cnt->accepting;
#endif
}


void
print_worker_stats (wctx_t *ctx)
{
    counter_t          *cnt     = &ctx->local->cnt;

    Warning(info, "worker states count:        %d", cnt->unique_states);
    Warning(info, "worker transitions count:   %d", cnt->unique_trans);
    Warning(info, "worker scc count:           %d", cnt->scc_count);
}


void
ufscc_print_stats   (run_t *run, wctx_t *ctx)
{
    counter_t              *reduced = (counter_t *) run->reduced;

    // SCC statistics
    Warning(info, "total scc count:            %d", reduced->scc_count);
    Warning(info, "unique states count:        %d", reduced->unique_states);
    Warning(info, "unique transitions count:   %d", reduced->unique_trans);
    Warning(info, "- self-loop count:          %d", reduced->selfloop);
    Warning(info, "- claim dead count:         %d", reduced->claimdead);
    Warning(info, "- claim found count:        %d", reduced->claimfound);
    Warning(info, "- claim success count:      %d", reduced->claimsuccess);
    Warning(info, " ");
#if LTL_CHECK
    Warning(info, "State space has %d states, %d are accepting",
            reduced->unique_states, reduced->accepting);
    Warning(info, " ");
#endif

    run_report_total (run);

    (void) ctx;
}


int
ufscc_state_seen (void *ptr, transition_info_t *ti, ref_t ref, int seen)
{
    wctx_t             *ctx    = (wctx_t *) ptr;
    uf_alg_shared_t    *shared = (uf_alg_shared_t*) ctx->run->shared;

    return uf_owner (shared->uf, ref + 1, ctx->id);
    (void) seen; (void) ti;
}


void
ufscc_shared_init   (run_t *run)
{
    uf_alg_shared_t    *shared;

    set_alg_local_init    (run->alg, ufscc_local_init);
    set_alg_global_init   (run->alg, ufscc_global_init);
    set_alg_global_deinit (run->alg, ufscc_global_deinit); 
    set_alg_local_deinit  (run->alg, ufscc_local_deinit);
    set_alg_print_stats   (run->alg, ufscc_print_stats);
    set_alg_run           (run->alg, ufscc_run);
    set_alg_reduce        (run->alg, ufscc_reduce);
    set_alg_state_seen    (run->alg, ufscc_state_seen);

    run->shared = RTmallocZero (sizeof (uf_alg_shared_t));
    shared      = (uf_alg_shared_t*) run->shared;
    shared->uf  = uf_create();

    if (trc_output) {
        // Prepare parallel reachability (should be done in shared, .i.e. global and once)
        if (strategy[1] == Strat_None) strategy[1] = Strat_DFS;
        shared->reach_run = run_create (false);
        alg_shared_init_strategy (shared->reach_run, strategy[1]);
        set_alg_state_seen (shared->reach_run->alg, reach_scc_seen);
    }
}

#define     PATH_IDX    DUMMY_IDX

static void
construct_parent_path (wctx_t *ctx, dfs_stack_t stack, ref_t from, ref_t until)
{
    alg_local_t        *loc        = ctx->local;
    raw_data_t          state_data;

    Warning (info, "\tConstructing reverse path from %zu to %zu", from, until);

    dfs_stack_clear (loc->search_stack); // no longer needed

    ref_t               x = from;
    while (true) {
        Warning (infoLong, "\tWriting %zu", x);
        state_info_set (loc->target, x, LM_NULL_LATTICE);
        state_data = dfs_stack_push (loc->search_stack, NULL);
        state_info_serialize (loc->target, state_data);

        ref_t *x_parent = get_parent_ref (loc->rctx, x);
        ref_t next_x = *x_parent;
        *x_parent = PATH_IDX; // Mark as path
        if (x == until) break;

        x = next_x;
        HREassert (x != 0 || until == 0, "Failed to find reverse path from %zu to %zu on %zu", from, until, x);
    }

    // reorder
    while (dfs_stack_size(loc->search_stack) != 0) {
        state_data = dfs_stack_pop (loc->search_stack);
        dfs_stack_push (stack, state_data);
        dfs_stack_enter (stack); // trace.h expects this
    }
}

/**
 * Builds and reports counter example (CE) trace
 *
 * There is one parallel reachability context with tracing to aid with CE
 * reconstruction (shared->reach_run and ctx->local->rctx).
 * The UFSCC maintained the parent_refs of all visited states in this
 * reachability context. So there is a parent path from 'accepting' to
 * the initial state.
 *
 * This method will complete the parent path to an accepting cycle.
 * First it collects the prefix trace to the accepting state from the parent path
 * marking the parent references with PATH_IDX.
 * Then it uses the parallel reachability context to search for a path in the
 * model from the accepting state back to the prefix path. It searches only in the
 * partial SCC which contains the accepting state and uses the permanent
 * locking of nodes in the UF structure to mark states as visited (in the
 * parallel reachility phase).
 */
void
report_lasso (wctx_t *ctx, ref_t accepting)
{
    if (no_exit) return;

    alg_local_t        *loc        = ctx->local;
    uf_alg_shared_t    *shared     = (uf_alg_shared_t*) ctx->run->shared;

    // stop other threads in UFSCC
    int master = run_stop (ctx->run);
    if (master) {
        Warning (info, " ");
        Warning (info, "Accepting cycle FOUND at depth ~%zu%s!",
                 dfs_stack_nframes(loc->search_stack), loc->target->ref == ctx->state->ref ? " (self loop)" : "");
        Warning (info, " ");
    }
    if (!trc_output) {
        global->exit_status = LTSMIN_EXIT_COUNTER_EXAMPLE;
        return;
    }

    if ( master ) {
        size_t              len = state_info_serialize_int_size (ctx->state);
        dfs_stack_t         trace_stack = dfs_stack_create (len);

        Warning (info, "Construction counter example");

        HREassert (accepting != DUMMY_IDX, "A slave thread reach the master thread code");
        shared->lasso_acc = accepting;

        // replace global state store with own visited set
        shared->lasso_end = DUMMY_IDX;

        // wait for others (parent_ref should be complete)
        HREbarrier (HREglobal());

        // Construct path from ctx->initial-> to loc->target->ref
        construct_parent_path (ctx, trace_stack, shared->lasso_acc, ctx->initial->ref);
        dfs_stack_leave (trace_stack); dfs_stack_pop (trace_stack); // pop accepting (it will be re-entered)

        // signal run prepared
        HREbarrier (HREglobal());

        Warning (info, "Performing parallel %s in SCC", key_search(strategies, strategy[1] & ~Strat_TA));

        // set root state as new initial state
        state_info_set (loc->rctx->initial, shared->lasso_acc, LM_NULL_LATTICE);
        alg_run (shared->reach_run, loc->rctx);  // parallel reachability

        HREassert (shared->lasso_end != DUMMY_IDX, "No path back to root %zu found in the SCC", shared->lasso_acc);
        HREbarrier (HREglobal());

        // use new parent ref to complete lasso
        construct_parent_path (ctx, trace_stack, shared->lasso_end, shared->lasso_acc);
        // push lasso root once more to close cycle
        construct_parent_path (ctx, trace_stack, shared->lasso_root, shared->lasso_root);
        dfs_stack_leave (trace_stack);

        double uw = cct_finalize (global->tables, "BOGUS, you should not see this string.");
        Warning (infoLong, "Parallel chunk tables under-water mark: %.2f", uw);

        Warning (info, "  ");
        Warning (info, "Writing counter example trace of length %zu", dfs_stack_size(trace_stack));
        find_and_write_dfs_stack_trace (ctx->model, trace_stack);

    } else {
        // slave

        // sync with master
        HREbarrier (HREglobal());
        // wait for leader to prepare
        HREbarrier (HREglobal());

        // set root state as new initial state
        //state_info_first (loc->rctx->initial, shared->lasso_root_state);
        state_info_set (loc->rctx->initial, shared->lasso_acc, LM_NULL_LATTICE);
        alg_run (shared->reach_run, loc->rctx);  // parallel reachability

        HREbarrier (HREglobal());
    }
    global->exit_status = LTSMIN_EXIT_COUNTER_EXAMPLE;
}

int
reach_scc_seen (void *ptr, transition_info_t *ti, ref_t ref, int seen)
{
    if (ti->group == GB_UNKNOWN_GROUP) return 1;

    wctx_t             *rctx = (wctx_t *) ptr;
    wctx_t             *ctx = rctx->parent;
    uf_alg_shared_t    *shared = (uf_alg_shared_t *) ctx->run->shared;
    ref_t               src = rctx->state->ref;
    Printf (debug, "SCC candidate state %zu --(%d)--> %zu: ", src, ti->group, ref);
    ref_t              *ref_parent = get_parent_ref (rctx, ref);

    if (atomic_read (ref_parent) == PATH_IDX) {
        shared->lasso_root = ref;
        shared->lasso_end = src;
        run_stop (shared->reach_run); // terminate reachability if cycle is found
        Printf (debug, "END\n");
        return 1;
    } else {
        if (!uf_sameset(shared->uf, ref + 1, shared->lasso_acc + 1)) {
            Printf (debug, "out\n");
            return 1; // avoid revisiting states outside of SCC
        }
        if (uf_try_grab(shared->uf, ref)) {
            *ref_parent = 0; // allow reset
            Printf (debug, "new\n");
            return 0;
        } else {
            Printf (debug, "old\n");
            return 1;
        }

    }
    (void) seen;
}