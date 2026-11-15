#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <math.h>

#include "rade_bpf.h"
#include "rade_dsp.h"
#include "_kiss_fft_guts.h"

#define FS 8000
#define BANDWIDTH_HZ 1500
#define CENTER_FREQ_HZ 1500

int main(int argc, char** argv)
{
    if (argc < 3 || argc > 4)
    {
        fprintf(stderr, "Usage: %s [iterations] [samples/iteration] [output.f32]\n", argv[0]);
        return -1;
    }

    int iterations = atoi(argv[1]);
    int samplesPerIteration = atoi(argv[2]);

    FILE* outfp = NULL;
    if (argc == 4)
    {
        outfp = fopen(argv[3], "wb");
        if (!outfp)
        {
            fprintf(stderr, "Warning: could not open %s for writing\n", argv[3]);
        }
    }

    int totalSamples = iterations * samplesPerIteration;

    // Initialize items needed for test
    rade_bpf bpf;
    rade_bpf_init(&bpf, 101, FS, BANDWIDTH_HZ, CENTER_FREQ_HZ, samplesPerIteration);

    RADE_COMP* input = calloc(sizeof(RADE_COMP), totalSamples);
    assert(input != NULL);

    RADE_COMP* output = calloc(sizeof(RADE_COMP), totalSamples);
    assert(output != NULL);
   
    RADE_COMP* fftoutput = calloc(sizeof(RADE_COMP), totalSamples);
    assert(fftoutput != NULL);
   
    kiss_fft_cfg fftcfg = kiss_fft_alloc(totalSamples, 0, NULL, NULL);
    assert(fftcfg != NULL);
 
    // Generate sine wave at the center frequency
    for (int i = 0; i < (totalSamples); i++)
    {
        input[i].real = cos(2 * M_PI * CENTER_FREQ_HZ * i / FS);
    }

    // Execute BPF
    for (int i = 0; i < iterations; i++)
    {
        rade_bpf_process(&bpf, &output[i * samplesPerIteration], &input[i * samplesPerIteration], samplesPerIteration);
    }

    if (outfp)
    {
        fwrite(output, sizeof(RADE_COMP), totalSamples, outfp);
        fclose(outfp);
    }

    // Analyze result:
    //   1. Multiply each element of output by the Hanning window.
    //   2. Execute FFT on result of (1).
    //   3. Take square of the magnitude of each element of (2).
    //   4. Add positive and negative powers and ensure positive is > 40dB above negative.
    for (int i = 0; i < totalSamples; i++)
    {
        float hanning = 0.5f - 0.5f * cosf((2.0f * M_PI * i) / (totalSamples - 1));
        output[i] = rade_cscale(output[i], hanning);
    }

    kiss_fft(fftcfg, (kiss_fft_cpx*)output, (kiss_fft_cpx*)fftoutput);

    float positivePower = 0.0f;
    float negativePower = 0.0f;
    for (int i = 0; i < totalSamples; i++)
    {
        float magnitudeSquared  = rade_cabs2(fftoutput[i]);
        if (i < (totalSamples / 2))
        {
            positivePower += magnitudeSquared;
        }
        else
        {
            negativePower += magnitudeSquared;
        }
    }
    
    kiss_fft_free(fftcfg); 
    free(input);
    free(output);
    free(fftoutput);

    if (10.0f*log10f(positivePower/negativePower) > 40.0f)
    {
        printf("PASS\n");
        return 0;
    }
    else
    {
        printf("FAIL (pos = %f, neg = %f)\n", positivePower, negativePower);
        return -1;
    }
}
