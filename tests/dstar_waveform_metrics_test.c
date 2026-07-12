#include "dstar_waveform_metrics.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>

static int failures = 0;

static void expect_true(const char * name, BOOL condition)
{
    printf("%s %s\n", condition ? "[ OK ]" : "[FAIL]", name);
    if (!condition) {
        failures++;
    }
}

static void advance_timestamp(uint32 * seconds, uint64 * fraction, uint64 delta_ps)
{
    static const uint64 picoseconds_per_second = 1000000000000ULL;
    *fraction += delta_ps;
    while (*fraction >= picoseconds_per_second) {
        *fraction -= picoseconds_per_second;
        (*seconds)++;
    }
}

static dstar_waveform_metric_snapshot run_rate_window(
    uint64 interval_ps,
    uint32 missing_packets)
{
    dstar_waveform_metric_state state;
    dstar_waveform_metric_snapshot snapshot = {0};
    dstar_waveform_metrics_reset(&state);

    uint32 seconds = 100U;
    uint64 fraction = 0U;
    BOOL ready = dstar_waveform_metrics_record_packet(
        &state, 0x4A000000U, TRUE, seconds, fraction, 128U,
        0U, 10U, 5334U, 0U, &snapshot);
    expect_true("first packet establishes timing baseline", !ready);

    for (uint32 i = 0U; i < DSTAR_WAVEFORM_METRIC_REPORT_INTERVALS; i++) {
        advance_timestamp(&seconds, &fraction, interval_ps);
        ready = dstar_waveform_metrics_record_packet(
            &state, 0x4A000000U, TRUE, seconds, fraction, 128U,
            i == 20U ? missing_packets : 0U,
            10U + i % 4U, 5334U, i == 30U ? 3U : 0U, &snapshot);
    }
    expect_true("188 intervals produce one snapshot", ready);
    return snapshot;
}

int main(void)
{
    const dstar_waveform_metric_snapshot nominal =
        run_rate_window(5333333333ULL, 2U);
    expect_true("nominal cadence is 24 kHz",
                fabs(nominal.rx_sample_rate_hz - 24000.0) < 0.1);
    expect_true("VITA missing packet count is preserved",
                nominal.vita_sequence_gaps == 2U);
    expect_true("turnaround mean is aggregated",
                nominal.turnaround_mean_us > 10.0
                    && nominal.turnaround_mean_us < 14.0);
    expect_true("turnaround maximum is aggregated",
                nominal.turnaround_max_us == 13U);
    expect_true("queue maximum is aggregated", nominal.queue_max == 3U);

    const dstar_waveform_metric_snapshot degraded =
        run_rate_window(5450000000ULL, 0U);
    expect_true("slower cadence reports below 23.76 kHz",
                degraded.rx_sample_rate_hz < 23760.0);

    expect_true("nominal sync interval has no inferred deficit",
                dstar_waveform_metrics_infer_source_blocks(
                    10080U, 10080U, 128U, 2U) == 0U);
    expect_true("one missing 128-sample block is inferred",
                dstar_waveform_metrics_infer_source_blocks(
                    9952U, 10080U, 128U, 2U) == 1U);
    expect_true("two missing source blocks are inferred",
                dstar_waveform_metrics_infer_source_blocks(
                    9824U, 10080U, 128U, 2U) == 2U);
    expect_true("three missing source blocks are inferred",
                dstar_waveform_metrics_infer_source_blocks(
                    9696U, 10080U, 128U, 2U) == 3U);
    expect_true("PLL rounding within tolerance is accepted",
                dstar_waveform_metrics_infer_source_blocks(
                    9951U, 10080U, 128U, 2U) == 1U);
    expect_true("errors outside tolerance are rejected",
                dstar_waveform_metrics_infer_source_blocks(
                    9949U, 10080U, 128U, 2U) == 0U);
    expect_true("missed sync multiples are normalized",
                dstar_waveform_metrics_infer_source_blocks(
                    19904U, 10080U, 128U, 2U) == 2U);
    expect_true("late sync is not called a source deficit",
                dstar_waveform_metrics_infer_source_blocks(
                    10090U, 10080U, 128U, 2U) == 0U);

    dstar_waveform_metric_state source_state;
    dstar_waveform_metric_snapshot stream_snapshot = {0};
    dstar_waveform_metrics_reset(&source_state);
    dstar_waveform_metrics_record_packet(
        &source_state, 0x4A000000U, TRUE, 100U, 0U, 128U,
        0U, 10U, 5334U, 0U, &stream_snapshot);
    dstar_waveform_metrics_record_sync_interval(
        &source_state, 9824U, 10080U, 128U, 2U);
    expect_true("source deficits accumulate in the current window",
                source_state.source_block_deficits == 2U);

    const BOOL stream_ready = dstar_waveform_metrics_record_packet(
        &source_state, 0x4A000002U, TRUE, 200U, 0U, 128U,
        0U, 10U, 5334U, 0U, &stream_snapshot);
    expect_true("a replacement RX stream establishes a fresh baseline",
                !stream_ready && source_state.stream_id == 0x4A000002U
                    && source_state.source_block_deficits == 0U);

    dstar_waveform_metric_state tx_state;
    dstar_waveform_metric_snapshot tx_snapshot = {0};
    dstar_waveform_metrics_reset(&tx_state);
    dstar_waveform_metrics_record_packet(
        &tx_state, 0x81000000U, TRUE, 10U, 0U, 128U,
        1U, 0U, 5334U, 512U, &tx_snapshot);
    dstar_waveform_metrics_record_tx_activity(
        &tx_state, 3U, 2U, 1U, 4U, 700U);
    dstar_waveform_metrics_record_tail(&tx_state, 480U, 20000U);
    dstar_waveform_metrics_record_tx_pipeline(
        &tx_state, 19U, 390U, 20U, 1U, 2U, 3U,
        4U, 5U, 19U, 1U, 6U);
    expect_true("partial TX window is emitted at unkey",
                dstar_waveform_metrics_finish_window(
                    &tx_state, &tx_snapshot));
    expect_true("TX activity counters remain distinct",
                tx_snapshot.vita_sequence_gaps == 1U
                    && tx_snapshot.null_frames == 3U
                    && tx_snapshot.pcm_clips == 2U
                    && tx_snapshot.pcm_invalid == 1U
                    && tx_snapshot.send_failures == 4U);
    expect_true("TX queue and tail telemetry are preserved",
                tx_snapshot.queue_max == 700U
                    && tx_snapshot.tail_samples == 480U
                    && tx_snapshot.tail_us == 20000U);
    expect_true("TX pre-roll and drain telemetry are preserved",
                tx_snapshot.pre_roll_frames == 19U
                    && tx_snapshot.pre_roll_delay_ms == 390U
                    && tx_snapshot.ambe_queue_max == 20U
                    && tx_snapshot.ambe_underflows == 1U
                    && tx_snapshot.ambe_overflows == 2U
                    && tx_snapshot.ambe_sequence_errors == 3U
                    && tx_snapshot.vocoder_submit_failures == 4U
                    && tx_snapshot.vocoder_pending_max == 5U
                    && tx_snapshot.drain_frames == 19U
                    && tx_snapshot.drain_timeouts == 1U
                    && tx_snapshot.drain_discarded_frames == 6U);
    expect_true("finished TX window is not emitted twice",
                !dstar_waveform_metrics_finish_window(
                    &tx_state, &tx_snapshot));

    dstar_waveform_metric_state discontinuity_state;
    dstar_waveform_metric_snapshot discontinuity_snapshot = {0};
    dstar_waveform_metrics_reset(&discontinuity_state);
    dstar_waveform_metrics_record_packet(
        &discontinuity_state, 0x4A000000U, TRUE, 0U, 0U, 128U,
        0U, 10U, 5334U, 0U, &discontinuity_snapshot);
    const BOOL discontinuity_ready = dstar_waveform_metrics_record_packet(
        &discontinuity_state, 0x4A000000U, TRUE, UINT32_MAX, 0U, 128U,
        0U, 10U, 5334U, 0U, &discontinuity_snapshot);
    expect_true("an unrepresentable timestamp discontinuity is rejected",
                !discontinuity_ready && discontinuity_state.interval_count == 0U);

    if (failures == 0) {
        printf("\nAll D-STAR waveform metrics tests passed.\n");
    } else {
        printf("\n%d test(s) failed.\n", failures);
    }
    return failures == 0 ? 0 : 1;
}
