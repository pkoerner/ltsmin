#include <hre/config.h>


#include <stdlib.h>

#include <dm/dm.h>
#include <hre/user.h>
#include <pins-lib/pins-util.h>


size_t
pins_get_state_label_count (model_t model)
{
    lts_type_t          ltstype = GBgetLTStype (model);
    return lts_type_get_state_label_count (ltstype);
}

size_t
pins_get_edge_label_count (model_t model)
{
    lts_type_t          ltstype = GBgetLTStype (model);
    return lts_type_get_edge_label_count (ltstype);
}

size_t
pins_get_state_variable_count (model_t model)
{
    lts_type_t          ltstype = GBgetLTStype (model);
    return lts_type_get_state_length (ltstype);
}

size_t
pins_get_group_count (model_t model)
{
    matrix_t           *write_info = GBgetDMInfo (model);
    return dm_nrows (write_info);
}

void
pins_add_edge_label_visible (model_t model, int act_label, int act_index)
{
    int *visibles = GBgetPorGroupVisibility (model);
    HREassert (visibles != NULL, "pins_add_edge_label_visible: No (lower) PINS layer uses POR visibility.");

    int* groups_of_edge = NULL;
    const int n = GBgroupsOfEdge(model, act_label, act_index, groups_of_edge);
    if (n > 0) {
        for (int i = 0; i < n; i++) {
            visibles[groups_of_edge[i]] = 1;
        }
        RTfree(groups_of_edge);
    } else {
        chunk c = GBchunkGet(model, lts_type_get_edge_label_typeno(GBgetLTStype(model), act_label), act_index);
        char s[c.len];
        chunk2string(c, c.len, s);
        Abort("There is no group that can produce edge \"%s\"", s);

    }
}

void
pins_add_state_variable_visible (model_t model, int index)
{
    int                *visible = GBgetPorGroupVisibility(model);
    HREassert (visible != NULL, "pins_add_state_variable_visible: No (lower) PINS layer uses POR visibility.");
    matrix_t           *wr_info = GBgetDMInfoMayWrite (model);
    int                 ngroups = dm_nrows (wr_info);
    for (int i = 0; i < ngroups; i++) {
        if (dm_is_set(wr_info, i, index)) {
            visible[i] = 1;
        }
    }
}

void
pins_add_state_label_visible (model_t model, int index)
{
    int                *visible = GBgetPorStateLabelVisibility(model);
    HREassert (visible != NULL, "pins_add_state_label_visible: No (lower) PINS layer uses POR visibility.");
    visible[index] = 1;
}
