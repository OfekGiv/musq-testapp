#!/usr/bin/env python3
"""Compare a --tx-file stream with what the requester recorded (--rx-record).

    scripts/stream_compare.py <original file> <record prefix>.w<N> [--show 10]

The requester sends the file as chunks, one per request: [8 B chunk index][file
bytes], with ESP SN = chunk index. The recording holds, in arrival order, each
response's chunk bytes (<prefix>.data, chunk_bytes per arrival) and its chunk
index (<prefix>.idx, uint64 per arrival); <prefix>.json has the sizes.

IDENTICAL means every chunk arrived exactly once, in order, with the file's
bytes. Then <prefix>.data equals the file byte for byte (for a file whose size
is a multiple of the chunk size, `cmp` shows it directly). Otherwise every
arrival is classified:

  in order    index = highest index seen before + 1
  reordered   index < highest seen before, first arrival of it (RFC 4737);
              extent = highest seen before - index
  gap         index > highest seen before + 1 (chunks skipped at that point)
  duplicate   index seen before
  bad index   index beyond the file
  corrupted   index valid, bytes differ from that chunk of the file
  missing     chunks that never arrived

Exit status 0 when IDENTICAL, 1 otherwise.
"""
import argparse
import json
import sys

import numpy as np

BLOCK = 1 << 20  # arrivals compared per block


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("original")
    ap.add_argument("record", help="record prefix including .w<N>, e.g. /dev/shm/rec.w0")
    ap.add_argument("--show", type=int, default=10, help="anomalies to list (default 10)")
    a = ap.parse_args()

    meta = json.load(open(a.record + ".json"))
    ch, nchunks, size = meta["chunk_bytes"], meta["file_chunks"], meta["file_size"]
    orig = np.memmap(a.original, dtype=np.uint8, mode="r")
    if orig.size != size:
        sys.exit(f"{a.original} has {orig.size} B, the recording was made from {size} B")
    idx = np.fromfile(a.record + ".idx", dtype=np.uint64)
    n = idx.size
    data = np.memmap(a.record + ".data", dtype=np.uint8, mode="r", shape=(n, ch)) if n else np.zeros((0, ch), np.uint8)
    full = size // ch                      # chunks that fill a whole row
    orig2d = orig[: full * ch].reshape(full, ch)
    last = None
    if size % ch:                          # the last chunk, zero-padded as sent
        last = np.zeros(ch, np.uint8)
        last[: size - full * ch] = orig[full * ch:]

    print(f"{a.record}: flow {meta['src_ip']} (lcore {meta['lcore']}), file {size} B = {nchunks} chunks of {ch} B")
    print(f"  sent {meta['chunks_sent']} chunks ({meta['tx_refused']} refused by the requester's TX and re-sent), "
          f"recorded {n} arrivals ({meta['bad_frames']} frames without a chunk, {meta['overflow']} beyond the buffer)")

    # Classification by index.
    valid = idx < nchunks
    iv = np.where(valid, idx, 0).astype(np.int64)
    first = np.zeros(n, bool)
    if n:
        _, pos = np.unique(np.where(valid, iv, -1), return_index=True)
        first[pos] = True
    first &= valid
    # highest valid index over the arrivals before each one (-1 before the first)
    prev_max = np.empty(n, np.int64)
    if n:
        run = np.maximum.accumulate(np.where(valid, iv, -1))
        prev_max[0] = -1
        prev_max[1:] = run[:-1]
    reordered = first & (iv < prev_max)
    gap = first & (iv > prev_max + 1)
    dup = valid & ~first
    bad = ~valid
    in_order = first & (iv == prev_max + 1)
    missing = nchunks - int(first.sum())
    skipped = int((iv[gap] - prev_max[gap] - 1).sum())

    # Content check, block by block.
    corrupted = np.zeros(n, bool)
    for b in range(0, n, BLOCK):
        e = min(n, b + BLOCK)
        rows = data[b:e]
        ix = iv[b:e]
        ok_rows = valid[b:e] & (ix < full)
        exp = orig2d[np.where(ok_rows, ix, 0)]
        diff = np.any(rows != exp, axis=1) & ok_rows
        if last is not None:
            lm = valid[b:e] & (ix == full)
            if lm.any():
                diff |= lm & np.any(rows != last, axis=1)
        corrupted[b:e] = diff

    identical = (n == nchunks and bool(in_order.all()) and not corrupted.any())
    print(f"  in order {int(in_order.sum())}, reordered {int(reordered.sum())}, gaps {int(gap.sum())} "
          f"({skipped} chunks skipped), duplicates {int(dup.sum())}, bad index {int(bad.sum())}, "
          f"corrupted {int(corrupted.sum())}, missing {missing}")
    if reordered.any():
        ext = (prev_max[reordered] - iv[reordered]).astype(np.int64)
        edges = [1, 2, 4, 8, 16, 32, 64, 128, 256, 1024, 4096, 1 << 62]
        hist = []
        lo = 1
        for hi in edges[1:]:
            c = int(((ext >= lo) & (ext < hi)).sum())
            if c:
                hist.append(f"{lo}-{hi - 1 if hi < (1 << 62) else 'inf'}: {c}")
            lo = hi
        print(f"  reorder extent (chunks): max {int(ext.max())}, mean {ext.mean():.2f}; " + ", ".join(hist))
        firsts = np.flatnonzero(reordered)
        print(f"  first reordered arrival #{int(firsts[0])}, last #{int(firsts[-1])} of {n} "
              f"(reordered arrivals span {100.0 * (firsts[-1] - firsts[0] + 1) / max(n, 1):.1f}% of the stream)")
    # cmp-style first difference between the file and the recorded bytes
    rec_flat = np.memmap(a.record + ".data", dtype=np.uint8, mode="r") if n else np.zeros(0, np.uint8)
    m = min(size, rec_flat.size)
    first_diff = None
    for b in range(0, m, BLOCK * ch):
        e = min(m, b + BLOCK * ch)
        d = np.flatnonzero(orig[b:e] != rec_flat[b:e])
        if d.size:
            first_diff = b + int(d[0])
            break
    if first_diff is not None:
        print(f"  cmp: file and recorded data differ first at byte {first_diff} (chunk {first_diff // ch})")
    elif rec_flat.size != size:
        print(f"  cmp: recorded data is {rec_flat.size} B, the file {size} B; equal over the first {m} B")
    else:
        print("  cmp: recorded data is byte-identical to the file")

    if not identical and a.show:
        kinds = [("reordered", reordered), ("gap", gap), ("duplicate", dup), ("bad index", bad),
                 ("corrupted", corrupted)]
        anomalies = sorted((int(j), k) for k, msk in kinds for j in np.flatnonzero(msk)[: a.show])[: a.show]
        print(f"  first anomalies (arrival #, kind, chunk index, highest index before it):")
        for j, k in anomalies:
            print(f"    #{j:<10} {k:<10} chunk {int(idx[j]):<12} highest before {int(prev_max[j])}")
        if missing and missing <= a.show:
            seen = np.zeros(nchunks, bool)
            seen[iv[first]] = True
            print(f"  missing chunks: {np.flatnonzero(~seen).tolist()}")
    print("  VERDICT:", "IDENTICAL" if identical else "DIFFERENT")
    sys.exit(0 if identical else 1)


if __name__ == "__main__":
    main()
