#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <unistd.h>
#include <omp.h>
#include "mt19937p.h"
#include <mpi.h>
#include <immintrin.h>

// Global definitions

#define MASK_1 0xffffffffffffffff
#define MASK_0 0x0000000000000000

// Number of 32b words in vector width. Determine based on what platform
// we're running on. Also set the corresponding AVX intrinsics and data
// types.

#define VECTOR_NWORDS 8
#define VECTOR_NBYTES 32
#define vec         __m256i
#define vec_set     _mm256_set_epi32
#define vec_set1    _mm256_set1_epi32
#define vec_load    _mm256_load_si256
#define vec_add     _mm256_add_epi32
#define vec_cmpgt   _mm256_cmpgt_epi32
#define vec_setzero _mm256_setzero_si256
#define vec_and     _mm256_and_si256
#define vec_testc   _mm256_testc_si256
#define vec_andnot  _mm256_andnot_si256
#define vec_store   _mm256_store_si256

void transpose_array(int M, int *A, int *copied)
{
    int row, column;

    #pragma omp parallel for collapse(2)
    for (column = 0; column < M; ++column){
        for (row = 0; row < M; ++row){
            copied[(row * M) + column] = A[(column * M) + row];
        }
    }
}

//ldoc on
/**
 * # The basic recurrence
 *
 * At the heart of the method is the following basic recurrence.
 * If $l_{ij}^s$ represents the length of the shortest path from
 * $i$ to $j$ that can be attained in at most $2^s$ steps, then
 * $$
 *   l_{ij}^{s+1} = \min_k \{ l_{ik}^s + l_{kj}^s \}.
 * $$
 * That is, the shortest path of at most $2^{s+1}$ hops that connects
 * $i$ to $j$ consists of two segments of length at most $2^s$, one
 * from $i$ to $k$ and one from $k$ to $j$.  Compare this with the
 * following formula to compute the entries of the square of a
 * matrix $A$:
 * $$
 *   a_{ij}^2 = \sum_k a_{ik} a_{kj}.
 * $$
 * These two formulas are identical, save for the niggling detail that
 * the latter has addition and multiplication where the former has min
 * and addition.  But the basic pattern is the same, and all the
 * tricks we learned when discussing matrix multiplication apply -- or
 * at least, they apply in principle.  I'm actually going to be lazy
 * in the implementation of `square`, which computes one step of
 * this basic recurrence.  I'm not trying to do any clever blocking.
 * You may choose to be more clever in your assignment, but it is not
 * required.
 *
 * The return value for `square` is true if `l` and `lnew` are
 * identical, and false otherwise.
 */

int square(int n,               // Number of nodes
           int* restrict l,
           int* restrict l_transposed,     // Partial distance at step s
           int* restrict lnew)  // Partial distance at step s+1
{
    int done = 1;
    int vec_blocks = n / VECTOR_NWORDS;
    #pragma omp parallel for collapse(2) shared(l, lnew) reduction(&& : done)
    for (int j = 0; j < n; ++j) {
        for (int i = 0; i < n; ++i) {
            int i_start = i*n;
            int j_start = j*n;

            int lij = l[j_start + i];
            int* sums = _mm_malloc(VECTOR_NWORDS * sizeof(int), VECTOR_NBYTES);

            for (int kb = 0; kb < vec_blocks; ++kb) {
                int kb_offset = kb * VECTOR_NWORDS;
                int *lik_vec_addr = l_transposed + i_start + kb_offset;
                int *lkj_vec_addr = l + j_start + kb_offset;

                vec lij_vec = vec_set1(lij);
                vec lik_vec = vec_load((vec *)lik_vec_addr);
                vec lkj_vec = vec_load((vec *)lkj_vec_addr);

                vec sum_vec = vec_add(lik_vec, lkj_vec);

                vec mask_vec = vec_cmpgt(lij_vec, sum_vec);
                vec zero_vec = vec_setzero();

                // we assume that there is no junk

                if (!vec_testc(mask_vec, zero_vec)){
                    done = 0;  
                    vec_store((vec *)sums, sum_vec);
                    for (int z = 0; z < VECTOR_NWORDS; z++){
                        if (sums[z] < lij){
                            lij = sums[z];
                        }
                    }
                }
            }

            _mm_free(sums);

            // for (int k = 0; k < n; ++k) {
            //     // int lik = l[k*n+i];
            //     int lik = l_transposed[i_start + k];
            //     int lkj = l[j_start + k];
                
            //     if (lik + lkj < lij) {
            //         lij = lik+lkj;
            //         done = 0;
            //     }
            // }
            lnew[j_start+i] = lij;
        }
    }
    return done;
}

/**
 *
 * The value $l_{ij}^0$ is almost the same as the $(i,j)$ entry of
 * the adjacency matrix, except for one thing: by convention, the
 * $(i,j)$ entry of the adjacency matrix is zero when there is no
 * edge between $i$ and $j$; but in this case, we want $l_{ij}^0$
 * to be "infinite".  It turns out that it is adequate to make
 * $l_{ij}^0$ longer than the longest possible shortest path; if
 * edges are unweighted, $n+1$ is a fine proxy for "infinite."
 * The functions `infinitize` and `deinfinitize` convert back 
 * and forth between the zero-for-no-edge and $n+1$-for-no-edge
 * conventions.
 */

static inline void infinitize(int n, int* l)
{
    for (int i = 0; i < n*n; ++i)
        if (l[i] == 0)
            l[i] = n+1;
}

static inline void deinfinitize(int n, int* l)
{
    for (int i = 0; i < n*n; ++i)
        if (l[i] == n+1)
            l[i] = 0;
}

/**
 *
 * Of course, any loop-free path in a graph with $n$ nodes can
 * at most pass through every node in the graph.  Therefore,
 * once $2^s \geq n$, the quantity $l_{ij}^s$ is actually
 * the length of the shortest path of any number of hops.  This means
 * we can compute the shortest path lengths for all pairs of nodes
 * in the graph by $\lceil \lg n \rceil$ repeated squaring operations.
 *
 * The `shortest_path` routine attempts to save a little bit of work
 * by only repeatedly squaring until two successive matrices are the
 * same (as indicated by the return value of the `square` routine).
 */

int shortest_paths(int n, int* restrict l)
{
    // Generate l_{ij}^0 from adjacency matrix representation
    infinitize(n, l);
    for (int i = 0; i < n*n; i += n+1)
        l[i] = 0;

    // Repeated squaring until nothing changes
    int* restrict lnew = (int*) calloc(n*n, sizeof(int));
    int* restrict l_transposed = (int*) calloc(n*n, sizeof(int));

    // memcpy(lnew, l, n*n * sizeof(int));

    int iters = 0;
    for (int done = 0; !done; ) {
        transpose_array(n, l, l_transposed);
        done = square(n, l, l_transposed, lnew);
        memcpy(l, lnew, n*n * sizeof(int));
        iters += 1;
    }
    free(lnew);
    deinfinitize(n, l);

    return iters;
}

/**
 * # The random graph model
 *
 * Of course, we need to run the shortest path algorithm on something!
 * For the sake of keeping things interesting, let's use a simple random graph
 * model to generate the input data.  The $G(n,p)$ model simply includes each
 * possible edge with probability $p$, drops it otherwise -- doesn't get much
 * simpler than that.  We use a thread-safe version of the Mersenne twister
 * random number generator in lieu of coin flips.
 */

int* gen_graph(int n, double p)
{
    int* l = calloc(n*n, sizeof(int));
    struct mt19937p state;
    sgenrand(10302011UL, &state);
    for (int j = 0; j < n; ++j) {
        for (int i = 0; i < n; ++i)
            l[j*n+i] = (genrand(&state) < p);
        l[j*n+j] = 0;
    }
    return l;
}

/**
 * # Result checks
 *
 * Simple tests are always useful when tuning code, so I have included
 * two of them.  Since this computation doesn't involve floating point
 * arithmetic, we should get bitwise identical results from run to
 * run, even if we do optimizations that change the associativity of
 * our computations.  The function `fletcher16` computes a simple
 * [simple checksum][wiki-fletcher] over the output of the
 * `shortest_paths` routine, which we can then use to quickly tell
 * whether something has gone wrong.  The `write_matrix` routine
 * actually writes out a text representation of the matrix, in case we
 * want to load it into MATLAB to compare results.
 *
 * [wiki-fletcher]: http://en.wikipedia.org/wiki/Fletcher's_checksum
 */

int fletcher16(int* data, int count)
{
    int sum1 = 0;
    int sum2 = 0;
    for(int index = 0; index < count; ++index) {
          sum1 = (sum1 + data[index]) % 255;
          sum2 = (sum2 + sum1) % 255;
    }
    return (sum2 << 8) | sum1;
}

void write_matrix(const char* fname, int n, int* a)
{
    FILE* fp = fopen(fname, "w+");
    if (fp == NULL) {
        fprintf(stderr, "Could not open output file: %s\n", fname);
        exit(-1);
    }
    for (int i = 0; i < n; ++i) {
        for (int j = 0; j < n; ++j) 
            fprintf(fp, "%d ", a[j*n+i]);
        fprintf(fp, "\n");
    }
    fclose(fp);
}

/**
 * # The `main` event
 */

const char* usage =
    "path.x -- Parallel all-pairs shortest path on a random graph\n"
    "Flags:\n"
    "  - n -- number of nodes (200)\n"
    "  - p -- probability of including edges (0.05)\n"
    "  - i -- file name where adjacency matrix should be stored (none)\n"
    "  - o -- file name where output matrix should be stored (none)\n";

int main(int argc, char** argv)
{
    int n    = 200;            // Number of nodes
    double p = 0.05;           // Edge probability
    const char* ifname = NULL; // Adjacency matrix file name
    const char* ofname = NULL; // Distance matrix file name

    // Option processing
    extern char* optarg;
    const char* optstring = "hn:d:p:o:i:";
    int c;
    while ((c = getopt(argc, argv, optstring)) != -1) {
        switch (c) {
        case 'h':
            fprintf(stderr, "%s", usage);
            return -1;
        case 'n': n = atoi(optarg); break;
        case 'p': p = atof(optarg); break;
        case 'o': ofname = optarg;  break;
        case 'i': ifname = optarg;  break;
        }
    }

    // Graph generation + output
    int* l = gen_graph(n, p);
    if (ifname)
        write_matrix(ifname,  n, l);

    // Time the shortest paths code
    double t0 = omp_get_wtime();
    int iters = shortest_paths(n, l);
    double t1 = omp_get_wtime();

    printf("== OpenMP with %d threads\n", omp_get_max_threads());
    printf("n:     %d\n", n);
    printf("p:     %g\n", p);
    printf("Time:  %g\n", t1-t0);
    printf("Check: %X\n", fletcher16(l, n*n));
    printf("Iterations: %d\n", iters);
    printf("\n");

    // Generate output file
    if (ofname)
        write_matrix(ofname, n, l);

    // Clean up
    free(l);
    return 0;
}
