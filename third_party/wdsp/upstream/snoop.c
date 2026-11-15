
#include "comm.h"

typedef struct _snoop
{
	int ringtime;
	int rate;
	int size;
	int inptr;
	int outptr;
	double* inbuff;
	int ringsize;
	double* ring;
	int detect_mode;
	double* buff[2];
	int buff_select;
	unsigned int buff_count;
	unsigned int beat_time;
	unsigned int beat_count;
	unsigned int IQ_count;
	double old_I;
	double old_Q;
} snoop, *SNOOP;

SNOOP create_snoop(double* inbuff, int size, int rate, int ringtime)
{
	SNOOP a = (SNOOP)malloc0(sizeof(snoop));
	a->detect_mode = 1;
	a->size = size;
	a->rate = rate;
	a->ringtime = ringtime;
	a->inptr = 0;
	a->outptr = 0;
	a->inbuff = inbuff;
	a->ringsize = a->ringtime * a->rate;	// complex samples
	a->ring = (double*)malloc0(a->ringsize * sizeof(complex));
	a->buff[0] = (double*)malloc0(a->size * sizeof(complex));
	a->buff[1] = (double*)malloc0(a->size * sizeof(complex));
	a->buff_select = 0;
	a->buff_count = 0;
	a->beat_time = 10 * a->rate / a->size;	// 10 seconds
	a->beat_count = 0;
	a->IQ_count = 0;
	a->old_I = 0.0;
	a->old_Q = 0.0;
	return a;
}

void xsnoop(channel)
{
	static int created;
	static SNOOP a;
	if (!created)
	{
		created = 1;
		a = create_snoop(txa[channel].midbuff, ch[channel].dsp_size, ch[channel].dsp_rate, 10);
	}
	// Heart Beat
	int heartbeat = 0;
	if (a->beat_count++ >= a->beat_time)
	{
		heartbeat = 1;
		a->beat_count = 0;
	}

	// Detect Mode:  Detect Problem Buffers.
	if (a->detect_mode)
	{
		// check for repeated non-zero buffers
		int buff_repeat = 1;	// assume repeated; disprove
		int buffs_zero = 1;		// assume both buffs are zero; disprove
		memcpy(a->buff[a->buff_select], a->inbuff, a->size * sizeof(complex));
		a->buff_select = 1 - a->buff_select;
		if (a->buff_count >= 1)
		{
			for (int i = 0; i < 2 * a->size; i++)
			{
				// not a repeated buffer if any entry != previous
				if (a->buff[0][i] != a->buff[1][i]) buff_repeat = 0;
				// if either entry !=0.0, we're not comparing two zero buffers
				if ((a->buff[0][i] != 0.0) || (a->buff[1][i] != 0.0)) buffs_zero = 0;
			}
		}
		a->buff_count++;
		// check for NaN, INF, Subnormals, Over-range values
		int nan_count = 0;
		int inf_count = 0;
		int subnorm_count = 0;
		int overrange_count = 0;
		double overrange = 2.0;
		for (int i = 0; i < 2 * a->size; i++)
		{
			if (isnan(a->inbuff[i])) nan_count++;
			if (isinf(a->inbuff[i])) inf_count++;
			if (fpclassify(a->inbuff[i]) == FP_SUBNORMAL) subnorm_count++;
			if (fabs(a->inbuff[i]) > overrange) overrange_count++;
		}
		// check for magnitude discontinuities
		double discontinuity = 0.50;
		int IQ_discontinuity = 0;
		for (int i = 0; i < a->size; i++)
		{
			double I = a->inbuff[2 * i + 0];
			double Q = a->inbuff[2 * i + 1];
			if ((a->IQ_count >= 1) 
				&& ((fabs(a->old_I - I) > discontinuity)
					|| (fabs(a->old_Q - Q) > discontinuity)))
				IQ_discontinuity = 1;
			a->old_I = I;
			a->old_Q = Q;
			a->IQ_count++;
		}
		// deliver results
		if (heartbeat)
			dprintf("***** Snoop Heartbeat! *****\n");
		if (!buffs_zero && buff_repeat)
			dprintf("***** Repeated Non-Zero Buffer Detected *****\n");
		if (nan_count > 0) 
			dprintf("***** NaN Detected *****, Count = %d\n", nan_count);
		if (inf_count > 0)
			dprintf("***** Infinite Value Detected *****, Count = %d\n", inf_count);
		if (subnorm_count > 0)
			dprintf("***** Subnormal Value Detected *****, Count = %d\n", subnorm_count);
		if (overrange_count > 0)
			dprintf("***** Overrange Value Detected *****, Count = %d\n", overrange_count);
		if (IQ_discontinuity)
			dprintf("***** I-Q Discontinuity Detected *****\n");
	}
}