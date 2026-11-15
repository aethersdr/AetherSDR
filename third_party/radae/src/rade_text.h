//==========================================================================
// Name:            rade_text.h
// Purpose:         Handles reliable text (callsign) in RADE EOO frame.
// Authors:         Mooneer Salem
// Source:          FreeDV-GUI src/pipeline/rade_text.h (BSD-2-Clause)
//==========================================================================

#ifndef RADE_TEXT_H
#define RADE_TEXT_H

#ifdef __cplusplus
extern "C"
{
#endif

    typedef void *rade_text_t;

    typedef void (*on_text_rx_t)(rade_text_t rt, const char *txt_ptr,
                                 int length, void *state);

    rade_text_t rade_text_create(void);
    void        rade_text_destroy(rade_text_t ptr);

    void rade_text_generate_tx_string(rade_text_t ptr, const char *str,
                                      int strlength, float *syms, int symSize);

    void rade_text_set_rx_callback(rade_text_t ptr, on_text_rx_t text_rx_fn,
                                   void *state);

    void rade_text_rx(rade_text_t ptr, float *syms, int symSize);

    void rade_text_enable_stats_output(rade_text_t ptr, int enable);

#ifdef __cplusplus
}
#endif

#endif /* RADE_TEXT_H */
