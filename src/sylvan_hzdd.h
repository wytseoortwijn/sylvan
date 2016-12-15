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

/**
 * This is an implementation of Hybrid Multi-Terminal Zero-Suppressed Binary Decision Diagrams.
 */

/* Do not include this file directly. Instead, include sylvan.h */

#ifndef SYLVAN_HZDD_H
#define SYLVAN_HZDD_H

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

/**
 * Hybrid ZDDs, combining ZDD and BDD minimisation rules.
 *
 * Each edge to a node has a tag. Tag 0xfffff is magical "*".
 * The edge to False always has tag *.
 * The edge to a terminal for an empty domain also has tag *.
 *
 * Edges to nodes and terminals are interpreted under a given domain.
 * - tag X means all variables from X to the node/terminal use the ZDD rule
 *           and all variables before X use the BDD rule
 * - tag * means all variables use the BDD rule
 */

/**
 * An HZDD is a 64-bit value. The low 40 bits are an index into the unique table.
 * The highest 1 bit is the complement edge, indicating negation.
 * For Boolean HZDDs, this means "not X", for Integer and Real HZDDs, this means "-X".
 */
typedef uint64_t HZDD;
typedef HZDD HZDDMAP;

/**
 * Todo check:
 * Special tag "0xfffff" means "bottom" for when there are NO (0,k) nodes
 * Complement edges: transfer only to low child, not to high child.
 * Note that this value of "true" is for the "empty" domain
 */
#define hzdd_complement    ((HZDD)0x8000000000000000LL)
#define hzdd_emptydomain   ((HZDD)0x000fffff00000000LL)
#define hzdd_false         ((HZDD)0x000fffff00000000LL)
#define hzdd_true          ((HZDD)0x800fffff00000000LL)
#define hzdd_invalid       ((HZDD)0xffffffffffffffffLL)

/**
 * Initialize HZDD functionality.
 * This initializes internal and external referencing datastructures,
 * and registers them in the garbage collection framework.
 */
void sylvan_init_hzdd(void);

/**
 * Create a HZDD terminal of type <type> and value <value>.
 * For custom types, the value could be a pointer to some external struct.
 */
HZDD hzdd_makeleaf(uint32_t type, uint64_t value);

/**
 * Create an internal HZDD node of Boolean variable <var>, with low edge <low> and high edge <high>.
 * <var> is a 24-bit integer.
 * Please note that this does NOT check variable ordering!
 */
HZDD hzdd_makenode(uint32_t var, HZDD low, HZDD high, uint32_t nextvar);

/**
 * ...
 */
HZDD hzdd_extendtag(HZDD dd, uint32_t from, uint32_t to);

/**
 * Returns 1 is the HZDD is a terminal, or 0 otherwise.
 */
int hzdd_isleaf(HZDD hzdd);
#define hzdd_isnode(hzdd) (hzdd_isleaf(hzdd) ? 0 : 1)

/**
 * For HZDD terminals, returns <type> and <value>
 */
uint32_t hzdd_gettype(HZDD terminal);
uint64_t hzdd_getvalue(HZDD terminal);

/**
 * For internal HZDD nodes, returns <var>, <low> and <high>
 */
uint32_t hzdd_getvar(HZDD node);
HZDD hzdd_getlow(HZDD node);
HZDD hzdd_gethigh(HZDD node);
HZDD hzdd_eval(HZDD dd, uint32_t variable, int value);

/**
 * Compute the complement of the HZDD.
 * For Boolean HZDDs, this means "not X".
 */
// #define hzdd_hascomp(dd) ((dd & hzdd_complement) ? 1 : 0)
// #define hzdd_comp(dd) (dd ^ hzdd_complement)
// #define hzdd_not(dd) (dd ^ hzdd_complement)
// TODO: create proper hzdd_not

HZDD hzdd_ithvar(uint32_t var);

/**
 * Convert an MTBDD to a HZDD.
 */
TASK_DECL_2(HZDD, hzdd_from_mtbdd, MTBDD, MTBDD);
#define hzdd_from_mtbdd(dd, domain) CALL(hzdd_from_mtbdd, dd, domain)

/**
 * Compute the and operator for two boolean HZDDs
 */
TASK_DECL_2(HZDD, hzdd_and, HZDD, HZDD);
#define hzdd_and(a, b) CALL(hzdd_and, a, b)

/**
 * Count the number of HZDD nodes and terminals (excluding hzdd_false and hzdd_true) in the given <count> HZDDs
 */
size_t hzdd_nodecount_more(const HZDD *hzdds, size_t count);

static inline size_t
hzdd_nodecount(const HZDD dd) {
    return hzdd_nodecount_more(&dd, 1);
}

/**
 * Write a .dot representation of a given HZDD
 */
void hzdd_fprintdot(FILE *out, HZDD mtbdd);
#define hzdd_printdot(dd, cb) hzdd_fprintdot(stdout, dd)

/**
 * Write a .dot representation of a given HZDD, but without complement edges.
 */
// void hzdd_fprintdot_nc(FILE *out, HZDD mtbdd);
// #define hzdd_printdot_nc(dd, cb) hzdd_fprintdot_nc(stdout, dd)

/**
 * Garbage collection
 * Sylvan supplies two default methods to handle references to nodes, but the user
 * is encouraged to implement custom handling. Simply add a handler using sylvan_gc_add_mark
 * and let the handler call hzdd_gc_mark_rec for every HZDD that should be saved
 * during garbage collection.
 */

/**
 * Call hzdd_gc_mark_rec for every hzdd you want to keep in your custom mark functions.
 */
VOID_TASK_DECL_1(hzdd_gc_mark_rec, HZDD);
#define hzdd_gc_mark_rec(hzdd) CALL(hzdd_gc_mark_rec, hzdd)

/**
 * Default external referencing. During garbage collection, HZDDs marked with hzdd_ref will
 * be kept in the forest.
 * It is recommended to prefer hzdd_protect and hzdd_unprotect.
 */
HZDD hzdd_ref(HZDD a);
void hzdd_deref(HZDD a);
size_t hzdd_count_refs(void);

/**
 * Default external pointer referencing. During garbage collection, the pointers are followed and the HZDD
 * that they refer to are kept in the forest.
 */
void hzdd_protect(HZDD* ptr);
void hzdd_unprotect(HZDD* ptr);
size_t hzdd_count_protected(void);

/**
 * If hzdd_set_ondead is set to a callback, then this function marks HZDDs (terminals).
 * When they are dead after the mark phase in garbage collection, the callback is called for marked HZDDs.
 * The ondead callback can either perform cleanup or resurrect dead terminals.
 */
#define hzdd_notify_ondead(dd) llmsset_notify_ondead(nodes, dd&~hzdd_complement)

/**
 * Infrastructure for internal references (per-thread, e.g. during HZDD operations)
 * Use hzdd_refs_push and hzdd_refs_pop to put HZDDs on a thread-local reference stack.
 * Use hzdd_refs_spawn and hzdd_refs_sync around SPAWN and SYNC operations when the result
 * of the spawned Task is a HZDD that must be kept during garbage collection.
 */
typedef struct hzdd_refs_internal
{
    size_t r_size, r_count;
    size_t s_size, s_count;
    HZDD *results;
    Task **spawns;
} *hzdd_refs_internal_t;

extern DECLARE_THREAD_LOCAL(hzdd_refs_key, hzdd_refs_internal_t);

static inline HZDD
hzdd_refs_push(HZDD hzdd)
{
    LOCALIZE_THREAD_LOCAL(hzdd_refs_key, hzdd_refs_internal_t);
    if (hzdd_refs_key->r_count >= hzdd_refs_key->r_size) {
        hzdd_refs_key->r_size *= 2;
        hzdd_refs_key->results = (HZDD*)realloc(hzdd_refs_key->results, sizeof(HZDD) * hzdd_refs_key->r_size);
    }
    hzdd_refs_key->results[hzdd_refs_key->r_count++] = hzdd;
    return hzdd;
}

static inline void
hzdd_refs_pop(int amount)
{
    LOCALIZE_THREAD_LOCAL(hzdd_refs_key, hzdd_refs_internal_t);
    hzdd_refs_key->r_count-=amount;
}

static inline void
hzdd_refs_spawn(Task *t)
{
    LOCALIZE_THREAD_LOCAL(hzdd_refs_key, hzdd_refs_internal_t);
    if (hzdd_refs_key->s_count >= hzdd_refs_key->s_size) {
        hzdd_refs_key->s_size *= 2;
        hzdd_refs_key->spawns = (Task**)realloc(hzdd_refs_key->spawns, sizeof(Task*) * hzdd_refs_key->s_size);
    }
    hzdd_refs_key->spawns[hzdd_refs_key->s_count++] = t;
}

static inline HZDD
hzdd_refs_sync(HZDD result)
{
    LOCALIZE_THREAD_LOCAL(hzdd_refs_key, hzdd_refs_internal_t);
    hzdd_refs_key->s_count--;
    return result;
}

#ifdef __cplusplus
}
#endif /* __cplusplus */

#endif
