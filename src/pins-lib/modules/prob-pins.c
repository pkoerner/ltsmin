#include <hre/config.h>

#include <dlfcn.h>
#include <limits.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>

#include <dm/dm.h>
#include <hre/runtime.h>
#include <hre/stringindex.h>
#include <hre/unix.h>
#include <ltsmin-lib/ltsmin-standard.h>
#include <pins-lib/pins.h>
#include <pins-lib/pins-util.h>
#include <prob-lib/prob_helpers.h>
#include <prob-lib/prob_client.h>
#include <pins-lib/modules/prob-pins.h>
#include <util-lib/chunk_support.h>
#include <util-lib/util.h>


#define PROB_IS_INIT_EQUALS_FALSE_GUARD 0
#define PROB_IS_INIT_EQUALS_TRUE_GUARD  1

// is_init is a reserved state variable
static const char* IS_INIT = "is_init";

static int no_close = 0;

static char* zocket_prefix = "/tmp/ltsmin-";

typedef struct prob_context {
    size_t num_vars;
    int op_type_no;
    prob_client_t prob_client;
    int* op_type;
    int* var_type;
    char* zocket;
} prob_context_t;

static void
prob_popt(poptContext con, enum poptCallbackReason reason, const struct poptOption *opt, const char *arg, void *data)
{
    (void) con; (void) opt; (void) arg; (void) data;

    switch (reason) {
    case POPT_CALLBACK_REASON_PRE:
        break;
    case POPT_CALLBACK_REASON_POST:
        GBregisterLoader("mch", ProBstartProb);
        GBregisterLoader("eventb", ProBstartProb);
        GBregisterLoader("tla", ProBstartProb);

        GBregisterLoader("probz", ProBcreateZocket);

        Warning(info, "ProB module initialized");
        return;
    case POPT_CALLBACK_REASON_OPTION:
        break;
    }

    Abort("unexpected call to prob_popt");
}

struct poptOption prob_options[] = {
    { NULL, 0, POPT_ARG_CALLBACK | POPT_CBFLAG_POST | POPT_CBFLAG_SKIPOPTION, (void*) &prob_popt, 0, NULL, NULL },
    { "no-close", 0, POPT_ARG_NONE, &no_close, 0, "do not close the ProB connection (so that it can be reused)", NULL },
    { "zocket-prefix", 0, POPT_ARG_STRING|POPT_ARGFLAG_SHOW_DEFAULT, &zocket_prefix, 0, "use a default ZMQ socket prefix, ignored for a file with a .probz extension", NULL },
    POPT_TABLEEND };

static prob_context_t*
create_context(model_t model)
{
    prob_context_t* ctx = (prob_context_t*) RTmalloc(sizeof(prob_context_t));
    GBsetContext(model, ctx);
    return ctx;
}

static void
prob_load_model(model_t model);

void
ProBcreateZocket(model_t model, const char* file)
{
    if (HREpeers(HREglobal()) > 1) {
        if (HREme(HREglobal()) == 0) {
            Warning(error, "The \".probz\" extension is incompatible with parallelism. "
                "If you want to exploit parallelism, supply the machine file directly. "
                "Consult the manpage \"man prob2lts-mc\" for further information. "
                "If you don't need parallelism, you can also supply the --procs=1 option.");
        }
        HREabort(LTSMIN_EXIT_FAILURE);
    }
    prob_context_t* ctx = create_context(model);

    ctx->zocket = RTmalloc(sizeof(char[PATH_MAX]));
    ctx->zocket = realpath(file, ctx->zocket);

    // check file exists
    struct stat st;
    if (stat(ctx->zocket, &st) != 0) Abort("Zocket does not exist: %s", ctx->zocket);

    prob_load_model(model);
}

void
ProBstartProb(model_t model, const char* file)
{
    prob_context_t* ctx = create_context(model);

    char abs_filename[PATH_MAX];
    char* ret_filename = realpath(file, abs_filename);

    // check file exists
    struct stat st;
    if (stat(ret_filename, &st) != 0) Abort("File does not exist: %s", file);

    const char* zocket = "%s%d.probz";
    ctx->zocket = RTmalloc(snprintf(NULL, 0, zocket, zocket_prefix, HREme(HREglobal())) + 1);
    sprintf(ctx->zocket, zocket, zocket_prefix, HREme(HREglobal()));

    if (stat(ctx->zocket, &st) == 0) Abort("File already exists at %s", ctx->zocket);

    Warning(info, "Creating zocket for HRE worker %d at %s", HREme(HREglobal()), ctx->zocket);

    const char* command = "probcli %s -ltsmin2 %s &";
    char buf[snprintf(NULL, 0, command, ret_filename, ctx->zocket) + 1];
    sprintf(buf, command, ret_filename, ctx->zocket);

    if (system(buf) != 0) Abort("Could not launch 'probcli', make sure it is on your PATH");

    prob_load_model(model);
}

static ProBState
pins2prob_state(model_t model, int* pins)
{
    prob_context_t* ctx = (prob_context_t*) GBgetContext(model);

    ProBState prob;

    prob.size = ctx->num_vars;
    prob.chunks = RTmalloc(sizeof(ProBChunk) * prob.size);

    Debugf("pins2prob state (%d): ", ctx->num_vars);
    for (size_t i = 0; i < ctx->num_vars; i++) {
        chunk c = pins_chunk_get (model, ctx->var_type[i], pins[i]);

        prob.chunks[i].data = c.data;
        prob.chunks[i].size = c.len;
        Debugf("(%u)", c.len);
#ifdef LTSMIN_DEBUG
        for (unsigned int j = 0; j < c.len; j++) Debugf("%x", c.data[j]);
#endif

        Debugf(",");
    }
    Debugf("\n");

    return prob;
}

static void
prob2pins_state(ProBState s, int *state, model_t model)
{
    prob_context_t* ctx = (prob_context_t*) GBgetContext(model);
    HREassert(s.size == ctx->num_vars, "expecting %zu chunks, but got %zu", ctx->num_vars, s.size);

    Debugf("prob2pins state (%zu): ", s.size);
    for (size_t i = 0; i < s.size; i++) {
        chunk c;
        c.data = s.chunks[i].data;
        c.len = s.chunks[i].size;
        Debugf("(%u)", c.len);
#ifdef LTSMIN_DEBUG
        for (unsigned int j = 0; j < c.len; j++) Debugf("%x", c.data[j]);
#endif
        Debugf(",");
        int chunk_id = pins_chunk_put (model, ctx->var_type[i], c);
        state[i] = chunk_id;
    }
    Debugf("\n");
}

static int
get_successors_long(model_t model, int group, int *src, TransitionCB cb, void *ctx)
{
    prob_context_t* prob_ctx = (prob_context_t*) GBgetContext(model);

    /* Don't give any successors for the init group if we have already initialized.
     * This prevents adding a self loop to the initial state. */
    if (group == 0 && src[prob_ctx->num_vars] == 1) return 0;

    // Don't give any successors for groups other than the init group if we have not initialized
    if (group > 0 && src[prob_ctx->num_vars] == 0) return 0;

    int operation_type = prob_ctx->op_type_no;

    chunk op_name = pins_chunk_get (model, operation_type, prob_ctx->op_type[group]);

    ProBState prob = pins2prob_state(model, src);

    int nr_successors;
    ProBState *successors = prob_next_state(prob_ctx->prob_client, prob, op_name.data, &nr_successors);
    prob_destroy_state(&prob);

    int s[prob_ctx->num_vars + 1];
    s[prob_ctx->num_vars] = 1;
    for (int i = 0; i < nr_successors; i++) {

        int transition_labels[1] = { prob_ctx->op_type[group] };
        transition_info_t transition_info = { transition_labels, group, 0 };

        prob2pins_state(successors[i], s, model);
        prob_destroy_state(successors + i);
        cb(ctx, &transition_info, s, NULL);
    }

    RTfree(successors);

    return nr_successors;
}

void
prob_exit(model_t model)
{
    prob_context_t* ctx = (prob_context_t*) GBgetContext(model);

    if (!no_close) {
        Warning(info, "terminating ProB connection");
        prob_terminate(ctx->prob_client);
        Warning(info, "disconnecting from zocket %s", prob_get_zocket(ctx->prob_client));
        prob_disconnect(ctx->prob_client);
        prob_client_destroy(ctx->prob_client);
    }
}



static int
get_state_label_long(model_t model, int label, int *src) {
    prob_context_t* prob_ctx = (prob_context_t*) GBgetContext(model);
    ProBState prob = pins2prob_state(model, src);
    lts_type_t ltstype = GBgetLTStype(model);
    char *label_s = lts_type_get_state_label_name(ltstype, label);
    int res = prob_get_state_label(prob_ctx->prob_client, prob, label_s);
    //prob_destroy_state(&prob);
    return res;
}


static void setup_state_labels(model_t model,
                               ProBInitialResponse init,
                               string_index_t si_guards,
                               lts_type_t ltstype,
                               int bool_type,
                               int guard_type) {
    // init state labels
    const int sl_inv_size = init.state_labels.nr_rows;
    const int sl_guards_size = init.guard_labels.nr_rows + 2; // two special guards
    const int sl_size = sl_inv_size + sl_guards_size;
    lts_type_set_state_label_count(ltstype, sl_size);

    // set up two special guards
    lts_type_set_state_label_name(ltstype, PROB_IS_INIT_EQUALS_FALSE_GUARD, "guard_is_init==0");
    lts_type_set_state_label_typeno(ltstype, PROB_IS_INIT_EQUALS_FALSE_GUARD, guard_type);

    lts_type_set_state_label_name(ltstype, PROB_IS_INIT_EQUALS_TRUE_GUARD,  "guard_is_init==1");
    lts_type_set_state_label_typeno(ltstype, PROB_IS_INIT_EQUALS_TRUE_GUARD, guard_type);

    for (int i = 2; i < sl_guards_size; i++) { // move all other guards by two
        lts_type_set_state_label_name(ltstype, i, init.guard_labels.rows[i-2].transition_group.data + 2); // skip the 'DA'
        // guards will be known as 'guard_X'
        lts_type_set_state_label_typeno(ltstype, i, guard_type);
        SIputAt(si_guards, init.guard_labels.rows[i-2].transition_group.data, i);
    }

    for (int i = 0; i < sl_inv_size; i++) {
        // the state label is actually saved in the field named transition_group
        lts_type_set_state_label_name(ltstype, i + sl_guards_size, init.state_labels.rows[i].transition_group.data + 2); // skip the 'DA'
        // invariants will be known as 'invX'
        lts_type_set_state_label_typeno(ltstype, i + sl_guards_size, bool_type);
    }

    sl_group_t *sl_group_all = RTmallocZero(sizeof(sl_group_t) + sl_size * sizeof(int));
    sl_group_all->count = sl_size;
    for (int i = 0; i < sl_group_all->count; i++) {
        sl_group_all->sl_idx[i] = i;
    }
    GBsetStateLabelGroupInfo(model, GB_SL_ALL, sl_group_all);
    if (sl_guards_size > 2) {
        sl_group_t *sl_group_guard = RTmallocZero(sizeof(sl_group_t) + sl_guards_size * sizeof(int));
        sl_group_guard->count = sl_guards_size;
        for (int i = 0; i < sl_group_guard->count; i++) {
            sl_group_guard->sl_idx[i] = i;
        }
        GBsetStateLabelGroupInfo(model, GB_SL_GUARDS, sl_group_guard);
    }

}


static void setup_variables(prob_context_t *ctx,
                            ProBInitialResponse init,
                            string_index_t var_si,
                            lts_type_t ltstype,
                            int bool_type) {
    ctx->num_vars = init.variables.size;

    lts_type_set_state_length(ltstype, ctx->num_vars + 1);

    /* One state variable is artificial to make sure the DA$INIT_STATE group
     * can only fire when we are in the initial state. This is necessary when
     * doing backward reachability. */
    lts_type_set_state_name(ltstype, ctx->num_vars, IS_INIT);
    lts_type_set_state_typeno(ltstype, ctx->num_vars, bool_type);

    ctx->var_type = RTmalloc(sizeof(int[ctx->num_vars]));

    for (size_t i = 0; i < ctx->num_vars; i++) {
        const char* type = init.variable_types.chunks[i].data;

        HREassert(type != NULL, "invalid type name");

        int is_new = 0;
        const int type_no = lts_type_add_type(ltstype, type, &is_new);
        ctx->var_type[i] = type_no;

        if (is_new) lts_type_set_format(ltstype, type_no, LTStypeChunk);

        const char* name = init.variables.chunks[i].data;
        if (strcmp(name, IS_INIT) == 0) {
            Abort("State variable name \"%s\" is reserved, please use another name", name);
        }
        SIputAt(var_si, name, i);

        lts_type_set_state_name(ltstype, i, name);
        lts_type_set_state_typeno(ltstype, i, type_no);
    }
}

static int setup_transition_groups(model_t model,
                                   prob_context_t *ctx,
                                   ProBInitialResponse init,
                                   string_index_t op_si) {
    const int num_groups = init.transition_groups.size;

    ctx->op_type = RTmalloc(sizeof(int[num_groups]));

    for (int i = 0; i < num_groups; i++) {
        const char* name = init.transition_groups.chunks[i].data;
        const int at = pins_chunk_put (model, ctx->op_type_no, chunk_str(name));
        ctx->op_type[i] = at;
        SIputAt(op_si,name,i);
    }
    return num_groups;
}


static void setup_guard_info(model_t model,
                             int num_groups,
                             ProBInitialResponse init,
                             string_index_t op_si,
                             string_index_t si_guards) {
    guard_t **guard_info = RTmalloc(num_groups * sizeof(guard_t*));
    for (int i = 0; i < num_groups; i++) {
        int idx_transition_group = SIlookup(op_si, init.guard_info.rows[i].transition_group.data);
        guard_info[idx_transition_group] = RTmalloc(sizeof(int) * (init.guard_info.nr_rows +1));
        guard_info[idx_transition_group]->count = init.guard_info.rows[i].variables.size + 1;

        // additionally set is_init == false or is_init == true as a guard
        if (idx_transition_group == 0) {
            // just making sure
            assert(0 == strcmp(init.guard_info.rows[i].transition_group.data, "DA$init_state"));
            guard_info[idx_transition_group]->guard[0] = PROB_IS_INIT_EQUALS_FALSE_GUARD; 
        } else {
            guard_info[idx_transition_group]->guard[0] = PROB_IS_INIT_EQUALS_TRUE_GUARD; 
        }

        for (size_t j = 0; j < init.guard_info.rows[i].variables.size; j++) {
            int idx_guard = SIlookup(si_guards, init.guard_info.rows[i].variables.chunks[j].data);
            guard_info[idx_transition_group]->guard[j + 1] = idx_guard;
        }
    }
    SIdestroy(&si_guards);
    GBsetGuardsInfo(model, guard_info);
}

static void setup_state_label_info(model_t model,
                                   prob_context_t *ctx, 
                                   ProBInitialResponse init,
                                   string_index_t var_si) {
    const int sl_inv_size = init.state_labels.nr_rows;
    const int sl_guards_size = init.guard_labels.nr_rows + 2; // two special guards (see below)
    const int sl_size = sl_inv_size + sl_guards_size;

    matrix_t* sl_info = RTmalloc(sizeof(matrix_t));
    dm_create(sl_info, sl_size, ctx->num_vars + 1);

    // two special guards:
    // first guard is is_init == 0 depends on is_init, which is the last variable in the list
    dm_set(sl_info, PROB_IS_INIT_EQUALS_FALSE_GUARD, ctx->num_vars);
    // second guard is is_init == 1 depends on is_init, and depends on the same variable
    dm_set(sl_info, PROB_IS_INIT_EQUALS_TRUE_GUARD, ctx->num_vars);

    for (int i = 2; i < sl_guards_size; i++) {
        // all other guards move up two slots
        for (size_t j = 0; j < init.guard_labels.rows[i-2].variables.size; j++) {
            const char* var = init.guard_labels.rows[i-2].variables.chunks[j].data;
            const int col = SIlookup(var_si, var);
            dm_set(sl_info, i, col); 
        }
    }

    for (int i = 0; i < sl_inv_size; i++) {
        for (size_t j = 0; j < init.state_labels.rows[i].variables.size; j++) {
            const char* var = init.state_labels.rows[i].variables.chunks[j].data;
            const int col = SIlookup(var_si, var);
            dm_set(sl_info, i+sl_guards_size, col);
        }
    }
    GBsetStateLabelInfo(model, sl_info);
}

static void setup_read_write_matrices(model_t model,
                                      prob_context_t *ctx,
                                      int num_groups,
                                      ProBInitialResponse init,
                                      string_index_t var_si,
                                      string_index_t op_si) {
    matrix_t* must_write = RTmalloc(sizeof(matrix_t));
    matrix_t* read = RTmalloc(sizeof(matrix_t));
    matrix_t* dm = RTmalloc(sizeof(matrix_t));
    dm_create(must_write, num_groups, ctx->num_vars + 1);
    dm_create(read, num_groups, ctx->num_vars + 1);

    GBsetDMInfoMustWrite(model, must_write);
    GBsetDMInfoRead(model, read);

    GBsetDMInfo(model, dm);



    // set all variables for init group to write dependent
    for (size_t i = 0; i < ctx->num_vars + 1; i++) dm_set(must_write, 0, i);
    // also set the init var to read dependent for all groups
    for (int i = 0; i < num_groups; i++) dm_set(read, i, ctx->num_vars);

    for (size_t i = 0; i < init.must_write.nr_rows; i++) {
        const char* name = init.must_write.rows[i].transition_group.data;
        const int vars = init.must_write.rows[i].variables.size;
        const int row = SIlookup(op_si, name);
        for (int j = 0; j < vars; j++) {
            const char* var = init.must_write.rows[i].variables.chunks[j].data;
            const int col = SIlookup(var_si, var);
            dm_set(must_write, row, col);
        }
    }

    for (size_t i = 0; i < init.may_write.nr_rows; i++) {
        const char* name = init.may_write.rows[i].transition_group.data;
        const int vars = init.may_write.rows[i].variables.size;
        const int row = SIlookup(op_si, name);
        for (int j = 0; j < vars; j++) {
            const char* var = init.may_write.rows[i].variables.chunks[j].data;
            const int col = SIlookup(var_si, var);
            dm_set(must_write, row, col);
            dm_set(read, row, col);
        }
    }

    for (size_t i = 0; i < init.reads_action.nr_rows; i++) {
        const char* name = init.reads_action.rows[i].transition_group.data;
        const int vars = init.reads_action.rows[i].variables.size;
        const int row = SIlookup(op_si, name);
        for (int j = 0; j < vars; j++) {
            const char* var = init.reads_action.rows[i].variables.chunks[j].data;
            const int col = SIlookup(var_si, var);
            dm_set(read, row, col);
        }
    }

    for (size_t i = 0; i < init.reads_guard.nr_rows; i++) {
        const char* name = init.reads_guard.rows[i].transition_group.data;
        const int vars = init.reads_guard.rows[i].variables.size;
        const int row = SIlookup(op_si, name);
        for (int j = 0; j < vars; j++) {
            const char* var = init.reads_guard.rows[i].variables.chunks[j].data;
            const int col = SIlookup(var_si, var);
            dm_set(read, row, col);
        }
    }

    dm_copy(must_write, dm);
    dm_apply_or(dm, read);

}

static void
prob_load_model(model_t model)
{
    Warning(info, "ProB init");

    prob_context_t* ctx = (prob_context_t*) GBgetContext(model);

    ctx->prob_client = prob_client_create();

    const char* ipc = "ipc://";
    char zocket[strlen(ipc) + strlen(ctx->zocket) + 1];
    sprintf(zocket, "%s%s", ipc, ctx->zocket);
    RTfree(ctx->zocket);

    Warning(info, "connecting to zocket %s", zocket);
    prob_connect(ctx->prob_client, zocket);

    ProBInitialResponse init = prob_init(ctx->prob_client);

    lts_type_t ltstype = lts_type_create();
    int bool_is_new, bool_type   = lts_type_add_type (ltstype, LTSMIN_TYPE_BOOL, &bool_is_new);
    int guard_is_new, guard_type = lts_type_add_type (ltstype, LTSMIN_TYPE_GUARD, &guard_is_new);

    // add an "Operation" type for edge labels
    int is_new = 0;
    ctx->op_type_no = lts_type_add_type(ltstype, "Operation", &is_new);
    if (!is_new) Abort("Can not add type");
    lts_type_set_format(ltstype, ctx->op_type_no, LTStypeChunk);

    lts_type_set_edge_label_count(ltstype, 1);
    lts_type_set_edge_label_name(ltstype, 0, "action");
    lts_type_set_edge_label_typeno(ltstype, 0, ctx->op_type_no);

    string_index_t si_guards = SIcreate();
    setup_state_labels(model, init, si_guards, ltstype, bool_type, guard_type);


    string_index_t var_si = SIcreate();
    setup_variables(ctx, init, var_si, ltstype, bool_type);


    // done with ltstype
    lts_type_validate(ltstype);

    // make sure to set the lts-type before anything else in the GB
    GBsetLTStype(model, ltstype);

    if (bool_is_new) {
        pins_chunk_put_at (model, bool_type, chunk_str(LTSMIN_VALUE_BOOL_FALSE), 0);
        pins_chunk_put_at (model, bool_type, chunk_str(LTSMIN_VALUE_BOOL_TRUE ), 1);
    }

    if (guard_is_new) {
        pins_chunk_put_at (model, guard_type, chunk_str(LTSMIN_VALUE_GUARD_FALSE), 0);
        pins_chunk_put_at (model, guard_type, chunk_str(LTSMIN_VALUE_GUARD_TRUE ), 1);
        pins_chunk_put_at (model, guard_type, chunk_str(LTSMIN_VALUE_GUARD_MAYBE), 2);
    }


    string_index_t op_si = SIcreate();

    const int num_groups = setup_transition_groups(model, ctx, init, op_si);

    setup_guard_info(model, num_groups, init, op_si, si_guards);

    setup_state_label_info(model, ctx, init, var_si);


    setup_read_write_matrices(model, ctx, num_groups, init, var_si, op_si);

    int init_state[ctx->num_vars + 1];
    prob2pins_state(init.initial_state, init_state, model);
    init_state[ctx->num_vars] = 0;
    GBsetInitialState(model, init_state);

    prob_destroy_initial_response(&init);

    GBsetNextStateLong(model, get_successors_long);
    GBsetStateLabelLong(model, get_state_label_long);

    GBsetExit(model, prob_exit);

    SIdestroy(&var_si);
    SIdestroy(&op_si);
}
