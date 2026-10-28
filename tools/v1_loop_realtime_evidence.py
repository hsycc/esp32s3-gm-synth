#!/usr/bin/env python3
"""Reject incomplete concurrent application logs; keep timing scope explicit."""
import argparse
import json
from pathlib import Path
import re

from tools.v1_loop_realtime_manifest import PROFILES

FATAL = re.compile(r'REALTIME_FAIL|Guru Meditation|(?:assert|Assertion)(?:ion)?[^\n]*failed|abort\(\)|panic.ed|task_wdt:')
ANSI = re.compile(r'\x1b\[[0-?]*[ -/]*[@-~]')
HOST_CASES = {
    'default_off', 'continuous_real_provider', 'missing_program_keeps_audio',
    'residency_failure_keeps_audio', 'stop_ready_before_commit',
    'stop_committed_before_ack', 'stop_during_real_analysis',
    'cancelled_analysis_direct_fallback', 'storage_exclusive_then_reusable',
    'real_fm_cohort_headroom_rejected',
}


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


def validate(native, target, profile, analysis):
    if profile not in PROFILES or analysis not in (0, 1):
        raise ValueError('unknown profile or analysis mode')
    native, target = ANSI.sub('', native), ANSI.sub('', target)
    host = validate_host(native)
    if FATAL.search(target):
        raise ValueError('fatal target marker; preserve the complete failure log')
    spec = PROFILES[profile]
    begin = fields(target, 'REALTIME_BEGIN')
    if begin != dict(profile=profile, analysis=str(analysis), synthetic=str(int(spec['synthetic'])),
                     root_application='1', control_core='0', audio_core='1'):
        raise ValueError('wrong application/profile/core ownership')
    ready = fields(target, 'REALTIME_READY')
    status, session, plans, analyses, session_bytes, storage_bytes, storage_psram, capacity = numbers(
        ready, 'status', 'session', 'plans', 'analyses', 'session_bytes', 'storage_bytes', 'storage_psram', 'arena_capacity')
    expected_plans = (3 if profile == 'qemu' else 1) if analysis else 0
    if (status != 1 or session != 1 or plans != expected_plans or analyses != expected_plans or
            min(session_bytes, storage_bytes) <= 0 or storage_psram != int(spec['physical']) or
            capacity != spec['arena_bytes']):
        raise ValueError('startup admission or memory profile mismatch')
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
            rejected or clipped or not 1 <= voices <= 32 or not 0 <= loops <= voices or switches != 2 or
            (analysis and (loops == 0 or overlap == 0)) or (not analysis and loops)):
        raise ValueError('continuous render/commit/actual loop execution mismatch')
    control = fields(target, 'REALTIME_CONTROL')
    queued, ons, offs, cc, sustain, pc, services, prepared, acks, ack_failures, reads, read_bytes, io_yields, analysis_yields = numbers(
        control, 'queued', 'note_on', 'note_off', 'cc', 'sustain', 'program_change', 'services', 'prepared',
        'acknowledgements', 'ack_failures', 'io_reads', 'io_bytes', 'io_yields', 'analysis_yields')
    expected_events = (19, 4, 3, 10) if profile == 'qemu' else (68, 28, 27, 11)
    if ((queued, ons, offs, cc) != expected_events or events != queued or sustain != 4 or pc != 2 or
            services < 2 or prepared != 2 or acks != 2 or ack_failures or
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
    if cohort_avg > cohort_max or not 0 <= cohort_p95 <= cohort_p99 <= cohort_max + 15:
        raise ValueError('cohort histogram bounds invalid')
    if spec['physical']:
        if (any(health_values[key] for key in ('final_backlog_us', 'exceeded', 'stalls', 'write_errors',
                                             'timeouts', 'partial', 'dma_queue_overflows')) or
                health_values['writes'] != blocks or health_values['sent'] == 0 or
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
                cohort_clipped or cohort_max > 1451 or cohort_final or cohort_backlog >= 11610 or cohort.get('gate') != 'PASS'):
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
    if fields(target, 'REALTIME_GATES') != dict(integration='PASS',
            cohort='PASS' if spec['physical'] else 'NOT_RUN', digital_headroom='PASS'):
        raise ValueError('integration/headroom gates did not independently pass')
    stop = fields(target, 'REALTIME_STOP')
    if stop != dict(started='1', audio_stopped='1', control_stopped='1', releases='1', mapped='0',
                    arena_live='0', live_refs='0', acquirers='0', stale_refs='0'):
        raise ValueError('workers/resources not stopped exactly once')
    if fields(target, 'REALTIME_PASS') != dict(runtime_approved='0', physical_timing_approved='0'):
        raise ValueError('invalid approval claim')
    memory = re.findall(r'^REALTIME_MEMORY phase=(\w+) internal_free=(\d+) internal_largest=(\d+) internal_min=(\d+) psram_free=(\d+) psram_largest=(\d+)$', target, re.M)
    if [row[0] for row in memory] != ['before', 'ready', 'joined', 'stopped']:
        raise ValueError('missing/duplicate/out-of-order memory lifecycle')
    if target.count('Returned from app_main()') != 1:
        raise ValueError('incomplete or repeated application execution')
    ordered = ['REALTIME_BEGIN', 'REALTIME_READY', 'REALTIME_ASSET', 'REALTIME_ACTORS', 'REALTIME_GATES', 'REALTIME_RESULT',
               'REALTIME_CONTROL', 'REALTIME_TIMING', 'REALTIME_STAGES', 'REALTIME_HEALTH', 'REALTIME_COHORT',
               'REALTIME_PCM', 'REALTIME_STOP', 'REALTIME_PASS', 'Returned from app_main()']
    if [target.index(name) for name in ordered] != sorted(target.index(name) for name in ordered):
        raise ValueError('invalid execution marker order')
    return dict(schema=1, scope='concurrent control/audio actors, pinned native SF2, joint commit and joined shutdown',
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


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('native', 'target', 'output'):
        parser.add_argument('--' + name, type=Path, required=True)
    parser.add_argument('--profile', choices=PROFILES, required=True)
    parser.add_argument('--analysis', type=int, choices=(0, 1), required=True)
    args = parser.parse_args()
    try:
        result = validate(args.native.read_text(), args.target.read_text(), args.profile, args.analysis)
    except (OSError, ValueError) as error:
        parser.exit(2, f'realtime evidence rejected: {error}\n')
    args.output.write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps(result, indent=2))
