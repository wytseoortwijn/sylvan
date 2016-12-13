#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <time.h>
#include <sys/types.h>
#include <sys/time.h>
#include <inttypes.h>

#include "llmsset.h"
#include "sylvan.h"
#include "test_assert.h"

__thread uint64_t seed = 1;

uint64_t
xorshift_rand(void)
{
    uint64_t x = seed;
    if (seed == 0) seed = rand();
    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    seed = x;
    return x * 2685821657736338717LL;
}

double
uniform_deviate(uint64_t seed)
{
    return seed * (1.0 / (0xffffffffffffffffL + 1.0));
}

int
rng(int low, int high)
{
    return low + uniform_deviate(xorshift_rand()) * (high-low);
}

static inline BDD
make_random(int i, int j)
{
    if (i == j) return rng(0, 2) ? sylvan_true : sylvan_false;

    BDD yes = make_random(i+1, j);
    BDD no = make_random(i+1, j);
    BDD result = sylvan_invalid;

    switch(rng(0, 4)) {
    case 0:
        result = no;
        sylvan_deref(yes);
        break;
    case 1:
        result = yes;
        sylvan_deref(no);
        break;
    case 2:
        result = sylvan_ref(sylvan_makenode(i, yes, no));
        sylvan_deref(no);
        sylvan_deref(yes);
        break;
    case 3:
    default:
        result = sylvan_ref(sylvan_makenode(i, no, yes));
        sylvan_deref(no);
        sylvan_deref(yes);
        break;
    }

    return result;
}

static MDD
make_random_ldd_set(int depth, int maxvalue, int elements)
{
    uint32_t values[depth];
    MDD result = mtbdd_false; // empty set
    for (int i=0; i<elements; i++) {
        lddmc_refs_push(result);
        for (int j=0; j<depth; j++) {
            values[j] = rng(0, maxvalue);
        }
        result = lddmc_union_cube(result, values, depth);
        lddmc_refs_pop(1);
    }
    return result;
}

int testEqual(BDD a, BDD b)
{
	if (a == b) return 1;

	if (a == sylvan_invalid) {
		fprintf(stderr, "a is invalid!\n");
		return 0;
	}

	if (b == sylvan_invalid) {
		fprintf(stderr, "b is invalid!\n");
		return 0;
	}

    fprintf(stderr, "a and b are not equal!\n");

    sylvan_fprint(stderr, a);fprintf(stderr, "\n");
    sylvan_fprint(stderr, b);fprintf(stderr, "\n");

	return 0;
}

int
test_bdd()
{
    test_assert(sylvan_makenode(sylvan_ithvar(1), sylvan_true, sylvan_true) == sylvan_not(sylvan_makenode(sylvan_ithvar(1), sylvan_false, sylvan_false)));
    test_assert(sylvan_makenode(sylvan_ithvar(1), sylvan_false, sylvan_true) == sylvan_not(sylvan_makenode(sylvan_ithvar(1), sylvan_true, sylvan_false)));
    test_assert(sylvan_makenode(sylvan_ithvar(1), sylvan_true, sylvan_false) == sylvan_not(sylvan_makenode(sylvan_ithvar(1), sylvan_false, sylvan_true)));
    test_assert(sylvan_makenode(sylvan_ithvar(1), sylvan_false, sylvan_false) == sylvan_not(sylvan_makenode(sylvan_ithvar(1), sylvan_true, sylvan_true)));

    return 0;
}

int
test_cube()
{
    LACE_ME;
    const BDDSET vars = sylvan_set_fromarray(((BDDVAR[]){1,2,3,4,6,8}), 6);

    uint8_t cube[6], check[6];
    int i, j;
    for (i=0;i<6;i++) cube[i] = rng(0,3);
    BDD bdd = sylvan_cube(vars, cube);

    sylvan_sat_one(bdd, vars, check);
    for (i=0; i<6;i++) test_assert(cube[i] == check[i] || (cube[i] == 2 && check[i] == 0));

    BDD picked_single = sylvan_pick_single_cube(bdd, vars);
    test_assert(testEqual(sylvan_and(picked_single, bdd), picked_single));
    assert(sylvan_satcount(picked_single, vars)==1);

    BDD picked = sylvan_pick_cube(bdd);
    test_assert(testEqual(sylvan_and(picked, bdd), picked));

    BDD t1 = sylvan_cube(vars, ((uint8_t[]){1,1,2,2,0,0}));
    BDD t2 = sylvan_cube(vars, ((uint8_t[]){1,1,1,0,0,2}));
    test_assert(testEqual(sylvan_union_cube(t1, vars, ((uint8_t[]){1,1,1,0,0,2})), sylvan_or(t1, t2)));
    t2 = sylvan_cube(vars, ((uint8_t[]){2,2,2,1,1,0}));
    test_assert(testEqual(sylvan_union_cube(t1, vars, ((uint8_t[]){2,2,2,1,1,0})), sylvan_or(t1, t2)));
    t2 = sylvan_cube(vars, ((uint8_t[]){1,1,1,0,0,0}));
    test_assert(testEqual(sylvan_union_cube(t1, vars, ((uint8_t[]){1,1,1,0,0,0})), sylvan_or(t1, t2)));

    bdd = make_random(1, 16);
    for (j=0;j<10;j++) {
        for (i=0;i<6;i++) cube[i] = rng(0,3);
        BDD c = sylvan_cube(vars, cube);
        test_assert(sylvan_union_cube(bdd, vars, cube) == sylvan_or(bdd, c));
    }

    for (i=0;i<10;i++) {
        picked = sylvan_pick_cube(bdd);
        test_assert(testEqual(sylvan_and(picked, bdd), picked));
    }

    // simple test for mtbdd_enum_all
    uint8_t arr[6];
    MTBDD leaf = mtbdd_enum_all_first(mtbdd_true, vars, arr, NULL);
    test_assert(leaf == mtbdd_true);
    test_assert(mtbdd_enum_all_first(mtbdd_true, vars, arr, NULL) == mtbdd_true);
    test_assert(arr[0] == 0 && arr[1] == 0 && arr[2] == 0 && arr[3] == 0 && arr[4] == 0 && arr[5] == 0);
    test_assert(mtbdd_enum_all_next(mtbdd_true, vars, arr, NULL) == mtbdd_true);
    test_assert(arr[0] == 0 && arr[1] == 0 && arr[2] == 0 && arr[3] == 0 && arr[4] == 0 && arr[5] == 1);
    test_assert(mtbdd_enum_all_next(mtbdd_true, vars, arr, NULL) == mtbdd_true);
    test_assert(arr[0] == 0 && arr[1] == 0 && arr[2] == 0 && arr[3] == 0 && arr[4] == 1 && arr[5] == 0);
    test_assert(mtbdd_enum_all_next(mtbdd_true, vars, arr, NULL) == mtbdd_true);
    test_assert(arr[0] == 0 && arr[1] == 0 && arr[2] == 0 && arr[3] == 0 && arr[4] == 1 && arr[5] == 1);
    test_assert(mtbdd_enum_all_next(mtbdd_true, vars, arr, NULL) == mtbdd_true);
    test_assert(arr[0] == 0 && arr[1] == 0 && arr[2] == 0 && arr[3] == 1 && arr[4] == 0 && arr[5] == 0);
    test_assert(mtbdd_enum_all_next(mtbdd_true, vars, arr, NULL) == mtbdd_true);
    test_assert(arr[0] == 0 && arr[1] == 0 && arr[2] == 0 && arr[3] == 1 && arr[4] == 0 && arr[5] == 1);
    test_assert(mtbdd_enum_all_next(mtbdd_true, vars, arr, NULL) == mtbdd_true);
    test_assert(arr[0] == 0 && arr[1] == 0 && arr[2] == 0 && arr[3] == 1 && arr[4] == 1 && arr[5] == 0);

    mtbdd_enum_all_first(mtbdd_true, vars, arr, NULL);
    size_t count = 1;
    while (mtbdd_enum_all_next(mtbdd_true, vars, arr, NULL) != mtbdd_false) {
        test_assert(count < 64);
        count++;
    }
    test_assert(count == 64);

    return 0;
}

static int
test_operators()
{
    // We need to test: xor, and, or, nand, nor, imp, biimp, invimp, diff, less
    LACE_ME;

    //int i;
    BDD a = sylvan_ithvar(1);
    BDD b = sylvan_ithvar(2);
    BDD one = make_random(1, 12);
    BDD two = make_random(6, 24);

    // Test or
    test_assert(testEqual(sylvan_or(a, b), sylvan_makenode(1, b, sylvan_true)));
    test_assert(testEqual(sylvan_or(a, b), sylvan_or(b, a)));
    test_assert(testEqual(sylvan_or(one, two), sylvan_or(two, one)));

    // Test and
    test_assert(testEqual(sylvan_and(a, b), sylvan_makenode(1, sylvan_false, b)));
    test_assert(testEqual(sylvan_and(a, b), sylvan_and(b, a)));
    test_assert(testEqual(sylvan_and(one, two), sylvan_and(two, one)));

    // Test xor
    test_assert(testEqual(sylvan_xor(a, b), sylvan_makenode(1, b, sylvan_not(b))));
    test_assert(testEqual(sylvan_xor(a, b), sylvan_xor(a, b)));
    test_assert(testEqual(sylvan_xor(a, b), sylvan_xor(b, a)));
    test_assert(testEqual(sylvan_xor(one, two), sylvan_xor(two, one)));
    test_assert(testEqual(sylvan_xor(a, b), sylvan_ite(a, sylvan_not(b), b)));

    // Test diff
    test_assert(testEqual(sylvan_diff(a, b), sylvan_diff(a, b)));
    test_assert(testEqual(sylvan_diff(a, b), sylvan_diff(a, sylvan_and(a, b))));
    test_assert(testEqual(sylvan_diff(a, b), sylvan_and(a, sylvan_not(b))));
    test_assert(testEqual(sylvan_diff(a, b), sylvan_ite(b, sylvan_false, a)));
    test_assert(testEqual(sylvan_diff(one, two), sylvan_diff(one, two)));
    test_assert(testEqual(sylvan_diff(one, two), sylvan_diff(one, sylvan_and(one, two))));
    test_assert(testEqual(sylvan_diff(one, two), sylvan_and(one, sylvan_not(two))));
    test_assert(testEqual(sylvan_diff(one, two), sylvan_ite(two, sylvan_false, one)));

    // Test biimp
    test_assert(testEqual(sylvan_biimp(a, b), sylvan_makenode(1, sylvan_not(b), b)));
    test_assert(testEqual(sylvan_biimp(a, b), sylvan_biimp(b, a)));
    test_assert(testEqual(sylvan_biimp(one, two), sylvan_biimp(two, one)));

    // Test nand / and
    test_assert(testEqual(sylvan_not(sylvan_and(a, b)), sylvan_nand(b, a)));
    test_assert(testEqual(sylvan_not(sylvan_and(one, two)), sylvan_nand(two, one)));

    // Test nor / or
    test_assert(testEqual(sylvan_not(sylvan_or(a, b)), sylvan_nor(b, a)));
    test_assert(testEqual(sylvan_not(sylvan_or(one, two)), sylvan_nor(two, one)));

    // Test xor / biimp
    test_assert(testEqual(sylvan_xor(a, b), sylvan_not(sylvan_biimp(b, a))));
    test_assert(testEqual(sylvan_xor(one, two), sylvan_not(sylvan_biimp(two, one))));

    // Test imp
    test_assert(testEqual(sylvan_imp(a, b), sylvan_ite(a, b, sylvan_true)));
    test_assert(testEqual(sylvan_imp(one, two), sylvan_ite(one, two, sylvan_true)));
    test_assert(testEqual(sylvan_imp(one, two), sylvan_not(sylvan_diff(one, two))));
    test_assert(testEqual(sylvan_invimp(one, two), sylvan_not(sylvan_less(one, two))));
    test_assert(testEqual(sylvan_imp(a, b), sylvan_invimp(b, a)));
    test_assert(testEqual(sylvan_imp(one, two), sylvan_invimp(two, one)));

    return 0;
}

int
test_relprod()
{
    LACE_ME;

    BDDVAR vars[] = {0,2,4};
    BDDVAR all_vars[] = {0,1,2,3,4,5};

    BDDSET vars_set = sylvan_set_fromarray(vars, 3);
    BDDSET all_vars_set = sylvan_set_fromarray(all_vars, 6);

    BDD s, t, next, prev;
    BDD zeroes, ones;

    // transition relation: 000 --> 111 and !000 --> 000
    t = sylvan_false;
    t = sylvan_union_cube(t, all_vars_set, ((uint8_t[]){0,1,0,1,0,1}));
    t = sylvan_union_cube(t, all_vars_set, ((uint8_t[]){1,0,2,0,2,0}));
    t = sylvan_union_cube(t, all_vars_set, ((uint8_t[]){2,0,1,0,2,0}));
    t = sylvan_union_cube(t, all_vars_set, ((uint8_t[]){2,0,2,0,1,0}));

    s = sylvan_cube(vars_set, (uint8_t[]){0,0,1});
    zeroes = sylvan_cube(vars_set, (uint8_t[]){0,0,0});
    ones = sylvan_cube(vars_set, (uint8_t[]){1,1,1});

    next = sylvan_relnext(s, t, all_vars_set);
    prev = sylvan_relprev(t, next, all_vars_set);
    test_assert(next == zeroes);
    test_assert(prev == sylvan_not(zeroes));

    next = sylvan_relnext(next, t, all_vars_set);
    prev = sylvan_relprev(t, next, all_vars_set);
    test_assert(next == ones);
    test_assert(prev == zeroes);

    t = sylvan_cube(all_vars_set, (uint8_t[]){0,0,0,0,0,1});
    test_assert(sylvan_relprev(t, s, all_vars_set) == zeroes);
    test_assert(sylvan_relprev(t, sylvan_not(s), all_vars_set) == sylvan_false);
    test_assert(sylvan_relnext(s, t, all_vars_set) == sylvan_false);
    test_assert(sylvan_relnext(zeroes, t, all_vars_set) == s);

    t = sylvan_cube(all_vars_set, (uint8_t[]){0,0,0,0,0,2});
    test_assert(sylvan_relprev(t, s, all_vars_set) == zeroes);
    test_assert(sylvan_relprev(t, zeroes, all_vars_set) == zeroes);
    test_assert(sylvan_relnext(sylvan_not(zeroes), t, all_vars_set) == sylvan_false);

    return 0;
}

int
test_compose()
{
    LACE_ME;

    BDD a = sylvan_ithvar(1);
    BDD b = sylvan_ithvar(2);

    BDD a_or_b = sylvan_or(a, b);

    BDD one = make_random(3, 16);
    BDD two = make_random(8, 24);

    BDDMAP map = sylvan_map_empty();

    map = sylvan_map_add(map, 1, one);
    map = sylvan_map_add(map, 2, two);

    test_assert(sylvan_map_key(map) == 1);
    test_assert(sylvan_map_value(map) == one);
    test_assert(sylvan_map_key(sylvan_map_next(map)) == 2);
    test_assert(sylvan_map_value(sylvan_map_next(map)) == two);

    test_assert(testEqual(one, sylvan_compose(a, map)));
    test_assert(testEqual(two, sylvan_compose(b, map)));

    test_assert(testEqual(sylvan_or(one, two), sylvan_compose(a_or_b, map)));

    map = sylvan_map_add(map, 2, one);
    test_assert(testEqual(sylvan_compose(a_or_b, map), one));

    map = sylvan_map_add(map, 1, two);
    test_assert(testEqual(sylvan_or(one, two), sylvan_compose(a_or_b, map)));

    test_assert(testEqual(sylvan_and(one, two), sylvan_compose(sylvan_and(a, b), map)));

    // test that composing [0:=true] on "0" yields true
    map = sylvan_map_add(sylvan_map_empty(), 1, sylvan_true);
    test_assert(testEqual(sylvan_compose(a, map), sylvan_true));

    // test that composing [0:=false] on "0" yields false
    map = sylvan_map_add(sylvan_map_empty(), 1, sylvan_false);
    test_assert(testEqual(sylvan_compose(a, map), sylvan_false));

    return 0;
}

int
test_ldd()
{
    // very basic testing of makenode
    for (int i=0; i<10; i++) {
        uint32_t value = rng(0, 100);
        MDD m = lddmc_makenode(value, lddmc_true, lddmc_false);
        test_assert(lddmc_getvalue(m) == value);
        test_assert(lddmc_getdown(m) == lddmc_true);
        test_assert(lddmc_getright(m) == lddmc_false);
        test_assert(lddmc_iscopy(m) == 0);
        test_assert(lddmc_follow(m, value) == lddmc_true);
        for (int j=0; j<100; j++) {
            uint32_t other_value = rng(0, 100);
            if (value != other_value) test_assert(lddmc_follow(m, other_value) == lddmc_false);
        }
    }

    // test handling of the copy node by primitives
    MDD m = lddmc_make_copynode(lddmc_true, lddmc_false);
    test_assert(lddmc_iscopy(m) == 1);
    test_assert(lddmc_getvalue(m) == 0);
    test_assert(lddmc_getdown(m) == lddmc_true);
    test_assert(lddmc_getright(m) == lddmc_false);
    m = lddmc_extendnode(m, 0, lddmc_true);
    test_assert(lddmc_iscopy(m) == 1);
    test_assert(lddmc_getvalue(m) == 0);
    test_assert(lddmc_getdown(m) == lddmc_true);
    test_assert(lddmc_getright(m) != lddmc_false);
    test_assert(lddmc_follow(m, 0) == lddmc_true);
    test_assert(lddmc_getvalue(lddmc_getright(m)) == 0);
    test_assert(lddmc_iscopy(lddmc_getright(m)) == 0);
    test_assert(lddmc_makenode(0, lddmc_true, lddmc_false) == lddmc_getright(m));

    LACE_ME;
    // test union_cube
    for (int i=0; i<100; i++) {
        int depth = rng(1, 6);
        int elements = rng(1, 30);
        m = make_random_ldd_set(depth, 10, elements);
        assert(m != lddmc_true);
        assert(m != lddmc_false);
        assert(lddmc_satcount(m) <= elements);
        assert(lddmc_satcount(m) >= 1);
    }

    // test simply transition relation
    {
        MDD states, rel, meta, expected;

        // relation: (0,0) to (1,1)
        rel = lddmc_cube((uint32_t[]){0,1,0,1}, 4);
        test_assert(lddmc_satcount(rel) == 1);
        // relation: (0,0) to (2,2)
        rel = lddmc_union_cube(rel, (uint32_t[]){0,2,0,2}, 4);
        test_assert(lddmc_satcount(rel) == 2);
        // meta: read write read write
        meta = lddmc_cube((uint32_t[]){1,2,1,2}, 4);
        test_assert(lddmc_satcount(meta) == 1);
        // initial state: (0,0)
        states = lddmc_cube((uint32_t[]){0,0}, 2);
        test_assert(lddmc_satcount(states) == 1);
        // relprod should give two states
        states = lddmc_relprod(states, rel, meta);
        test_assert(lddmc_satcount(states) == 2);
        // relprod should give states (1,1) and (2,2)
        expected = lddmc_cube((uint32_t[]){1,1}, 2);
        expected = lddmc_union_cube(expected, (uint32_t[]){2,2}, 2);
        test_assert(states == expected);

        // now test relprod union on the simple example
        states = lddmc_cube((uint32_t[]){0,0}, 2);
        states = lddmc_relprod_union(states, rel, meta, states);
        test_assert(lddmc_satcount(states) == 3);
        test_assert(states == lddmc_union(states, expected));

        // now create transition (1,1) --> (1,1) (using copy nodes)
        rel = lddmc_cube_copy((uint32_t[]){1,0,1,0}, (int[]){0,1,0,1}, 4);
        states = lddmc_relprod(states, rel, meta);
        // the result should be just state (1,1)
        test_assert(states == lddmc_cube((uint32_t[]){1,1}, 2));

        MDD statezero = lddmc_cube((uint32_t[]){0,0}, 2);
        states = lddmc_union_cube(statezero, (uint32_t[]){1,1}, 2);
        test_assert(lddmc_relprod_union(states, rel, meta, statezero) == states);

        // now create transition (*,*) --> (*,*) (copy nodes)
        rel = lddmc_cube_copy((uint32_t[]){0,0}, (int[]){1,1}, 2);
        meta = lddmc_cube((uint32_t[]){4,4}, 2);
        states = make_random_ldd_set(2, 10, 10);
        MDD states2 = make_random_ldd_set(2, 10, 10);
        test_assert(lddmc_union(states, states2) == lddmc_relprod_union(states, rel, meta, states2));
    }

    return 0;
}

int
test_hzdd()
{
    LACE_ME;

    BDD dd;
    // create a BDD domain 0,1,2,3,4
    const BDD dom = mtbdd_fromarray((uint32_t[]){0,1,2,3,4}, 5);

    dd = mtbdd_cube(dom, (uint8_t[]){0,0,0,0,0}, mtbdd_true);
    FILE *ff = fopen("test2.dot", "w");
    hzdd_fprintdot(ff, hzdd_from_mtbdd(dd, dom));
    fclose(ff);

    HZDD a = hzdd_makenode(4, hzdd_false, hzdd_true | hzdd_emptydomain);
    test_assert(a == hzdd_ithvar(4));

    HZDD b = hzdd_ithvar(5);
    test_assert(b == hzdd_from_mtbdd(sylvan_ithvar(5), sylvan_ithvar(5)));

    BDD a_and_b_bdd = sylvan_and(sylvan_ithvar(4), sylvan_ithvar(5));
    BDD a_and_b_dom = sylvan_and(sylvan_ithvar(4), sylvan_ithvar(5));
    HZDD a_and_b = hzdd_and(a, b);
    a_and_b = hzdd_from_mtbdd(a_and_b_bdd, a_and_b_dom);

    FILE *f = fopen("test.dot", "w");
    hzdd_fprintdot(f, a_and_b);
    fclose(f);

    test_assert(a_and_b == hzdd_from_mtbdd(a_and_b_bdd, a_and_b_dom));

    return 0;
}

int runtests()
{
    // we are not testing garbage collection
    sylvan_gc_disable();

    if (test_bdd()) return 1;
    for (int j=0;j<10;j++) if (test_cube()) return 1;
    for (int j=0;j<10;j++) if (test_relprod()) return 1;
    for (int j=0;j<10;j++) if (test_compose()) return 1;
    for (int j=0;j<10;j++) if (test_operators()) return 1;

    if (test_ldd()) return 1;
    if (test_hzdd()) return 1;

    return 0;
}

int main()
{
    // Standard Lace initialization with 1 worker
	lace_init(1, 0);
	lace_startup(0, NULL, NULL);

    // Simple Sylvan initialization, also initialize BDD, MTBDD and LDD support
	sylvan_init_package(1LL<<20, 1LL<<20, 1LL<<16, 1LL<<16);
	sylvan_init_bdd();
    sylvan_init_mtbdd();
    sylvan_init_ldd();
    sylvan_init_hzdd();

    int res = runtests();

    sylvan_quit();
    lace_exit();

    return res;
}
