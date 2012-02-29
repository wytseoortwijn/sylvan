#include "sylvan_runtime.h"
#include "sylvan.h"
#include "llgcset.h"
#include "llcache.h"
#include <pthread.h>
#include <string.h>
#include <stdio.h>
#include <assert.h>
#include <math.h>

#define complementmark 0x80000000

/**
 * Exported BDD constants
 */
const BDD sylvan_true = 0 | complementmark;
const BDD sylvan_false = 0;
const BDD sylvan_invalid = 0x7fffffff; // uint32_t

/**
 * "volatile" macros
 */
#define atomicsylvan_read(s) atomic32_read(s)
#define atomicsylvan_write(a, b) atomic32_write(a, b)

/**
 * Mark handling macros
 */
#define BDD_HASMARK(s)              (s&complementmark)
#define BDD_TOGGLEMARK(s)           (s^complementmark)
#define BDD_STRIPMARK(s)            (s&~complementmark)
#define BDD_TRANSFERMARK(from, to)  (to ^ (from & complementmark))
#define BDD_ISCONSTANT(s)           (BDD_STRIPMARK(s) == 0)

__attribute__ ((packed))
struct bddnode {
    BDD low;
    BDD high;
    BDDVAR level;
    uint8_t flags; // for marking, e.g. in node_count 
    char pad[SYLVAN_PAD(sizeof(BDD)*2+sizeof(BDDVAR)+sizeof(uint8_t), 16)];
    // 4,4,2,1,5 (pad). 
}; // 16 bytes

typedef struct bddnode* bddnode_t;

#define op_ite 0
#define op_not 1
#define op_substitute 2
#define op_exists 3
#define op_forall 4
#define op_param 5
/*
typedef unsigned char bdd_ops;

__attribute__ ((packed))
struct bddop {
    BDDVAR level; // 2 bytes
    bdd_ops type; // 1 byte
    unsigned char parameters; // 1 byte, the cumulative number of BDD parameters
    union { // 12 bytes max
        struct {
            BDDOP a;
            BDDOP b;
            BDDOP c;
        } param_ite;
        struct {
            BDDOP a;
        } param_not;
        struct {
            BDDOP a;
            BDDVAR from;
            BDD to;
        } param_substitute;
        struct {
            BDDOP a;
            BDDVAR var;
        } param_quantify;
        struct {
            unsigned short param;
        } param_param;
    };
};

typedef struct bddop* bddop_t;
*/
int initialized = 0;

static struct {
    llgcset_t data;
#if CACHE
    llcache_t cache; // operations cache
#endif
} _bdd;

static int granularity = 1; // default

// max number of parameters (set to: 5, 13, 29 to get bddcache node size 32, 64, 128)
#define MAXPARAM 3

#if CACHE
/*
 * Temporary "operations"
 * 0 = ite
 * 1 = relprods
 * 2 = relprods_reversed
 * 3 = count
 * 4 = exists
 * 5 = forall
 */
__attribute__ ((packed))
struct bddcache {
    BDDOP operation;
    BDD params[MAXPARAM];
//  uint32_t parameters; // so we don't have to read <<operation>>
    BDD result;
};

typedef struct bddcache* bddcache_t;

static const int cache_key_length = sizeof(struct bddcache) - sizeof(BDD);
static const int cache_data_length = sizeof(struct bddcache);
#endif

// Structures for statistics
typedef enum {
  C_cache_new,
  C_cache_exists,
  C_cache_reuse,
  C_cache_overwritten,
  C_gc_user,
  C_gc_hashtable_full,
  C_gc_deadlist_full,
  C_ite,
  C_exists,
  C_forall,
  C_relprods,
  C_relprods_reversed,
  C_MAX
} Counters;

#define N_CNT_THREAD 48

struct {
    unsigned int thread_id;
} thread_to_id_map[N_CNT_THREAD];

static int get_thread_id() {
    unsigned int id = pthread_self();
    int i=0;
    for (;i<N_CNT_THREAD;i++) {
        if (thread_to_id_map[i].thread_id == 0) {
            if (cas(&thread_to_id_map[i].thread_id, 0, id)) {
                return i;
            }
        } else if (thread_to_id_map[i].thread_id == id) {
            return i;
        }
    }
    assert(0); // NOT ENOUGH SPACE!!
    return -1;
}

struct {
    uint64_t count[C_MAX];
    char pad[SYLVAN_PAD(sizeof(uint64_t)*C_MAX, 64)];
} sylvan_stats[N_CNT_THREAD];

#if COLORSTATS
#define BLACK "\33[22;30m"
#define GRAY "\33[01;30m"
#define RED "\33[22;31m"
#define LRED "\33[01;31m"
#define GREEN "\33[22;32m"
#define LGREEN "\33[01;32m"
#define BLUE "\33[22;34m"
#define LBLUE "\33[01;34m"
#define BROWN "\33[22;33m"
#define YELLOW "\33[01;33m"
#define CYAN "\33[22;36m"
#define LCYAN "\33[22;36m"
#define MAGENTA "\33[22;35m"
#define LMAGENTA "\33[01;35m"
#define NC "\33[0m"
#define BOLD "\33[1m"
#define ULINE "\33[4m" //underline
#define BLINK "\33[5m"
#define INVERT "\33[7m"
#else
#define LRED 
#define NC
#define BOLD
#define ULINE
#define BLUE
#define RED
#endif

void sylvan_reset_counters()
{
    if (initialized == 0) return;

    int i,j;
    for (i=0;i<N_CNT_THREAD;i++) {
        thread_to_id_map[i].thread_id = 0;
        for (j=0;j<C_MAX;j++) {
            sylvan_stats[i].count[j] = 0;
        }
    }
}

void sylvan_report_stats()
{
    if (initialized == 0) return;

    int i,j;

    printf(LRED  "****************\n");
    printf(     "* ");
    printf(NC BOLD"SYLVAN STATS"); 
    printf(NC LRED             " *\n");
    printf(     "****************\n");
    printf(NC ULINE "Memory usage\n" NC BLUE);
    printf("BDD table:          ");
    llgcset_print_size(_bdd.data, stdout);
    printf("\n");
    printf("Cache:              ");
    llcache_print_size(_bdd.cache, stdout);
    printf("\n");
    printf(NC ULINE "Cache\n" NC BLUE);

    uint64_t totals[C_MAX];
    for (i=0;i<C_MAX;i++) totals[i] = 0;
    for (i=0;i<N_CNT_THREAD;i++) {
        for (j=0;j<C_MAX;j++) totals[j] += sylvan_stats[i].count[j];
    }

    uint64_t total_cache = totals[C_cache_new] + totals[C_cache_exists] + totals[C_cache_reuse];
    printf("New results:         %lu of %lu\n", totals[C_cache_new], total_cache);
    printf("Existing results:    %lu of %lu\n", totals[C_cache_exists], total_cache);
    printf("Reused results:      %lu of %lu\n", totals[C_cache_reuse], total_cache);
    printf("Overwritten results: %lu of %lu\n", totals[C_cache_overwritten], total_cache);
    printf(NC ULINE "GC\n" NC BLUE);
    printf("GC user-request:     %lu\n", totals[C_gc_user]);
    printf("GC full table:       %lu\n", totals[C_gc_hashtable_full]);
    printf("GC full dead-list:   %lu\n", totals[C_gc_deadlist_full]);
    printf(NC ULINE "Call counters (ITE, exists, forall, relprods, reversed relprods)\n" NC BLUE);
    for (i=0;i<N_CNT_THREAD;i++) {
        if (thread_to_id_map[i].thread_id != 0) 
            printf("Thread %02d:           %ld, %ld, %ld, %ld, %ld\n", i, 
                sylvan_stats[i].count[C_ite], sylvan_stats[i].count[C_exists], sylvan_stats[i].count[C_forall],
                sylvan_stats[i].count[C_relprods], sylvan_stats[i].count[C_relprods_reversed]);
    }
    printf(LRED  "****************" NC " \n");
}

#if STATS
#define SV_CNT(s) (sylvan_stats[get_thread_id()].count[s]+=1);
#else
#define SV_CNT(s) ; /* Empty */
#endif

/**
 * Macro's to convert BDD indices to nodes and vice versa
 */
#define GETNODE(bdd)        ((bddnode_t)llgcset_index_to_ptr(_bdd.data, BDD_STRIPMARK(bdd)))

/**
 * When a bdd node is deleted, unref the children
 */
void sylvan_bdd_delete(const void* data, bddnode_t node)
{
    sylvan_deref(node->low);
    sylvan_deref(node->high);
}

/**
 * Called pre-gc : first, gc the cache to free nodes
 */
void sylvan_bdd_pregc(const void* data, gc_reason reason) 
{
    if (reason == gc_user) { SV_CNT(C_gc_user); }
    else if (reason == gc_hashtable_full) { SV_CNT(C_gc_hashtable_full); }

#if CACHE
    llcache_clear(_bdd.cache);
#endif
}
#if CACHE
/**
 * When a cache item is deleted, deref all involved BDDs
 */
void sylvan_cache_delete(const void *p, const bddcache_t cache)
{
    assert (cache->result != sylvan_invalid);

    int i;
    for (i=0;i<MAXPARAM/*cache->parameters*/;i++) {
        sylvan_deref(cache->params[i]);
    }
    sylvan_deref(cache->result);
}
#endif

/** Random number generator */
unsigned long rng_hash_128(unsigned long long seed[]);

unsigned long get_random() 
{
    static unsigned long long seed[2]; // 256 bits
    return rng_hash_128(seed);
}

void sylvan_package_init()
{
}

void sylvan_package_exit()
{
}


/**
 * Initialize sylvan
 * - datasize / cachesize : number of bits ...
 */
void sylvan_init(size_t tablesize, size_t cachesize, int _granularity)
{
    if (initialized != 0) return;
    initialized = 1;

    sylvan_reset_counters();

    granularity = _granularity;
    
    // Sanity check
    if (sizeof(struct bddnode) != 16) {
        fprintf(stderr, "Invalid size of bdd nodes: %ld\n", sizeof(struct bddnode));
        exit(1);
    }
#if CACHE
/*
    if (sizeof(struct bddcache) != next_pow2(sizeof(struct bddcache))) {
        fprintf(stderr, "Invalid size of bdd operation cache: %ld\n", sizeof(struct bddcache));
        exit(1);
    }
*/
#endif
    //fprintf(stderr, "Sylvan\n");
    
    if (tablesize >= 30) {
        rt_report_and_exit(1, "BDD_init error: tablesize must be < 30!");
    }
    _bdd.data = llgcset_create(10, sizeof(struct bddnode), 1<<tablesize, (llgcset_delete_f)&sylvan_bdd_delete, sylvan_bdd_pregc, NULL);


#if CACHE    
    if (cachesize >= 30) {
        rt_report_and_exit(1, "BDD_init error: cachesize must be <= 30!");
    }
    
    _bdd.cache = llcache_create(cache_key_length, cache_data_length, 1<<cachesize, (llcache_delete_f)&sylvan_cache_delete, NULL);

#endif
}

void sylvan_quit()
{
    if (initialized == 0) return;
    initialized = 0;

#if CACHE
    llcache_free(_bdd.cache);
#endif
    llgcset_free(_bdd.data);
}

BDD sylvan_ref(BDD a) 
{
    assert(a != sylvan_invalid);
    if (!BDD_ISCONSTANT(a)) llgcset_ref(_bdd.data, BDD_STRIPMARK(a));
    return a;
}

void sylvan_deref(BDD a)
{
    assert(a != sylvan_invalid);
    if (BDD_ISCONSTANT(a)) return;
    llgcset_deref(_bdd.data, BDD_STRIPMARK(a));
}

void sylvan_gc()
{
    if (initialized == 0) return;
    llgcset_gc(_bdd.data, gc_user);
}

/**
 * MAKENODE (level, low, high)
 * Requires ref on low, high.
 * Ensures ref on result node.
 * This will ref the result node. Refs on low and high disappear.
 */
inline BDD sylvan_makenode(BDDVAR level, BDD low, BDD high)
{
    BDD result;
    struct bddnode n;
    memset(&n, 0, sizeof(struct bddnode));

    if (low == high) {
        sylvan_deref(high);
        return low;
    }
    n.level = level;
    n.flags = 0;

    // Normalization to keep canonicity
    // low will have no mark

    int created;

    if (BDD_HASMARK(low)) {
        // ITE(a,not b,c) == not ITE(a,b,not c)
        n.low = BDD_STRIPMARK(low);
        n.high = BDD_TOGGLEMARK(high);

        if (llgcset_get_or_create(_bdd.data, &n, &created, &result) == 0) {
            rt_report_and_exit(1, "BDD Unique table full!");
        }
        
        if (!created) {
            sylvan_deref(low);
            sylvan_deref(high);
        }

        return result | complementmark;
    } else {
        n.low = low;
        n.high = high;

        if(llgcset_get_or_create(_bdd.data, &n, &created, &result) == 0) {
            rt_report_and_exit(1, "BDD Unique table full!");
        }

        if (!created) {
            sylvan_deref(low);
            sylvan_deref(high);
        }

        return result;
    }
}

inline BDD sylvan_ithvar(BDDVAR level)
{
    return sylvan_makenode(level, sylvan_false, sylvan_true);
}

inline BDD sylvan_nithvar(BDDVAR level)
{
    return sylvan_makenode(level, sylvan_true, sylvan_false);
}

inline BDDVAR sylvan_var(BDD bdd)
{
    assert(!BDD_ISCONSTANT(bdd));
    return GETNODE(bdd)->level;
}

/**
 * Get the n=0 child.
 * This will ref the result node.
 */
inline BDD sylvan_low(BDD bdd)
{
    if (bdd == sylvan_false || bdd == sylvan_true) return bdd;
    BDD low = GETNODE(bdd)->low;
    sylvan_ref(low);
    return BDD_TRANSFERMARK(bdd, low);
}

/**
 * Get the n=1 child.
 * This will ref the result node.
 */
inline BDD sylvan_high(BDD bdd)
{
    if (bdd == sylvan_false || bdd == sylvan_true) return bdd;
    BDD high = GETNODE(bdd)->high;
    sylvan_ref(high);
    return BDD_TRANSFERMARK(bdd, high);
}

// Macros for internal use (no ref)
#define LOW(a) ((BDD_ISCONSTANT(a))?a:BDD_TRANSFERMARK(a, GETNODE(a)->low))
#define HIGH(a) ((BDD_ISCONSTANT(a))?a:BDD_TRANSFERMARK(a, GETNODE(a)->high))

/**
 * Get the complement of the BDD.
 * This will ref the result node.
 */
inline BDD sylvan_not(BDD bdd)
{
    sylvan_ref(bdd);
    return BDD_TOGGLEMARK(bdd);
}

BDD sylvan_and(BDD a, BDD b)
{
    return sylvan_ite(a, b, sylvan_false);
}

BDD sylvan_xor(BDD a, BDD b)
{
    return sylvan_ite(a, BDD_TOGGLEMARK(b), b);
}

BDD sylvan_or(BDD a, BDD b) 
{
    return sylvan_ite(a, sylvan_true, b);
}

BDD sylvan_nand(BDD a, BDD b)
{
    return sylvan_ite(a, BDD_TOGGLEMARK(b), sylvan_true);
}

BDD sylvan_nor(BDD a, BDD b)
{
    return sylvan_ite(a, sylvan_false, BDD_TOGGLEMARK(b));
}

BDD sylvan_imp(BDD a, BDD b)
{
    return sylvan_ite(a, b, sylvan_true);
}

BDD sylvan_biimp(BDD a, BDD b)
{
    return sylvan_ite(a, b, BDD_TOGGLEMARK(b));
}

BDD sylvan_diff(BDD a, BDD b) 
{
    return sylvan_ite(a, BDD_TOGGLEMARK(b), sylvan_false);
}

BDD sylvan_less(BDD a, BDD b)
{
    return sylvan_ite(a, sylvan_false, b);
}

BDD sylvan_invimp(BDD a, BDD b) 
{
    return sylvan_ite(a, sylvan_false, BDD_TOGGLEMARK(b));
}

/**
 * Calculate standard triples. Find trivial cases.
 * Returns either
 * - sylvan_invalid | complement
 * - sylvan_invalid
 * - a result BDD
 * This function does not alter reference counters.
 */
static BDD sylvan_triples(BDD *_a, BDD *_b, BDD *_c)
{
    BDD a=*_a, b=*_b, c=*_c;
    
    // TERMINAL CASE (attempt 1)
    // ITE(T,B,C) = B
    // ITE(F,B,C) = C
    if (a == sylvan_true) return b;
    if (a == sylvan_false) return c;
    
    // Normalization to standard triples
    // ITE(A,A,C) = ITE(A,T,C)
    // ITE(A,~A,C) = ITE(A,F,C)
    if (BDD_STRIPMARK(a) == BDD_STRIPMARK(b)) {
        if (a == b) b = sylvan_true;
        else b = sylvan_false;
    }
    
    // ITE(A,B,A) = ITE(A,B,T)
    // ITE(A,B,~A) = ITE(A,B,F)
    if (BDD_STRIPMARK(a) == BDD_STRIPMARK(c)) {
        if (a != c) c = sylvan_true;
        else c = sylvan_false;
    }
    
    if (b == c) return b;
    if (b == sylvan_true && c == sylvan_false) return a;
    if (b == sylvan_false && c == sylvan_true) return BDD_TOGGLEMARK(a);
    
    if (BDD_ISCONSTANT(b) && BDD_STRIPMARK(c) < BDD_STRIPMARK(a)) {
        if (b == sylvan_false) {
            // ITE(A,F,C) = ITE(~C,F,~A)
            //            = (A and F) or (~A and C)
            //            = F or (~A and C)
            //            = (~C and F) or (C and ~A)
            //            = ITE(~C,F,~A)
            BDD t = a;
            a = BDD_TOGGLEMARK(c);
            c = BDD_TOGGLEMARK(t);
        } else {
            // ITE(A,T,C) = ITE(C,T,A)
            //            = (A and T) or (~A and C)
            //            = A or (~A and C)
            //            = C or (~C and A)
            //            = (C and T) or (~C and A)
            //            = ITE(C,T,A)
            BDD t = a;
            a = c;
            c = t;
        }
    }
    
    if (BDD_ISCONSTANT(c) && BDD_STRIPMARK(b) < BDD_STRIPMARK(a)) {
        if (c == sylvan_false) {
            // ITE(A,B,F) = ITE(B,A,F)
            //            = (A and B) or (~A and F)
            //            = (A and B) or F
            //            = (B and A) or (~B and F)
            BDD t = a;
            a = b;
            b = t;
        } else {
            // ITE(A,B,T) = ITE(~B,~A,T)
            //            = (A and B) or (~A and T)
            //            = (A and B) or ~A
            //            = (~B and ~A) or B
            //            = (~B and ~A) or (B and T)
            //            = ITE(~B,~A,T)
            BDD t = a;
            a = BDD_TOGGLEMARK(b);
            b = BDD_TOGGLEMARK(t);
        }
    }
    
    if (BDD_STRIPMARK(b) == BDD_STRIPMARK(c)) {
        // b and c not constants...
        // 1. if A then B else not-B = if B then A else not-A
        // 2. if A then not-B else B = if not-B then A else not-A
        if (BDD_STRIPMARK(a) > BDD_STRIPMARK(b)) {
            // a > b, exchange:
            b = a;
            a = BDD_TOGGLEMARK(c);
            c = BDD_TOGGLEMARK(b); // (old a)
        }
    }
    
    // TERMINAL CASE
    // ITE(~A,B,C) = ITE(A,C,B)
    if (BDD_HASMARK(a)) {
        a = BDD_STRIPMARK(a);
        BDD t = c;
        c = b;
        b = t;
    }
    
    /**
     * Apply De Morgan: ITE(A,B,C) = ~ITE(A,~B,~C)
     *
     * Proof:
     *   ITE(A,B,C) = (A and B) or (~A and C)
     *              = (A or C) and (~A or B)
     *              = ~(~(A or C) or ~(~A or B))
     *              = ~((~A and ~C) or (A and ~B))
     *              = ~((A and ~B) or (~A and ~C))
     *              = ~ITE(A,~B,~C)
     */
    if (BDD_HASMARK(b)) {
        b = BDD_TOGGLEMARK(b);
        c = BDD_TOGGLEMARK(c);
        *_a=a;
        *_b=b;
        *_c=c;
        return sylvan_invalid | complementmark;
    }
    
    *_a=a;
    *_b=b;
    *_c=c;
    return sylvan_invalid;
}

/**
 * At entry, all BDDs should be ref'd by caller.
 * At exit, they still are ref'd by caller, and the result it ref'd, and any items in the OC are ref'd.
 */
BDD sylvan_ite_do(BDD a, BDD b, BDD c, BDDVAR caller_var, int cachenow)
{
    // Standard triples
    BDD r = sylvan_triples(&a, &b, &c);
    if (BDD_STRIPMARK(r) != sylvan_invalid) {
        return sylvan_ref(r);
    }
    
    SV_CNT(C_ite);

    // The value of a,b,c may be changed, but the reference counters are not changed at this point.
    
#if CACHE 
    struct bddcache template_cache_node;
    if (cachenow) {
        // Check cache
        memset(&template_cache_node, 0, sizeof(struct bddcache));
        template_cache_node.operation = 0; // ITE operation
        //template_cache_node.parameters = 3;
        template_cache_node.params[0] = a;
        template_cache_node.params[1] = b;
        template_cache_node.params[2] = c;
        template_cache_node.result = sylvan_invalid;
    
        uint32_t cache_idx;
        if (llcache_get_and_hold(_bdd.cache, &template_cache_node, &cache_idx)) {
            BDD res = sylvan_ref(template_cache_node.result);
            llcache_release(_bdd.cache, cache_idx);
            SV_CNT(C_cache_reuse);
            return BDD_TRANSFERMARK(r, res);
        }
    }
#endif
    
    // No result, so we need to calculate...
    bddnode_t na = BDD_ISCONSTANT(a) ? 0 : GETNODE(a);
    bddnode_t nb = BDD_ISCONSTANT(b) ? 0 : GETNODE(b);
    bddnode_t nc = BDD_ISCONSTANT(c) ? 0 : GETNODE(c);
        
    // Get lowest level
    BDDVAR level = 0xffff;
    if (na && level > na->level) level = na->level;
    if (nb && level > nb->level) level = nb->level;
    if (nc && level > nc->level) level = nc->level;

    // Calculate "cachenow" for child
    int child_cachenow = granularity < 2 ? 1 : caller_var / granularity != level / granularity;

    // Get cofactors
    BDD aLow = a, aHigh = a;
    BDD bLow = b, bHigh = b;
    BDD cLow = c, cHigh = c;
    if (na && level == na->level) {
        aLow = BDD_TRANSFERMARK(a, na->low);
        aHigh = BDD_TRANSFERMARK(a, na->high);
    }
    if (nb && level == nb->level) {
        bLow = BDD_TRANSFERMARK(b, nb->low);
        bHigh = BDD_TRANSFERMARK(b, nb->high);
    }
    if (nc && level == nc->level) {
        cLow = BDD_TRANSFERMARK(c, nc->low);
        cHigh = BDD_TRANSFERMARK(c, nc->high);
    }
    
    // Recursive computation
    BDD low = sylvan_ite_do(aLow, bLow, cLow, level, child_cachenow);
    BDD high = sylvan_ite_do(aHigh, bHigh, cHigh, level, child_cachenow);
    BDD result = sylvan_makenode(level, low, high);
    
    /*
     * We gained ref on low, high
     * We exchanged ref on low, high for ref on result
     * Ref it again for the result.
     */

#if CACHE
    if (cachenow) {
        template_cache_node.result = result;
        uint32_t cache_idx;
        int cache_res = llcache_put_and_hold(_bdd.cache, &template_cache_node, &cache_idx);
        if (cache_res == 0) {
            llcache_release(_bdd.cache, cache_idx);
            // It existed!
            assert(result == template_cache_node.result);
            SV_CNT(C_cache_exists);
            // No need to ref
        } else if (cache_res == 1) {
            // Created new!
            sylvan_ref(a);
            sylvan_ref(b);
            sylvan_ref(c);
            sylvan_ref(result);
            llcache_release(_bdd.cache, cache_idx);
            SV_CNT(C_cache_new);
        } else if (cache_res == 2) {
            // Replaced existing!
            sylvan_ref(a);
            sylvan_ref(b);
            sylvan_ref(c);
            sylvan_ref(result);
            llcache_release(_bdd.cache, cache_idx);
            sylvan_cache_delete(NULL, &template_cache_node);
            SV_CNT(C_cache_new);
            SV_CNT(C_cache_overwritten);
        }
    }
#endif

    return BDD_TRANSFERMARK(r, result);
}

BDD sylvan_ite(BDD a, BDD b, BDD c)
{
    return sylvan_ite_do(a, b, c, 0, 1);
}

/**
 * Calculates \exists variables . a
 * Requires caller has ref on a, variables
 * Ensures caller as ref on a, variables and on result
 */
BDD sylvan_exists_do(BDD a, BDD variables, BDDVAR caller_var, int cachenow)
{
    // Trivial cases
    if (BDD_ISCONSTANT(a)) return a;
    
    SV_CNT(C_exists);

#if CACHE
    struct bddcache template_cache_node;
    // Save variables
    const BDD _variables = variables;
    if (cachenow) {
        // Check cache
        memset(&template_cache_node, 0, sizeof(struct bddcache));
        template_cache_node.operation = 4; // EXISTS operation
        //template_cache_node.parameters = 2;
        template_cache_node.params[0] = a;
        template_cache_node.params[1] = variables;
        template_cache_node.result = sylvan_invalid;

        uint32_t cache_idx;
        if (llcache_get_and_hold(_bdd.cache, &template_cache_node, &cache_idx)) {
            BDD result = sylvan_ref(template_cache_node.result);
            llcache_release(_bdd.cache, cache_idx);
            SV_CNT(C_cache_reuse);
            return result;
        }
    }
#endif
 
    // a != constant    
    bddnode_t na = GETNODE(a);
        
    // Get lowest level
    BDDVAR level = na->level;
    
    // Get cofactors
    BDD aLow = BDD_TRANSFERMARK(a, na->low);
    BDD aHigh = BDD_TRANSFERMARK(a, na->high);
    
    // Calculate "cachenow" for child
    int child_cachenow = granularity < 2 ? 1 : caller_var / granularity != level / granularity;

    // Skip variables not in a
    while (variables != sylvan_false && sylvan_var(variables) < level) {
        // Without increasing ref counter..
        variables = BDD_TRANSFERMARK(variables, GETNODE(variables)->low);
    }

    BDD result;

    if (variables == sylvan_false) {
        result = sylvan_ref(a);
    }
    // variables != sylvan_true (always)
    // variables != sylvan_false
    
    else if (sylvan_var(variables) == level) {
        // quantify
        BDD low = sylvan_exists_do(aLow, LOW(variables), level, child_cachenow);
        if (low == sylvan_true) {
            result = sylvan_true;
        } else {
            BDD high = sylvan_exists_do(aHigh, LOW(variables), level, child_cachenow);
            if (high == sylvan_true) {
                sylvan_deref(low);
                result = sylvan_true;
            }
            else if (low == sylvan_false && high == sylvan_false) {
                result = sylvan_false;
            }
            else {
                result = sylvan_ite(low, sylvan_true, high); // or 
                sylvan_deref(low);
                sylvan_deref(high);
            }
        }
    } else {
        // no quantify
        BDD low, high;
        high = sylvan_exists_do(aHigh, variables, level, child_cachenow);
        low = sylvan_exists_do(aLow, variables, level, child_cachenow);
        result = sylvan_makenode(level, low, high);
    }

#if CACHE
    if (cachenow) {
        template_cache_node.result = result;
        uint32_t cache_idx;
        int cache_res = llcache_put_and_hold(_bdd.cache, &template_cache_node, &cache_idx);
        if (cache_res == 0) {
            llcache_release(_bdd.cache, cache_idx);
            // It existed!
            assert(result == template_cache_node.result);
            SV_CNT(C_cache_exists);
            // No need to ref
        } else if (cache_res == 1) {
            // Created new!
            sylvan_ref(a);
            sylvan_ref(_variables);
            sylvan_ref(result);
            llcache_release(_bdd.cache, cache_idx);
            SV_CNT(C_cache_new);
        } else if (cache_res == 2) {
            // Replaced existing!
            sylvan_ref(a);
            sylvan_ref(_variables);
            sylvan_ref(result);
            llcache_release(_bdd.cache, cache_idx);
            sylvan_cache_delete(NULL, &template_cache_node);
        }
    }
#endif

    return result;
}

BDD sylvan_exists(BDD a, BDD variables)
{
    return sylvan_exists_do(a, variables, 0, 1);
}

/**
 * Calculates \forall variables . a
 * Requires ref on a, variables
 * Ensures ref on a, variables, result
 */
BDD sylvan_forall_do(BDD a, BDD variables, BDDVAR caller_var, int cachenow)
{
    // Trivial cases
    if (BDD_ISCONSTANT(a)) return a;

    SV_CNT(C_forall);

#if CACHE
    struct bddcache template_cache_node;
    // Save variables
    BDD _variables = variables;
    if (cachenow) {
        // Check cache
        memset(&template_cache_node, 0, sizeof(struct bddcache));
        template_cache_node.operation = 5; // FORALL operation
        //template_cache_node.parameters = 2;
        template_cache_node.params[0] = a;
        template_cache_node.params[1] = variables;
        template_cache_node.result = sylvan_invalid;

        uint32_t cache_idx;
        if (llcache_get_and_hold(_bdd.cache, &template_cache_node, &cache_idx)) {
            BDD result = sylvan_ref(template_cache_node.result);
            llcache_release(_bdd.cache, cache_idx);
            SV_CNT(C_cache_reuse);
            return result;
        }
    }
#endif
 
    // a != constant
    bddnode_t na = GETNODE(a);
        
    // Get lowest level
    BDDVAR level = na->level;
    
    // Calculate "cachenow" for child
    int child_cachenow = granularity < 2 ? 1 : caller_var / granularity != level / granularity;

    // Get cofactors
    BDD aLow = BDD_TRANSFERMARK(a, na->low);
    BDD aHigh = BDD_TRANSFERMARK(a, na->high);
    BDD result;
    
    // Skip variables not in a
    while (variables != sylvan_false && sylvan_var(variables) < level) {
        // Custom code to not modify the ref counters
        variables = BDD_TRANSFERMARK(variables, GETNODE(variables)->low);
    }

    if (variables == sylvan_false) {
        result = sylvan_ref(a);
    }
    // variables != sylvan_true (always)
    // variables != sylvan_false
    
    else if (sylvan_var(variables) == level) {
        // quantify
        BDD low = sylvan_forall_do(aLow, LOW(variables), level, child_cachenow);
        if (low == sylvan_false) {
            result = sylvan_false;
        } else {
            BDD high = sylvan_forall_do(aHigh, LOW(variables), level, child_cachenow);
            if (high == sylvan_false) {
                sylvan_deref(low);
                result = sylvan_false;
            }
            else if (low == sylvan_true && high == sylvan_true) {
                result = sylvan_true;
            }
            else {
                result = sylvan_ite(low, high, sylvan_false); // and
                sylvan_deref(low);
                sylvan_deref(high);
            }
        }
    } else {
        // no quantify
        BDD low, high;
        high = sylvan_forall_do(aHigh, variables, level, child_cachenow);
        low = sylvan_forall_do(aLow, variables, level, child_cachenow);
        result = sylvan_makenode(level, low, high);
    }

#if CACHE
    if (cachenow) {
        template_cache_node.result = result;
        uint32_t cache_idx;
        int cache_res = llcache_put_and_hold(_bdd.cache, &template_cache_node, &cache_idx);
        if (cache_res == 0) {
            llcache_release(_bdd.cache, cache_idx);
            // It existed!
            assert(result == template_cache_node.result);
            SV_CNT(C_cache_exists);
            // No need to ref
        } else if (cache_res == 1) {
            // Created new!
            sylvan_ref(a);
            sylvan_ref(_variables);
            sylvan_ref(result);
            llcache_release(_bdd.cache, cache_idx);
            SV_CNT(C_cache_new);
        } else if (cache_res == 2) {
            // Replaced existing!
            sylvan_ref(a);
            sylvan_ref(_variables);
            sylvan_ref(result);
            llcache_release(_bdd.cache, cache_idx);
            sylvan_cache_delete(NULL, &template_cache_node);
        }
    }
#endif

    return result;
}

BDD sylvan_forall(BDD a, BDD variables)
{
    return sylvan_forall_do(a, variables, 0, 1);
}

/**
 * Very specialized RelProdS. Calculates ( \exists X (A /\ B) ) [X'/X]
 * Assumptions on variables: 
 * - every variable 0, 2, 4 etc is in X except if in excluded_variables
 * - every variable 1, 3, 5 etc is in X' (except if in excluded_variables)
 * - (excluded_variables should really only contain variables from X...)
 * - the substitution X'/X substitutes 1 by 0, 3 by 2, etc.
 */
BDD sylvan_relprods_partial_do(BDD a, BDD b, BDD excluded_variables, BDDVAR caller_var, int cachenow)
{
    // Trivial case
    if (a == sylvan_true && b == sylvan_true) return sylvan_true;
    if (a == sylvan_false || b == sylvan_false) return sylvan_false;

    SV_CNT(C_relprods);

#if CACHE
    struct bddcache template_cache_node;
    // Save excluded variables
    const BDD _excluded_variables = excluded_variables;
    if (cachenow) {
        // Check cache
        memset(&template_cache_node, 0, sizeof(struct bddcache));
        template_cache_node.operation = 1; // RelProdS operation
        //template_cache_node.parameters = 3;
        template_cache_node.params[0] = a;
        template_cache_node.params[1] = b;
        template_cache_node.params[2] = excluded_variables;
        template_cache_node.result = sylvan_invalid;
        
        uint32_t cache_idx;
        if (llcache_get_and_hold(_bdd.cache, &template_cache_node, &cache_idx)) {
            BDD result = sylvan_ref(template_cache_node.result);
            llcache_release(_bdd.cache, cache_idx);
            SV_CNT(C_cache_reuse);
            return result;
        }
    }
#endif

    // No result, so we need to calculate...
    bddnode_t na = BDD_ISCONSTANT(a) ? 0 : GETNODE(a);
    bddnode_t nb = BDD_ISCONSTANT(b) ? 0 : GETNODE(b);
        
    // Get lowest level
    BDDVAR level = 0xffff;
    if (na && level > na->level) level = na->level;
    if (nb && level > nb->level) level = nb->level;
    
    // Calculate "cachenow" for child
    int child_cachenow = granularity < 2 ? 1 : caller_var / granularity != level / granularity;

    // Get cofactors
    BDD aLow = a, aHigh = a;
    BDD bLow = b, bHigh = b;
    if (na && level == na->level) {
        aLow = BDD_TRANSFERMARK(a, na->low);
        aHigh = BDD_TRANSFERMARK(a, na->high);
    }
    if (nb && level == nb->level) {
        bLow = BDD_TRANSFERMARK(b, nb->low);
        bHigh = BDD_TRANSFERMARK(b, nb->high);
    }
    
    // Check if excluded variable
    int is_excluded = 0;
    while (excluded_variables != sylvan_false) {
        BDDVAR var = sylvan_var(excluded_variables);
        if (var == level) {
            is_excluded = 1;
            break;
        }
        else if (var > level) {
            break;
        }
        // var < level
        // do not mess with ref counts...
        else excluded_variables = BDD_TRANSFERMARK(excluded_variables, GETNODE(excluded_variables)->low);
    }
    
    // Recursive computation
    BDD low, high, result;
    
    if (0==(level&1) && is_excluded == 0) {
        low = sylvan_relprods_partial_do(aLow, bLow, excluded_variables, level, child_cachenow);
        // variable in X: quantify
        if (low == sylvan_true) {
            result = sylvan_true;
        }
        else {
            high = sylvan_relprods_partial_do(aHigh, bHigh, excluded_variables, level, child_cachenow);
            if (high == sylvan_true) {
                sylvan_deref(low);
                result = sylvan_true;
            }
            else if (low == sylvan_false && high == sylvan_false) {
                result = sylvan_false;
            }
            else {
                result = sylvan_ite(low, sylvan_true, high);
                sylvan_deref(low);
                sylvan_deref(high);
            }
        }
    } 
    else {
        high = sylvan_relprods_partial_do(aHigh, bHigh, excluded_variables, level, child_cachenow);
        low = sylvan_relprods_partial_do(aLow, bLow, excluded_variables, level, child_cachenow);

        // variable in X': substitute
        if (is_excluded == 0) result = sylvan_makenode(level-1, low, high);

        // variable not in X or X': normal behavior
        else result = sylvan_makenode(level, low, high);
    }
    
#if CACHE
    if (cachenow) {
        template_cache_node.result = result;
        uint32_t cache_idx;
        int cache_res = llcache_put_and_hold(_bdd.cache, &template_cache_node, &cache_idx);
        if (cache_res == 0) {
            llcache_release(_bdd.cache, cache_idx);
            // It existed!
            assert(result == template_cache_node.result);
            SV_CNT(C_cache_exists);
            // No need to ref
        } else if (cache_res == 1) {
            // Created new!
            sylvan_ref(a);
            sylvan_ref(b);
            sylvan_ref(_excluded_variables);
            sylvan_ref(result);
            llcache_release(_bdd.cache, cache_idx);
            SV_CNT(C_cache_new);
        } else if (cache_res == 2) {
            // Replaced existing!
            sylvan_ref(a);
            sylvan_ref(b);
            sylvan_ref(_excluded_variables);
            sylvan_ref(result);
            llcache_release(_bdd.cache, cache_idx);
            sylvan_cache_delete(NULL, &template_cache_node);
        } 
    }
#endif

    return result;
}

BDD sylvan_relprods_partial(BDD a, BDD b, BDD excluded_variables) 
{
    return sylvan_relprods_partial_do(a, b, excluded_variables, 0, 1);
}

BDD sylvan_relprods(BDD a, BDD b) 
{
    return sylvan_relprods_partial(a, b, sylvan_false);
}

/**
 * Very specialized RelProdS. Calculates \exists X' (A[X/X'] /\ B)
 * Assumptions:
 * - A is only defined on variables in X
 * - every variable 0, 2, 4 etc is in X (exclude)
 * - every variable 1, 3, 5 etc is in X' (exclude)
 * - variables in exclude_variables are not in X or X'
 * - the substitution X/X' substitutes 0 by 1, 2 by 3, etc.
 */
BDD sylvan_relprods_reversed_partial_do(BDD a, BDD b, BDD excluded_variables, BDDVAR caller_var, int cachenow) 
{
    // Trivial case
    if (a == sylvan_true && b == sylvan_true) return sylvan_true;
    if (a == sylvan_false || b == sylvan_false) return sylvan_false;
    
    SV_CNT(C_relprods_reversed);

#if CACHE
    struct bddcache template_cache_node;
    // Save excluded variables
    const BDD _excluded_variables = excluded_variables;
    if (cachenow) {
        // Check cache
        memset(&template_cache_node, 0, sizeof(struct bddcache));
        template_cache_node.operation = 2; // RelProdS operation
        //template_cache_node.parameters = 3;
        template_cache_node.params[0] = a;
        template_cache_node.params[1] = b;
        template_cache_node.params[2] = excluded_variables;
        template_cache_node.result = sylvan_invalid;
    
        uint32_t cache_idx;
        if (llcache_get_and_hold(_bdd.cache, &template_cache_node, &cache_idx)) {
            BDD result = sylvan_ref(template_cache_node.result);
            llcache_release(_bdd.cache, cache_idx);
            SV_CNT(C_cache_reuse);
            return result;
        }
    }
#endif

    // No result, so we need to calculate...
    bddnode_t na = BDD_ISCONSTANT(a) ? 0 : GETNODE(a);
    bddnode_t nb = BDD_ISCONSTANT(b) ? 0 : GETNODE(b);
    
    // x = TopVariables(S(x_a), x_b)
    BDDVAR x_a, x_b, S_x_a, x=0xFFFF;
    if (na) {
        x_a = na->level;
        S_x_a = x_a + 1;
        x = S_x_a;
    }
    if (nb) {
        x_b = nb->level;
        if (x > x_b) x = x_b;
    }
   
    // Check if excluded variable
    int is_excluded = 0;
    while (excluded_variables != sylvan_false) {
        BDDVAR var = sylvan_var(excluded_variables);
        if (na && var == x_a) {
            S_x_a--; // so actually, S(x_a) = x_a
            // Recalculate x
            x = S_x_a; 
            if (nb && x > x_b) x = x_b;
        }
        if (var == x) is_excluded = 1;
        else if (var > x) {
            break;
        }
        // var < level
        // do not modify ref counters
        excluded_variables = BDD_TRANSFERMARK(excluded_variables, GETNODE(excluded_variables)->low);
    }

    // OK , so now S_x_a = S(x_a) properly, and 
    // if S_x_a == x then x_a == S'(x)

    // Calculate "cachenow" for child
    int child_cachenow = granularity < 2 ? 1 : caller_var / granularity != x / granularity;

    // Get cofactors
    BDD aLow = a, aHigh = a;
    BDD bLow = b, bHigh = b;
    if (na && x == S_x_a && x_a == na->level) {
        aLow = BDD_TRANSFERMARK(a, na->low);
        aHigh = BDD_TRANSFERMARK(a, na->high);
    }
    
    if (nb && x == nb->level) {
        bLow = BDD_TRANSFERMARK(b, nb->low);
        bHigh = BDD_TRANSFERMARK(b, nb->high);
    }

    BDD low, high, result;

    // if x \in X'
    if ((x&1) == 1 && is_excluded == 0) {
        low = sylvan_relprods_reversed_partial_do(aLow, bLow, excluded_variables, x, child_cachenow);
        // variable in X': quantify
        if (low == sylvan_true) {
            result = sylvan_true;
        } else {
            high = sylvan_relprods_reversed_partial_do(aHigh, bHigh, excluded_variables, x, child_cachenow);
            if (high == sylvan_true) {
                sylvan_deref(low);
                result = sylvan_true;
            } else if (low == sylvan_false && high == sylvan_false) {
                result = sylvan_false;
            } else {
                result = sylvan_ite(low, sylvan_true, high);
                sylvan_deref(low);
                sylvan_deref(high);
            }
        }
    } 
    // if x \in X OR if excluded (works in either case)
    else {
        low = sylvan_relprods_reversed_partial_do(aLow, bLow, excluded_variables, x, child_cachenow);
        high = sylvan_relprods_reversed_partial_do(aHigh, bHigh, excluded_variables, x, child_cachenow);
        result = sylvan_makenode(x, low, high);
    }

#if CACHE
    if (cachenow) {
        template_cache_node.result = result;
        uint32_t cache_idx;
        int cache_res = llcache_put_and_hold(_bdd.cache, &template_cache_node, &cache_idx);
        if (cache_res == 0) {
            llcache_release(_bdd.cache, cache_idx);
            // It existed!
            assert(result == template_cache_node.result);
            SV_CNT(C_cache_exists);
            // No need to ref
        } else if (cache_res == 1) {
            // Created new!
            sylvan_ref(a);
            sylvan_ref(b);
            sylvan_ref(_excluded_variables);
            sylvan_ref(result);
            llcache_release(_bdd.cache, cache_idx);
            SV_CNT(C_cache_new);
        } else if (cache_res == 2) {
            // Replaced existing!
            sylvan_ref(a);
            sylvan_ref(b);
            sylvan_ref(_excluded_variables);
            sylvan_ref(result);
            llcache_release(_bdd.cache, cache_idx);
            sylvan_cache_delete(NULL, &template_cache_node);
        }
    }
#endif

    return result;
}

BDD sylvan_relprods_reversed_partial(BDD a, BDD b, BDD excluded_variables) 
{
    return sylvan_relprods_reversed_partial_do(a, b, excluded_variables, 0, 1);
}

BDD sylvan_relprods_reversed(BDD a, BDD b) 
{
    return sylvan_relprods_reversed_partial(a, b, sylvan_false);
}

uint32_t sylvan_nodecount_do_1(BDD a) 
{
    if (BDD_ISCONSTANT(a)) return 0;
    bddnode_t na = GETNODE(a);
    if (na->flags & 1) return 0;
    na->flags |= 1; // mark
    uint32_t result = 1;
    result += sylvan_nodecount_do_1(na->low);
    result += sylvan_nodecount_do_1(na->high);
    return result;
}

void sylvan_nodecount_do_2(BDD a) 
{
    if (BDD_ISCONSTANT(a)) return;
    bddnode_t na = GETNODE(a);
    if (!(na->flags & 1)) return;
    na->flags &= ~1; // unmark
    sylvan_nodecount_do_2(na->low);
    sylvan_nodecount_do_2(na->high);
}

uint32_t sylvan_nodecount(BDD a) 
{
    uint32_t result = sylvan_nodecount_do_1(a);
    sylvan_nodecount_do_2(a);
    return result;
}

long double sylvan_pathcount(BDD bdd)
{
    if (bdd == sylvan_false) return 0.0;
    if (bdd == sylvan_true) return 1.0;
    long double high = sylvan_pathcount(HIGH(bdd));
    long double low = sylvan_pathcount(LOW(bdd));
    return high+low;
}


long double sylvan_satcount_do(BDD bdd, BDD variables)
{
    if (bdd == sylvan_false) return 0.0;
    if (bdd == sylvan_true) {
        long double result = 1.0L;
        while (variables != sylvan_false) {
            variables = LOW(variables);
            result *= 2.0L;
        }
        return result;
    }
    if (variables == sylvan_false) {
        fprintf(stderr, "ERROR in sylvan_satcount: 'bdd' contains variable %d not in 'variables'!\n", sylvan_var(bdd));
        assert(0); 
    }
    if (variables == sylvan_true) {
        fprintf(stderr, "ERROR in sylvan_satcount: invalid 'variables'!\n");
        assert(0);
    }
    // bdd != constant
    // variables != constant
    if (sylvan_var(bdd) > sylvan_var(variables)) {
        return 2.0L * sylvan_satcount_do(bdd, LOW(variables));
    } else {
        long double high = sylvan_satcount_do(HIGH(bdd), LOW(variables));
        long double low = sylvan_satcount_do(LOW(bdd), LOW(variables));
        return high+low;
    }
}

long double sylvan_satcount(BDD bdd, BDD variables)
{
    return sylvan_satcount_do(bdd, variables);
}

static void sylvan_fprint_1(FILE *out, BDD bdd)
{
    if (bdd==sylvan_invalid) return;
    if (BDD_ISCONSTANT(bdd)) return;
    bddnode_t n = GETNODE(bdd);
    if (n->flags & 0x2) return;
    n->flags |= 0x2;
    
    fprintf(out, "%08X: (%u, low=%s%08X, high=%s%08X) %s\n", 
        bdd, n->level, 
        BDD_HASMARK(n->low)?"~":"", BDD_STRIPMARK(n->low),
        BDD_HASMARK(n->high)?"~":"", BDD_STRIPMARK(n->high),
        n->flags & 0x1?"*":"");
        
    sylvan_fprint_1(out, BDD_STRIPMARK(n->low));
    sylvan_fprint_1(out, BDD_STRIPMARK(n->high));
}

static void sylvan_print_2(BDD bdd)
{
    if (bdd==sylvan_invalid) return;
    if (BDD_ISCONSTANT(bdd)) return;
    bddnode_t n = GETNODE(bdd);
    if (n->flags & 0x2) {
        n->flags &= ~0x2;
        sylvan_print_2(n->low);
        sylvan_print_2(n->high);
    }
}

void sylvan_print(BDD bdd)
{
    sylvan_fprint(stdout, bdd);
}

void sylvan_fprint(FILE *out, BDD bdd)
{
    if (bdd == sylvan_invalid) return;
    fprintf(out, "Dump of %08X:\n", bdd);
    sylvan_fprint_1(out, bdd);
    sylvan_print_2(bdd);
}

llgcset_t __sylvan_get_internal_data() 
{
    return _bdd.data;
}

#if CACHE
llcache_t __sylvan_get_internal_cache() 
{
    return _bdd.cache;
}
#endif

/* EXPOSE LL CACHE */
struct llcache
{
    size_t             padded_data_length;
    size_t             key_length;
    size_t             data_length;
    size_t             cache_size;  
    uint32_t           mask;         // size-1
    uint32_t           *table;        // table with hashes
    uint8_t            *data;         // table with data
    llcache_delete_f   cb_delete;    // delete function (callback pre-delete)
    void               *cb_data;
};


long long sylvan_count_refs()
{
    long long result = 0;
    
    int i;
    for (i=0;i<_bdd.data->table_size;i++) {
        uint32_t c = _bdd.data->table[i];
        if (c == 0) continue; // not in use (never used)
        if (c == 0x7fffffff) continue; // not in use (tombstone)
        
        c &= 0x0000ffff;
        assert (c!=0x0000ffff); // "about to be deleted" should not be visible here
        
        assert (c!=0x0000fffe); // If this fails, implement behavior for saturated nodes
        
        result += c; // for now, ignore saturated...
        
        bddnode_t n = GETNODE(i);

        //fprintf(stderr, "Node %08X var=%d low=%08X high=%08X rc=%d\n", i, n->level, n->low, n->high, c);
        
        if (!BDD_ISCONSTANT(n->low)) result--; // dont include internals
        if (!BDD_ISCONSTANT(n->high)) result--; // dont include internals
    }
    
#if CACHE    
    for (i=0;i<_bdd.cache->cache_size;i++) {
        uint32_t c = _bdd.cache->table[i];
        if (c == 0) continue;
        if (c == 0x7fffffff) continue;
        
        bddcache_t n = (bddcache_t)&_bdd.cache->data[i * _bdd.cache->padded_data_length];
        
        //fprintf(stderr, "Cache %08X ", i);
        
        int j;
        for (j=0; j<MAXPARAM/*n->parameters*/; j++) {
            //fprintf(stderr, "%d=%08X ", j, n->params[j]);
            if (BDD_ISCONSTANT(n->params[j])) continue;
            result--;
        }
                
        //fprintf(stderr, "res=%08X\n", n->result);
        
        if (n->result != sylvan_invalid && (!BDD_ISCONSTANT(n->result))) result--;
    }
#endif    

    return result;
}

// Some 
static BDD *ser_arr; // serialize array
static long ser_offset; // offset...
static uint32_t ser_count = 0;

void sylvan_save_reset() 
{
    ser_count = 0;
    int i;
    // This is a VERY expensive loop.
    for (i=0;i<_bdd.data->table_size;i++) {
        bddnode_t n = GETNODE(i);
        *(uint32_t*)&n->pad = 0;        
    }
}

static void sylvan_save_dummy(FILE *f)
{
    ser_offset = ftell(f);
    fwrite(&ser_count, 4, 1, f);
}

static void sylvan_save_update(FILE *f)
{
    long off = ftell(f);
    fseek(f, ser_offset, SEEK_SET);
    fwrite(&ser_count, 4, 1, f);
    fseek(f, off, SEEK_SET);
}

uint32_t sylvan_save_bdd(FILE* f, BDD bdd) 
{
    if (BDD_ISCONSTANT(bdd)) return bdd;

    bddnode_t n = GETNODE(bdd);
    uint32_t *pnum = (uint32_t*)&n->pad;

    if (*pnum == 0) {
        uint32_t low = sylvan_save_bdd(f, n->low);
        uint32_t high = sylvan_save_bdd(f, n->high);
    
        if (ser_count == 0) sylvan_save_dummy(f);
        ser_count++;
        *pnum = ser_count;

        fwrite(&low, 4, 1, f);
        fwrite(&high, 4, 1, f);
        fwrite(&n->level, sizeof(BDDVAR), 1, f);
    }

    return BDD_TRANSFERMARK(bdd, *pnum);
}

void sylvan_save_done(FILE *f)
{
    sylvan_save_update(f);
}

void sylvan_load(FILE *f) 
{
    fread(&ser_count, 4, 1, f);

    ser_arr = (BDD*)malloc(sizeof(BDD) * ser_count);

    unsigned long i;
    for (i=1;i<=ser_count;i++) {
        uint32_t low, high;
        BDDVAR var;
        fread(&low, 4, 1, f);
        fread(&high, 4, 1, f);
        fread(&var, sizeof(BDDVAR), 1, f);

        assert (BDD_STRIPMARK(low) < i);
        assert (BDD_STRIPMARK(high) < i);

        BDD _low = BDD_ISCONSTANT(low) ? low : BDD_TRANSFERMARK(low, ser_arr[BDD_STRIPMARK(low)-1]);
        BDD _high = BDD_ISCONSTANT(high) ? high : BDD_TRANSFERMARK(high, ser_arr[BDD_STRIPMARK(high)-1]);
        ser_arr[i-1] = sylvan_makenode(var, _low, _high);
    }
}

BDD sylvan_load_translate(uint32_t bdd) 
{
    if (BDD_ISCONSTANT(bdd)) return bdd;
    return BDD_TRANSFERMARK(bdd, ser_arr[BDD_STRIPMARK(bdd)-1]);
}

void sylvan_load_done()
{
    free(ser_arr);
}
