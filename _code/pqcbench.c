/*
 * pqcbench.c  -  per-operation timing harness for the YSTE 2027 project
 *
 * WHAT IT DOES
 *   Runs ML-KEM, ML-DSA, and SLH-DSA in short round-robin "blocks" for a set
 *   total time, timing every single operation, and writes the raw timings to
 *   CSV. Within each cycle the three algorithms run in a shuffled order, so
 *   all three see the same thermal history and none always follows the same
 *   algorithm.
 *
 *   One "iteration" = three operations:
 *     KEM: op1 = keygen,  op2 = encaps, op3 = decaps
 *     SIG: op1 = keygen,  op2 = sign,   op3 = verify
 *
 * OUTPUT (two files, prefix given on the command line)
 *   <prefix>_blocks.csv  one row per block:
 *     blockId, alg, startMonoNs, endMonoNs, startWallNs, iterations, failures
 *   <prefix>_ops.csv     one row per iteration:
 *     blockId, tOffUs, op1Ns, op2Ns, op3Ns
 *     (tOffUs = microseconds from the start of the block to the start of
 *      this iteration, so slow operations can be placed in time)
 *
 *   startMonoNs uses CLOCK_MONOTONIC, the same clock Python's time.monotonic()
 *   reads on Linux, so it lines up with the logger's monoTimeS column.
 *
 * USAGE
 *   pqcbench <outPrefix> <durationS> <blockS> <seed> [kemName] [dsaName] [slhName]
 *   e.g. taskset -c 3 ./pqcbench logs/run01 1800 5 42
 *
 * BUILD (on the Pi, liboqs built in ~/liboqs/build)
 *   gcc -O2 -Wall -o pqcbench pqcbench.c \
 *       -I ~/liboqs/build/include -L ~/liboqs/build/lib -loqs -lcrypto -lpthread
 *
 * Written by Claude (Anthropic) as a tool for Callum's project; declared in
 * the report. Every timing decision is explained in the comments.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <signal.h>
#include <time.h>
#include <unistd.h>
#include <oqs/oqs.h>

/* ---------- timing helpers ---------- */

/* Nanoseconds from a given Linux clock. CLOCK_MONOTONIC never jumps
 * backwards (unlike wall time, which NTP can change mid-run). On the Pi 4
 * it ticks at 54 MHz (~18.5 ns resolution), and reading it costs well under
 * 1 us, which is small next to a ~70 us ML-KEM operation. 
 * 
 * >> aka this function does the same thing as doing time.monotonic_ns() in python
 * 
 * */
static uint64_t nowNs(clockid_t clk) {
    struct timespec ts;
    clock_gettime(clk, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

/* ---------- clean stop on Ctrl+C / kill ---------- */

/* A signal handler may only safely set a flag. The main loop checks it
 * between blocks, so a stopped run still ends with a complete block and
 * flushed files (same idea as the finally: block in loggingv1.py). */
static volatile sig_atomic_t stopRequested = 0;
static void onSignal(int sig) { (void)sig; stopRequested = 1; }

/* ---------- one algorithm under test ---------- */

typedef struct {
    const char *name;      /* liboqs name, e.g. "ML-KEM-768" */
    int isKem;             /* 1 = KEM, 0 = signature */
    OQS_KEM *kem;
    OQS_SIG *sig;
    /* buffers allocated once at start, so malloc never happens while timing */
    uint8_t *pk, *sk, *ct, *ss1, *ss2, *msg, *sigBuf;
    double sumNs[3];       /* running totals, only for the end-of-run summary */
    uint64_t count;
} Alg;

#define MSG_LEN 32         /* sign a fresh random 32-byte message each time */

static int setupAlg(Alg *a, const char *name, int isKem) {
    memset(a, 0, sizeof *a);
    a->name = name;
    a->isKem = isKem;
    if (isKem) {
        a->kem = OQS_KEM_new(name);   /* NULL if the name is wrong or disabled */
        if (!a->kem) return -1;
        a->pk  = malloc(a->kem->length_public_key);
        a->sk  = malloc(a->kem->length_secret_key);
        a->ct  = malloc(a->kem->length_ciphertext);
        a->ss1 = malloc(a->kem->length_shared_secret);
        a->ss2 = malloc(a->kem->length_shared_secret);
    } else {
        a->sig = OQS_SIG_new(name);
        if (!a->sig) return -1;
        a->pk     = malloc(a->sig->length_public_key);
        a->sk     = malloc(a->sig->length_secret_key);
        a->sigBuf = malloc(a->sig->length_signature);
        a->msg    = malloc(MSG_LEN);
    }
    return 0;
}

static void freeAlg(Alg *a) {
    free(a->pk); free(a->sk); free(a->ct); free(a->ss1); free(a->ss2);
    free(a->msg); free(a->sigBuf);
    if (a->kem) OQS_KEM_free(a->kem);
    if (a->sig) OQS_SIG_free(a->sig);
}

/* Run one iteration (three timed operations). Writes the three durations
 * into t[0..2]. Returns 0 if the crypto checked out, 1 if it failed.
 * Only the library call itself sits between the two clock reads. */
static int runIteration(Alg *a, uint32_t t[3]) {
    uint64_t t0, t1;
    int bad = 0;
    if (a->isKem) {
        t0 = nowNs(CLOCK_MONOTONIC);
        bad |= OQS_KEM_keypair(a->kem, a->pk, a->sk) != OQS_SUCCESS;
        t1 = nowNs(CLOCK_MONOTONIC); t[0] = (uint32_t)(t1 - t0);

        t0 = nowNs(CLOCK_MONOTONIC);
        bad |= OQS_KEM_encaps(a->kem, a->ct, a->ss1, a->pk) != OQS_SUCCESS;
        t1 = nowNs(CLOCK_MONOTONIC); t[1] = (uint32_t)(t1 - t0);

        t0 = nowNs(CLOCK_MONOTONIC);
        bad |= OQS_KEM_decaps(a->kem, a->ss2, a->ct, a->sk) != OQS_SUCCESS;
        t1 = nowNs(CLOCK_MONOTONIC); t[2] = (uint32_t)(t1 - t0);

        /* correctness check (untimed): both sides must get the same secret.
         * Proves the harness is really doing the cryptography. */
        bad |= memcmp(a->ss1, a->ss2, a->kem->length_shared_secret) != 0;
    } else {
        size_t sigLen = 0;
        OQS_randombytes(a->msg, MSG_LEN);           /* untimed */

        t0 = nowNs(CLOCK_MONOTONIC);
        bad |= OQS_SIG_keypair(a->sig, a->pk, a->sk) != OQS_SUCCESS;
        t1 = nowNs(CLOCK_MONOTONIC); t[0] = (uint32_t)(t1 - t0);

        t0 = nowNs(CLOCK_MONOTONIC);
        bad |= OQS_SIG_sign(a->sig, a->sigBuf, &sigLen, a->msg, MSG_LEN, a->sk) != OQS_SUCCESS;
        t1 = nowNs(CLOCK_MONOTONIC); t[1] = (uint32_t)(t1 - t0);

        t0 = nowNs(CLOCK_MONOTONIC);
        bad |= OQS_SIG_verify(a->sig, a->msg, MSG_LEN, a->sigBuf, sigLen, a->pk) != OQS_SUCCESS;
        t1 = nowNs(CLOCK_MONOTONIC); t[2] = (uint32_t)(t1 - t0);
    }
    /* uint32 ns holds up to ~4.29 s per op; SLH-DSA sign is ~0.14 s */
    return bad;
}

/* ---------- main ---------- */

int main(int argc, char **argv) {
    if (argc < 5) {
        fprintf(stderr, "usage: %s <outPrefix> <durationS> <blockS> <seed> "
                        "[kemName] [dsaName] [slhName]\n", argv[0]);
        return 2;
    }
    const char *prefix  = argv[1];
    double durationS    = atof(argv[2]);
    double blockS       = atof(argv[3]);
    unsigned seed       = (unsigned)strtoul(argv[4], NULL, 10);
    const char *kemName = argc > 5 ? argv[5] : "ML-KEM-768";
    const char *dsaName = argc > 6 ? argv[6] : "ML-DSA-65";
    const char *slhName = argc > 7 ? argv[7] : "SLH_DSA_PURE_SHAKE_128F";

    if (durationS <= 0 || blockS <= 0) {
        fprintf(stderr, "durationS and blockS must be > 0\n");
        return 2;
    }

    OQS_init();
    Alg algs[3];
    if (setupAlg(&algs[0], kemName, 1) || setupAlg(&algs[1], dsaName, 0) ||
        setupAlg(&algs[2], slhName, 0)) {
        fprintf(stderr, "could not create an algorithm - check the names "
                        "match liboqs exactly (e.g. SLH_DSA_PURE_SHAKE_128F)\n");
        return 1;
    }

    /* Buffer one block of timings in memory and write it to disk only when
     * the block ends, so file writing never happens inside the timed loop.
     * Capacity assumes no iteration is faster than 50 us (ML-KEM is ~215 us).
     * If the buffer fills, the block just ends early - this is recorded. */
    size_t cap = (size_t)(blockS * 20000.0) + 16;
    uint32_t (*times)[3] = malloc(cap * sizeof *times);
    uint32_t *offs = malloc(cap * sizeof *offs);

    char path[512];
    snprintf(path, sizeof path, "%s_blocks.csv", prefix);
    FILE *fb = fopen(path, "w");
    snprintf(path, sizeof path, "%s_ops.csv", prefix);
    FILE *fo = fopen(path, "w");
    if (!fb || !fo || !times || !offs) {
        fprintf(stderr, "could not open output files or allocate buffers\n");
        return 1;
    }
    fprintf(fb, "blockId,alg,startMonoNs,endMonoNs,startWallNs,iterations,failures\n");
    fprintf(fo, "blockId,tOffUs,op1Ns,op2Ns,op3Ns\n");

    signal(SIGINT, onSignal);
    signal(SIGTERM, onSignal);
    srand(seed);   /* seed is logged by the run script, so the order is repeatable */

    /* Print the setup so the run log records exactly what ran */
    printf("liboqs %s | %s, %s, %s | duration %.0f s, block %.1f s, seed %u\n",
           OQS_version(), kemName, dsaName, slhName, durationS, blockS, seed);
    fflush(stdout);

    uint64_t runStart = nowNs(CLOCK_MONOTONIC);
    uint64_t runEnd   = runStart + (uint64_t)(durationS * 1e9);
    uint64_t blockNs  = (uint64_t)(blockS * 1e9);
    long blockId = 0;
    int order[3] = {0, 1, 2};
    long totalFailures = 0;

    /* The time check is only made at the start of each cycle, so a run
     * always ends on a whole cycle and every algorithm gets the same number
     * of blocks (the run can overrun durationS by up to one cycle). */
    while (!stopRequested && nowNs(CLOCK_MONOTONIC) < runEnd) {
        /* Fisher-Yates shuffle: a new random order of the three each cycle */
        for (int i = 2; i > 0; i--) {
            int j = rand() % (i + 1);
            int tmp = order[i]; order[i] = order[j]; order[j] = tmp;
        }
        for (int k = 0; k < 3 && !stopRequested; k++) {
            Alg *a = &algs[order[k]];
            size_t n = 0;
            long failures = 0;
            uint64_t wallStart = nowNs(CLOCK_REALTIME);
            uint64_t bStart = nowNs(CLOCK_MONOTONIC);
            uint64_t bEnd = bStart + blockNs;
            uint64_t tNow = bStart;

            /* Whole iterations until the block's time is up. A block can
             * overrun by up to one iteration (~0.15 s for SLH-DSA); the real
             * end time is recorded, so this is visible in the data. */
            while (tNow < bEnd && n < cap) {
                offs[n] = (uint32_t)((tNow - bStart) / 1000);
                failures += runIteration(a, times[n]);
                n++;
                tNow = nowNs(CLOCK_MONOTONIC);
            }
            uint64_t bStop = tNow;

            /* ---- untimed: write the block to disk ---- */
            for (size_t i = 0; i < n; i++) {
                fprintf(fo, "%ld,%u,%u,%u,%u\n", blockId, offs[i],
                        times[i][0], times[i][1], times[i][2]);
                a->sumNs[0] += times[i][0];
                a->sumNs[1] += times[i][1];
                a->sumNs[2] += times[i][2];
            }
            a->count += n;
            fprintf(fb, "%ld,%s,%llu,%llu,%llu,%zu,%ld\n", blockId, a->name,
                    (unsigned long long)bStart, (unsigned long long)bStop,
                    (unsigned long long)wallStart, n, failures);
            /* flush + fsync each block so a crash or power cut keeps
             * everything up to the last finished block */
            fflush(fo); fsync(fileno(fo));
            fflush(fb); fsync(fileno(fb));

            totalFailures += failures;
            if (failures) {
                fprintf(stderr, "block %ld: %ld failed iterations in %s\n",
                        blockId, failures, a->name);
            }
            blockId++;
        }
    }

    /* Quick sanity summary (means only - real analysis uses medians from
     * the raw CSV). Compare these against speed_kem / speed_sig. */
    printf("stopped after %ld blocks, %.1f s%s\n", blockId,
           (nowNs(CLOCK_MONOTONIC) - runStart) / 1e9,
           stopRequested ? " (signal)" : "");
    for (int i = 0; i < 3; i++) {
        Alg *a = &algs[i];
        double c = a->count ? (double)a->count : 1.0;
        printf("%-26s n=%-9llu mean us: %10.1f %10.1f %10.1f\n", a->name,
               (unsigned long long)a->count,
               a->sumNs[0] / c / 1000, a->sumNs[1] / c / 1000, a->sumNs[2] / c / 1000);
    }
    printf("failures: %ld\n", totalFailures);

    fclose(fo); fclose(fb);
    free(times); free(offs);
    for (int i = 0; i < 3; i++) freeAlg(&algs[i]);
    OQS_destroy();
    return totalFailures ? 3 : 0;
}
