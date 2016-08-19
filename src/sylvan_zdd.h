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

/**
 * This is a multi-core implementation of Zero-suppressed Binary Decision Diagrams.
 * 
 * Unlike BDDs, the interpretation of a ZDD depends on the "domain" of variables.
 * Variables not encountered in the ZDD are *false*.
 * The representation of the universe set is NOT the leaf "true".
 * Also, no complement edges. They do not work here.
 * Thus, computing "not" is not a trivial constant operation.
 * 
 * To represent "domain" and "set of variables" we use the same cubes
 * as for BDDs, i.e., var1 \and var2 \and var3... 
 * 
 * All operations with multiple input ZDDs interpret the ZDDs in the same domain.
 * For some operations, this domain must be supplied.
 */

/* Do not include this file directly. Instead, include sylvan.h */

#ifndef SYLVAN_ZDD_H
#define SYLVAN_ZDD_H

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

typedef uint64_t ZDD;
typedef ZDD ZDDMAP;

#define zdd_false       ((uint64_t)0)
#define zdd_true        ((uint64_t)1)

/**
 * Initialize ZDD functionality.
 * This initializes internal and external referencing datastructures,
 * and registers them in the garbage collection framework.
 */
void sylvan_init_zdd();

/**
 * Create an internal ZDD node of Boolean variable <var>, with low edge <low> and high edge <high>.
 * <var> is a 24-bit integer.
 */
ZDD _zdd_makenode(uint32_t var, ZDD low, ZDD high);
#define zdd_makenode(var, low, high) (high == zdd_false ? low : _zdd_makenode(var, low, high))

/**
 * Returns 1 is the ZDD is a terminal, or 0 otherwise.
 */
#define zdd_isleaf(dd) ((dd == zdd_false || dd == zdd_true) ? 1 : 0)
#define zdd_isnode(dd) (zdd_isleaf(dd) ? 0 : 1)

uint32_t zdd_getvar(ZDD dd);
ZDD zdd_getlow(ZDD dd);
ZDD zdd_gethigh(ZDD dd);

/**
 * Create the conjunction of variables in arr, i.e.,
 * arr[0] \and arr[1] \and ... \and arr[length-1]
 */
ZDD zdd_set_fromarray(uint32_t* arr, size_t length);

/**
 * Create a ZDD cube representing the conjunction of variables in their positive or negative
 * form depending on whether the cube[idx] equals 0 (negative), 1 (positive) or 2 (any).
 * Use cube[idx]==3 for "s=s'" in interleaved variables (matches with next variable)
 * <variables> is the cube of variables (var1 \and var2 \and ... \and varn)
 * The resulting ZDD is defined on the domain <variables>.
 */
ZDD zdd_cube(ZDD variables, uint8_t *cube, ZDD terminal);

/**
 * Merge two set of variables / domains
 */
ZDD zdd_set_union(ZDD cube1, ZDD cube2);

/**
 * Obtain ZDD representing variable.
 */
ZDD zdd_ithvar(uint32_t var);
ZDD zdd_nithvar(uint32_t var);

/**
 * Extend the domain of a ZDD, such that for all valuations
 * of the introduced variables, the interpretation
 * of (dd,from_domain) is equal to (result,to_domain).
 * The <from_domain> must be >= zdd_support(dd) and
 * the <to_domain> must be >= from_domain.
 */
ZDD zdd_extend_domain(ZDD dd, ZDD from_domain, ZDD to_domain);

/**
 * Count the number of satisfying assignments (minterms) leading to the leaf true.
 * We do not need to give the domain, as skipped variables do not increase the number of minterms.
 */
TASK_DECL_1(double, zdd_satcount, ZDD);
#define zdd_satcount(dd, dom) CALL(zdd_satcount, dd)

// TODO: add pathcount; and check if in CUDD path count and minterm count are different

/**
 * Count the number of nodes in a ZDD
 */
size_t zdd_nodecount(ZDD zdd);

/**
 * Compute IF <f> THEN <g> ELSE <h>.
 */
TASK_DECL_3(ZDD, zdd_ite, ZDD, ZDD, ZDD);
#define zdd_ite(f, g, h) CALL(zdd_ite, f, g, h)

/**
 * Compute the negation of a ZDD.
 * We also need to know the domain of the ZDD.
 */
TASK_DECL_2(ZDD, zdd_not, ZDD, ZDD);
#define zdd_not(dd, domain) CALL(zdd_not, dd, domain)

/**
 * Compute logical AND of <a> and <b>.
 */
TASK_DECL_2(ZDD, zdd_and, ZDD, ZDD);
#define zdd_and(a, b) CALL(zdd_and, a, b)

/**
 * Compute logical OR of <a> and <b>.
 */
TASK_DECL_2(ZDD, zdd_or, ZDD, ZDD);
#define zdd_or(a, b) CALL(zdd_or, a, b)

/**
 * Compute logical XOR of <a> and <b>.
 */
TASK_DECL_2(ZDD, zdd_xor, ZDD, ZDD);
#define zdd_xor(a, b) CALL(zdd_xor, a, b)

/**
 * Compute logical EQUIV of <a> and <b>.
 * Also called bi-implication. (a <-> b)
 * This operation requires the variable domain <dom>.
 */
TASK_DECL_3(ZDD, zdd_equiv, ZDD, ZDD, ZDD);
#define zdd_equiv(a, b, dom) CALL(zdd_equiv, a, b, dom)

/**
 * Compute logical IMP of <a> and <b>. (a -> b)
 * This operation requires the variable domain <dom>.
 */
TASK_DECL_3(ZDD, zdd_imp, ZDD, ZDD, ZDD);
#define zdd_imp(a, b, dom) CALL(zdd_imp, a, b, dom)

/**
 * Compute logical INVIMP of <a> and <b>. (b <- a)
 * This operation requires the variable domain <dom>.
 */
TASK_DECL_3(ZDD, zdd_invimp, ZDD, ZDD, ZDD);
#define zdd_invimp(a, b, dom) CALL(zdd_invimp, a, b, dom)

// add binary operators
// zdd_diff (no domain) == a and not b
// zdd_less (no domain) == not a and b
// zdd_nand (domain)    == not (a and b)
// zdd_nor (domain)     == not a and not b

/**
 * Compute \exists <vars>: <a> and <b>.
 */
TASK_DECL_3(ZDD, zdd_and_exists, ZDD, ZDD, ZDD);
#define zdd_and_exists(a, b, vars) CALL(zdd_and_exists, a, b, vars)

/**
 * Compute \exists <vars>: <dd>.
 */
TASK_DECL_2(ZDD, zdd_exists, ZDD, ZDD);
#define zdd_exists(dd, vars) CALL(zdd_exists, dd, vars)

/**
 * Calculate the support of a ZDD, i.e. the cube of all variables that appear in the ZDD nodes.
 */
TASK_DECL_1(ZDD, zdd_support, ZDD);
#define zdd_support(dd) CALL(zdd_support, dd)

/**
 * Function composition, for each node with variable <key> which has a <key,value> pair in <map>,
 * replace the node by the result of zdd_ite(<value>, <low>, <high>).
 * Each <value> in <map> must be a Boolean ZDD.
 */
// TASK_DECL_2(ZDD, zdd_compose, ZDD, ZDDMAP);
// #define zdd_compose(dd, map) CALL(zdd_compose, dd, map)

/**
 * Given a ZDD <dd> and a cube of variables <variables> expected in <dd>,
 * zdd_enum_first and zdd_enum_next enumerates the unique paths in <dd> that lead to a non-False leaf.
 * 
 * The function returns the leaf (or zdd_false if no new path is found) and encodes the path
 * in the supplied array <arr>: 0 for a low edge, 1 for a high edge, and 2 if the variable is skipped.
 *
 * The supplied array <arr> must be large enough for all variables in <variables>.
 *
 * Usage:
 * ZDD leaf = zdd_enum_first(dd, variables, arr, NULL);
 * while (leaf != zdd_false) {
 *     .... // do something with arr/leaf
 *     leaf = zdd_enum_next(dd, variables, arr, NULL);
 * }
 *
 * The callback is an optional function that returns 0 when the given terminal node should be skipped.
 */
// typedef int (*zdd_enum_filter_cb)(ZDD);
// ZDD zdd_enum_first(ZDD dd, ZDD variables, uint8_t *arr, zdd_enum_filter_cb filter_cb);
// ZDD zdd_enum_next(ZDD dd, ZDD variables, uint8_t *arr, zdd_enum_filter_cb filter_cb);

/**
 * For debugging.
 * Tests if all nodes in the ZDD are correctly ``marked'' in the nodes table.
 * Tests if variables in the internal nodes appear in-order.
 * In Debug mode, this will cause assertion failures instead of returning 0.
 * Returns 1 if all is fine, or 0 otherwise.
 */
// TASK_DECL_1(int, zdd_test_isvalid, ZDD);
// #define zdd_test_isvalid(zdd) CALL(zdd_test_isvalid, zdd)

/**
 * Write a DOT representation of a ZDD
 * The callback function is required for custom terminals.
 */
// void zdd_fprintdot(FILE *out, ZDD zdd);
// #define zdd_printdot(zdd) zdd_fprintdot(stdout, zdd)

/**
 * ZDDMAP, maps uint32_t variables to ZDDs.
 * A ZDDMAP node has variable level, low edge going to the next ZDDMAP, high edge to the mapped ZDD
 */
#define zdd_map_empty() zdd_false
#define zdd_map_isempty(map) (map == zdd_false ? 1 : 0)
#define zdd_map_key(map) zdd_getvar(map)
#define zdd_map_value(map) zdd_gethigh(map)
#define zdd_map_next(map) zdd_getlow(map)

/**
 * Return 1 if the map contains the key, 0 otherwise.
 */
int zdd_map_contains(ZDDMAP map, uint32_t key);

/**
 * Retrieve the number of keys in the map.
 */
size_t zdd_map_count(ZDDMAP map);

/**
 * Add the pair <key,value> to the map, overwrites if key already in map.
 */
ZDDMAP zdd_map_add(ZDDMAP map, uint32_t key, ZDD value);

/**
 * Add all values from map2 to map1, overwrites if key already in map1.
 */
ZDDMAP zdd_map_addall(ZDDMAP map1, ZDDMAP map2);

/**
 * Remove the key <key> from the map and return the result
 */
ZDDMAP zdd_map_remove(ZDDMAP map, uint32_t key);

/**
 * Remove all keys in the cube <variables> from the map and return the result
 */
ZDDMAP zdd_map_removeall(ZDDMAP map, ZDD variables);

/**
 * Garbage collection
 */

/**
 * Call zdd_gc_mark_rec for every zdd you want to keep in your custom mark functions.
 */
VOID_TASK_DECL_1(zdd_gc_mark_rec, ZDD);
#define zdd_gc_mark_rec(zdd) CALL(zdd_gc_mark_rec, zdd)

/**
 * Default external referencing. During garbage collection, ZDDs marked with zdd_ref will
 * be kept in the forest.
 * It is recommended to prefer zdd_protect and zdd_unprotect.
 */
ZDD zdd_ref(ZDD a);
void zdd_deref(ZDD a);
size_t zdd_count_refs();

/**
 * Default external pointer referencing. During garbage collection, the pointers are followed and the ZDD
 * that they refer to are kept in the forest.
 */
void zdd_protect(ZDD* ptr);
void zdd_unprotect(ZDD* ptr);
size_t zdd_count_protected();

/**
 * If sylvan_set_ondead is set to a callback, then this function marks ZDDs (terminals).
 * When they are dead after the mark phase in garbage collection, the callback is called for marked ZDDs.
 * The ondead callback can either perform cleanup or resurrect dead terminals.
 */
#define zdd_notify_ondead(dd) llmsset_notify_ondead(nodes, dd&~zdd_complement)

/**
 * Infrastructure for internal references (per-thread, e.g. during ZDD operations)
 * Use zdd_refs_push and zdd_refs_pop to put ZDDs on a thread-local reference stack.
 * Use zdd_refs_spawn and zdd_refs_sync around SPAWN and SYNC operations when the result
 * of the spawned Task is a ZDD that must be kept during garbage collection.
 */
typedef struct zdd_refs_internal
{
    size_t r_size, r_count;
    size_t s_size, s_count;
    ZDD *results;
    Task **spawns;
} *zdd_refs_internal_t;

extern DECLARE_THREAD_LOCAL(zdd_refs_key, zdd_refs_internal_t);

static inline ZDD
zdd_refs_push(ZDD zdd)
{
    LOCALIZE_THREAD_LOCAL(zdd_refs_key, zdd_refs_internal_t);
    if (zdd_refs_key->r_count >= zdd_refs_key->r_size) {
        zdd_refs_key->r_size *= 2;
        zdd_refs_key->results = (ZDD*)realloc(zdd_refs_key->results, sizeof(ZDD) * zdd_refs_key->r_size);
    }
    zdd_refs_key->results[zdd_refs_key->r_count++] = zdd;
    return zdd;
}

static inline void
zdd_refs_pop(int amount)
{
    LOCALIZE_THREAD_LOCAL(zdd_refs_key, zdd_refs_internal_t);
    zdd_refs_key->r_count-=amount;
}

static inline void
zdd_refs_spawn(Task *t)
{
    LOCALIZE_THREAD_LOCAL(zdd_refs_key, zdd_refs_internal_t);
    if (zdd_refs_key->s_count >= zdd_refs_key->s_size) {
        zdd_refs_key->s_size *= 2;
        zdd_refs_key->spawns = (Task**)realloc(zdd_refs_key->spawns, sizeof(Task*) * zdd_refs_key->s_size);
    }
    zdd_refs_key->spawns[zdd_refs_key->s_count++] = t;
}

static inline ZDD
zdd_refs_sync(ZDD result)
{
    LOCALIZE_THREAD_LOCAL(zdd_refs_key, zdd_refs_internal_t);
    zdd_refs_key->s_count--;
    return result;
}

#ifdef __cplusplus
}
#endif /* __cplusplus */

/* TODO
 * Functions supported by CuDD
 * - zddOne (compute representation of universe given domain)
 * - zddToBdd
 * - bddToZdd
 * - const functions, specifically diffConst
 * - product and quotient of "unate covers" (zddUnateProduct, zddDivide)
 * - product and quotien of "binate covers" (zddProduct, zddWeakDiv)
 * - complement of a cover (zddComplement)
 * - compute irredundant sum of products... zddIsop
 * - compute BDD of ZDD cover
 * - reorder functions
 * - "realignment of variables"
 * - print a SOP representation of a ZDD
 * Functions that can be done with zdd_compose
 * - zddChange (subst variable by its complement)
 * - cofactor
 * Functions supported by EXTRA
 * - zomg, quite a lot.
 */

#endif
