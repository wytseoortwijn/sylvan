#include <argp.h>
#include <inttypes.h>
#include <locale.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

#ifdef HAVE_PROFILER
#include <gperftools/profiler.h>
#endif

#include <sylvan.h>
#include <llmsset.h>

/* Configuration */
static int report_levels = 0; // report states at end of every level
static int report_table = 0; // report table size at end of every level
static int report_nodes = 0; // report number of nodes of BDDs
static int strategy = 1; // 0 = BFS, 1 = PAR, 2 = SAT
static int check_deadlocks = 0; // set to 1 to check for deadlocks
static int print_transition_matrix = 0; // print transition relation matrix
static int workers = 0; // autodetect
static char* model_filename = NULL; // filename of model
#ifdef HAVE_PROFILER
static char* profile_filename = NULL; // filename for profiling
#endif

/* argp configuration */
static struct argp_option options[] =
{
    {"workers", 'w', "<workers>", 0, "Number of workers (default=0: autodetect)", 0},
    {"strategy", 's', "<bfs|par|sat>", 0, "Strategy for reachability (default=par)", 0},
#ifdef HAVE_PROFILER
    {"profiler", 'p', "<filename>", 0, "Filename for profiling", 0},
#endif
    {"deadlocks", 3, 0, 0, "Check for deadlocks", 1},
    {"count-nodes", 5, 0, 0, "Report #nodes for BDDs", 1},
    {"count-states", 1, 0, 0, "Report #states at each level", 1},
    {"count-table", 2, 0, 0, "Report table usage at each level", 1},
    {"print-matrix", 4, 0, 0, "Print transition matrix", 1},
    {0, 0, 0, 0, 0, 0}
};
static error_t
parse_opt(int key, char *arg, struct argp_state *state)
{
    switch (key) {
    case 'w':
        workers = atoi(arg);
        break;
    case 's':
        if (strcmp(arg, "bfs")==0) strategy = 0;
        else if (strcmp(arg, "par")==0) strategy = 1;
        else if (strcmp(arg, "sat")==0) strategy = 2;
        else argp_usage(state);
        break;
    case 4:
        print_transition_matrix = 1;
        break;
    case 3:
        check_deadlocks = 1;
        break;
    case 1:
        report_levels = 1;
        break;
    case 2:
        report_table = 1;
        break;
#ifdef HAVE_PROFILER
    case 'p':
        profile_filename = arg;
        break;
#endif
    case ARGP_KEY_ARG:
        if (state->arg_num >= 1) argp_usage(state);
        model_filename = arg;
        break;
    case ARGP_KEY_END:
        if (state->arg_num < 1) argp_usage(state);
        break;
    default:
        return ARGP_ERR_UNKNOWN;
    }
    return 0;
}
static struct argp argp = { options, parse_opt, "<model>", 0, 0, 0, 0 };

/* Globals */
typedef struct set
{
    BDD bdd;
    BDD variables; // all variables in the set (used by satcount)
} *set_t;

typedef struct relation
{
    BDD bdd;
    BDD variables; // all variables in the relation (used by relprod)
} *rel_t;

static int vector_size; // size of vector
static int statebits, actionbits; // number of bits for state, number of bits for action
static int bits_per_integer; // number of bits per integer in the vector
static int next_count; // number of partitions of the transition relation
static rel_t *next; // each partition of the transition relation

/* Obtain current wallclock time */
static double
wctime()
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (tv.tv_sec + 1E-6 * tv.tv_usec);
}

static double t_start;
#define INFO(s, ...) fprintf(stdout, "[% 8.2f] " s, wctime()-t_start, ##__VA_ARGS__)
#define Abort(...) { fprintf(stderr, __VA_ARGS__); exit(-1); }

/* Load a set from file */
#define set_load(f) CALL(set_load, f)
TASK_1(set_t, set_load, FILE*, f)
{
    sylvan_serialize_fromfile(f);

    size_t set_bdd, set_vector_size, set_state_vars;
    if ((fread(&set_bdd, sizeof(size_t), 1, f) != 1) ||
        (fread(&set_vector_size, sizeof(size_t), 1, f) != 1) ||
        (fread(&set_state_vars, sizeof(size_t), 1, f) != 1)) {
        Abort("Invalid input file!\n");
    }

    set_t set = (set_t)malloc(sizeof(struct set));
    set->bdd = sylvan_serialize_get_reversed(set_bdd);
    set->variables = sylvan_support(sylvan_serialize_get_reversed(set_state_vars));

    sylvan_protect(&set->bdd);
    sylvan_protect(&set->variables);

    return set;
}

/* Load a relation from file */
#define rel_load(f) CALL(rel_load, f)
TASK_1(rel_t, rel_load, FILE*, f)
{
    sylvan_serialize_fromfile(f);

    size_t rel_bdd, rel_vars;
    if ((fread(&rel_bdd, sizeof(size_t), 1, f) != 1) ||
        (fread(&rel_vars, sizeof(size_t), 1, f) != 1)) {
        Abort("Invalid input file!\n");
    }

    rel_t rel = (rel_t)malloc(sizeof(struct relation));
    rel->bdd = sylvan_serialize_get_reversed(rel_bdd);
    rel->variables = sylvan_support(sylvan_serialize_get_reversed(rel_vars));

    sylvan_protect(&rel->bdd);
    sylvan_protect(&rel->variables);

    return rel;
}

#define print_example(example, variables) CALL(print_example, example, variables)
VOID_TASK_2(print_example, BDD, example, BDDSET, variables)
{
    uint8_t str[vector_size * bits_per_integer];

    if (example != sylvan_false) {
        sylvan_sat_one(example, variables, str);
        printf("[");
        for (int i=0; i<vector_size; i++) {
            uint32_t res = 0;
            for (int j=0; j<bits_per_integer; j++) {
                if (str[bits_per_integer*i+j] == 1) res++;
                res<<=1;
            }
            if (i>0) printf(",");
            printf("%" PRIu32, res);
        }
        printf("]");
    }
}

TASK_2(BDD, go_sat, BDD, set, int, idx)
{
    if (set == sylvan_false) return sylvan_false;
    if (idx == next_count) return set;

    BDD result;
    // cache
    const BDD s = set;
    if (cache_get(s | (200LL<<40), idx, 0, &result)) return result;

    // NOTE: the cache "could" be much better by adding each intermediate result too??

    BDDVAR var = sylvan_var(next[idx]->bdd);

    if (set == sylvan_true || var <= sylvan_var(set)) {
        // count number of relations starting here
        int count = idx+1;
        while (count < next_count && var == sylvan_var(next[count]->bdd)) count++;
        count -= idx;
        // apply fixpoint
        BDD prev = sylvan_false;
        while (prev != set) {
            prev = set;
            bdd_refs_push(set);
            // first recursive
            if (idx < 16) printf("going in %d\n", idx+count);
            set = CALL(go_sat, set, idx+count);
            // apply all relations once
            // printf("applying %d-%d\n", idx, idx+count-1);
            for (int i=0;i<count;i++) {
                bdd_refs_push(set);
                BDD step = sylvan_relnext(set, next[idx+i]->bdd, next[idx+i]->variables);
                bdd_refs_push(step);
                set = sylvan_or(set, step);
                bdd_refs_pop(2);
            }
            bdd_refs_pop(1);
        }
        result = set;
    } else {
        // recursive
        bdd_refs_spawn(SPAWN(go_sat, sylvan_low(set), idx));
        BDD high = bdd_refs_push(CALL(go_sat, sylvan_high(set), idx));
        BDD low = bdd_refs_sync(SYNC(go_sat));
        result = sylvan_makenode(sylvan_var(set), low, high);
    }

    cache_put(s | (200LL<<40), idx, 0, result);
    return result;
}

/* Saturation strategy */
VOID_TASK_1(sat, set_t, set)
{
    set->bdd = CALL(go_sat, set->bdd, 0);
}

/* Straight-forward implementation of parallel reduction */
TASK_5(BDD, go_par, BDD, cur, BDD, visited, size_t, from, size_t, len, BDD*, deadlocks)
{
    if (len == 1) {
        // Calculate NEW successors (not in visited)
        BDD succ = sylvan_relnext(cur, next[from]->bdd, next[from]->variables);
        bdd_refs_push(succ);
        if (deadlocks) {
            // check which BDDs in deadlocks do not have a successor in this relation
            BDD anc = sylvan_relprev(next[from]->bdd, succ, next[from]->variables);
            bdd_refs_push(anc);
            *deadlocks = sylvan_diff(*deadlocks, anc);
            bdd_refs_pop(1);
        }
        BDD result = sylvan_diff(succ, visited);
        bdd_refs_pop(1);
        return result;
    } else {
        BDD deadlocks_left;
        BDD deadlocks_right;
        if (deadlocks) {
            deadlocks_left = *deadlocks;
            deadlocks_right = *deadlocks;
            sylvan_protect(&deadlocks_left);
            sylvan_protect(&deadlocks_right);
        }

        // Recursively calculate left+right
        bdd_refs_spawn(SPAWN(go_par, cur, visited, from, (len+1)/2, deadlocks ? &deadlocks_left: NULL));
        BDD right = bdd_refs_push(CALL(go_par, cur, visited, from+(len+1)/2, len/2, deadlocks ? &deadlocks_right : NULL));
        BDD left = bdd_refs_push(bdd_refs_sync(SYNC(go_par)));

        // Merge results of left+right
        BDD result = sylvan_or(left, right);
        bdd_refs_pop(2);

        if (deadlocks) {
            bdd_refs_push(result);
            *deadlocks = sylvan_and(deadlocks_left, deadlocks_right);
            sylvan_unprotect(&deadlocks_left);
            sylvan_unprotect(&deadlocks_right);
            bdd_refs_pop(1);
        }

        return result;
    }
}

/* PAR strategy, parallel strategy (operations called in parallel *and* parallelized by Sylvan) */
VOID_TASK_1(par, set_t, set)
{
    BDD visited = set->bdd;
    BDD next_level = visited;
    BDD cur_level = sylvan_false;
    BDD deadlocks = sylvan_false;

    sylvan_protect(&visited);
    sylvan_protect(&next_level);
    sylvan_protect(&cur_level);
    sylvan_protect(&deadlocks);

    int iteration = 1;
    do {
        // calculate successors in parallel
        cur_level = next_level;
        deadlocks = cur_level;

        next_level = CALL(go_par, cur_level, visited, 0, next_count, check_deadlocks ? &deadlocks : NULL);

        if (check_deadlocks && deadlocks != sylvan_false) {
            INFO("Found %'0.0f deadlock states... ", sylvan_satcount(deadlocks, set->variables));
            if (deadlocks != sylvan_false) {
                printf("example: ");
                print_example(deadlocks, set->variables);
                check_deadlocks = 0;
            }
            printf("\n");
        }

        // visited = visited + new
        visited = sylvan_or(visited, next_level);

        if (report_table && report_levels) {
            size_t filled, total;
            sylvan_table_usage(&filled, &total);
            INFO("Level %d done, %'0.0f states explored, table: %0.1f%% full (%'zu nodes)\n",
                iteration, sylvan_satcount_cached(visited, set->variables),
                100.0*(double)filled/total, filled);
        } else if (report_table) {
            size_t filled, total;
            sylvan_table_usage(&filled, &total);
            INFO("Level %d done, table: %0.1f%% full (%'zu nodes)\n",
                iteration,
                100.0*(double)filled/total, filled);
        } else if (report_levels) {
            INFO("Level %d done, %'0.0f states explored\n", iteration, sylvan_satcount(visited, set->variables));
        } else {
            INFO("Level %d done\n", iteration);
        }
        iteration++;
    } while (next_level != sylvan_false);

    set->bdd = visited;

    sylvan_unprotect(&visited);
    sylvan_unprotect(&next_level);
    sylvan_unprotect(&cur_level);
    sylvan_unprotect(&deadlocks);
}

/* Sequential version of merge-reduction */
TASK_5(BDD, go_bfs, BDD, cur, BDD, visited, size_t, from, size_t, len, BDD*, deadlocks)
{
    if (len == 1) {
        // Calculate NEW successors (not in visited)
        BDD succ = sylvan_relnext(cur, next[from]->bdd, next[from]->variables);
        bdd_refs_push(succ);
        if (deadlocks) {
            // check which BDDs in deadlocks do not have a successor in this relation
            BDD anc = sylvan_relprev(next[from]->bdd, succ, next[from]->variables);
            bdd_refs_push(anc);
            *deadlocks = sylvan_diff(*deadlocks, anc);
            bdd_refs_pop(1);
        }
        BDD result = sylvan_diff(succ, visited);
        bdd_refs_pop(1);
        return result;
    } else {
        BDD deadlocks_left;
        BDD deadlocks_right;
        if (deadlocks) {
            deadlocks_left = *deadlocks;
            deadlocks_right = *deadlocks;
            sylvan_protect(&deadlocks_left);
            sylvan_protect(&deadlocks_right);
        }

        // Recursively calculate left+right
        BDD left = CALL(go_bfs, cur, visited, from, (len+1)/2, deadlocks ? &deadlocks_left : NULL);
        bdd_refs_push(left);
        BDD right = CALL(go_bfs, cur, visited, from+(len+1)/2, len/2, deadlocks ? &deadlocks_right : NULL);
        bdd_refs_push(right);

        // Merge results of left+right
        BDD result = sylvan_or(left, right);
        bdd_refs_pop(2);

        if (deadlocks) {
            bdd_refs_push(result);
            *deadlocks = sylvan_and(deadlocks_left, deadlocks_right);
            sylvan_unprotect(&deadlocks_left);
            sylvan_unprotect(&deadlocks_right);
            bdd_refs_pop(1);
        }

        return result;
    }
}

/* BFS strategy, sequential strategy (but operations are parallelized by Sylvan) */
VOID_TASK_1(bfs, set_t, set)
{
    BDD visited = set->bdd;
    BDD next_level = visited;
    BDD cur_level = sylvan_false;
    BDD deadlocks = sylvan_false;

    sylvan_protect(&visited);
    sylvan_protect(&next_level);
    sylvan_protect(&cur_level);
    sylvan_protect(&deadlocks);

    int iteration = 1;
    do {
        // calculate successors in parallel
        cur_level = next_level;
        deadlocks = cur_level;

        next_level = CALL(go_bfs, cur_level, visited, 0, next_count, check_deadlocks ? &deadlocks : NULL);

        if (check_deadlocks && deadlocks != sylvan_false) {
            INFO("Found %'0.0f deadlock states... ", sylvan_satcount(deadlocks, set->variables));
            if (deadlocks != sylvan_false) {
                printf("example: ");
                print_example(deadlocks, set->variables);
                check_deadlocks = 0;
            }
            printf("\n");
        }

        // visited = visited + new
        visited = sylvan_or(visited, next_level);

        if (report_table && report_levels) {
            size_t filled, total;
            sylvan_table_usage(&filled, &total);
            INFO("Level %d done, %'0.0f states explored, table: %0.1f%% full (%'zu nodes)\n",
                iteration, sylvan_satcount_cached(visited, set->variables),
                100.0*(double)filled/total, filled);
        } else if (report_table) {
            size_t filled, total;
            sylvan_table_usage(&filled, &total);
            INFO("Level %d done, table: %0.1f%% full (%'zu nodes)\n",
                iteration,
                100.0*(double)filled/total, filled);
        } else if (report_levels) {
            INFO("Level %d done, %'0.0f states explored\n", iteration, sylvan_satcount(visited, set->variables));
        } else {
            INFO("Level %d done\n", iteration);
        }
        iteration++;
    } while (next_level != sylvan_false);

    set->bdd = visited;

    sylvan_unprotect(&visited);
    sylvan_unprotect(&next_level);
    sylvan_unprotect(&cur_level);
    sylvan_unprotect(&deadlocks);
}

void
gnomesort_next()
{
    int i = 1, j = 2;
    rel_t t;
    while (i < next_count) {
        rel_t *p = &next[i], *q = p-1;
        if (sylvan_var((*q)->bdd) > sylvan_var((*p)->bdd)) {
            t = *q;
            *q = *p;
            *p = t;
            if (--i) continue;
        }
        i = j++;
    }
}

static void
print_matrix(BDD vars)
{
    for (int i=0; i<vector_size; i++) {
        if (sylvan_set_isempty(vars)) {
            fprintf(stdout, "-");
        } else {
            BDDVAR next_s = 2*((i+1)*bits_per_integer);
            if (sylvan_set_var(vars) < next_s) {
                fprintf(stdout, "+");
                for (;;) {
                    vars = sylvan_set_next(vars);
                    if (sylvan_set_isempty(vars)) break;
                    if (sylvan_set_var(vars) >= next_s) break;
                }
            } else {
                fprintf(stdout, "-");
            }
        }
    }
}

VOID_TASK_0(gc_start)
{
    INFO("(GC) Starting garbage collection...\n");
}

VOID_TASK_0(gc_end)
{
    INFO("(GC) Garbage collection done.\n");
}

int
main(int argc, char **argv)
{
    argp_parse(&argp, argc, argv, 0, 0, 0);
    setlocale(LC_NUMERIC, "en_US.utf-8");
    t_start = wctime();

    FILE *f = fopen(model_filename, "r");
    if (f == NULL) {
        fprintf(stderr, "Cannot open file '%s'!\n", model_filename);
        return -1;
    }

    // Init Lace
    lace_init(workers, 1000000); // auto-detect number of workers, use a 1,000,000 size task queue
    lace_startup(0, NULL, NULL); // auto-detect program stack, do not use a callback for startup

    LACE_ME;

    // Init Sylvan
    // Nodes table size: 24 bytes * 2**N_nodes
    // Cache table size: 36 bytes * 2**N_cache
    // With: N_nodes=25, N_cache=24: 1.3 GB memory
    sylvan_init_package(1LL<<21, 1LL<<27, 1LL<<20, 1LL<<26);
    sylvan_init_bdd(6); // granularity 6 is decent default value - 1 means "use cache for every operation"
    sylvan_gc_add_mark(0, TASK(gc_start));
    sylvan_gc_add_mark(40, TASK(gc_end));

    /* Load domain information */
    if ((fread(&vector_size, sizeof(int), 1, f) != 1) ||
        (fread(&statebits, sizeof(int), 1, f) != 1) ||
        (fread(&actionbits, sizeof(int), 1, f) != 1)) {
        Abort("Invalid input file!\n");
    }

    bits_per_integer = statebits;
    statebits *= vector_size;

    // Read initial state
    set_t states = set_load(f);

    // Read transitions
    if (fread(&next_count, sizeof(int), 1, f) != 1) Abort("Invalid input file!\n");
    next = (rel_t*)malloc(sizeof(rel_t) * next_count);

    int i;
    for (i=0; i<next_count; i++) {
        next[i] = rel_load(f);
    }

    /* Done */
    fclose(f);

    if (strategy == 2) {
        // sort the transition relations (gnome sort)
        gnomesort_next();
    }

    if (print_transition_matrix) {
        for (i=0; i<next_count; i++) {
            INFO("");
            print_matrix(next[i]->variables);
            fprintf(stdout, "\n");
        }
    }

    // Report statistics
    INFO("Read file '%s'\n", model_filename);
    INFO("%d integers per state, %d bits per integer, %d transition groups\n", vector_size, bits_per_integer, next_count);

    if (report_nodes) {
        INFO("BDD nodes:\n");
        INFO("Initial states: %zu BDD nodes\n", sylvan_nodecount(states->bdd));
        for (i=0; i<next_count; i++) {
            INFO("Transition %d: %zu BDD nodes\n", i, sylvan_nodecount(next[i]->bdd));
        }
    }

#ifdef HAVE_PROFILER
    if (profile_filename != NULL) ProfilerStart(profile_filename);
#endif
    if (strategy == 1) {
        double t1 = wctime();
        CALL(par, states);
        double t2 = wctime();
        INFO("PAR Time: %f\n", t2-t1);
    } else if (strategy == 2) {
        double t1 = wctime();
        CALL(sat, states);
        double t2 = wctime();
        INFO("SAT Time: %f\n", t2-t1);
    } else {
        double t1 = wctime();
        CALL(bfs, states);
        double t2 = wctime();
        INFO("BFS Time: %f\n", t2-t1);
    }
#ifdef HAVE_PROFILER
    if (profile_filename != NULL) ProfilerStop();
#endif

    // Now we just have states
    INFO("Final states: %'0.0f states\n", sylvan_satcount_cached(states->bdd, states->variables));
    if (report_nodes) {
        INFO("Final states: %'zu BDD nodes\n", sylvan_nodecount(states->bdd));
    }

    sylvan_stats_report(stdout, 1);

    return 0;
}
