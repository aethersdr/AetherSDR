/* Bounded waveform-buffer queue regression test. */

#include "aether_buffer_queue.h"
#include "utils.h"

#include <stdio.h>

int main(void)
{
    lock_printf_init();
    lock_malloc_init();

    aether_buffer_queue queue;
    int failed = !aether_buffer_queue_init(&queue, 3U);
    for (uint64 id = 1U; id <= 5U && !failed; id++) {
        BufferDescriptor descriptor = hal_BufferRequest(1U, 1U);
        if (descriptor == NULL) {
            failed = 1;
            break;
        }
        descriptor->correlation_id = id;
        BOOL dropped = FALSE;
        (void)aether_buffer_queue_push(&queue, descriptor, &dropped);
        if ((id <= 3U && dropped) || (id > 3U && !dropped)) {
            failed = 1;
        }
    }

    failed += aether_buffer_queue_depth(&queue) != 3U;
    failed += aether_buffer_queue_dropped(&queue) != 2U;
    for (uint64 expected = 3U; expected <= 5U; expected++) {
        BufferDescriptor descriptor = aether_buffer_queue_pop(&queue);
        failed += descriptor == NULL
            || descriptor->correlation_id != expected;
        hal_BufferRelease(&descriptor);
    }
    failed += aether_buffer_queue_pop(&queue) != NULL;
    aether_buffer_queue_destroy(&queue);

    printf("%s bounded waveform buffer queue\n",
           failed == 0 ? "[ OK ]" : "[FAIL]");
    return failed == 0 ? 0 : 1;
}
