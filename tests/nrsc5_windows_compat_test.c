// Native behavior of the private nrsc5 Windows C adapters. No radio or socket.
#include <complex.h>
#include <pthread.h>
#include <math.h>
#include <stdio.h>

static int failures = 0;

static void check(int value, const char *message)
{
    if (!value) {
        ++failures;
        fprintf(stderr, "FAIL: %s\n", message);
    }
}

static int nearValue(float value, double expected)
{
    return isfinite(value) && fabs((double)value - expected) < 0.00001;
}

static void complexArithmetic(void)
{
    volatile float complex inputA = CMPLXF(3.0f, 4.0f);
    volatile float complex inputB = CMPLXF(-2.0f, 0.5f);
    const float complex a = inputA;
    const float complex b = inputB;
    const float complex product = a * b;
    const float complex quotient = a / b;
    check(nearValue(crealf(product), -8.0) && nearValue(cimagf(product), -6.5),
          "native C complex multiplication preserves both components");
    check(nearValue(crealf(quotient), -4.0 / 4.25) && nearValue(cimagf(quotient), -9.5 / 4.25),
          "native C complex division preserves both components");
    check(nearValue(cabsf(a), 5.0) && nearValue(cargf(a), atan2(4.0, 3.0)),
          "CRT magnitude and argument agree with independent real arithmetic");
    const float complex conjugate = conjf(a);
    check(nearValue(crealf(conjugate), 3.0) && nearValue(cimagf(conjugate), -4.0),
          "conjugation preserves real and negates imaginary components");
    const float complex exponential = cexpf(CMPLXF(0.3f, -0.6f));
    check(nearValue(crealf(exponential), exp(0.3) * cos(-0.6))
              && nearValue(cimagf(exponential), exp(0.3) * sin(-0.6)),
          "CRT exponential preserves amplitude and signed phase");
    const float complex imaginaryUnit = I * I;
    check(nearValue(crealf(imaginaryUnit), -1.0) && nearValue(cimagf(imaginaryUnit), 0.0),
          "imaginary unit participates in compiler complex arithmetic");
    const float complex zeros = conjf(CMPLXF(-0.0f, 0.0f));
    check(signbit(crealf(zeros)) && signbit(cimagf(zeros)),
          "component construction and conjugation preserve signed zero");
    const double pi = acos(-1.0);
    check(nearValue(cargf(CMPLXF(-1.0f, 0.0f)), pi)
              && nearValue(cargf(CMPLXF(-1.0f, -0.0f)), -pi),
          "CRT argument preserves the signed-zero branch boundary");
    const float complex independent = CMPLXF(INFINITY, 2.0f);
    check(isinf(crealf(independent)) && cimagf(independent) == 2.0f,
          "component construction does not contaminate the other component");
}

enum { kWorkers = 4, kIterations = 20000 };
static pthread_mutex_t plannerMutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t changed;
static int ready = 0;
static int start = 0;
static int count = 0;
static volatile LONG workerFailures = 0;

static void *contend(void *argument)
{
    (void)argument;
    if (pthread_mutex_lock(&plannerMutex) != 0) {
        InterlockedIncrement(&workerFailures);
        return NULL;
    }
    ++ready;
    pthread_cond_broadcast(&changed);
    while (!start) {
        if (pthread_cond_wait(&changed, &plannerMutex) != 0) {
            InterlockedIncrement(&workerFailures);
            pthread_mutex_unlock(&plannerMutex);
            return NULL;
        }
    }
    pthread_mutex_unlock(&plannerMutex);
    for (int iteration = 0; iteration < kIterations; ++iteration) {
        if (pthread_mutex_lock(&plannerMutex) != 0) {
            InterlockedIncrement(&workerFailures);
            return NULL;
        }
        ++count;
        if (pthread_mutex_unlock(&plannerMutex) != 0) {
            InterlockedIncrement(&workerFailures);
            return NULL;
        }
    }
    return NULL;
}

static int mutexAndCondition(void)
{
    pthread_t workers[kWorkers];
    check(pthread_cond_init(&changed, NULL) == 0, "condition initializes");
    for (int worker = 0; worker < kWorkers; ++worker) {
        if (pthread_create(&workers[worker], NULL, contend, NULL) != 0) {
            check(0, "native CRT worker starts");
            return 0;
        }
    }
    check(pthread_mutex_lock(&plannerMutex) == 0, "static planner mutex locks");
    while (ready != kWorkers) {
        check(pthread_cond_wait(&changed, &plannerMutex) == 0,
              "condition releases and reacquires the static mutex");
    }
    start = 1;
    check(pthread_cond_broadcast(&changed) == 0, "broadcast wakes waiting workers");
    check(pthread_mutex_unlock(&plannerMutex) == 0, "static planner mutex unlocks");
    for (int worker = 0; worker < kWorkers; ++worker) {
        check(pthread_join(workers[worker], NULL) == 0, "native CRT worker joins and closes");
    }
    check(workerFailures == 0 && count == kWorkers * kIterations,
          "contended static planner mutex protects every update");
    pthread_mutex_t initialized;
    check(pthread_mutex_init(&initialized, NULL) == 0
              && pthread_mutex_lock(&initialized) == 0
              && pthread_mutex_unlock(&initialized) == 0,
          "dynamically initialized mutex locks and unlocks");
    return 1;
}

int main(void)
{
    complexArithmetic();
    if (!mutexAndCondition()) {
        return 1;
    }
    printf("%s - %d failure(s), %d protected updates\n",
           failures ? "FAIL" : "ALL PASS", failures, count);
    return failures ? 1 : 0;
}
