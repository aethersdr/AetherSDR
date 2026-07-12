#include "dstar_waveform_metrics.h"

#include <stdint.h>
#include <string.h>

static const uint64 DSTAR_VITA_PICOSECONDS_PER_SECOND = 1000000000000ULL;

static BOOL dstar_waveform_metrics_timestamp_delta(
    uint32 current_seconds,
    uint64 current_fraction,
    uint32 previous_seconds,
    uint64 previous_fraction,
    uint64 * delta_ps)
{
    if (delta_ps == NULL) {
        return FALSE;
    }

    const int64_t seconds_delta =
        (int64_t)current_seconds - (int64_t)previous_seconds;
    const int64_t fractional_delta =
        (int64_t)current_fraction - (int64_t)previous_fraction;
    if (seconds_delta > INT64_MAX / (int64_t)DSTAR_VITA_PICOSECONDS_PER_SECOND
        || seconds_delta < INT64_MIN / (int64_t)DSTAR_VITA_PICOSECONDS_PER_SECOND) {
        return FALSE;
    }

    int64_t signed_delta_ps =
        seconds_delta * (int64_t)DSTAR_VITA_PICOSECONDS_PER_SECOND;
    if ((fractional_delta > 0 && signed_delta_ps > INT64_MAX - fractional_delta)
        || (fractional_delta < 0 && signed_delta_ps < INT64_MIN - fractional_delta)) {
        return FALSE;
    }
    signed_delta_ps += fractional_delta;
    if (signed_delta_ps <= 0) {
        return FALSE;
    }

    *delta_ps = (uint64)signed_delta_ps;
    return TRUE;
}

void dstar_waveform_metrics_reset(dstar_waveform_metric_state * state)
{
    if (state != NULL) {
        memset(state, 0, sizeof(*state));
    }
}

uint32 dstar_waveform_metrics_infer_source_blocks(
    uint64 sync_interval_samples,
    uint64 expected_sync_interval_samples,
    uint32 source_block_samples,
    uint32 tolerance_samples)
{
    if (sync_interval_samples == 0U || expected_sync_interval_samples == 0U
        || source_block_samples == 0U) {
        return 0U;
    }

    uint64 sync_intervals =
        (sync_interval_samples + expected_sync_interval_samples / 2U)
        / expected_sync_interval_samples;
    if (sync_intervals == 0U) {
        sync_intervals = 1U;
    }
    if (sync_intervals > UINT64_MAX / expected_sync_interval_samples) {
        return 0U;
    }

    const uint64 expected_samples =
        sync_intervals * expected_sync_interval_samples;
    if (sync_interval_samples >= expected_samples) {
        return 0U;
    }

    const uint64 deficit_samples = expected_samples - sync_interval_samples;
    const uint64 inferred_blocks =
        (deficit_samples + source_block_samples / 2U) / source_block_samples;
    if (inferred_blocks == 0U || inferred_blocks > UINT32_MAX) {
        return 0U;
    }

    const uint64 represented_samples = inferred_blocks * source_block_samples;
    const uint64 error_samples = represented_samples > deficit_samples
        ? represented_samples - deficit_samples
        : deficit_samples - represented_samples;
    return error_samples <= tolerance_samples ? (uint32)inferred_blocks : 0U;
}

void dstar_waveform_metrics_record_sync_interval(
    dstar_waveform_metric_state * state,
    uint64 sync_interval_samples,
    uint64 expected_sync_interval_samples,
    uint32 source_block_samples,
    uint32 tolerance_samples)
{
    if (state == NULL) {
        return;
    }

    const uint32 blocks = dstar_waveform_metrics_infer_source_blocks(
        sync_interval_samples,
        expected_sync_interval_samples,
        source_block_samples,
        tolerance_samples);
    if (UINT32_MAX - state->source_block_deficits < blocks) {
        state->source_block_deficits = UINT32_MAX;
    } else {
        state->source_block_deficits += blocks;
    }
}

static void dstar_waveform_metrics_reset_window(
    dstar_waveform_metric_state * state)
{
    state->interval_count = 0U;
    state->vita_sequence_gaps = 0U;
    state->source_block_deficits = 0U;
    state->turnaround_over_budget = 0U;
    state->queue_max = 0U;
    state->delivered_samples = 0U;
    state->total_delta_ps = 0U;
    state->turnaround_total_us = 0U;
    state->turnaround_max_us = 0U;
    state->null_frames = 0U;
    state->pcm_clips = 0U;
    state->pcm_invalid = 0U;
    state->send_failures = 0U;
    state->tail_samples = 0U;
    state->tail_us = 0U;
    state->pre_roll_frames = 0U;
    state->pre_roll_delay_ms = 0U;
    state->ambe_queue_max = 0U;
    state->ambe_underflows = 0U;
    state->ambe_overflows = 0U;
    state->ambe_sequence_errors = 0U;
    state->vocoder_submit_failures = 0U;
    state->vocoder_pending_max = 0U;
    state->drain_frames = 0U;
    state->drain_timeouts = 0U;
    state->drain_discarded_frames = 0U;
}

static uint32 dstar_waveform_metrics_saturated_add(uint32 current,
                                                   uint32 increment)
{
    return UINT32_MAX - current < increment
        ? UINT32_MAX
        : current + increment;
}

BOOL dstar_waveform_metrics_finish_window(
    dstar_waveform_metric_state * state,
    dstar_waveform_metric_snapshot * snapshot)
{
    if (state == NULL || snapshot == NULL) {
        return FALSE;
    }
    if (state->interval_count == 0U
            && state->vita_sequence_gaps == 0U
            && state->source_block_deficits == 0U
            && state->null_frames == 0U
            && state->pcm_clips == 0U
            && state->pcm_invalid == 0U
            && state->send_failures == 0U
            && state->tail_samples == 0U
            && state->tail_us == 0U
            && state->pre_roll_frames == 0U
            && state->ambe_underflows == 0U
            && state->ambe_overflows == 0U
            && state->ambe_sequence_errors == 0U
            && state->vocoder_submit_failures == 0U
            && state->drain_timeouts == 0U
            && state->drain_discarded_frames == 0U) {
        return FALSE;
    }

    memset(snapshot, 0, sizeof(*snapshot));
    snapshot->interval_count = state->interval_count;
    snapshot->rx_sample_rate_hz = state->total_delta_ps == 0U
        ? 0.0
        : (double)state->delivered_samples
            * (double)DSTAR_VITA_PICOSECONDS_PER_SECOND
            / (double)state->total_delta_ps;
    snapshot->turnaround_mean_us = state->interval_count == 0U
        ? 0.0
        : (double)state->turnaround_total_us
            / (double)state->interval_count;
    snapshot->turnaround_max_us = state->turnaround_max_us;
    snapshot->vita_sequence_gaps = state->vita_sequence_gaps;
    snapshot->source_block_deficits = state->source_block_deficits;
    snapshot->turnaround_over_budget = state->turnaround_over_budget;
    snapshot->queue_max = state->queue_max;
    snapshot->null_frames = state->null_frames;
    snapshot->pcm_clips = state->pcm_clips;
    snapshot->pcm_invalid = state->pcm_invalid;
    snapshot->send_failures = state->send_failures;
    snapshot->tail_samples = state->tail_samples;
    snapshot->tail_us = state->tail_us;
    snapshot->pre_roll_frames = state->pre_roll_frames;
    snapshot->pre_roll_delay_ms = state->pre_roll_delay_ms;
    snapshot->ambe_queue_max = state->ambe_queue_max;
    snapshot->ambe_underflows = state->ambe_underflows;
    snapshot->ambe_overflows = state->ambe_overflows;
    snapshot->ambe_sequence_errors = state->ambe_sequence_errors;
    snapshot->vocoder_submit_failures = state->vocoder_submit_failures;
    snapshot->vocoder_pending_max = state->vocoder_pending_max;
    snapshot->drain_frames = state->drain_frames;
    snapshot->drain_timeouts = state->drain_timeouts;
    snapshot->drain_discarded_frames = state->drain_discarded_frames;

    dstar_waveform_metrics_reset_window(state);
    return TRUE;
}

void dstar_waveform_metrics_record_tx_activity(
    dstar_waveform_metric_state * state,
    uint32 null_frames,
    uint32 pcm_clips,
    uint32 pcm_invalid,
    uint32 send_failures,
    uint32 queue_depth)
{
    if (state == NULL) {
        return;
    }
    state->null_frames = dstar_waveform_metrics_saturated_add(
        state->null_frames, null_frames);
    state->pcm_clips = dstar_waveform_metrics_saturated_add(
        state->pcm_clips, pcm_clips);
    state->pcm_invalid = dstar_waveform_metrics_saturated_add(
        state->pcm_invalid, pcm_invalid);
    state->send_failures = dstar_waveform_metrics_saturated_add(
        state->send_failures, send_failures);
    if (queue_depth > state->queue_max) {
        state->queue_max = queue_depth;
    }
}

void dstar_waveform_metrics_record_tail(
    dstar_waveform_metric_state * state,
    uint32 tail_samples,
    uint64 tail_us)
{
    if (state == NULL) {
        return;
    }
    state->tail_samples = tail_samples;
    state->tail_us = tail_us;
}

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
    uint32 drain_discarded_frames)
{
    if (state == NULL) {
        return;
    }
    state->pre_roll_frames = pre_roll_frames;
    state->pre_roll_delay_ms = pre_roll_delay_ms;
    state->ambe_queue_max = ambe_queue_max;
    state->ambe_underflows = dstar_waveform_metrics_saturated_add(
        state->ambe_underflows, ambe_underflows);
    state->ambe_overflows = dstar_waveform_metrics_saturated_add(
        state->ambe_overflows, ambe_overflows);
    state->ambe_sequence_errors = dstar_waveform_metrics_saturated_add(
        state->ambe_sequence_errors, ambe_sequence_errors);
    state->vocoder_submit_failures = dstar_waveform_metrics_saturated_add(
        state->vocoder_submit_failures, vocoder_submit_failures);
    state->vocoder_pending_max = vocoder_pending_max;
    state->drain_frames = drain_frames;
    state->drain_timeouts = dstar_waveform_metrics_saturated_add(
        state->drain_timeouts, drain_timeouts);
    state->drain_discarded_frames = dstar_waveform_metrics_saturated_add(
        state->drain_discarded_frames, drain_discarded_frames);
}

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
    dstar_waveform_metric_snapshot * snapshot)
{
    if (state == NULL || snapshot == NULL || sample_count == 0U) {
        return FALSE;
    }

    if (!state->stream_selected) {
        state->stream_selected = TRUE;
        state->stream_id = stream_id;
    } else if (state->stream_id != stream_id) {
        dstar_waveform_metrics_reset(state);
        state->stream_selected = TRUE;
        state->stream_id = stream_id;
    }

    if (UINT32_MAX - state->vita_sequence_gaps < missing_packets) {
        state->vita_sequence_gaps = UINT32_MAX;
    } else {
        state->vita_sequence_gaps += missing_packets;
    }
    if (queue_depth > state->queue_max) {
        state->queue_max = queue_depth;
    }

    if (!timestamp_valid || timestamp_frac >= DSTAR_VITA_PICOSECONDS_PER_SECOND) {
        state->have_timestamp = FALSE;
        return FALSE;
    }

    if (!state->have_timestamp) {
        state->have_timestamp = TRUE;
        state->last_timestamp_int = timestamp_int;
        state->last_timestamp_frac = timestamp_frac;
        return FALSE;
    }

    uint64 delta_ps = 0U;
    const BOOL valid_delta = dstar_waveform_metrics_timestamp_delta(
        timestamp_int,
        timestamp_frac,
        state->last_timestamp_int,
        state->last_timestamp_frac,
        &delta_ps);
    state->last_timestamp_int = timestamp_int;
    state->last_timestamp_frac = timestamp_frac;
    if (!valid_delta) {
        return FALSE;
    }

    state->interval_count++;
    state->delivered_samples += sample_count;
    state->total_delta_ps += delta_ps;
    state->turnaround_total_us += turnaround_us;
    if (turnaround_us > state->turnaround_max_us) {
        state->turnaround_max_us = turnaround_us;
    }
    if (turnaround_us > turnaround_budget_us) {
        state->turnaround_over_budget++;
    }
    if (state->interval_count < DSTAR_WAVEFORM_METRIC_REPORT_INTERVALS) {
        return FALSE;
    }
    return dstar_waveform_metrics_finish_window(state, snapshot);
}
