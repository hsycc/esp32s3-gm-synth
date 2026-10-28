#!/usr/bin/env python3
"""Reject incomplete concurrent application logs; keep timing scope explicit."""
import argparse
import json
from pathlib import Path
import re

from tools.v1_loop_realtime_manifest import PROFILES, PROFILES_V2, OUTPUT_MODES

FATAL = re.compile(r'REALTIME_FAIL|Guru Meditation|(?:assert|Assertion)(?:ion)?[^\n]*failed|abort\(\)|panic.ed|task_wdt:')
ANSI = re.compile(r'\x1b\[[0-?]*[ -/]*[@-~]')
HOST_CASES = {
    'default_off', 'continuous_real_provider', 'missing_program_keeps_audio',
    'residency_failure_keeps_audio', 'stop_ready_before_commit',
    'stop_committed_before_ack', 'stop_during_real_analysis',
    'cancelled_analysis_direct_fallback', 'storage_exclusive_then_reusable',
    'real_fm_cohort_headroom_rejected',
}
OUTPUT_CASES = {
    'exact_bypass_and_master', 'delay_identity_and_drain',
    'int32_extremes_invalid_calls', 'stereo_linked_smooth_attack_release',
    'unchanged_real_fm_matrix_deterministic', 'multichannel_musical_replay_deterministic',
    'two_channel_concurrent_prepare_stop',
}
UNITY_Q30 = 1 << 30


def replay_events(initial, replacement, first=0, second=0, cleanup=0):
    """Independent canonical decoder for the published two-channel score.

    These explicit tuples intentionally bind the evidence checker to the score
    contract, not just to a target's self-reported event count or hash.
    """
    on, off, cc, pc, pitch, pressure = range(6)
    score = [
        (0, cc, 0, 120, 0), (0, cc, 1, 120, 0),
        (0, cc, 0, 64, 0), (0, cc, 1, 64, 0),
        (0, cc, 0, 91, 40), (0, cc, 1, 91, 40),
        (0, cc, 0, 93, 20), (0, cc, 1, 93, 20),
        (0, cc, 0, 7, 100), (0, cc, 1, 7, 100),
        (0, cc, 0, 11, 127), (0, cc, 1, 11, 127),
        (0, cc, 0, 10, 40), (0, cc, 1, 10, 90),
        (0, cc, 0, 21, 0), (0, cc, 1, 21, 0),
        (0, pitch, 0, 0, 0, 8192), (0, pitch, 1, 0, 0, 8192),
        (0, pressure, 0, 0, 0), (0, pressure, 1, 0, 0),
        (0, on, 0, 48, 64), (0, on, 0, 55, 64), (0, on, 0, 60, 64),
        (0, on, 1, 60, 72), (5519, off, 1, 60, 0),
        (11025, on, 1, 64, 76), (16548, off, 1, 64, 0),
        (22050, cc, 0, 64, 127), (22050, off, 0, 48, 0),
        (22050, off, 0, 55, 0), (22050, off, 0, 60, 0),
        (22050, on, 1, 67, 80), (27565, pitch, 1, 0, 0, 10240),
        (33075, off, 1, 67, 0), (33075, on, 1, 69, 76),
        (38587, pitch, 1, 0, 0, 8192), (44100, cc, 0, 64, 0),
        (44100, off, 1, 69, 0), (44100, on, 0, 53, 64),
        (44100, on, 0, 60, 64), (44100, on, 0, 65, 64),
        (44100, on, 1, 65, 72), (49617, pressure, 1, 80, 0),
        (55125, off, 1, 65, 0), (55125, on, 1, 64, 76),
        (60646, cc, 1, 11, 96), (66150, off, 1, 64, 0),
        (66150, on, 1, 62, 72), (71662, cc, 1, 21, 64),
        (77175, off, 1, 62, 0), (77175, off, 0, 53, 0),
        (77175, off, 0, 60, 0), (77175, off, 0, 65, 0),
        (80000, cc, 1, 11, 127), (82750, cc, 1, 21, 0),
        (82750, on, 0, 60, 64),
    ]
    rows = []
    for index, base in enumerate((first, second)):
        for item in score:
            frame, kind, channel, a, b, *value = item
            rows.append((base + frame, kind, channel, a + 2 * index if kind in (on, off) else a,
                         b, value[0] if value else 0))
        rows.append((base + 88200, pc, 1, initial if index else replacement, 0, 0))
    rows.extend([(cleanup, cc, 0, 64, 0, 0), (cleanup, cc, 1, 64, 0, 0),
                 (cleanup, off, 0, 62, 0, 0)])
    return rows


def replay_hash(initial, replacement, first=0, second=0, cleanup=0):
    result = 14695981039346656037
    for frame, kind, channel, a, b, value in replay_events(initial, replacement, first, second, cleanup):
        data = frame.to_bytes(8, 'little') + bytes((kind, channel, a, b)) + value.to_bytes(2, 'little')
        for byte in data:
            result = ((result ^ byte) * 1099511628211) & ((1 << 64) - 1)
    return result


def one_line(text, tag):
    matches = re.findall(r'^' + re.escape(tag) + r'(?: (.*))?$', text, re.M)
    if len(matches) != 1:
        raise ValueError('missing or duplicate evidence: ' + tag)
    return matches[0]


def fields(text, tag):
    return parse_fields(one_line(text, tag), tag)


def parse_fields(line, tag):
    result = {}
    for token in line.split():
        pair = token.split('=', 1)
        if len(pair) != 2 or pair[0] in result:
            raise ValueError('invalid or duplicate field: ' + tag)
        result[pair[0]] = pair[1]
    return result


def numbers(row, *keys):
    result = []
    for key in keys:
        if not re.fullmatch(r'\d+', row.get(key, '')):
            raise ValueError('missing/invalid numeric field: ' + key)
        result.append(int(row[key]))
    return result


def validate_host(text):
    if FATAL.search(text):
        raise ValueError('fatal native marker')
    summary = re.fullmatch(r'suites=(\d+) checks=(\d+) PASS actor_ownership=1',
                           one_line(text, 'REALTIME_HOST_RESULT'))
    cases = re.findall(r'^REALTIME_HOST_CASE (\w+)(?: .*?)? PASS(?: physical_timing_approved=0)?$', text, re.M)
    if (summary is None or int(summary[1]) != len(cases) or int(summary[2]) == 0 or
            len(set(cases)) != len(cases) or not HOST_CASES.issubset(cases)):
        raise ValueError('native owner/stop/concurrency coverage missing')
    bank_rows = re.findall(r'^REALTIME_BANK_RESULT (.+)$', text, re.M)
    if len(bank_rows) != 2:
        raise ValueError('missing paired real-bank negative headroom evidence')
    bank_results = []
    for index, line in enumerate(bank_rows):
        row = parse_fields(line, 'REALTIME_BANK_RESULT')
        voices, loops, clipped, headroom, timing = numbers(
            row, 'voices', 'loops', 'clipped', 'headroom_pass', 'physical_timing_approved')
        if (row.get('mode') != ('off', 'on')[index] or voices != 24 or loops != index * 24 or
                clipped == 0 or headroom or timing):
            raise ValueError('real-bank failing headroom cohort was hidden or mislabeled')
        bank_results.append(dict(mode=row['mode'], voices=voices, loops=loops, clipped=clipped,
                                 headroom_pass=False, physical_timing_approved=False))
    return dict(suites=int(summary[1]), checks=int(summary[2]),
                known_headroom_failure=bank_results)


def validate_output_host(text):
    if FATAL.search(text) or 'OUTPUT_HOST_FAIL' in text:
        raise ValueError('fatal output native marker')
    summary = fields(text, 'OUTPUT_HOST_PASS')
    checks, count, physical, listening = numbers(
        summary, 'checks', 'cases', 'physical_timing_approved', 'listening_approved')
    cases = re.findall(r'^OUTPUT_HOST_CASE (\w+) PASS(?: physical_timing_approved=0)?(?: listening_approved=0)?$', text, re.M)
    if (checks == 0 or count != len(cases) or len(set(cases)) != count or
            not OUTPUT_CASES.issubset(cases) or physical or listening):
        raise ValueError('output safety/determinism/musical coverage missing')
    rows = re.findall(r'^OUTPUT_BANK_RESULT (.+)$', text, re.M)
    if len(rows) != 4:
        raise ValueError('four real-bank Loop/output combinations required')
    results = {}
    for line in rows:
        row = parse_fields(line, 'OUTPUT_BANK_RESULT')
        key = (row.get('loop'), row.get('output'))
        if key in results or key[0] not in ('off', 'on') or key[1] not in OUTPUT_MODES:
            raise ValueError('duplicate or unknown bank combination')
        voices, loops, events, cohort_raw, raw, raw_peak, post, post_peak, frames, emitted, latency, gain, limited, checksum, deterministic, physical, listening = numbers(
            row, 'voices', 'loops', 'events', 'cohort_raw_overload', 'raw_overload_samples', 'raw_peak_abs',
            'post_hard_clipped', 'post_peak_abs', 'input_frames', 'output_frames', 'latency_frames',
            'gain_min_q30', 'limited_frames', 'checksum', 'deterministic',
            'physical_timing_approved', 'listening_approved')
        enabled = key[0] == 'on'
        protected = key[1] == 'peak'
        if (voices != 24 or loops != 24 * enabled or events != 68 or
                cohort_raw != (4445 if enabled else 9915) or raw != (4479 if enabled else 9950) or
                raw_peak <= 32767 or frames == 0 or latency != 64 * protected or
                emitted != frames + latency or not 0 < gain <= UNITY_Q30 or
                not 0 <= limited <= frames or not 0 <= checksum < 1 << 64 or
                deterministic != 1 or physical or listening):
            raise ValueError('original FM cohort/raw overload or output frame accounting changed')
        if protected:
            if post or post_peak > 32767 or gain == UNITY_Q30 or limited == 0:
                raise ValueError('peak candidate did not protect the actual overloaded bank render')
        elif post != raw or post_peak != raw_peak or gain != UNITY_Q30 or limited:
            raise ValueError('bypass no longer preserves the original failing output')
        results[key] = row
    for loop in ('off', 'on'):
        for field in ('raw_overload_samples', 'raw_peak_abs', 'cohort_raw_overload', 'input_frames'):
            if results[(loop, 'bypass')][field] != results[(loop, 'peak')][field]:
                raise ValueError('peak candidate altered source workload or raw overload evidence')
    music = {}
    for line in re.findall(r'^OUTPUT_REPLAY_RESULT (.+)$', text, re.M):
        row = parse_fields(line, 'OUTPUT_REPLAY_RESULT')
        key = (row.get('loop'), row.get('output'))
        if key in music or key not in results:
            raise ValueError('duplicate or unknown musical output combination')
        values = {k: numbers(row, k)[0] for k in (
            'configured_channels', 'channel_mask', 'loop_channel_mask', 'events', 'program_switches',
            'voices', 'loops', 'channel0_voices', 'channel1_voices', 'channel0_loops', 'channel1_loops',
            'definition_hash', 'trace_hash', 'raw_overload_samples', 'raw_peak_abs', 'post_hard_clipped',
            'post_peak_abs', 'input_frames', 'output_frames', 'latency_frames', 'gain_min_q30',
            'limited_frames', 'checksum', 'deterministic', 'physical_timing_approved', 'listening_approved')}
        enabled, protected = key[0] == 'on', key[1] == 'peak'
        if (values['configured_channels'] != 2 or values['channel_mask'] != 3 or
                values['loop_channel_mask'] != 3 * enabled or values['events'] != 117 or
                values['program_switches'] != 2 or not 2 <= values['voices'] <= 32 or
                not 0 <= values['loops'] <= values['voices'] or
                values['channel0_voices'] < 3 or values['channel1_voices'] < 1 or
                any(not 0 <= values[f'channel{c}_loops'] <= values[f'channel{c}_voices'] for c in (0, 1)) or
                (enabled and min(values['channel0_loops'], values['channel1_loops']) == 0) or
                (not enabled and (values['loops'] or values['channel0_loops'] or values['channel1_loops'])) or
                values['definition_hash'] != replay_hash(16, 1) or
                values['trace_hash'] != replay_hash(16, 1, 1024, 90496, 179904) or
                values['input_frames'] != 245440 or values['latency_frames'] != 64 * protected or
                values['output_frames'] != values['input_frames'] + values['latency_frames'] or
                values['raw_overload_samples'] or values['post_hard_clipped'] or
                not 0 < values['post_peak_abs'] == values['raw_peak_abs'] <= 32767 or
                values['gain_min_q30'] != UNITY_Q30 or values['limited_frames'] or
                not 0 <= values['checksum'] < 1 << 64 or values['deterministic'] != 1 or
                values['physical_timing_approved'] or values['listening_approved']):
            raise ValueError('musical render/channel isolation/independent output evidence changed')
        music[key] = dict(loop=key[0], output=key[1], **values)
    if set(music) != set(results):
        raise ValueError('four musical Loop/output combinations required')
    for loop in ('off', 'on'):
        if music[(loop, 'bypass')]['raw_peak_abs'] != music[(loop, 'peak')]['raw_peak_abs']:
            raise ValueError('musical raw waveform changed between output modes')
    concurrent = fields(text, 'OUTPUT_CONCURRENT_RESULT')
    channels, overlap, unacked, released, stale, physical = numbers(
        concurrent, 'configured_channels', 'audio_blocks_during_prepare', 'stop_unacked',
        'released', 'stale_refs', 'physical_timing_approved')
    if channels != 2 or overlap < 152 or unacked != 1 or released != 1 or stale or physical:
        raise ValueError('two-channel concurrent prepare/stop evidence missing')
    return dict(cases=count, checks=checks, bank_matrix=list(results.values()),
                musical_matrix=list(music.values()), concurrent_prepare=concurrent,
                physical_timing_approved=False, listening_approved=False)


def validate_output_target(text, spec, frames, clipped, mode):
    row = fields(text, 'REALTIME_OUTPUT')
    raw_peak, raw, post_peak, post, limited, gain, inputs, outputs, latency, drained, drain_us = numbers(
        row, 'raw_peak_abs', 'raw_overload_samples', 'post_peak_abs', 'post_hard_clipped',
        'limited_frames', 'gain_min_q30', 'input_frames', 'output_frames', 'latency_frames',
        'drain_frames', 'drain_wall_us')
    if (row.get('mode') != mode or inputs != frames or latency != (64 if mode == 'peak' else 0) or
            outputs != inputs + latency or drained != latency or post != clipped or
            not 0 <= raw <= inputs * 2 or not 0 <= post <= outputs * 2 or
            not 0 <= post_peak <= raw_peak <= (1 << 31) or
            not 0 <= limited <= inputs or not 0 < gain <= UNITY_Q30):
        raise ValueError('output mode, independent metrics or drain accounting mismatch')
    if mode == 'peak':
        if post or post_peak > 32767 or (raw and (limited == 0 or gain == UNITY_Q30)):
            raise ValueError('peak output safety gate failed')
    elif post != raw or post_peak != raw_peak or limited or gain != UNITY_Q30 or drain_us:
        raise ValueError('bypass output or diagnostics were changed')
    if spec['physical'] and raw == 0:
        raise ValueError('fixed physical overloaded workload was not observed')
    return dict(mode=mode, **{k: int(v) for k, v in row.items() if k != 'mode'})


def validate_replay(text, spec, frames, analysis):
    row = fields(text, 'REALTIME_REPLAY')
    count, definition, checksum, first, second, cleanup, end, switches, channels, loops, concurrent, preparing, complete, voices_peak, loops_peak = numbers(
        row, 'events', 'definition_hash', 'event_checksum', 'first_frame', 'second_frame',
        'cleanup_frame', 'end_frame', 'program_switches', 'channel_mask', 'loop_channel_mask',
        'concurrent_blocks', 'prepare_with_other_loop', 'complete', 'voices_peak', 'loop_voices_peak')
    if (row.get('name') != 'two-channel-phrase-v1' or count != 117 or
            definition != replay_hash(spec['initial_program'], spec['replacement_program']) or
            not 0 <= first < first + 88200 < second < second + 88200 < cleanup < end <= frames or
            end != cleanup + 65536 or any(at % 64 for at in (first, second, cleanup)) or
            switches != 2 or channels != 3 or loops != 3 * analysis or concurrent == 0 or
            (analysis and preparing == 0) or (not analysis and preparing) or complete != 1 or
            not 2 <= voices_peak <= 32 or not 0 <= loops_peak <= voices_peak or
            (analysis and loops_peak < 2) or (not analysis and loops_peak)):
        raise ValueError('actual two-channel musical/replacement coverage missing')
    if checksum != replay_hash(spec['initial_program'], spec['replacement_program'], first, second, cleanup):
        raise ValueError('replay absolute event trace does not match its declared score/timestamps')
    return dict(name=row['name'], **{k: int(v) for k, v in row.items() if k != 'name'})


def validate(native, target, profile, analysis, output_mode=None, allow_known_headroom_failure=False):
    if profile not in PROFILES or analysis not in (0, 1) or output_mode not in (None, *OUTPUT_MODES):
        raise ValueError('unknown profile or analysis mode')
    v2 = output_mode is not None
    if allow_known_headroom_failure and not (v2 and profile == 'physical' and output_mode == 'bypass'):
        raise ValueError('known headroom collection is restricted to physical bypass')
    native, target = ANSI.sub('', native), ANSI.sub('', target)
    host = validate_host(native)
    output_host = validate_output_host(native) if v2 else None
    known_failure = ('REALTIME_FAIL reason=digital_headroom runtime_approved=0 '
                     'physical_timing_approved=0')
    collecting_failure = allow_known_headroom_failure and target.splitlines().count(known_failure) == 1
    checked_target = target.replace(known_failure, '') if collecting_failure else target
    if FATAL.search(checked_target):
        raise ValueError('fatal target marker; preserve the complete failure log')
    spec = (PROFILES_V2 if v2 else PROFILES)[profile]
    begin = fields(target, 'REALTIME_BEGIN')
    expected_begin = dict(profile=profile, analysis=str(analysis), synthetic=str(int(spec['synthetic'])),
                          root_application='1', control_core='0', audio_core='1')
    if v2:
        expected_begin.update(schema='2', output_mode=output_mode, channels='2')
    if begin != expected_begin:
        raise ValueError('wrong application/profile/core ownership')
    probe = None
    if v2:
        probe = fields(target, 'REALTIME_OUTPUT_PROBE')
        expected_probe = dict(cases=1, input_frames=64, bypass_raw_overload=80, bypass_clipped=80,
            peak_raw_overload=80, peak_clipped=0, raw_peak_abs=2147483648, peak_peak_abs=32766,
            peak_gain_min_q30=16383, peak_limited_frames=64, peak_latency_frames=64,
            peak_output_frames=128, first_zero=1, pass_result=1, physical_timing_approved=0)
        expected_probe['pass'] = expected_probe.pop('pass_result')
        if probe != {k: str(v) for k, v in expected_probe.items()}:
            raise ValueError('actual target integer-extreme output protection probe failed')
    ready = fields(target, 'REALTIME_READY')
    status, session, plans, analyses, session_bytes, storage_bytes, storage_psram, capacity = numbers(
        ready, 'status', 'session', 'plans', 'analyses', 'session_bytes', 'storage_bytes', 'storage_psram', 'arena_capacity')
    expected_plans = (3 if profile == 'qemu' else 1) if analysis else 0
    if (status != 1 or session != 1 or plans != expected_plans or analyses != expected_plans or
            min(session_bytes, storage_bytes) <= 0 or storage_psram != int(spec['physical']) or
            capacity != spec['arena_bytes']):
        raise ValueError('startup admission or memory profile mismatch')
    if v2 and numbers(ready, 'channel1_plans', 'channel1_analyses') != [expected_plans, expected_plans]:
        raise ValueError('second channel did not independently initialize its loop path')
    asset = fields(target, 'REALTIME_ASSET')
    if asset != dict(bytes=str(spec['bytes']), sha256=spec['sha256'],
                     initial_program=str(spec['initial_program']), replacement_program=str(spec['replacement_program'])):
        raise ValueError('source identity/workload mismatch')
    if fields(target, 'REALTIME_ACTORS') != dict(control_core='0', audio_core='1'):
        raise ValueError('observed actors did not execute on their assigned cores')
    result = fields(target, 'REALTIME_RESULT')
    frames, blocks, checksum, events, rejected, clipped, voices, loops, switches, overlap = numbers(
        result, 'frames', 'blocks', 'checksum', 'events', 'rejected', 'clipped', 'voices_peak',
        'loop_voices_peak', 'program_switches', 'blocks_during_prepare')
    if (blocks != spec['blocks'] or frames != blocks * 64 or not 0 <= checksum < 1 << 64 or
            rejected or (clipped and not collecting_failure) or not 1 <= voices <= 32 or not 0 <= loops <= voices or switches != 2 or
            (analysis and (loops == 0 or overlap == 0)) or (not analysis and loops)):
        raise ValueError('continuous render/commit/actual loop execution mismatch')
    control = fields(target, 'REALTIME_CONTROL')
    queued, ons, offs, cc, sustain, pc, services, prepared, acks, ack_failures, reads, read_bytes, io_yields, analysis_yields = numbers(
        control, 'queued', 'note_on', 'note_off', 'cc', 'sustain', 'program_change', 'services', 'prepared',
        'acknowledgements', 'ack_failures', 'io_reads', 'io_bytes', 'io_yields', 'analysis_yields')
    expected_events = (19, 4, 3, 10) if profile == 'qemu' else (68, 28, 27, 11)
    if v2:
        expected_events = (136, 32, 30, 56) if profile == 'qemu' else (186, 56, 54, 58)
        if numbers(control, 'pitch_bend', 'channel_pressure') != [8, 6]:
            raise ValueError('musical pitch/pressure coverage missing')
    expected_pc = 4 if v2 else 2
    if ((queued, ons, offs, cc) != expected_events or events != queued or sustain != (14 if v2 else 4) or pc != expected_pc or
            services < expected_pc or prepared != expected_pc or acks != expected_pc or ack_failures or
            min(reads, read_bytes, io_yields) <= 0 or (analysis and not analysis_yields) or
            (not analysis and analysis_yields)):
        raise ValueError('MIDI/control work or acknowledgement coverage mismatch')
    timing = fields(target, 'REALTIME_TIMING')
    startup, hashing, prepare_avg, prepare_max, render_avg, p95, p99, maximum, bucket, physical = numbers(
        timing, 'startup_us', 'hash_us', 'prepare_avg_us', 'prepare_max_us', 'render_avg_us',
        'render_p95_upper_us', 'render_p99_upper_us', 'render_max_us', 'quantile_bucket_us', 'physical')
    if (physical != int(spec['physical']) or bucket != 16 or hashing > startup or
            prepare_avg > prepare_max or render_avg > maximum or not 0 <= p95 <= p99 <= maximum + 15):
        raise ValueError('timing labels or histogram bounds invalid')
    stages = fields(target, 'REALTIME_STAGES')
    read_us, read_max, analysis_us, analysis_max, analysis_calls, read_excludes, analysis_includes = numbers(
        stages, 'read_us', 'read_max_us', 'analysis_us', 'analysis_max_us', 'analysis_calls',
        'read_excludes_yield', 'analysis_includes_yield')
    if (read_max > read_us or analysis_max > analysis_us or analysis_calls == 0 or
            read_excludes != 1 or analysis_includes != 1):
        raise ValueError('control stage timing scope mismatch')
    health = fields(target, 'REALTIME_HEALTH')
    health_values = {key: numbers(health, key)[0] for key in (
        'render_soft_overruns', 'work_soft_overruns', 'max_backlog_us', 'final_backlog_us',
        'min_dma_margin_us', 'exceeded', 'stalls', 'writes', 'write_errors', 'timeouts', 'partial',
        'sent', 'dma_queue_overflows', 'event_queue_drops', 'audio_stack', 'control_stack')}
    if (health_values['event_queue_drops'] or health_values['audio_stack'] < 2048 or
            health_values['control_stack'] < 2048):
        raise ValueError('queue or stack headroom gate failed')
    cohort = fields(target, 'REALTIME_COHORT')
    requested, required, cohort_blocks, voices_min, voices_max, loops_min, loops_max, cohort_clipped, cohort_avg, cohort_p95, cohort_p99, cohort_max, cohort_backlog, cohort_final = numbers(
        cohort, 'requested', 'required', 'blocks', 'voices_min', 'voices_max', 'loops_min', 'loops_max', 'clipped',
        'render_avg_us', 'render_p95_upper_us', 'render_p99_upper_us', 'render_max_us', 'max_backlog_us', 'final_backlog_us')
    if requested != 24:
        raise ValueError('unexpected measured cohort')
    if v2:
        cohort_raw, = numbers(cohort, 'raw_overload_samples')
        if (spec['physical'] and not 0 < cohort_raw <= 4096 * 64 * 2 or
                not spec['physical'] and cohort_raw):
            raise ValueError('fixed cohort raw overload evidence missing or falsely claimed')
        if output_mode == 'bypass' and cohort_raw != cohort_clipped:
            raise ValueError('cohort bypass raw/post clipping diverged')
    if cohort_avg > cohort_max or not 0 <= cohort_p95 <= cohort_p99 <= cohort_max + 15:
        raise ValueError('cohort histogram bounds invalid')
    if spec['physical']:
        if (any(health_values[key] for key in ('final_backlog_us', 'exceeded', 'stalls', 'write_errors',
                                             'timeouts', 'partial', 'dma_queue_overflows')) or
                health_values['writes'] != blocks + int(v2 and output_mode == 'peak') or health_values['sent'] == 0 or
                health_values['max_backlog_us'] >= 11610 or health_values['min_dma_margin_us'] == 0):
            raise ValueError('physical DMA/backlog/write gate failed')
        armed = fields(target, 'REALTIME_I2S_ARMED')
        for key, value in dict(sample_rate='44100', frames='64', dma_desc='8', dma_frames='64',
                               dma_window_us='11610', master_q15='32767', dac_db='-12', mono_avg_lr='1').items():
            if armed.get(key) != value:
                raise ValueError('physical output configuration mismatch')
        bclk, mclk, dma_bytes = numbers(armed, 'bclk', 'mclk', 'dma_bytes')
        if (abs(bclk - 44100 * 32) * 100 > 44100 * 32 or
                abs(mclk - 44100 * 256) * 100 > 44100 * 256 or not dma_bytes):
            raise ValueError('physical clocks/DMA preload mismatch')
        if (required != 1 or cohort_blocks != 4096 or voices_min != 24 or voices_max != 24 or
                loops_min != 24 * analysis or loops_max != 24 * analysis or
                (cohort_clipped and not collecting_failure) or cohort_max > 1451 or cohort_final or cohort_backlog >= 11610 or cohort.get('gate') != 'PASS'):
            raise ValueError('new-path 24-Voice cohort did not pass; keep measured failure visible')
    elif (required or cohort_blocks or any((voices_min, voices_max, loops_min, loops_max, cohort_clipped,
                                          cohort_avg, cohort_p95, cohort_p99, cohort_max, cohort_backlog, cohort_final)) or
          cohort.get('gate') != 'NOT_RUN' or re.search(r'^REALTIME_I2S_ARMED', target, re.M) or
          any(health_values[key] for key in ('writes', 'write_errors', 'timeouts', 'partial', 'sent', 'dma_queue_overflows'))):
        raise ValueError('QEMU must not claim physical execution or cohort results')
    pcm = fields(target, 'REALTIME_PCM')
    pcm_capacity, high_water, used, failures, stale = numbers(
        pcm, 'capacity', 'high_water', 'used', 'allocation_failures', 'stale_refs')
    if not 0 <= used <= high_water <= pcm_capacity == spec['arena_bytes'] or failures or stale:
        raise ValueError('PCM/ref safety gate failed')
    output = validate_output_target(target, spec, frames, clipped, output_mode) if v2 else None
    replay = validate_replay(target, spec, frames, analysis) if v2 else None
    expected_gates = dict(integration='PASS', cohort='PASS' if spec['physical'] else 'NOT_RUN',
                          digital_headroom='FAIL' if collecting_failure else 'PASS')
    if v2:
        expected_gates['raw_headroom'] = 'FAIL' if output['raw_overload_samples'] else 'PASS'
        expected_gates['replay'] = 'PASS'
    if collecting_failure and (not clipped or not output['raw_overload_samples']):
        raise ValueError('known failure collector requires an actual visible overload')
    if fields(target, 'REALTIME_GATES') != expected_gates:
        raise ValueError('integration/headroom gates did not independently pass')
    stop = fields(target, 'REALTIME_STOP')
    if stop != dict(started='1', audio_stopped='1', control_stopped='1', releases='1', mapped='0',
                    arena_live='0', live_refs='0', acquirers='0', stale_refs='0'):
        raise ValueError('workers/resources not stopped exactly once')
    final_marker = 'REALTIME_FAIL' if collecting_failure else 'REALTIME_PASS'
    expected_final = dict(runtime_approved='0', physical_timing_approved='0')
    if collecting_failure:
        expected_final['reason'] = 'digital_headroom'
        if re.search(r'^REALTIME_PASS\b', target, re.M):
            raise ValueError('known headroom failure cannot claim PASS')
    if fields(target, final_marker) != expected_final:
        raise ValueError('invalid approval or final failure claim')
    memory = re.findall(r'^REALTIME_MEMORY phase=(\w+) internal_free=(\d+) internal_largest=(\d+) internal_min=(\d+) psram_free=(\d+) psram_largest=(\d+)$', target, re.M)
    if [row[0] for row in memory] != ['before', 'ready', 'joined', 'stopped']:
        raise ValueError('missing/duplicate/out-of-order memory lifecycle')
    if target.count('Returned from app_main()') != 1:
        raise ValueError('incomplete or repeated application execution')
    ordered = ['REALTIME_BEGIN', 'REALTIME_READY', 'REALTIME_ASSET', 'REALTIME_ACTORS', 'REALTIME_GATES', 'REALTIME_RESULT',
               'REALTIME_CONTROL', 'REALTIME_TIMING', 'REALTIME_STAGES', 'REALTIME_HEALTH', 'REALTIME_COHORT',
               'REALTIME_PCM', 'REALTIME_STOP', final_marker, 'Returned from app_main()']
    if v2:
        ordered[ordered.index('REALTIME_CONTROL'):ordered.index('REALTIME_CONTROL')] = ['REALTIME_OUTPUT', 'REALTIME_REPLAY']
        ordered.insert(1, 'REALTIME_OUTPUT_PROBE')
    positions = [target.index(name) if name == 'Returned from app_main()'
                 else re.search(r'^' + re.escape(name) + r'(?: |$)', target, re.M).start()
                 for name in ordered]
    if positions != sorted(positions):
        raise ValueError('invalid execution marker order')
    report = dict(schema=1, scope='concurrent control/audio actors, pinned native SF2, joint commit and joined shutdown',
                profile=profile, analysis=analysis, synthetic=spec['synthetic'], host=host,
                runtime_approved=False, physical_timing_approved=False, release_approved=False,
                physical_execution=spec['physical'], observed_hard_gate_pass=True,
                frames=frames, blocks=blocks, checksum=checksum, events=events,
                loop_voices_peak=loops, blocks_during_prepare=overlap,
                session_bytes=session_bytes, storage_bytes=storage_bytes,
                arena_bytes=capacity, pcm_high_water=high_water,
                render_histogram={key: int(value) for key, value in timing.items()},
                control_stages={key: int(value) for key, value in stages.items()},
                health=health_values, cohort=cohort, source_releases=1)
    if v2:
        report.update(schema=2, scope='two-channel concurrent playback and independent raw/output headroom evidence',
                      output_mode=output_mode, channels=2, output_host=output_host,
                      output=output, replay=replay, gates=expected_gates,
                      output_probe={k: int(v) for k, v in probe.items()},
                      listening_approved=False, qualified=not collecting_failure,
                      observed_hard_gate_pass=not collecting_failure,
                      known_headroom_failure_collected=collecting_failure)
    return report


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('native', 'target', 'output'):
        parser.add_argument('--' + name, type=Path, required=True)
    parser.add_argument('--profile', choices=PROFILES, required=True)
    parser.add_argument('--analysis', type=int, choices=(0, 1), required=True)
    parser.add_argument('--output-mode', choices=OUTPUT_MODES,
                        help='required for schema-2 output/replay evidence; omit only for archived schema 1')
    parser.add_argument('--allow-known-headroom-failure', action='store_true',
                        help='collect an otherwise-valid physical bypass clipping FAIL, never qualify it')
    args = parser.parse_args()
    try:
        result = validate(args.native.read_text(), args.target.read_text(), args.profile, args.analysis,
                          args.output_mode, args.allow_known_headroom_failure)
    except (OSError, ValueError) as error:
        parser.exit(2, f'realtime evidence rejected: {error}\n')
    args.output.write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps(result, indent=2))
