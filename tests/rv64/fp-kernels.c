// Compiler-generated F/D code for function-model (FP=1) vs Spike: matrix product, Horner, Newton sqrt, int <-> float
// conversions, fma, comparisons and a float/double mix.  A wrong result makes the program return non-zero.
typedef unsigned long u64;
#define N 16
static inline double fma_d(double a, double b, double c) { double r; __asm__("fmadd.d %0, %1, %2, %3" : "=f"(r) : "f"(a), "f"(b), "f"(c)); return r; }
static double A[N][N], B[N][N], C[N][N];
static float fa[64], fb[64];
static volatile double sink;

static double horner(double x) {
  double r = 0.0;
  for (int i = 0; i < 12; i++) r = r * x + (double)(i + 1) / 7.0;
  return r;
}
static double newton(double v) {
  double x = v > 1.0 ? v / 2.0 : 1.0;
  for (int i = 0; i < 40; i++) x = 0.5 * (x + v / x);
  return x;
}

// start.S leaves mstatus.FS = Off and the compiler's prologue already saves FP registers: turn the FPU on before the C code
__asm__(".globl main\nmain:\n li t0, 0x2000\n csrs mstatus, t0\n j fp_main\n");
int fp_main(void) {
  for (int i = 0; i < N; i++)
    for (int j = 0; j < N; j++) { A[i][j] = (double)(i * N + j) * 0.25 - 7.0; B[i][j] = (double)((i ^ j) + 1) / 3.0; }
  for (int i = 0; i < N; i++)
    for (int j = 0; j < N; j++) {
      double s = 0.0;
      for (int k = 0; k < N; k++) s = fma_d(A[i][k], B[k][j], s);
      C[i][j] = s;
    }
  double tr = 0.0;
  for (int i = 0; i < N; i++) tr += C[i][i];
  for (int i = 0; i < 64; i++) { fa[i] = (float)i * 1.5f - 20.0f; fb[i] = (float)(63 - i) / 8.0f; }
  float fs = 0.0f;
  for (int i = 0; i < 64; i++) fs += fa[i] * fb[i] - (fa[i] < fb[i] ? fa[i] : fb[i]);
  double h = horner(0.9), q = newton(2.0), q2 = newton(1.0e6);
  long ci = (long)(tr * 3.0) + (int)fs + (long)(h * 1000.0);
  unsigned long cu = (unsigned long)(q * 1.0e9) + (unsigned int)(q2);
  double back = (double)ci + (double)cu + (float)ci;
  float nan = 0.0f / (fa[20] - fa[20] + 0.0f * 0.0f);   // 0 / 0 = NaN at run time
  int ok = (nan != nan) && (q > 1.41421 && q < 1.41422) && (q2 > 999.99 && q2 < 1000.01) && (back > 0.0 || back <= 0.0);
  sink = back + fs + h;
  return ok ? 0 : 1;
}
