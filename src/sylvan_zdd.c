/*
 * Copyright 2011-2016 Tom van Dijk
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

#include <refs.h>
#include <sha2.h>
#include <sylvan.h>
#include <sylvan_common.h>
#include <sylvan_mtbdd_int.h>

/* Primitives */
int
mtbdd_isleaf(MTBDD bdd)
{
    if (bdd == mtbdd_true || bdd == mtbdd_false) return 1;
    return mtbddnode_isleaf(GETNODE(bdd));
}

// for nodes
uint32_t
mtbdd_getvar(MTBDD node)
{
    return mtbddnode_getvariable(GETNODE(node));
}

MTBDD
mtbdd_getlow(MTBDD mtbdd)
{
    return node_getlow(mtbdd, GETNODE(mtbdd));
}

MTBDD
mtbdd_gethigh(MTBDD mtbdd)
{
    return node_gethigh(mtbdd, GETNODE(mtbdd));
}

// for leaves
uint32_t
mtbdd_gettype(MTBDD leaf)
{
    return mtbddnode_gettype(GETNODE(leaf));
}

uint64_t
mtbdd_getvalue(MTBDD leaf)
{
    return mtbddnode_getvalue(GETNODE(leaf));
}

// for leaf type 0 (integer)
int64_t
mtbdd_getint64(MTBDD leaf)
{
    uint64_t value = mtbdd_getvalue(leaf);
    return *(int64_t*)&value;
}

// for leaf type 1 (double)
double
mtbdd_getdouble(MTBDD leaf)
{
    uint64_t value = mtbdd_getvalue(leaf);
    return *(double*)&value;
}

/**
 * Implementation of garbage collection
 */

/* Recursively mark MDD nodes as 'in use' */
VOID_TASK_IMPL_1(mtbdd_gc_mark_rec, MDD, mtbdd)
{
    if (mtbdd == mtbdd_true) return;
    if (mtbdd == mtbdd_false) return;

    if (llmsset_mark(nodes, mtbdd&(~mtbdd_complement))) {
        mtbddnode_t n = GETNODE(mtbdd);
        if (!mtbddnode_isleaf(n)) {
            SPAWN(mtbdd_gc_mark_rec, mtbddnode_getlow(n));
            CALL(mtbdd_gc_mark_rec, mtbddnode_gethigh(n));
            SYNC(mtbdd_gc_mark_rec);
        }
    }
}

/**
 * External references
 */

refs_table_t mtbdd_refs;
refs_table_t mtbdd_protected;
static int mtbdd_protected_created = 0;

MDD
mtbdd_ref(MDD a)
{
    if (a == mtbdd_true || a == mtbdd_false) return a;
    refs_up(&mtbdd_refs, a);
    return a;
}

void
mtbdd_deref(MDD a)
{
    if (a == mtbdd_true || a == mtbdd_false) return;
    refs_down(&mtbdd_refs, a);
}

size_t
mtbdd_count_refs()
{
    return refs_count(&mtbdd_refs);
}

void
mtbdd_protect(MTBDD *a)
{
    if (!mtbdd_protected_created) {
        // In C++, sometimes mtbdd_protect is called before Sylvan is initialized. Just create a table.
        protect_create(&mtbdd_protected, 4096);
        mtbdd_protected_created = 1;
    }
    protect_up(&mtbdd_protected, (size_t)a);
}

void
mtbdd_unprotect(MTBDD *a)
{
    if (mtbdd_protected.refs_table != NULL) protect_down(&mtbdd_protected, (size_t)a);
}

size_t
mtbdd_count_protected()
{
    return protect_count(&mtbdd_protected);
}

/* Called during garbage collection */
VOID_TASK_0(mtbdd_gc_mark_external_refs)
{
    // iterate through refs hash table, mark all found
    size_t count=0;
    uint64_t *it = refs_iter(&mtbdd_refs, 0, mtbdd_refs.refs_size);
    while (it != NULL) {
        SPAWN(mtbdd_gc_mark_rec, refs_next(&mtbdd_refs, &it, mtbdd_refs.refs_size));
        count++;
    }
    while (count--) {
        SYNC(mtbdd_gc_mark_rec);
    }
}

VOID_TASK_0(mtbdd_gc_mark_protected)
{
    // iterate through refs hash table, mark all found
    size_t count=0;
    uint64_t *it = protect_iter(&mtbdd_protected, 0, mtbdd_protected.refs_size);
    while (it != NULL) {
        BDD *to_mark = (BDD*)protect_next(&mtbdd_protected, &it, mtbdd_protected.refs_size);
        SPAWN(mtbdd_gc_mark_rec, *to_mark);
        count++;
    }
    while (count--) {
        SYNC(mtbdd_gc_mark_rec);
    }
}

/* Infrastructure for internal markings */
DECLARE_THREAD_LOCAL(mtbdd_refs_key, mtbdd_refs_internal_t);

VOID_TASK_0(mtbdd_refs_mark_task)
{
    LOCALIZE_THREAD_LOCAL(mtbdd_refs_key, mtbdd_refs_internal_t);
    size_t i, j=0;
    for (i=0; i<mtbdd_refs_key->r_count; i++) {
        if (j >= 40) {
            while (j--) SYNC(mtbdd_gc_mark_rec);
            j=0;
        }
        SPAWN(mtbdd_gc_mark_rec, mtbdd_refs_key->results[i]);
        j++;
    }
    for (i=0; i<mtbdd_refs_key->s_count; i++) {
        Task *t = mtbdd_refs_key->spawns[i];
        if (!TASK_IS_STOLEN(t)) break;
        if (TASK_IS_COMPLETED(t)) {
            if (j >= 40) {
                while (j--) SYNC(mtbdd_gc_mark_rec);
                j=0;
            }
            SPAWN(mtbdd_gc_mark_rec, *(BDD*)TASK_RESULT(t));
            j++;
        }
    }
    while (j--) SYNC(mtbdd_gc_mark_rec);
}

VOID_TASK_0(mtbdd_refs_mark)
{
    TOGETHER(mtbdd_refs_mark_task);
}

VOID_TASK_0(mtbdd_refs_init_task)
{
    mtbdd_refs_internal_t s = (mtbdd_refs_internal_t)malloc(sizeof(struct mtbdd_refs_internal));
    s->r_size = 128;
    s->r_count = 0;
    s->s_size = 128;
    s->s_count = 0;
    s->results = (BDD*)malloc(sizeof(BDD) * 128);
    s->spawns = (Task**)malloc(sizeof(Task*) * 128);
    SET_THREAD_LOCAL(mtbdd_refs_key, s);
}

VOID_TASK_0(mtbdd_refs_init)
{
    INIT_THREAD_LOCAL(mtbdd_refs_key);
    TOGETHER(mtbdd_refs_init_task);
    sylvan_gc_add_mark(10, TASK(mtbdd_refs_mark));
}

/**
 * Handling of custom leaves "registry"
 */

typedef struct
{
    mtbdd_hash_cb hash_cb;
    mtbdd_equals_cb equals_cb;
    mtbdd_create_cb create_cb;
    mtbdd_destroy_cb destroy_cb;
} customleaf_t;

static customleaf_t *cl_registry;
static size_t cl_registry_count;

static void
_mtbdd_create_cb(uint64_t *a, uint64_t *b)
{
    // for leaf
    if ((*a & 0x4000000000000000) == 0) return; // huh?
    uint32_t type = *a & 0xffffffff;
    if (type >= cl_registry_count) return; // not in registry
    customleaf_t *c = cl_registry + type;
    if (c->create_cb == NULL) return; // not in registry
    c->create_cb(b);
}

static void
_mtbdd_destroy_cb(uint64_t a, uint64_t b)
{
    // for leaf
    if ((a & 0x4000000000000000) == 0) return; // huh?
    uint32_t type = a & 0xffffffff;
    if (type >= cl_registry_count) return; // not in registry
    customleaf_t *c = cl_registry + type;
    if (c->destroy_cb == NULL) return; // not in registry
    c->destroy_cb(b);
}

static uint64_t
_mtbdd_hash_cb(uint64_t a, uint64_t b, uint64_t seed)
{
    // for leaf
    if ((a & 0x4000000000000000) == 0) return llmsset_hash(a, b, seed);
    uint32_t type = a & 0xffffffff;
    if (type >= cl_registry_count) return llmsset_hash(a, b, seed);
    customleaf_t *c = cl_registry + type;
    if (c->hash_cb == NULL) return llmsset_hash(a, b, seed);
    return c->hash_cb(b, seed ^ a);
}

static int
_mtbdd_equals_cb(uint64_t a, uint64_t b, uint64_t aa, uint64_t bb)
{
    // for leaf
    if (a != aa) return 0;
    if ((a & 0x4000000000000000) == 0) return b == bb ? 1 : 0;
    if ((aa & 0x4000000000000000) == 0) return b == bb ? 1 : 0;
    uint32_t type = a & 0xffffffff;
    if (type >= cl_registry_count) return b == bb ? 1 : 0;
    customleaf_t *c = cl_registry + type;
    if (c->equals_cb == NULL) return b == bb ? 1 : 0;
    return c->equals_cb(b, bb);
}

uint32_t
mtbdd_register_custom_leaf(mtbdd_hash_cb hash_cb, mtbdd_equals_cb equals_cb, mtbdd_create_cb create_cb, mtbdd_destroy_cb destroy_cb)
{
    uint32_t type = cl_registry_count;
    if (type == 0) type = 3;
    if (cl_registry == NULL) {
        cl_registry = (customleaf_t *)calloc(sizeof(customleaf_t), (type+1));
        cl_registry_count = type+1;
        llmsset_set_custom(nodes, _mtbdd_hash_cb, _mtbdd_equals_cb, _mtbdd_create_cb, _mtbdd_destroy_cb);
    } else if (cl_registry_count <= type) {
        cl_registry = (customleaf_t *)realloc(cl_registry, sizeof(customleaf_t) * (type+1));
        memset(cl_registry + cl_registry_count, 0, sizeof(customleaf_t) * (type+1-cl_registry_count));
        cl_registry_count = type+1;
    }
    customleaf_t *c = cl_registry + type;
    c->hash_cb = hash_cb;
    c->equals_cb = equals_cb;
    c->create_cb = create_cb;
    c->destroy_cb = destroy_cb;
    return type;
}

/**
 * Initialize and quit functions
 */

static void
mtbdd_quit()
{
    refs_free(&mtbdd_refs);
    if (mtbdd_protected_created) {
        protect_free(&mtbdd_protected);
        mtbdd_protected_created = 0;
    }
    if (cl_registry != NULL) {
        free(cl_registry);
        cl_registry = NULL;
        cl_registry_count = 0;
    }
}

void
sylvan_init_mtbdd()
{
    sylvan_register_quit(mtbdd_quit);
    sylvan_gc_add_mark(10, TASK(mtbdd_gc_mark_external_refs));
    sylvan_gc_add_mark(10, TASK(mtbdd_gc_mark_protected));

    // Sanity check
    if (sizeof(struct mtbddnode) != 16) {
        fprintf(stderr, "Invalid size of mtbdd nodes: %ld\n", sizeof(struct mtbddnode));
        exit(1);
    }

    refs_create(&mtbdd_refs, 1024);
    if (!mtbdd_protected_created) {
        protect_create(&mtbdd_protected, 4096);
        mtbdd_protected_created = 1;
    }

    LACE_ME;
    CALL(mtbdd_refs_init);

    cl_registry = NULL;
    cl_registry_count = 0;
}

/**
 * Primitives
 */
MTBDD
mtbdd_makeleaf(uint32_t type, uint64_t value)
{
    struct mtbddnode n;
    mtbddnode_makeleaf(&n, type, value);

    int custom = type < cl_registry_count && cl_registry[type].hash_cb != NULL ? 1 : 0;

    int created;
    uint64_t index = custom ? llmsset_lookupc(nodes, n.a, n.b, &created) : llmsset_lookup(nodes, n.a, n.b, &created);
    if (index == 0) {
        LACE_ME;

        sylvan_gc();

        index = custom ? llmsset_lookupc(nodes, n.a, n.b, &created) : llmsset_lookup(nodes, n.a, n.b, &created);
        if (index == 0) {
            fprintf(stderr, "BDD Unique table full, %zu of %zu buckets filled!\n", llmsset_count_marked(nodes), llmsset_get_size(nodes));
            exit(1);
        }
    }

    return (MTBDD)index;
}

MTBDD
mtbdd_makenode(uint32_t var, MTBDD low, MTBDD high)
{
    if (low == high) return low;

    // Normalization to keep canonicity
    // low will have no mark

    struct mtbddnode n;
    int mark, created;

    if (MTBDD_HASMARK(low)) {
        mark = 1;
        low = MTBDD_TOGGLEMARK(low);
        high = MTBDD_TOGGLEMARK(high);
    } else {
        mark = 0;
    }

    mtbddnode_makenode(&n, var, low, high);

    MTBDD result;
    uint64_t index = llmsset_lookup(nodes, n.a, n.b, &created);
    if (index == 0) {
        LACE_ME;

        mtbdd_refs_push(low);
        mtbdd_refs_push(high);
        sylvan_gc();
        mtbdd_refs_pop(2);

        index = llmsset_lookup(nodes, n.a, n.b, &created);
        if (index == 0) {
            fprintf(stderr, "BDD Unique table full, %zu of %zu buckets filled!\n", llmsset_count_marked(nodes), llmsset_get_size(nodes));
            exit(1);
        }
    }

    result = index;
    return mark ? result | mtbdd_complement : result;
}

/* Operations */

/**
 * Create the cube of variables in arr.
 */
MTBDD
mtbdd_fromarray(uint32_t* arr, size_t length)
{
    if (length == 0) return mtbdd_true;
    else if (length == 1) return mtbdd_makenode(*arr, mtbdd_false, mtbdd_true);
    else return mtbdd_makenode(*arr, mtbdd_false, mtbdd_fromarray(arr+1, length-1));
}

/**
 * Create a MTBDD cube representing the conjunction of variables in their positive or negative
 * form depending on whether the cube[idx] equals 0 (negative), 1 (positive) or 2 (any).
 * Use cube[idx]==3 for "s=s'" in interleaved variables (matches with next variable)
 * <variables> is the cube of variables
 */
MTBDD
mtbdd_cube(MTBDD variables, uint8_t *cube, MTBDD terminal)
{
    if (variables == mtbdd_true) return terminal;
    mtbddnode_t n = GETNODE(variables);

    BDD result;
    switch (*cube) {
    case 0:
        result = mtbdd_cube(node_gethigh(variables, n), cube+1, terminal);
        result = mtbdd_makenode(mtbddnode_getvariable(n), result, mtbdd_false);
        return result;
    case 1:
        result = mtbdd_cube(node_gethigh(variables, n), cube+1, terminal);
        result = mtbdd_makenode(mtbddnode_getvariable(n), mtbdd_false, result);
        return result;
    case 2:
        return mtbdd_cube(node_gethigh(variables, n), cube+1, terminal);
    case 3:
    {
        MTBDD variables2 = node_gethigh(variables, n);
        mtbddnode_t n2 = GETNODE(variables2);
        uint32_t var2 = mtbddnode_getvariable(n2);
        result = mtbdd_cube(node_gethigh(variables2, n2), cube+2, terminal);
        BDD low = mtbdd_makenode(var2, result, mtbdd_false);
        mtbdd_refs_push(low);
        BDD high = mtbdd_makenode(var2, mtbdd_false, result);
        mtbdd_refs_pop(1);
        result = mtbdd_makenode(mtbddnode_getvariable(n), low, high);
        return result;
    }
    default:
        return mtbdd_false; // ?
    }
}

/**
 * Compute IF <f> THEN <g> ELSE <h>.
 * <f> must be a Boolean MTBDD (or standard BDD).
 */
TASK_IMPL_3(MTBDD, mtbdd_ite, MTBDD, f, MTBDD, g, MTBDD, h)
{
    /* Terminal cases */
    if (f == mtbdd_true) return g;
    if (f == mtbdd_false) return h;
    if (g == h) return g;
    if (g == mtbdd_true && h == mtbdd_false) return f;
    if (h == mtbdd_true && g == mtbdd_false) return MTBDD_TOGGLEMARK(f);

    // If all MTBDD's are Boolean, then there could be further optimizations (see sylvan_bdd.c)

    /* Maybe perform garbage collection */
    sylvan_gc_test();

    /* Check cache */
    MTBDD result;
    if (cache_get3(CACHE_MTBDD_ITE, f, g, h, &result)) return result;

    /* Get top variable */
    int lg = mtbdd_isleaf(g);
    int lh = mtbdd_isleaf(h);
    mtbddnode_t nf = GETNODE(f);
    mtbddnode_t ng = lg ? 0 : GETNODE(g);
    mtbddnode_t nh = lh ? 0 : GETNODE(h);
    uint32_t vf = mtbddnode_getvariable(nf);
    uint32_t vg = lg ? 0 : mtbddnode_getvariable(ng);
    uint32_t vh = lh ? 0 : mtbddnode_getvariable(nh);
    uint32_t v = vf;
    if (!lg && vg < v) v = vg;
    if (!lh && vh < v) v = vh;

    /* Get cofactors */
    MTBDD flow, fhigh, glow, ghigh, hlow, hhigh;
    flow = (vf == v) ? node_getlow(f, nf) : f;
    fhigh = (vf == v) ? node_gethigh(f, nf) : f;
    glow = (!lg && vg == v) ? node_getlow(g, ng) : g;
    ghigh = (!lg && vg == v) ? node_gethigh(g, ng) : g;
    hlow = (!lh && vh == v) ? node_getlow(h, nh) : h;
    hhigh = (!lh && vh == v) ? node_gethigh(h, nh) : h;

    /* Recursive calls */
    mtbdd_refs_spawn(SPAWN(mtbdd_ite, fhigh, ghigh, hhigh));
    MTBDD low = mtbdd_refs_push(CALL(mtbdd_ite, flow, glow, hlow));
    MTBDD high = mtbdd_refs_sync(SYNC(mtbdd_ite));
    mtbdd_refs_pop(1);
    result = mtbdd_makenode(v, low, high);

    /* Store in cache */
    cache_put3(CACHE_MTBDD_ITE, f, g, h, result);
    return result;
}

/**
 * Multiply <a> and <b>, and abstract variables <vars> using summation.
 * This is similar to the "and_exists" operation in BDDs.
 */
TASK_IMPL_3(MTBDD, mtbdd_and_exists, MTBDD, a, MTBDD, b, MTBDD, v)
{
    /* Check terminal case */
    if (v == mtbdd_true) return mtbdd_apply(a, b, TASK(mtbdd_op_times));
    MTBDD result = CALL(mtbdd_op_times, &a, &b);
    if (result != mtbdd_invalid) {
        mtbdd_refs_push(result);
        result = mtbdd_abstract(result, v, TASK(mtbdd_abstract_op_plus));
        mtbdd_refs_pop(1);
        return result;
    }

    /* Maybe perform garbage collection */
    sylvan_gc_test();

    /* Check cache */
    if (cache_get3(CACHE_MTBDD_AND_EXISTS, a, b, v, &result)) return result;

    /* Now, v is not a constant, and either a or b is not a constant */

    /* Get top variable */
    int la = mtbdd_isleaf(a);
    int lb = mtbdd_isleaf(b);
    mtbddnode_t na = la ? 0 : GETNODE(a);
    mtbddnode_t nb = lb ? 0 : GETNODE(b);
    uint32_t va = la ? 0xffffffff : mtbddnode_getvariable(na);
    uint32_t vb = lb ? 0xffffffff : mtbddnode_getvariable(nb);
    uint32_t var = va < vb ? va : vb;

    mtbddnode_t nv = GETNODE(v);
    uint32_t vv = mtbddnode_getvariable(nv);

    if (vv < var) {
        /* Recursive, then abstract result */
        result = CALL(mtbdd_and_exists, a, b, node_gethigh(v, nv));
        mtbdd_refs_push(result);
        result = mtbdd_apply(result, result, TASK(mtbdd_op_plus));
        mtbdd_refs_pop(1);
    } else {
        /* Get cofactors */
        MTBDD alow, ahigh, blow, bhigh;
        alow  = (!la && va == var) ? node_getlow(a, na)  : a;
        ahigh = (!la && va == var) ? node_gethigh(a, na) : a;
        blow  = (!lb && vb == var) ? node_getlow(b, nb)  : b;
        bhigh = (!lb && vb == var) ? node_gethigh(b, nb) : b;

        if (vv == var) {
            /* Recursive, then abstract result */
            mtbdd_refs_spawn(SPAWN(mtbdd_and_exists, ahigh, bhigh, node_gethigh(v, nv)));
            MTBDD low = mtbdd_refs_push(CALL(mtbdd_and_exists, alow, blow, node_gethigh(v, nv)));
            MTBDD high = mtbdd_refs_push(mtbdd_refs_sync(SYNC(mtbdd_and_exists)));
            result = CALL(mtbdd_apply, low, high, TASK(mtbdd_op_plus));
            mtbdd_refs_pop(2);
        } else /* vv > v */ {
            /* Recursive, then create node */
            mtbdd_refs_spawn(SPAWN(mtbdd_and_exists, ahigh, bhigh, v));
            MTBDD low = mtbdd_refs_push(CALL(mtbdd_and_exists, alow, blow, v));
            MTBDD high = mtbdd_refs_sync(SYNC(mtbdd_and_exists));
            mtbdd_refs_pop(1);
            result = mtbdd_makenode(var, low, high);
        }
    }

    /* Store in cache */
    cache_put3(CACHE_MTBDD_AND_EXISTS, a, b, v, result);
    return result;
}

/**
 * Calculate the support of a MTBDD, i.e. the cube of all variables that appear in the MTBDD nodes.
 */
TASK_IMPL_1(MTBDD, mtbdd_support, MTBDD, dd)
{
    /* Terminal case */
    if (mtbdd_isleaf(dd)) return mtbdd_true;

    /* Maybe perform garbage collection */
    sylvan_gc_test();

    /* Check cache */
    MTBDD result;
    if (cache_get3(CACHE_MTBDD_SUPPORT, dd, 0, 0, &result)) return result;

    /* Recursive calls */
    mtbddnode_t n = GETNODE(dd);
    mtbdd_refs_spawn(SPAWN(mtbdd_support, node_getlow(dd, n)));
    MTBDD high = mtbdd_refs_push(CALL(mtbdd_support, node_gethigh(dd, n)));
    MTBDD low = mtbdd_refs_push(mtbdd_refs_sync(SYNC(mtbdd_support)));

    /* Compute result */
    result = mtbdd_makenode(mtbddnode_getvariable(n), mtbdd_false, mtbdd_times(low, high));
    mtbdd_refs_pop(2);

    /* Write to cache */
    cache_put3(CACHE_MTBDD_SUPPORT, dd, 0, 0, result);
    return result;
}

/**
 * Function composition, for each node with variable <key> which has a <key,value> pair in <map>,
 * replace the node by the result of mtbdd_ite(<value>, <low>, <high>).
 * Each <value> in <map> must be a Boolean MTBDD.
 */
TASK_IMPL_2(MTBDD, mtbdd_compose, MTBDD, a, MTBDDMAP, map)
{
    /* Terminal case */
    if (mtbdd_isleaf(a) || mtbdd_map_isempty(map)) return a;

    /* Determine top level */
    mtbddnode_t n = GETNODE(a);
    uint32_t v = mtbddnode_getvariable(n);

    /* Find in map */
    while (mtbdd_map_key(map) < v) {
        map = mtbdd_map_next(map);
        if (mtbdd_map_isempty(map)) return a;
    }

    /* Perhaps execute garbage collection */
    sylvan_gc_test();

    /* Check cache */
    MTBDD result;
    if (cache_get3(CACHE_MTBDD_COMPOSE, a, map, 0, &result)) return result;

    /* Recursive calls */
    mtbdd_refs_spawn(SPAWN(mtbdd_compose, node_getlow(a, n), map));
    MTBDD high = mtbdd_refs_push(CALL(mtbdd_compose, node_gethigh(a, n), map));
    MTBDD low = mtbdd_refs_push(mtbdd_refs_sync(SYNC(mtbdd_compose)));

    /* Calculate result */
    MTBDD r = mtbdd_map_key(map) == v ? mtbdd_map_value(map) : mtbdd_makenode(v, mtbdd_false, mtbdd_true);
    mtbdd_refs_push(r);
    result = CALL(mtbdd_ite, r, high, low);
    mtbdd_refs_pop(3);

    /* Store in cache */
    cache_put3(CACHE_MTBDD_COMPOSE, a, map, 0, result);
    return result;
}

/**
 * Calculate the number of satisfying variable assignments according to <variables>.
 */
TASK_IMPL_2(double, mtbdd_satcount, MTBDD, dd, size_t, nvars)
{
    /* Trivial cases */
    if (dd == mtbdd_false) return 0.0;
    if (mtbdd_isleaf(dd)) return powl(2.0L, nvars);

    /* Perhaps execute garbage collection */
    sylvan_gc_test();

    union {
        double d;
        uint64_t s;
    } hack;

    /* Consult cache */
    if (cache_get3(CACHE_BDD_SATCOUNT, dd, 0, nvars, &hack.s)) {
        sylvan_stats_count(BDD_SATCOUNT_CACHED);
        return hack.d;
    }

    SPAWN(mtbdd_satcount, mtbdd_gethigh(dd), nvars-1);
    double low = CALL(mtbdd_satcount, mtbdd_getlow(dd), nvars-1);
    hack.d = low + SYNC(mtbdd_satcount);

    cache_put3(CACHE_BDD_SATCOUNT, dd, 0, nvars, hack.s);
    return hack.d;
}

MTBDD
mtbdd_enum_first(MTBDD dd, MTBDD variables, uint8_t *arr, mtbdd_enum_filter_cb filter_cb)
{
    if (dd == mtbdd_false) {
        // the leaf dd is skipped
        return mtbdd_false;
    } else if (mtbdd_isleaf(dd)) {
        // a leaf for which the filter returns 0 is skipped
        if (filter_cb != NULL && filter_cb(dd) == 0) return mtbdd_false;
        // ok, we have a leaf that is not skipped, go for it!
        while (variables != mtbdd_true) {
            *arr++ = 2;
            variables = mtbdd_gethigh(variables);
        }
        return dd;
    } else {
        // if variables == true, then dd must be a leaf. But then this line is unreachable.
        // if this assertion fails, then <variables> is not the support of <dd>.
        assert(variables != mtbdd_true);

        // get next variable from <variables>
        uint32_t v = mtbdd_getvar(variables);
        variables = mtbdd_gethigh(variables);

        // check if MTBDD is on this variable
        mtbddnode_t n = GETNODE(dd);
        if (mtbddnode_getvariable(n) != v) {
            *arr = 2;
            return mtbdd_enum_first(dd, variables, arr+1, filter_cb);
        }

        // first maybe follow low
        MTBDD res = mtbdd_enum_first(node_getlow(dd, n), variables, arr+1, filter_cb);
        if (res != mtbdd_false) {
            *arr = 0;
            return res;
        }

        // if not low, try following high
        res = mtbdd_enum_first(node_gethigh(dd, n), variables, arr+1, filter_cb);
        if (res != mtbdd_false) {
            *arr = 1;
            return res;
        }
        
        // we've tried low and high, return false
        return mtbdd_false;
    }
}

MTBDD
mtbdd_enum_next(MTBDD dd, MTBDD variables, uint8_t *arr, mtbdd_enum_filter_cb filter_cb)
{
    if (mtbdd_isleaf(dd)) {
        // we find the leaf in 'enum_next', then we've seen it before...
        return mtbdd_false;
    } else {
        // if variables == true, then dd must be a leaf. But then this line is unreachable.
        // if this assertion fails, then <variables> is not the support of <dd>.
        assert(variables != mtbdd_true);

        variables = mtbdd_gethigh(variables);

        if (*arr == 0) {
            // previous was low
            mtbddnode_t n = GETNODE(dd);
            MTBDD res = mtbdd_enum_next(node_getlow(dd, n), variables, arr+1, filter_cb);
            if (res != mtbdd_false) {
                return res;
            } else {
                // try to find new in high branch
                res = mtbdd_enum_first(node_gethigh(dd, n), variables, arr+1, filter_cb);
                if (res != mtbdd_false) {
                    *arr = 1;
                    return res;
                } else {
                    return mtbdd_false;
                }
            }
        } else if (*arr == 1) {
            // previous was high
            mtbddnode_t n = GETNODE(dd);
            return mtbdd_enum_next(node_gethigh(dd, n), variables, arr+1, filter_cb);
        } else {
            // previous was either
            return mtbdd_enum_next(dd, variables, arr+1, filter_cb);
        }
    }
}

/**
 * Helper function for recursive unmarking
 */
static void
mtbdd_unmark_rec(MTBDD mtbdd)
{
    mtbddnode_t n = GETNODE(mtbdd);
    if (!mtbddnode_getmark(n)) return;
    mtbddnode_setmark(n, 0);
    if (mtbddnode_isleaf(n)) return;
    mtbdd_unmark_rec(mtbddnode_getlow(n));
    mtbdd_unmark_rec(mtbddnode_gethigh(n));
}

/**
 * Count number of leaves in MTBDD
 */

static size_t
mtbdd_leafcount_mark(MTBDD mtbdd)
{
    if (mtbdd == mtbdd_true) return 0; // do not count true/false leaf
    if (mtbdd == mtbdd_false) return 0; // do not count true/false leaf
    mtbddnode_t n = GETNODE(mtbdd);
    if (mtbddnode_getmark(n)) return 0;
    mtbddnode_setmark(n, 1);
    if (mtbddnode_isleaf(n)) return 1; // count leaf as 1
    return mtbdd_leafcount_mark(mtbddnode_getlow(n)) + mtbdd_leafcount_mark(mtbddnode_gethigh(n));
}

size_t
mtbdd_leafcount(MTBDD mtbdd)
{
    size_t result = mtbdd_leafcount_mark(mtbdd);
    mtbdd_unmark_rec(mtbdd);
    return result;
}

/**
 * Count number of nodes in MTBDD
 */

static size_t
mtbdd_nodecount_mark(MTBDD mtbdd)
{
    if (mtbdd == mtbdd_true) return 0; // do not count true/false leaf
    if (mtbdd == mtbdd_false) return 0; // do not count true/false leaf
    mtbddnode_t n = GETNODE(mtbdd);
    if (mtbddnode_getmark(n)) return 0;
    mtbddnode_setmark(n, 1);
    if (mtbddnode_isleaf(n)) return 1; // count leaf as 1
    return 1 + mtbdd_nodecount_mark(mtbddnode_getlow(n)) + mtbdd_nodecount_mark(mtbddnode_gethigh(n));
}

size_t
mtbdd_nodecount(MTBDD mtbdd)
{
    size_t result = mtbdd_nodecount_mark(mtbdd);
    mtbdd_unmark_rec(mtbdd);
    return result;
}

TASK_2(int, mtbdd_test_isvalid_rec, MTBDD, dd, uint32_t, parent_var)
{
    // check if True/False leaf
    if (dd == mtbdd_true || dd == mtbdd_false) return 1;

    // check if index is in array
    uint64_t index = dd & (~mtbdd_complement);
    assert(index > 1 && index < nodes->table_size);
    if (index <= 1 || index >= nodes->table_size) return 0;

    // check if marked
    int marked = llmsset_is_marked(nodes, index);
    assert(marked);
    if (marked == 0) return 0;

    // check if leaf
    mtbddnode_t n = GETNODE(dd);
    if (mtbddnode_isleaf(n)) return 1; // we're fine

    // check variable order
    uint32_t var = mtbddnode_getvariable(n);
    assert(var > parent_var);
    if (var <= parent_var) return 0;

    // check cache
    uint64_t result;
    if (cache_get3(CACHE_BDD_ISBDD, dd, 0, 0, &result)) {
        return result;
    }

    // check recursively
    SPAWN(mtbdd_test_isvalid_rec, node_getlow(dd, n), var);
    result = (uint64_t)CALL(mtbdd_test_isvalid_rec, node_gethigh(dd, n), var);
    if (!SYNC(mtbdd_test_isvalid_rec)) result = 0;

    // put in cache and return result
    cache_put3(CACHE_BDD_ISBDD, dd, 0, 0, result);
    return result;
}

TASK_IMPL_1(int, mtbdd_test_isvalid, MTBDD, dd)
{
    // check if True/False leaf
    if (dd == mtbdd_true || dd == mtbdd_false) return 1;

    // check if index is in array
    uint64_t index = dd & (~mtbdd_complement);
    assert(index > 1 && index < nodes->table_size);
    if (index <= 1 || index >= nodes->table_size) return 0;

    // check if marked
    int marked = llmsset_is_marked(nodes, index);
    assert(marked);
    if (marked == 0) return 0;

    // check if leaf
    mtbddnode_t n = GETNODE(dd);
    if (mtbddnode_isleaf(n)) return 1; // we're fine

    // check recursively
    uint32_t var = mtbddnode_getvariable(n);
    SPAWN(mtbdd_test_isvalid_rec, node_getlow(dd, n), var);
    int result = CALL(mtbdd_test_isvalid_rec, node_gethigh(dd, n), var);
    if (!SYNC(mtbdd_test_isvalid_rec)) result = 0;
    return result;
}

/**
 * Export to .dot file
 */

static void
mtbdd_fprintdot_rec(FILE *out, MTBDD mtbdd, print_terminal_label_cb cb)
{
    mtbddnode_t n = GETNODE(mtbdd); // also works for mtbdd_false
    if (mtbddnode_getmark(n)) return;
    mtbddnode_setmark(n, 1);

    if (mtbdd == mtbdd_true || mtbdd == mtbdd_false) {
        fprintf(out, "0 [shape=box, style=filled, label=\"F\"];\n");
    } else if (mtbddnode_isleaf(n)) {
        uint32_t type = mtbddnode_gettype(n);
        uint64_t value = mtbddnode_getvalue(n);
        fprintf(out, "%" PRIu64 " [shape=box, style=filled, label=\"", MTBDD_STRIPMARK(mtbdd));
        switch (type) {
        case 0:
            fprintf(out, "%" PRIu64, value);
            break;
        case 1:
            fprintf(out, "%f", *(double*)&value);
            break;
        case 2:
            fprintf(out, "%u/%u", (uint32_t)(value>>32), (uint32_t)value);
            break;
        default:
            cb(out, type, value);
            break;
        }
        fprintf(out, "\"];\n");
    } else {
        fprintf(out, "%" PRIu64 " [label=\"%" PRIu32 "\"];\n",
                MTBDD_STRIPMARK(mtbdd), mtbddnode_getvariable(n));

        mtbdd_fprintdot_rec(out, mtbddnode_getlow(n), cb);
        mtbdd_fprintdot_rec(out, mtbddnode_gethigh(n), cb);

        fprintf(out, "%" PRIu64 " -> %" PRIu64 " [style=dashed];\n",
                MTBDD_STRIPMARK(mtbdd), mtbddnode_getlow(n));
        fprintf(out, "%" PRIu64 " -> %" PRIu64 " [style=solid dir=both arrowtail=%s];\n",
                MTBDD_STRIPMARK(mtbdd), MTBDD_STRIPMARK(mtbddnode_gethigh(n)),
                mtbddnode_getcomp(n) ? "dot" : "none");
    }
}

void
mtbdd_fprintdot(FILE *out, MTBDD mtbdd, print_terminal_label_cb cb)
{
    fprintf(out, "digraph \"DD\" {\n");
    fprintf(out, "graph [dpi = 300];\n");
    fprintf(out, "center = true;\n");
    fprintf(out, "edge [dir = forward];\n");
    fprintf(out, "root [style=invis];\n");
    fprintf(out, "root -> %" PRIu64 " [style=solid dir=both arrowtail=%s];\n",
            MTBDD_STRIPMARK(mtbdd), MTBDD_HASMARK(mtbdd) ? "dot" : "none");

    mtbdd_fprintdot_rec(out, mtbdd, cb);
    mtbdd_unmark_rec(mtbdd);

    fprintf(out, "}\n");
}

/**
 * Return 1 if the map contains the key, 0 otherwise.
 */
int
mtbdd_map_contains(MTBDDMAP map, uint32_t key)
{
    while (!mtbdd_map_isempty(map)) {
        mtbddnode_t n = GETNODE(map);
        uint32_t k = mtbddnode_getvariable(n);
        if (k == key) return 1;
        if (k > key) return 0;
        map = node_getlow(map, n);
    }

    return 0;
}

/**
 * Retrieve the number of keys in the map.
 */
size_t
mtbdd_map_count(MTBDDMAP map)
{
    size_t r = 0;

    while (!mtbdd_map_isempty(map)) {
        r++;
        map = mtbdd_map_next(map);
    }

    return r;
}

/**
 * Add the pair <key,value> to the map, overwrites if key already in map.
 */
MTBDDMAP
mtbdd_map_add(MTBDDMAP map, uint32_t key, MTBDD value)
{
    if (mtbdd_map_isempty(map)) return mtbdd_makenode(key, mtbdd_map_empty(), value);

    mtbddnode_t n = GETNODE(map);
    uint32_t k = mtbddnode_getvariable(n);

    if (k < key) {
        // add recursively and rebuild tree
        MTBDDMAP low = mtbdd_map_add(node_getlow(map, n), key, value);
        return mtbdd_makenode(k, low, node_gethigh(map, n));
    } else if (k > key) {
        return mtbdd_makenode(key, map, value);
    } else {
        // replace old
        return mtbdd_makenode(key, node_getlow(map, n), value);
    }
}

/**
 * Add all values from map2 to map1, overwrites if key already in map1.
 */
MTBDDMAP
mtbdd_map_addall(MTBDDMAP map1, MTBDDMAP map2)
{
    if (mtbdd_map_isempty(map1)) return map2;
    if (mtbdd_map_isempty(map2)) return map1;

    mtbddnode_t n1 = GETNODE(map1);
    mtbddnode_t n2 = GETNODE(map2);
    uint32_t k1 = mtbddnode_getvariable(n1);
    uint32_t k2 = mtbddnode_getvariable(n2);

    MTBDDMAP result;
    if (k1 < k2) {
        MTBDDMAP low = mtbdd_map_addall(node_getlow(map1, n1), map2);
        result = mtbdd_makenode(k1, low, node_gethigh(map1, n1));
    } else if (k1 > k2) {
        MTBDDMAP low = mtbdd_map_addall(map1, node_getlow(map2, n2));
        result = mtbdd_makenode(k2, low, node_gethigh(map2, n2));
    } else {
        MTBDDMAP low = mtbdd_map_addall(node_getlow(map1, n1), node_getlow(map2, n2));
        result = mtbdd_makenode(k2, low, node_gethigh(map2, n2));
    }

    return result;
}

/**
 * Remove the key <key> from the map and return the result
 */
MTBDDMAP
mtbdd_map_remove(MTBDDMAP map, uint32_t key)
{
    if (mtbdd_map_isempty(map)) return map;

    mtbddnode_t n = GETNODE(map);
    uint32_t k = mtbddnode_getvariable(n);

    if (k < key) {
        MTBDDMAP low = mtbdd_map_remove(node_getlow(map, n), key);
        return mtbdd_makenode(k, low, node_gethigh(map, n));
    } else if (k > key) {
        return map;
    } else {
        return node_getlow(map, n);
    }
}

/**
 * Remove all keys in the cube <variables> from the map and return the result
 */
MTBDDMAP
mtbdd_map_removeall(MTBDDMAP map, MTBDD variables)
{
    if (mtbdd_map_isempty(map)) return map;
    if (variables == mtbdd_true) return map;

    mtbddnode_t n1 = GETNODE(map);
    mtbddnode_t n2 = GETNODE(variables);
    uint32_t k1 = mtbddnode_getvariable(n1);
    uint32_t k2 = mtbddnode_getvariable(n2);

    if (k1 < k2) {
        MTBDDMAP low = mtbdd_map_removeall(node_getlow(map, n1), variables);
        return mtbdd_makenode(k1, low, node_gethigh(map, n1));
    } else if (k1 > k2) {
        return mtbdd_map_removeall(map, node_gethigh(variables, n2));
    } else {
        return mtbdd_map_removeall(node_getlow(map, n1), node_gethigh(variables, n2));
    }
}
