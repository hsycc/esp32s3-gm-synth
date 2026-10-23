#!/usr/bin/env python3
"""Host-only A/B WAV export for three normalized GeneralUser GS SF2 loop seams.

Each crossfade overlaps N terminal samples with the first N loop samples,
then resumes at loop offset N. It is NOT an embedded DSP implementation.
"""
import argparse
import array
import json
import math
import struct
import sys
import wave
from pathlib import Path

# sample id, playback start, loop end, original shdr start/end
TARGETS = ((604,10958088,10966031,10933088,10966039),
           (602,10877516,10905625,10852516,10906168),
           (62,850300,896467,788476,896475))
FADES = (0,16,32,64)

def chunks(path):
    with path.open("rb") as f:
        if f.read(4) != b"RIFF":
            raise ValueError("not RIFF")
        length, kind = struct.unpack("<I4s", f.read(8))
        if kind != b"sfbk" or length+8 > path.stat().st_size:
            raise ValueError("invalid SoundFont header")
        found = {}
        pos = 12
        while pos + 8 <= length+8:
            f.seek(pos)
            tag, size = struct.unpack("<4sI", f.read(8))
            start, end = pos+8, pos+8+size
            if end > length+8:
                raise ValueError("RIFF chunk outside bounds")
            if tag == b"LIST" and size >= 4:
                parent = f.read(4)
                child = start+4
                while child+8 <= end:
                    f.seek(child)
                    code, n = struct.unpack("<4sI", f.read(8))
                    if child+8+n > end:
                        raise ValueError("RIFF child outside bounds")
                    found[(parent,code)] = (child+8,n)
                    child += 8+n+(n&1)
            pos = end+(size&1)
    return found[(b"sdta",b"smpl")], found[(b"pdta",b"shdr")]

def sample(path, target):
    sample_id, playback_start, loop_end, shdr_start, shdr_end = target
    (pcm_offset, pcm_bytes),(headers, headers_bytes) = chunks(path)
    if headers_bytes % 46 or (sample_id+1)*46 > headers_bytes:
        raise ValueError("invalid shdr size or sample ID")
    with path.open("rb") as f:
        f.seek(headers + sample_id*46)
        _,start,end,start_loop,end_loop,rate,_,_,_,_ = struct.unpack(
            "<20sIIIIIBbHH",f.read(46))
        if (start,end) != (shdr_start,shdr_end):
            raise ValueError("shdr changed: do not render unqualified sample")
        if not (start_loop < playback_start < loop_end == end_loop <= end):
            raise ValueError("bad loop/source geometry")
        if not 8000 <= rate <= 192000 or end*2 > pcm_bytes:
            raise ValueError("bad PCM bounds or rate")
        f.seek(pcm_offset + playback_start*2)
        count = loop_end - playback_start
        raw = f.read(count*2)
        if len(raw) != count*2:
            raise ValueError("truncated smpl")
    data = array.array("h")
    data.frombytes(raw)
    if sys.byteorder != "little":
        data.byteswap()
    return rate,list(data)

def render_loop(pcm, fade, total):
    size = len(pcm)
    if total <= size or not 0 <= fade < size//2:
        raise ValueError("bad fade/duration")
    out, wraps, i = [],[],0
    while len(out) < total:
        if fade and i >= size-fade:
            j = i-(size-fade)
            alpha_num = j+1
            val = round((pcm[i]*(fade+1-alpha_num)+pcm[j]*alpha_num)/(fade+1))
            out.append(max(-32768,min(32767,val)))
            i += 1
            if i == size:
                i = fade
                wraps.append(len(out))
        else:
            out.append(pcm[i])
            i += 1
            if i == size:
                i = 0
                wraps.append(len(out))
    return out,wraps

def measures(out,wraps,fade):
    first = wraps[0]
    radius = max(96,fade+32)
    jumps = [abs(out[i]-out[i-1]) for i in range(max(1,first-radius),
                                                      min(len(out),first+radius))]
    return dict(wrap_step_pcm16=abs(out[first]-out[first-1]),
                max_near_wrap_step_pcm16=max(jumps),
                rms_near_wrap_step_pcm16=round(math.sqrt(sum(x*x for x in jumps)/len(jumps)),2),
                wraps_in_clip=len(wraps))

def save_wave(path,values,rate):
    data = array.array("h",values)
    if sys.byteorder != "little":
        data.byteswap()
    with wave.open(str(path),"wb") as f:
        f.setnchannels(1)
        f.setsampwidth(2)
        f.setframerate(rate)
        f.writeframes(data.tobytes())

def run(sf2,output,seconds=2.0):
    output.mkdir(parents=True,exist_ok=True)
    rows=[]
    for target in TARGETS:
        rate,pcm=sample(sf2,target)
        frames=max(len(pcm)+1,round(seconds*rate))
        for fade in FADES:
            out,wraps=render_loop(pcm,fade,frames)
            filename=f"sample_{target[0]}_{'direct' if not fade else f'xfade_{fade}f'}.wav"
            save_wave(output/filename,out,rate)
            row=dict(sample_id=target[0],rate=rate,fade_frames=fade,
                     filename=filename,**measures(out,wraps,fade))
            rows.append(row)
            print("M7_SEAM_AUDIO",json.dumps(row,sort_keys=True))
    (output/"results.json").write_text(json.dumps(rows,indent=2)+"\n")
    lines=["# M7 SF2 loop seam A/B audio", "",
           "Host-only PCM16 native-sample-rate audition. No realtime DSP change.",
           "The crossfade overlaps the final N and initial N loop frames and resumes at +N.",
           "Loop period changes by N frames; check pitch/timbre and tempo.",
           "PCM16 jump measurements are not a subjective listening PASS.", "",
           "| Sample | Variant | Wrap step | Max nearby step | RMS nearby | WAV |",
           "|---:|---|---:|---:|---:|---|"]
    for r in rows:
        label="Direct" if r["fade_frames"]==0 else f"Crossfade {r['fade_frames']}f"
        lines.append(f"| {r['sample_id']} | {label} | {r['wrap_step_pcm16']} | "
                     f"{r['max_near_wrap_step_pcm16']} | {r['rms_near_wrap_step_pcm16']} | {r['filename']} |")
    lines.extend(["","WAVs are A/B reference renders, not firmware output.",""])
    (output/"REPORT.md").write_text("\n".join(lines))
    return rows

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument("sf2",type=Path)
    parser.add_argument("--output",required=True,type=Path)
    parser.add_argument("--seconds",type=float,default=2.0)
    args=parser.parse_args()
    if not 0.1 <= args.seconds <= 10.0:
        parser.error("seconds must be between 0.1 and 10")
    run(args.sf2,args.output,args.seconds)

if __name__=="__main__":
    main()
