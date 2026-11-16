#include "pd_capture.h"
#include <stdio.h>
int main(void){printf("{\"batch_bytes\":%zu,\"single_bytes\":%zu,\"payload_bytes\":%u,\"max_frames\":%u}\n",sizeof(struct pd_capture_batch),sizeof(struct pd_capture),8*PD_CAPTURE_SAMPLES*PD_CAPTURE_BATCH_MAX,PD_CAPTURE_BATCH_MAX);return 0;}
