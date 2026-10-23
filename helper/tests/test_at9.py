#!/usr/bin/env python3
# Art Swap helper - the snd0.at9 encoder (src/at9.c) against ps5-at9-converter.
#
#   python3 helper/tests/test_at9.py [WORK_DIR]
#
# Builds helper/tests/at9cli.c for this machine and makes test inputs: a sine
# sweep, white noise (also at 72 kb/s, where superframes must narrow their band
# to fit), silence, a mono file, a 44.1 kHz file, a 5.1 file, clicks that make
# the limiter work, a 32 kHz capture of Eternal Darkness, and music from its
# disc (EternalDarknessRecomp/dump, through FFmpeg; synthesised when there is
# no disc) cut to 30 s and to the longest that fits 2 MiB. Each input goes
# through ps5-at9-converter (beside this repo, or $PS5_AT9_CONVERTER) and
# through at9cli with the same excerpt, and both files are decoded with the
# converter's reference decoder. Checked for each:
#   - every frame decodes, and every superframe's unused tail is 0x01 bytes;
#   - the header (all bytes before the data) is the same, and so is the size;
#   - the decoded loudness is within 0.2 LU of the converter's;
#   - the C file's SNR against the converter's readied audio is at most 1 dB
#     below the converter's own;
#   - the readied audio and its loudness match the converter's.
# Then a table of both SNRs and loudnesses, how many superframes came out
# byte-identical, and the times. WORK_DIR keeps the files; by default they go
# in a temporary folder that is deleted at the end.
#
# Copyright (C) 2026 Gary Riches
# SPDX-License-Identifier: GPL-3.0-or-later
import json
import math
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import time
from pathlib import Path

import numpy as np

HERE = Path(__file__).resolve().parent
HELPER = HERE.parent
ROOT = HELPER.parent
CONVERTER = Path(os.environ.get("PS5_AT9_CONVERTER", ROOT.parent / "ps5-at9-converter"))
ED = ROOT.parent / "EternalDarknessRecomp"
sys.path.insert(0, str(CONVERTER))
from ps5at9 import at9file, atrac9, audio_io, dsp, wav  # noqa: E402

RATE = 48000
failures = []


def check(condition, what):
    print(("ok   " if condition else "FAIL ") + what)
    if not condition:
        failures.append(what)


# ---- inputs ----

def write_wav(path, pcm, rate, bits=16):
    wav.write_wav(str(path), np.atleast_2d(pcm), rate, bits=bits)


def music_like(seconds, rate, seed):
    """Chords with envelopes, a bass line, a kick and hi-hats, panned: something with music's shape."""
    rng = np.random.default_rng(seed)
    t = np.arange(int(seconds * rate)) / rate
    out = np.zeros((2, len(t)))
    beat = 0.5
    chords = [(220.0, 277.18, 329.63), (196.0, 246.94, 293.66), (174.61, 220.0, 261.63), (196.0, 246.94, 311.13)]
    for i, start in enumerate(np.arange(0.0, seconds, 4 * beat)):
        sel = (t >= start) & (t < start + 4 * beat + 0.5)
        tt = t[sel] - start
        env = np.minimum(tt / 0.02, 1.0) * np.exp(-tt * 0.8)
        for j, f in enumerate(chords[i % len(chords)]):
            tone = sum(np.sin(2 * np.pi * f * h * tt + j) / h ** 1.5 for h in range(1, 7))
            pan = 0.3 + 0.2 * j
            out[0, sel] += 0.12 * env * tone * (1 - pan)
            out[1, sel] += 0.12 * env * tone * pan
        bass = np.sin(2 * np.pi * chords[i % len(chords)][0] / 2 * tt) * np.exp(-tt * 1.5)
        out[:, sel] += 0.2 * bass
    for start in np.arange(0.0, seconds, beat):
        sel = (t >= start) & (t < start + 0.3)
        tt = t[sel] - start
        out[:, sel] += 0.5 * np.sin(2 * np.pi * (50 + 80 * np.exp(-tt * 30)) * tt) * np.exp(-tt * 12)
        hs = (t >= start + beat / 2) & (t < start + beat / 2 + 0.05)
        th = t[hs] - start - beat / 2
        noise = np.diff(rng.standard_normal(hs.sum() + 1))
        out[0, hs] += 0.05 * noise * np.exp(-th * 80)
        out[1, hs] += 0.04 * noise * np.exp(-th * 80)
    return out


def disc_file(disc, want):
    """One file out of a GameCube disc's file table"""
    with open(disc, "rb") as f:
        f.seek(0x424)
        fst_off, fst_size = struct.unpack(">II", f.read(8))
        f.seek(fst_off)
        fst = f.read(fst_size)
        count = struct.unpack_from(">I", fst, 8)[0]
        names = fst[count * 12:]
        stack = [(count, "")]
        for i in range(1, count):
            while i >= stack[-1][0]:
                stack.pop()
            word, a, b = struct.unpack_from(">III", fst, i * 12)
            o = word & 0xFFFFFF
            path = (stack[-1][1] + "/" + names[o:names.index(b"\0", o)].decode()).lstrip("/")
            if word >> 24:
                stack.append((b, path))
            elif path == want:
                f.seek(a)
                return f.read(b)
    return None


def real_music(work):
    """Eternal Darkness's inscath theme (133 s, 48 kHz) from the disc, or synthesised music when there is none"""
    out = work / "music.wav"
    disc = next((d for d in (ED / "dump/GEDP01.iso", ED / "dump/GEDE01.iso") if d.is_file()), None)
    if disc is not None and shutil.which("ffmpeg"):
        adp = disc_file(disc, "audio/music/inscath.adp")
        if adp is not None:
            (work / "inscath.adp").write_bytes(adp)
            subprocess.run(["ffmpeg", "-hide_banner", "-loglevel", "error", "-y", "-i", str(work / "inscath.adp"),
                            "-c:a", "pcm_s16le", str(out)], check=True)
            (work / "inscath.adp").unlink()
            return out, "ED inscath (disc)"
    write_wav(out, np.clip(music_like(120.0, RATE, 7), -1, 1), RATE)
    return out, "synthesised music"


def make_inputs(work):
    rng = np.random.default_rng(1)
    cases = []
    t = np.arange(20 * RATE) / RATE
    phase = 2 * np.pi * 20.0 * 20.0 / math.log(1000.0) * (np.power(1000.0, t / 20.0) - 1)
    write_wav(work / "sweep.wav", 0.5 * np.vstack([np.sin(phase), np.sin(phase + np.pi / 3)]), RATE)
    cases.append(("sweep", "sweep.wav", "48k stereo 16-bit", ["--start", "2", "--duration", "15"], 192))
    write_wav(work / "noise.wav", np.clip(0.2 * rng.standard_normal((2, 12 * RATE)), -1, 1), RATE, bits=32)
    cases.append(("noise", "noise.wav", "48k stereo float", ["--duration", "10"], 192))
    cases.append(("noise-72k", "noise.wav", "48k stereo float", ["--duration", "10"], 72))
    write_wav(work / "silence.wav", np.zeros((2, 5 * RATE)), RATE)
    cases.append(("silence", "silence.wav", "48k stereo 16-bit", ["--duration", "4"], 192))
    write_wav(work / "mono.wav", np.clip(music_like(12.0, RATE, 2).mean(axis=0), -1, 1), RATE)
    cases.append(("mono", "mono.wav", "48k mono 16-bit", ["--duration", "10"], 192))
    write_wav(work / "44k1.wav", np.clip(music_like(15.0, 44100, 3), -1, 1), 44100)
    cases.append(("44.1k", "44k1.wav", "44.1k stereo 16-bit", ["--start", "1.5", "--duration", "12"], 192))
    m = music_like(8.0, RATE, 4)
    surround = np.vstack([m[0], m[1], 0.5 * (m[0] + m[1]), 0.3 * np.sin(2 * np.pi * 40 * np.arange(m.shape[1]) / RATE),
                          np.roll(m[0], 2400), np.roll(m[1], 3600)])
    write_wav(work / "5.1.wav", np.clip(surround, -1, 1), RATE, bits=32)
    cases.append(("5.1", "5.1.wav", "48k 6-channel float", ["--duration", "6"], 192))
    clicks = 0.01 * np.vstack([np.sin(2 * np.pi * 440 * t[:10 * RATE]), np.sin(2 * np.pi * 660 * t[:10 * RATE])])
    for start in range(RATE // 3, 10 * RATE - 100, RATE // 2):
        clicks[:, start:start + 3] = [[0.9, -0.8, 0.5], [-0.9, 0.7, -0.4]]
    write_wav(work / "clicks.wav", clicks, RATE)
    cases.append(("clicks", "clicks.wav", "48k stereo 16-bit", ["--duration", "8"], 192))
    capture = ED / "logs/mark-1.wav"
    if capture.is_file():
        shutil.copy(capture, work / "ed32k.wav")
        cases.append(("ED 32k", "ed32k.wav", "ED capture, 32k stereo", ["--duration", "11"], 192))
    music, what = real_music(work)
    cases.append(("music 30s", music.name, what, ["--duration", "30"], 192))
    cases.append(("music 87s", music.name, what, ["--start", "20", "--duration", "max"], 192))
    cases.append(("music 144k", music.name, what, ["--start", "40", "--duration", "30"], 144))
    cases.append(("music 72k", music.name, what, ["--start", "60", "--duration", "30"], 72))
    return cases


# ---- the converter's pipeline (cli.run), for the readied audio it encodes ----

def converter_prepared(path, args, bitrate):
    opts = dict(zip(args[::2], args[1::2]))
    start = float(opts.get("--start", 0.0))
    duration = opts.get("--duration", "15")
    pcm, rate, offset = audio_io.load(str(path), start, None if duration == "max" else float(duration))
    pcm = dsp.resample(dsp.to_stereo(pcm), rate, RATE)
    skip = int(round(offset * RATE))
    available = max(0, pcm.shape[1] - skip)
    if duration == "max":
        count = min(available, at9file_max_samples(bitrate))
    else:
        count = min(available, int(round(float(duration) * RATE)))
    pcm = np.ascontiguousarray(pcm[:, skip:skip + count])
    source = dsp.integrated_loudness(pcm)
    if math.isfinite(source):
        pcm = pcm * 10.0 ** ((-28.0 - source) / 20.0)
    pcm, limited = dsp.limit_true_peak(pcm, -2.0, RATE)
    pcm = dsp.apply_fades(pcm, RATE, 0.01, 0.01)
    return pcm, source, dsp.integrated_loudness(pcm), limited


def at9file_max_samples(bitrate):
    from ps5at9.cli import max_samples_for
    return max_samples_for(bitrate, True)


def decode(blob, count):
    """The file through the reference decoder: (16-bit-rounded PCM as the console plays it, problems found)"""
    problems = []
    info = at9file.read_at9(blob)
    sfb = info.config.superframe_bytes
    trace = []
    try:
        out = atrac9.Decoder(info.config, trace=trace).decode(info.data)
    except atrac9.FormatError as exc:
        return None, ["decoder: %s" % exc]
    superframes = len(info.data) // sfb
    if len(trace) != superframes * 4:
        problems.append("%d frames decoded of %d" % (len(trace), superframes * 4))
    for s in range(superframes):
        end = -(-trace[4 * s + 3]["bits_end"] // 8)
        tail = info.data[s * sfb + end:(s + 1) * sfb]
        if tail.strip(b"\x01"):
            problems.append("superframe %d: tail is not 0x01" % s)
            break
    out = out[:, at9file.ENCODER_DELAY:at9file.ENCODER_DELAY + count]
    return np.clip(np.floor(out + 0.5), -32768, 32767) / 32768.0, problems


def snr(ref, test):
    noise = np.sum((test - ref) ** 2)
    signal = np.sum(ref ** 2)
    return 10 * math.log10(signal / max(noise, 1e-20)) if signal > 0 else math.nan


def main():
    work = Path(sys.argv[1]) if len(sys.argv) > 1 else Path(tempfile.mkdtemp(prefix="at9-test-"))
    keep = len(sys.argv) > 1
    work.mkdir(parents=True, exist_ok=True)
    if not (CONVERTER / "ps5_at9.py").is_file():
        sys.exit("no ps5-at9-converter at %s (set PS5_AT9_CONVERTER)" % CONVERTER)
    cli = work / "at9cli"
    cc = os.environ.get("CC", "cc")
    subprocess.run([cc, "-std=gnu11", "-O2", "-Wall", "-Wextra", "-Wno-unused-parameter", "-Werror", "-DARTSWAP_HOST",
                    "-I" + str(HELPER / "src"), str(HERE / "at9cli.c"), str(HELPER / "src/at9.c"),
                    str(HELPER / "src/at9_tables.c"), str(HELPER / "src/util.c"), "-lm", "-lpthread", "-o", str(cli)],
                   check=True)
    print("built %s" % cli)
    rows = []
    try:
        for name, file, what, args, bitrate in make_inputs(work):
            print("\n== %s: %s, %s, %d kb/s" % (name, what, " ".join(args), bitrate))
            src = work / file
            stem = name.replace(" ", "-").replace(".", "")
            py_out, c_out, dump = work / (stem + "-py.at9"), work / (stem + "-c.at9"), work / (stem + "-c.f64")
            param = work / (stem + "-param.json")
            param.write_text("{}\n")
            t0 = time.time()
            run = subprocess.run([sys.executable, "-I", str(CONVERTER / "ps5_at9.py"), "-i", str(src), "-o", str(py_out),
                                  "--bitrate", str(bitrate), "--param-json", str(param)] + args,
                                 capture_output=True, text=True)
            py_time = time.time() - t0
            check(run.returncode == 0, "%s: the converter ran (%s)" % (name, run.stderr.strip().splitlines()[-1:]))
            t0 = time.time()
            crun = subprocess.run([str(cli), "-i", str(src), "-o", str(c_out), "--bitrate", str(bitrate),
                                   "--dump", str(dump)] + args, capture_output=True, text=True)
            c_wall = time.time() - t0
            check(crun.returncode == 0, "%s: at9cli ran %s" % (name, crun.stderr.strip()))
            if run.returncode or crun.returncode:
                continue
            got = dict(line.split(" ", 1) for line in crun.stdout.strip().splitlines())
            count = int(got["count"])
            py_blob, c_blob = py_out.read_bytes(), c_out.read_bytes()

            ref, source, final, limited = converter_prepared(src, args, bitrate)
            check(ref.shape[1] == count, "%s: the same excerpt (%d samples)" % (name, count))
            prepared = np.fromfile(dump, dtype="<f8").reshape(2, count)
            diff = float(np.abs(prepared - ref).max())
            check(diff < 1e-6, "%s: readied audio matches the converter's (max difference %.1e)" % (name, diff))
            c_source, c_final = float(got["source_lufs"]), float(got["final_lufs"])
            same_lufs = (math.isinf(source) and math.isinf(c_source)) or abs(c_source - source) < 0.01
            check(same_lufs and (math.isinf(final) and math.isinf(c_final) or abs(c_final - final) < 0.01),
                  "%s: loudness %.4f -> %.4f LUFS (converter %.4f -> %.4f)" % (name, c_source, c_final, source, final))
            loud_json = json.loads(param.read_text())["pubtools"]["loudnessSnd0"]
            check(loud_json == "%.2f" % c_final, "%s: loudnessSnd0 %s, the converter's param.json %s"
                  % (name, "%.2f" % c_final, loud_json))

            check(len(c_blob) == len(py_blob), "%s: size %d (converter %d)" % (name, len(c_blob), len(py_blob)))
            header = c_blob[:at9file.header_size(True)] == py_blob[:at9file.header_size(True)]
            check(header, "%s: header identical" % name)
            dec_py, problems_py = decode(py_blob, count)
            dec_c, problems_c = decode(c_blob, count)
            check(not problems_py, "%s: converter's file decodes %s" % (name, problems_py))
            check(not problems_c, "%s: C file decodes, every tail 0x01 %s" % (name, problems_c))
            if dec_c is None or dec_py is None:
                continue
            lufs_py, lufs_c = dsp.integrated_loudness(dec_py), dsp.integrated_loudness(dec_c)
            both_silent = math.isinf(lufs_py) and math.isinf(lufs_c)
            check(both_silent or abs(lufs_c - lufs_py) <= 0.2,
                  "%s: decoded loudness %.3f vs %.3f LUFS" % (name, lufs_c, lufs_py))
            snr_py, snr_c = snr(ref, dec_py), snr(ref, dec_c)
            if math.isnan(snr_py):
                peak = max(float(np.abs(dec_py).max()), float(np.abs(dec_c).max()))
                check(peak <= 1 / 32768, "%s: silence decodes to silence (peak %.1e)" % (name, peak))
            else:
                check(snr_c >= snr_py - 1.0, "%s: SNR %.2f dB (converter %.2f dB)" % (name, snr_c, snr_py))
            sfs = at9file.read_at9(py_blob).config.superframe_bytes
            body_py, body_c = py_blob[at9file.header_size(True):], c_blob[at9file.header_size(True):]
            same = sum(body_py[i:i + sfs] == body_c[i:i + sfs] for i in range(0, len(body_py), sfs))
            rows.append((name, what, bitrate, count / RATE, snr_py, snr_c, lufs_py, lufs_c, header,
                         len(c_blob) == len(py_blob), "%d/%d" % (same, len(body_py) // sfs), limited, py_time,
                         float(got["time_prepare"]), float(got["time_encode"]), c_wall))
            for path in (py_out, c_out, dump, param):
                if not keep:
                    path.unlink()
        # The 2 MiB limit: one superframe past the longest excerpt is refused
        over = subprocess.run([str(cli), "-i", str(work / "music.wav"), "-o", str(work / "over.at9"), "--duration",
                               "%.6f" % ((at9file_max_samples(192) + 769) / RATE)], capture_output=True, text=True)
        check(over.returncode != 0 and "2 MiB" in over.stderr, "over 2 MiB refused: %s" % over.stderr.strip())
    finally:
        if not keep:
            shutil.rmtree(work, ignore_errors=True)

    print("\n%-11s %4s %7s %8s %8s %9s %9s %6s %5s %10s %4s %7s %7s %7s" % (
        "input", "kb/s", "secs", "py SNR", "C SNR", "py LUFS", "C LUFS", "header", "size", "same SFs",
        "lim", "py s", "C prep", "C enc"))
    for (name, what, bitrate, secs, snr_py, snr_c, lufs_py, lufs_c, header, size, same, limited, py_time,
         prep, enc, _wall) in rows:
        print("%-11s %4d %7.3f %8.2f %8.2f %9.3f %9.3f %6s %5s %10s %4s %7.2f %7.2f %7.2f" % (
            name, bitrate, secs, snr_py, snr_c, lufs_py, lufs_c, "same" if header else "DIFF",
            "same" if size else "DIFF", same, "yes" if limited else "no", py_time, prep, enc))
    print("\n%d failure(s)" % len(failures))
    for f in failures:
        print("  " + f)
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
