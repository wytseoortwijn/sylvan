/*
 * Copyright 2016 Tom van Dijk, Johannes Kepler University Linz
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <sylvan_config.h>

#include <assert.h>
#include <inttypes.h>
#include <math.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <sylvan.h>
#include <sylvan_int.h>

#include <sylvan_refs.h>

/* Primitives */
int
hzdd_isleaf(HZDD bdd)
{
    if (bdd == hzdd_true || bdd == hzdd_false) return 1;
    return hzddnode_isleaf(HZDD_GETNODE(bdd));
}

uint32_t
hzdd_getvar(HZDD node)
{
    return hzddnode_getvariable(HZDD_GETNODE(node));
}

HZDD
hzdd_getlow(HZDD hzdd)
{
    return hzddnode_low(hzdd, HZDD_GETNODE(hzdd));
}

HZDD
hzdd_gethigh(HZDD hzdd)
{
    return hzddnode_high(hzdd, HZDD_GETNODE(hzdd));
}

uint32_t
hzdd_gettype(HZDD leaf)
{
    return hzddnode_gettype(HZDD_GETNODE(leaf));
}

uint64_t
hzdd_getvalue(HZDD leaf)
{
    return hzddnode_getvalue(HZDD_GETNODE(leaf));
}

/**
 * Implementation of garbage collection
 */

/* Recursively mark MDD nodes as 'in use' */
VOID_TASK_IMPL_1(hzdd_gc_mark_rec, MDD, hzdd)
{
    if (hzdd == hzdd_true) return;
    if (hzdd == hzdd_false) return;

    if (llmsset_mark(nodes, HZDD_GETINDEX(hzdd))) {
        hzddnode_t n = HZDD_GETNODE(hzdd);
        if (!hzddnode_isleaf(n)) {
            SPAWN(hzdd_gc_mark_rec, hzddnode_getlow(n));
            CALL(hzdd_gc_mark_rec, hzddnode_gethigh(n));
            SYNC(hzdd_gc_mark_rec);
        }
    }
}

/**
 * External references
 */

refs_table_t hzdd_refs;
refs_table_t hzdd_protected;
static int hzdd_protected_created = 0;

MDD
hzdd_ref(MDD a)
{
    if (a == hzdd_true || a == hzdd_false) return a;
    refs_up(&hzdd_refs, HZDD_GETINDEX(a));
    return a;
}

void
hzdd_deref(MDD a)
{
    if (a == hzdd_true || a == hzdd_false) return;
    refs_down(&hzdd_refs, HZDD_GETINDEX(a));
}

size_t
hzdd_count_refs()
{
    return refs_count(&hzdd_refs);
}

void
hzdd_protect(HZDD *a)
{
    if (!hzdd_protected_created) {
        // In C++, sometimes hzdd_protect is called before Sylvan is initialized. Just create a table.
        protect_create(&hzdd_protected, 4096);
        hzdd_protected_created = 1;
    }
    protect_up(&hzdd_protected, (size_t)a);
}

void
hzdd_unprotect(HZDD *a)
{
    if (hzdd_protected.refs_table != NULL) protect_down(&hzdd_protected, (size_t)a);
}

size_t
hzdd_count_protected()
{
    return protect_count(&hzdd_protected);
}

/* Called during garbage collection */
VOID_TASK_0(hzdd_gc_mark_external_refs)
{
    // iterate through refs hash table, mark all found
    size_t count=0;
    uint64_t *it = refs_iter(&hzdd_refs, 0, hzdd_refs.refs_size);
    while (it != NULL) {
        SPAWN(hzdd_gc_mark_rec, refs_next(&hzdd_refs, &it, hzdd_refs.refs_size));
        count++;
    }
    while (count--) {
        SYNC(hzdd_gc_mark_rec);
    }
}

VOID_TASK_0(hzdd_gc_mark_protected)
{
    // iterate through refs hash table, mark all found
    size_t count=0;
    uint64_t *it = protect_iter(&hzdd_protected, 0, hzdd_protected.refs_size);
    while (it != NULL) {
        BDD *to_mark = (BDD*)protect_next(&hzdd_protected, &it, hzdd_protected.refs_size);
        SPAWN(hzdd_gc_mark_rec, *to_mark);
        count++;
    }
    while (count--) {
        SYNC(hzdd_gc_mark_rec);
    }
}

/* Infrastructure for internal markings */
DECLARE_THREAD_LOCAL(hzdd_refs_key, hzdd_refs_internal_t);

VOID_TASK_0(hzdd_refs_mark_task)
{
    LOCALIZE_THREAD_LOCAL(hzdd_refs_key, hzdd_refs_internal_t);
    size_t i, j=0;
    for (i=0; i<hzdd_refs_key->r_count; i++) {
        if (j >= 40) {
            while (j--) SYNC(hzdd_gc_mark_rec);
            j=0;
        }
        SPAWN(hzdd_gc_mark_rec, hzdd_refs_key->results[i]);
        j++;
    }
    for (i=0; i<hzdd_refs_key->s_count; i++) {
        Task *t = hzdd_refs_key->spawns[i];
        if (!TASK_IS_STOLEN(t)) break;
        if (TASK_IS_COMPLETED(t)) {
            if (j >= 40) {
                while (j--) SYNC(hzdd_gc_mark_rec);
                j=0;
            }
            SPAWN(hzdd_gc_mark_rec, *(BDD*)TASK_RESULT(t));
            j++;
        }
    }
    while (j--) SYNC(hzdd_gc_mark_rec);
}

VOID_TASK_0(hzdd_refs_mark)
{
    TOGETHER(hzdd_refs_mark_task);
}

VOID_TASK_0(hzdd_refs_init_task)
{
    hzdd_refs_internal_t s = (hzdd_refs_internal_t)malloc(sizeof(struct hzdd_refs_internal));
    s->r_size = 128;
    s->r_count = 0;
    s->s_size = 128;
    s->s_count = 0;
    s->results = (BDD*)malloc(sizeof(BDD) * 128);
    s->spawns = (Task**)malloc(sizeof(Task*) * 128);
    SET_THREAD_LOCAL(hzdd_refs_key, s);
}

VOID_TASK_0(hzdd_refs_init)
{
    INIT_THREAD_LOCAL(hzdd_refs_key);
    TOGETHER(hzdd_refs_init_task);
    sylvan_gc_add_mark(TASK(hzdd_refs_mark));
}

/**
 * Initialize and quit functions
 */

static int hzdd_initialized = 0;

static void
hzdd_quit()
{
    refs_free(&hzdd_refs);
    if (hzdd_protected_created) {
        protect_free(&hzdd_protected);
        hzdd_protected_created = 0;
    }

    hzdd_initialized = 0;
}

void
sylvan_init_hzdd()
{
    if (hzdd_initialized) return;
    hzdd_initialized = 1;

    sylvan_register_quit(hzdd_quit);
    sylvan_gc_add_mark(TASK(hzdd_gc_mark_external_refs));
    sylvan_gc_add_mark(TASK(hzdd_gc_mark_protected));

    refs_create(&hzdd_refs, 1024);
    if (!hzdd_protected_created) {
        protect_create(&hzdd_protected, 4096);
        hzdd_protected_created = 1;
    }

    LACE_ME;
    CALL(hzdd_refs_init);
}

/**
 * Primitives
 */
HZDD
hzdd_makeleaf(uint32_t type, uint64_t value)
{
    struct hzddnode n;
    hzddnode_makeleaf(&n, type, value);

    int created;
    uint64_t index = llmsset_lookup(nodes, n.a, n.b, &created);
    if (index == 0) {
        LACE_ME;

        sylvan_gc();

        index = llmsset_lookup(nodes, n.a, n.b, &created);
        if (index == 0) {
            fprintf(stderr, "BDD Unique table full, %zu of %zu buckets filled!\n", llmsset_count_marked(nodes), llmsset_get_size(nodes));
            exit(1);
        }
    }

    if (created) sylvan_stats_count(BDD_NODES_CREATED);
    else sylvan_stats_count(BDD_NODES_REUSED);

    return (HZDD)HZDD_SETTAG(index, 0xfffff);
}

HZDD
hzdd_makenode(uint32_t var, HZDD low, HZDD high)
{
    /* Normalization rules */
    
    struct hzddnode n;
    uint32_t tag;

    if (low == high) {
        /* both children are the same (BDD minimization) */
        return low;
    } else if (high == hzdd_false) {
        /* if high equals False, we skip the node (ZDD minimization) */
        uint32_t low_tag = HZDD_GETTAG(low);
        if (low_tag == (var+1)) {
            /* no nodes are skipped with (k,k) normalization */
            return HZDD_SETTAG(low, var);
        } else if (low_tag == 0xfffff) {
            /* special case... */
            /* no nodes are skipped with (k,k) normalization */
            return HZDD_SETTAG(low, var);
        } else {
            /* nodes are skipped, so we fix the tree */
            hzddnode_makenode(&n, var+1, low, low);
            tag = var;
        }
    } else {
        /* fine, go ahead */
        hzddnode_makenode(&n, var, low, high);
        tag = var;
    }

    /* if low had a mark, it is moved to the result (normalization of complement edges) */
    int mark = HZDD_HASMARK(low);

    HZDD result;
    int created;
    uint64_t index = llmsset_lookup(nodes, n.a, n.b, &created);
    if (index == 0) {
        LACE_ME;

        hzdd_refs_push(low);
        hzdd_refs_push(high);
        sylvan_gc();
        hzdd_refs_pop(2);

        index = llmsset_lookup(nodes, n.a, n.b, &created);
        if (index == 0) {
            fprintf(stderr, "BDD Unique table full, %zu of %zu buckets filled!\n", llmsset_count_marked(nodes), llmsset_get_size(nodes));
            exit(1);
        }
    }

    if (created) sylvan_stats_count(HZDD_NODES_CREATED);
    else sylvan_stats_count(HZDD_NODES_REUSED);

    result = HZDD_SETTAG(index, tag);
    return mark ? result | hzdd_complement : result;
}

HZDD
hzdd_makemapnode(uint32_t var, HZDD low, HZDD high)
{
    struct hzddnode n;
    uint64_t index;
    int created;

    // in an HZDDMAP, the low edges eventually lead to 0 and cannot have a low mark
    assert(!HZDD_HASMARK(low));

    hzddnode_makemapnode(&n, var, low, high);
    index = llmsset_lookup(nodes, n.a, n.b, &created);
    if (index == 0) {
        LACE_ME;

        hzdd_refs_push(low);
        hzdd_refs_push(high);
        sylvan_gc();
        hzdd_refs_pop(2);

        index = llmsset_lookup(nodes, n.a, n.b, &created);
        if (index == 0) {
            fprintf(stderr, "BDD Unique table full, %zu of %zu buckets filled!\n", llmsset_count_marked(nodes), llmsset_get_size(nodes));
            exit(1);
        }
    }

    if (created) sylvan_stats_count(BDD_NODES_CREATED);
    else sylvan_stats_count(BDD_NODES_REUSED);

    return index;
}

/**
 * Convert an MTBDD to a HZDD
 */
TASK_IMPL_2(HZDD, hzdd_from_mtbdd, MTBDD, dd, MTBDD, domain)
{
    /* Special treatment for True and False */
    if (dd == mtbdd_false) return hzdd_false;
    if (dd == mtbdd_true) return hzdd_true;

    /* Maybe perform garbage collection */
    sylvan_gc_test();

    /* Count operation */
    sylvan_stats_count(HZDD_FROM_MTBDD);

    /* First (maybe) match domain with dd */
    mtbddnode_t ndd = MTBDD_GETNODE(dd);
    mtbddnode_t ndomain = NULL;
    if (mtbddnode_isleaf(ndd)) {
        domain = mtbdd_true;
    } else {
        /* Get variable and cofactors */
        if (domain == mtbdd_true) {
            printf("hello houston we are a problem: %zx, %zx", dd, domain);
        }
        assert(domain != mtbdd_true && domain != mtbdd_false);
        ndomain = MTBDD_GETNODE(domain);
        uint32_t domain_var = mtbddnode_getvariable(ndomain);

        uint32_t var = mtbddnode_getvariable(ndd);
        while (domain_var != var) {
            assert(domain_var < var);
            domain = mtbddnode_followhigh(domain, ndomain);
            assert(domain != mtbdd_true && domain != mtbdd_false);
            ndomain = MTBDD_GETNODE(domain);
            domain_var = mtbddnode_getvariable(ndomain);
        }
     }

    /* Check cache */
    HZDD result;
    if (cache_get(CACHE_HZDD_FROM_MTBDD|dd, domain, 0, &result)) {
        sylvan_stats_count(HZDD_FROM_MTBDD_CACHED);
        return result;
    }

    if (mtbddnode_isleaf(ndd)) {
        /* Convert a leaf */
        uint32_t type = mtbddnode_gettype(ndd);
        uint64_t value = mtbddnode_getvalue(ndd);
        result = hzdd_makeleaf(type, value);
        return result;
    } else {
        /* Get variable and cofactors */
        uint32_t var = mtbddnode_getvariable(ndd);
        MTBDD dd_low = mtbddnode_followlow(dd, ndd);
        MTBDD dd_high = mtbddnode_followhigh(dd, ndd);

        /* Recursive */
        MTBDD next_domain = mtbddnode_followhigh(domain, ndomain);
        hzdd_refs_spawn(SPAWN(hzdd_from_mtbdd, dd_high, next_domain));
        HZDD low = hzdd_refs_push(CALL(hzdd_from_mtbdd, dd_low, next_domain));
        HZDD high = hzdd_refs_sync(SYNC(hzdd_from_mtbdd));
        hzdd_refs_pop(1);
        result = HZDD_SETTAG(hzdd_makenode(var, low, high), var);
    }

    /* Store in cache */
    if (cache_put(CACHE_HZDD_FROM_MTBDD|dd, domain, 0, result)) {
        sylvan_stats_count(HZDD_FROM_MTBDD_CACHEDPUT);
    }

    return result;
}

/**
 * Helper function for recursive unmarking
 */
static void
hzdd_unmark_rec(HZDD hzdd)
{
    hzddnode_t n = HZDD_GETNODE(hzdd);
    if (!hzddnode_getmark(n)) return;
    hzddnode_setmark(n, 0);
    if (hzddnode_isleaf(n)) return;
    hzdd_unmark_rec(hzddnode_getlow(n));
    hzdd_unmark_rec(hzddnode_gethigh(n));
}

/**
 * Count number of nodes in HZDD
 */

static size_t
hzdd_nodecount_mark(HZDD hzdd)
{
    if (hzdd == hzdd_true) return 0; // do not count true/false leaf
    if (hzdd == hzdd_false) return 0; // do not count true/false leaf
    hzddnode_t n = HZDD_GETNODE(hzdd);
    if (hzddnode_getmark(n)) return 0;
    hzddnode_setmark(n, 1);
    if (hzddnode_isleaf(n)) return 1; // count leaf as 1
    return 1 + hzdd_nodecount_mark(hzddnode_getlow(n)) + hzdd_nodecount_mark(hzddnode_gethigh(n));
}

size_t
hzdd_nodecount_more(const HZDD *hzdds, size_t count)
{
    size_t result = 0, i;
    for (i=0; i<count; i++) result += hzdd_nodecount_mark(hzdds[i]);
    for (i=0; i<count; i++) hzdd_unmark_rec(hzdds[i]);
    return result;
}

/**
 * Export to .dot file
 */
static inline int tag_to_label(HZDD hzdd)
{
    uint32_t tag = HZDD_GETTAG(hzdd);
    if (tag == 0xfffff) return -1;
    else return (int)tag;
}

static void
hzdd_fprintdot_rec(FILE *out, HZDD hzdd)
{
    hzddnode_t n = HZDD_GETNODE(hzdd); // also works for hzdd_false
    if (hzddnode_getmark(n)) return;
    hzddnode_setmark(n, 1);

    if (HZDD_GETINDEX(hzdd) == 0) {  // hzdd == hzdd_true || hzdd == hzdd_false
        fprintf(out, "0 [shape=box, style=filled, label=\"F\"];\n");
    } else if (hzddnode_isleaf(n)) {
        fprintf(out, "%" PRIu64 " [shape=box, style=filled, label=\"", HZDD_GETINDEX(hzdd));
        /* hzdd_fprint_leaf(out, hzdd); */  // TODO
        fprintf(out, "\"];\n");
    } else {
        fprintf(out, "%" PRIu64 " [label=\"%" PRIu32 "\"];\n",
                HZDD_GETINDEX(hzdd), hzddnode_getvariable(n));

        hzdd_fprintdot_rec(out, hzddnode_getlow(n));
        hzdd_fprintdot_rec(out, hzddnode_gethigh(n));

        fprintf(out, "%" PRIu64 " -> %" PRIu64 " [style=dashed, label=\" %d\"];\n",
                HZDD_GETINDEX(hzdd), HZDD_GETINDEX(hzddnode_getlow(n)),
                tag_to_label(hzddnode_getlow(n)));
        fprintf(out, "%" PRIu64 " -> %" PRIu64 " [style=solid dir=both arrowtail=%s, label=\" %d\"];\n",
                HZDD_GETINDEX(hzdd), HZDD_GETINDEX(hzddnode_gethigh(n)),
                hzddnode_getcomp(n) ? "dot" : "none", tag_to_label(hzddnode_gethigh(n)));
    }
}

void
hzdd_fprintdot(FILE *out, HZDD hzdd)
{
    fprintf(out, "digraph \"DD\" {\n");
    fprintf(out, "graph [dpi = 300];\n");
    fprintf(out, "center = true;\n");
    fprintf(out, "edge [dir = forward];\n");
    fprintf(out, "root [style=invis];\n");
    fprintf(out, "root -> %" PRIu64 " [style=solid dir=both arrowtail=%s label=\" %d\"];\n",
            HZDD_GETINDEX(hzdd), HZDD_HASMARK(hzdd) ? "dot" : "none", tag_to_label(hzdd));

    hzdd_fprintdot_rec(out, hzdd);
    hzdd_unmark_rec(hzdd);

    fprintf(out, "}\n");
}
