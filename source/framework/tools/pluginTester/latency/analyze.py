#!/usr/bin/env python3
"""Analyze a retained capture and write a summary without local paths or audio."""
import argparse
import csv
import gzip
import hashlib
import io
import json
from pathlib import Path

import numpy as np
from scipy.io import wavfile
from scipy.signal import correlate


def wave(path):
    raw = path.read_bytes() if path.exists() else gzip.decompress(path.with_suffix('.wav.gz').read_bytes())
    rate, pcm = wavfile.read(io.BytesIO(raw))
    if pcm.dtype.kind == 'i':
        audio = pcm.astype(np.float64) / (2 ** (pcm.dtype.itemsize * 8 - 1))
    elif pcm.dtype.kind == 'f':
        audio = pcm.astype(np.float64)
    else:
        raise ValueError(f'unsupported WAV type: {pcm.dtype}')
    if audio.ndim != 2 or audio.shape[1] != 2 or not np.isfinite(audio).all():
        raise ValueError('capture must contain finite stereo audio')
    return rate, audio, hashlib.sha256(raw).hexdigest()


def performance(blocks, rate, notes):
    warm = [b for b in blocks if int(b['sample']) >= 12 * rate]
    durations = np.array([float(b['render_ms']) for b in warm])
    late = np.array([float(b['late_ms']) for b in warm])
    period = np.array([int(b['count']) * 1000 / rate for b in warm])
    result = dict(warm_start_seconds=12, warm_callbacks=len(warm),
                  warm_p50_ms=float(np.percentile(durations, 50)),
                  warm_p99_ms=float(np.percentile(durations, 99)),
                  warm_max_ms=float(durations.max()),
                  warm_over_budget=int(np.sum(durations > period)),
                  warm_completion_after_deadline=int(np.sum(late + durations > period)))
    if notes:
        at = notes[0]['sample']
        cold = [float(b['render_ms']) for b in blocks
                if int(b['sample']) < at + rate / 4 and int(b['sample']) + int(b['count']) > at]
        result['first_note_window_seconds'] = 0.25
        result['first_note_max_ms'] = max(cold)
    return result


def note_onsets(level, rate, notes):
    result = []
    for note in notes:
        at = note['sample']
        start, end = at - round(.1 * rate), at + round(.8 * rate)
        pre_peak = float(level[max(0, start - round(.15 * rate)):start].max())
        thresholds = {}
        for threshold in (1 / 32768, .0001, .001, .005):
            hits = np.flatnonzero(level[start:end] > threshold)
            delay = int(hits[0] + start - at) if hits.size else None
            thresholds[str(threshold)] = dict(prequiet=pre_peak <= threshold,
                                              delay_samples=delay,
                                              delay_ms=None if delay is None else delay * 1000 / rate)
        result.append(dict(sample=at, pre_peak=pre_peak, thresholds=thresholds))
    return result


def input_delay(case, output, rate):
    input_rate, source, digest = wave(case / 'capture.input.wav')
    if input_rate != rate or source.shape != output.shape:
        raise ValueError('input/output shape or rate mismatch')
    windows = []
    for time in (11, 13, 15, 17):
        at, count, margin = round(time * rate), rate // 2, rate
        a = source[at:at + count, 0]
        b = output[at - margin:at + count + margin, 0]
        values = correlate(b, a, mode='valid', method='fft')
        peak = int(np.argmax(np.abs(values)))
        segment = b[peak:peak + count]
        coefficient = float(values[peak] / max(1e-30, np.linalg.norm(a) * np.linalg.norm(segment)))
        delay = peak - margin if abs(coefficient) > .2 else None
        windows.append(dict(seconds=time, correlation=coefficient, delay_samples=delay,
                            delay_ms=None if delay is None else delay * 1000 / rate,
                            output_rms=float(np.sqrt(np.mean(segment ** 2)))))
    return dict(input_wav_sha256=digest, windows=windows)


def transport_pulses(level, rate, stages):
    stages = {s['stage']: s for s in stages}
    start, loop, stop = (stages[s]['sample'] for s in (1, 3, 4))
    result = {}
    for threshold in (.0001, .001, .005):
        above = np.flatnonzero(level > threshold)
        pulses = above[np.r_[True, np.diff(above) > round(.02 * rate)]] if above.size else above
        active = pulses[(pulses >= start) & (pulses < stop)]
        post = active[active >= loop]
        errors = [(float(p) - loop - i * rate * 60 / stages[3]['bpm']) * 1000 / rate
                  for i, p in enumerate(post)]
        result[str(threshold)] = dict(pulse_count=len(active),
            first_delay_ms=None if not len(active) else float(active[0] - start) * 1000 / rate,
            loop_errors_ms=errors, pulses_after_stop=[int(p) for p in pulses if p >= stop])
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('case', type=Path)
    args = parser.parse_args()
    case = args.case
    run = json.loads((case / 'run.json').read_text())
    meta = json.loads((case / 'capture.json').read_text())
    rate, audio, digest = wave(case / 'capture.wav')
    with (case / 'capture.blocks.csv').open() as stream:
        blocks = list(csv.DictReader(stream))
    if (rate != meta['sample_rate'] or len(audio) != round(meta['seconds'] * rate)
            or sum(int(b['count']) for b in blocks) != len(audio)):
        raise ValueError('capture length/rate mismatch')
    position = 0
    for block in blocks:
        if int(block['sample']) != position or int(block['count']) <= 0:
            raise ValueError('callback timeline has a gap, overlap or empty block')
        position += int(block['count'])
    if digest != run['capture_sha256']['capture.wav']:
        raise ValueError('capture SHA-256 mismatch')
    for name in ('capture.json', 'capture.blocks.csv'):
        if hashlib.sha256((case / name).read_bytes()).hexdigest() != run['capture_sha256'][name]:
            raise ValueError(f'{name} SHA-256 mismatch')
    level = np.max(np.abs(audio), axis=1)
    summary = dict(schema=1, model=run['options']['model'], scenario=meta['scenario'],
        host_os=meta['host_os'], analysis_runtime=run['analysis_runtime'],
        rate=rate, block=meta['block_size'], seconds=meta['seconds'], phase=meta['note_phase'],
        variable=meta['variable_blocks'], offline=meta['offline_after_seconds'] >= 0,
        suppress_message_loop=meta['suppress_message_loop'], settings=run['settings'],
        host_sha256=run['host_sha256'], plugin_sha256=run['plugin_sha256'],
        initial_data_sha256=run['initial_data_sha256'], tools_sha256=run['tools_sha256'],
        output_wav_sha256=digest, audio_finite_before_quantization=meta['audio_finite_before_quantization'],
        audio_peak=float(level.max()), audio_peak_before_quantization=meta['audio_peak_before_quantization'],
        reported_latency_samples=sorted({int(b['reported_latency_samples']) for b in blocks}),
        performance=performance(blocks, rate, meta['notes']),
        notes=note_onsets(level, rate, meta['notes']))
    if summary['offline']:
        summary['performance']['warm_completion_after_deadline'] = None
    if meta['scenario'] == 'input':
        summary['input'] = input_delay(case, audio, rate)
        if summary['input']['input_wav_sha256'] != run['capture_sha256']['capture.input.wav']:
            raise ValueError('input capture SHA-256 mismatch')
    if meta['scenario'] == 'transport':
        summary['transport'] = transport_pulses(level, rate, meta['transport'])
    (case / 'summary.json').write_text(json.dumps(summary, indent=2) + '\n')
    print(json.dumps({k: summary[k] for k in ('scenario', 'audio_peak', 'reported_latency_samples', 'performance')}, indent=2))


if __name__ == '__main__':
    main()
