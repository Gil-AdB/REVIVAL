// M2 Max IPC / memory-latency calibration ladder.
// Establishes what IPC *means* on this box, so a render stage's measured IPC
// can be read against known compute-bound and latency-bound anchors.
// No PMU cache counters are reachable without root; this is the substitute.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <dlfcn.h>
#include <time.h>

typedef int (*tsc_t)(int, void *, size_t);
static tsc_t F;
static void rd(uint64_t *i, uint64_t *c) { uint64_t b[8] = {0}; F(1, b, sizeof b); *i = b[0]; *c = b[1]; }
static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec / 1e9; }
static void rep(const char *n, uint64_t di, uint64_t dc, double dt, double ops) {
	printf("%-30s IPC=%5.2f  cyc/access=%7.2f  instr/access=%5.2f  %6.1f ms\n",
	       n, (double)di / dc, (double)dc / ops, (double)di / ops, dt * 1e3);
}

// Pointer-chase over `bytes`, one 128B cache line per hop, random order.
// Serialised dependent loads => pure load-latency, zero MLP. The low-IPC anchor.
static void chase(const char *label, size_t bytes) {
	size_t n = bytes / 128;
	if (n < 2) return;
	void **a = NULL;
	size_t alloc = n * 128;
	if (posix_memalign((void **)&a, 16384, alloc) != 0 || !a) { printf("%-30s alloc fail\n", label); return; }
	memset(a, 0, alloc);
	size_t *perm = malloc(n * sizeof(size_t));
	for (size_t k = 0; k < n; k++) perm[k] = k;
	for (size_t k = n - 1; k > 0; k--) {
		size_t j = (((size_t)rand() << 15) ^ (size_t)rand()) % (k + 1);
		size_t t = perm[k]; perm[k] = perm[j]; perm[j] = t;
	}
	for (size_t k = 0; k < n; k++) a[perm[k] * 16] = (void *)&a[perm[(k + 1) % n] * 16];
	long IT = 8000000;
	void *p = (void *)&a[perm[0] * 16];
	uint64_t i0, c0, i1, c1;
	double t0 = now(); rd(&i0, &c0);
	for (long k = 0; k < IT; k++) p = *(void **)p;
	__asm__ __volatile__("" :: "r"(p));   // anti-DCE: p must materialise
	rd(&i1, &c1); double dt = now() - t0;
	rep(label, i1 - i0, c1 - c0, dt, (double)IT);
	printf("%30s   -> %.2f ns/hop\n", "", dt * 1e9 / IT);
	free(a); free(perm);
}

int main(void) {
	F = (tsc_t)dlsym(RTLD_DEFAULT, "thread_selfcounts");
	if (!F) { printf("thread_selfcounts unavailable\n"); return 1; }
	setvbuf(stdout, NULL, _IONBF, 0);

	// High-IPC anchor: 8 independent FP chains, no memory traffic at all.
	{
		double acc[8]; for (int j = 0; j < 8; j++) acc[j] = 1.0 + j;
		long IT = 20000000;
		uint64_t i0, c0, i1, c1;
		double t0 = now(); rd(&i0, &c0);
		double a0=acc[0],a1=acc[1],a2=acc[2],a3=acc[3],a4=acc[4],a5=acc[5],a6=acc[6],a7=acc[7];
		for (long k = 0; k < IT; k++) {
			a0=a0*1.0000001+0.5; a1=a1*1.0000001+0.5; a2=a2*1.0000001+0.5; a3=a3*1.0000001+0.5;
			a4=a4*1.0000001+0.5; a5=a5*1.0000001+0.5; a6=a6*1.0000001+0.5; a7=a7*1.0000001+0.5;
		}
		acc[0]=a0+a1+a2+a3+a4+a5+a6+a7;
		__asm__ __volatile__("" :: "r"(acc) : "memory");
		rd(&i1, &c1);
		rep("COMPUTE 8 indep FP chains", i1 - i0, c1 - c0, now() - t0, (double)IT * 8);
		printf("%30s   -> acc[0]=%g (anti-DCE)\n", "", acc[0]);
	}
	chase("L1  chase   24 KB", 24u * 1024);
	chase("L2  chase    1 MB", 1u << 20);
	chase("SLC chase   16 MB", 16u << 20);
	chase("DRAM chase 512 MB", 512u << 20);
	return 0;
}
