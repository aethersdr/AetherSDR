#ifndef DSTAR_WAVEFORM_METRICS_H_
#define DSTAR_WAVEFORM_METRICS_H_

#include "datatypes.h"

#define DSTAR_WAVEFORM_METRIC_REPORT_INTERVALS 188U

typedef struct _dstar_waveform_metric_snapshot {
    double rx_sample_rate_hz;
    double turnaround_mean_us;
    uint64 turnaround_max_us;
    uint32 vita_sequence_gaps;
    uint32 source_block_deficits;
    uint32 turnaround_over_budget;
    uint32 queue_max;
    uint32 interval_count;
    uint32 null_frames;
    uint32 pcm_clips;
    uint32 pcm_invalid;
    uint32 send_failures;
    uint32 tail_samples;
    uint64 tail_us;
    uint32 pre_roll_frames;
    uint32 pre_roll_delay_ms;
    uint32 ambe_queue_max;
    uint32 ambe_underflows;
    uint32 ambe_overflows;
    uint32 ambe_sequence_errors;
    uint32 vocoder_submit_failures;
    uint32 vocoder_pending_max;
    uint32 drain_frames;
    uint32 drain_timeouts;
    uint32 drain_discarded_frames;
} dstar_waveform_metric_snapshot;

typedef struct _dstar_waveform_metric_state {
    BOOL stream_selected;
    BOOL have_timestamp;
    uint32 stream_id;
    uint32 last_timestamp_int;
    uint64 last_timestamp_frac;
    uint32 interval_count;
    uint32 vita_sequence_gaps;
    uint32 source_block_deficits;
    uint32 turnaround_over_budget;
    uint32 queue_max;
    uint64 delivered_samples;
    uint64 total_delta_ps;
    uint64 turnaround_total_us;
    uint64 turnaround_max_us;
    uint32 null_frames;
    uint32 pcm_clips;
    uint32 pcm_invalid;
    uint32 send_failures;
    uint32 tail_samples;
    uint64 tail_us;
    uint32 pre_roll_frames;
    uint32 pre_roll_delay_ms;
    uint32 ambe_queue_max;
    uint32 ambe_underflows;
    uint32 ambe_overflows;
    uint32 ambe_sequence_errors;
    uint32 vocoder_submit_failures;
    uint32 vocoder_pending_max;
    uint32 drain_frames;
    uint32 drain_timeouts;
    uint32 drain_discarded_frames;
} dstar_waveform_metric_state;

void dstar_waveform_metrics_reset(dstar_waveform_metric_state * state);

uint32 dstar_waveform_metrics_infer_source_blocks(
    uint64 sync_interval_samples,
    uint64 expected_sync_interval_samples,
    uint32 source_block_samples,
    uint32 tolerance_samples);

void dstar_waveform_metrics_record_sync_interval(
    dstar_waveform_metric_state * state,
    uint64 sync_interval_samples,
    uint64 expected_sync_interval_samples,
    uint32 source_block_samples,
    uint32 tolerance_samples);

BOOL dstar_waveform_metrics_record_packet(
    dstar_waveform_metric_state * state,
    uint32 stream_id,
    BOOL timestamp_valid,
    uint32 timestamp_int,
    uint64 timestamp_frac,
    uint32 sample_count,
    uint32 missing_packets,
    uint64 turnaround_us,
    uint64 turnaround_budget_us,
    uint32 queue_depth,
    dstar_waveform_metric_snapshot * snapshot);

void dstar_waveform_metrics_record_tx_activity(
    dstar_waveform_metric_state * state,
    uint32 null_frames,
    uint32 pcm_clips,
    uint32 pcm_invalid,
    uint32 send_failures,
    uint32 queue_depth);
void dstar_waveform_metrics_record_tail(
    dstar_waveform_metric_state * state,
    uint32 tail_samples,
    uint64 tail_us);
void dstar_waveform_metrics_record_tx_pipeline(
    dstar_waveform_metric_state * state,
    uint32 pre_roll_frames,
    uint32 pre_roll_delay_ms,
    uint32 ambe_queue_max,
    uint32 ambe_underflows,
    uint32 ambe_overflows,
    uint32 ambe_sequence_errors,
    uint32 vocoder_submit_failures,
    uint32 vocoder_pending_max,
    uint32 drain_frames,
    uint32 drain_timeouts,
    uint32 drain_discarded_frames);
BOOL dstar_waveform_metrics_finish_window(
    dstar_waveform_metric_state * state,
    dstar_waveform_metric_snapshot * snapshot);

#endif /* DSTAR_WAVEFORM_METRICS_H_ */
