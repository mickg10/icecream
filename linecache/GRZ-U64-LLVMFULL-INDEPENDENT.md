# Independent GRZ unbounded-u64 scale replay — 43.106 GB LLVM

Date: 2026-08-17  
Runner: `mickg10/local-oracle`  
Host: `tt-quietbox` (`ttuser@tt-quietbox2`)  
CPU: AMD EPYC 8124P, 16 cores / 32 threads

## Verdict

The current unbounded whole-file COPY/ADD+BWT codec reconstructs a 43.106 GB LLVM corpus exactly,
produces 48,097,662 bytes, and clears the current core C/F rate bars:

```text
C encode:                   1.227679 GB/s (1,170.806 MiB/s)
F decode, one thread:       0.973474 GB/s (  928.377 MiB/s)
GRZ / complete z19-long:    0.655439x
raw / GRZ:                896.226595x
```

This is an **UNBOUNDED_U64_CEILING**, not a G0/G1/G2 result. It maps the complete input and allocates
the complete reconstructed output. Peak RSS is 42.27 GiB at C and 40.92 GiB at F. It proves 64-bit
addressing, cold-size headroom, and the core parser/replayer rate at this scale; it does not prove the
bounded retained-history design, chronological frame independence, or end-to-end compiler-pipe rate.

## Input and reference

| item | value |
|---|---:|
| included TUs | 8,141 |
| raw bytes | 43,106,403,854 |
| raw SHA-256 | `98ecb93c22e519008a51c0da166bad2980f91ec3805da2ad63f39aaa59eb8510` |
| ordered extraction manifest SHA-256 | `e511f1cd6aebdeed8cb4c4838c5c3f2fce6a0223d31d07155c9ebbd06a0399ec` |
| complete zstd-19 `--long=31` bytes | 73,382,413 |
| zstd frame SHA-256 | `63a5ba2a4cf9b7ab7b8599ceca27c2f7df3a3981c633af400d9cff3822a36d55` |
| zstd version | 1.5.7 |
| decoded zstd SHA-256 | same as raw |

The reference was produced over the complete 43,106,403,854-byte ordered input, not an `INT_MAX`
prefix. Local-oracle independently decoded it with the required 2 GiB window and hashed the decoded
stream:

```sh
/tmp/issue16-compression-tournament/zstd157/bin/zstd \
  -q -d --long=31 -c /tmp/zllvmfull.zst | sha256sum
```

The resulting digest equals the raw-input digest. The decode-plus-hash pass exited zero.

## Codec result

| measurement | value |
|---|---:|
| GRZ bytes | 48,097,662 |
| GRZ SHA-256 | `18c55f35eeb71556be2e8c0db763a3b4438401f91011441c8921a3a24e6eb4fc` |
| matches | 451,353 |
| GRZ / z19-long | 0.655438545x |
| saving versus z19-long | 25,284,751 bytes (34.456146%) |
| raw / GRZ | 896.226595x |
| decoded SHA-256 | same as raw |

## Rate measurements

The binding rate interpretation used here is:

```text
C encode >= 1,000,000,000 bytes/s
F decode >=   500,000,000 bytes/s, one thread
```

| path | interval | seconds | bytes/s | decimal GB/s | MiB/s | gate |
|---|---|---:|---:|---:|---:|:---:|
| C | codec-reported complete encode | 35.1121 | 1,227,679,456.768 | 1.227679 | 1,170.806 | PASS |
| C | process wall | 37.15 | 1,160,333,886.0 | 1.160334 | 1,106.580 | PASS |
| F | entropy decode + COPY/ADD replay, `-j 1` | 44.2810 | 973,474,037.488 | 0.973474 | 928.377 | PASS |
| F | replay + FIFO write + SHA-256 sink | 195.7430 | 220,219,389.0 | 0.220219 | 210.018 | diagnostic only |

The F gate row is the codec's single-thread reconstruction interval, matching the retained GRZ1/GRZ2
rate convention. The FIFO+SHA row is not a compiler-pipe benchmark: it includes hashing 43 GB and
overlapped with unrelated Firefox corpus I/O. It is retained to show that the current whole-file decoder
also waits until reconstruction is complete before writing its output. The grouped implementation must
measure streaming output separately.

## Memory and process evidence

| process | maximum RSS | interpretation |
|---|---:|---|
| C encode | 44,323,432 KiB = 42.270 GiB | complete input mapping becomes resident plus index/streams |
| F decode | 42,906,936 KiB = 40.919 GiB | complete reconstructed output allocation |

Both commands exited zero with no swaps. These values deliberately disqualify the row as bounded G2.

## Exact commands

```sh
taskset -c 0-15 ./grzc enc ii/llvmfull.ii \
  /tmp/issue16-grz-u64-localoracle-20260817/llvmfull.grz \
  -K 512 -s 5 -l 2 -k 1 -b 8 -j 8

taskset -c 0-15 ./grzc dec \
  /tmp/issue16-grz-u64-localoracle-20260817/llvmfull.grz \
  /tmp/issue16-grz-u64-localoracle-20260817/decoded.pipe -j 1
```

The FIFO was consumed by `sha256sum`; no second 43 GB decoded file was retained.

## Codec provenance

This run predates the binding grouped commit and therefore names the exact uncommitted capability bits:

```text
grz2.cpp SHA-256: 0425550e0d6d17423045ae75fe52eea6f64589b32fe82b64b6db25151b14171b
grzc SHA-256:     1129b08e26e2211bee3b7634439ba32f1fbad67d76b16e5acec0d795b105bcd4
```

It must not be substituted for the forthcoming commit containing:

```text
suffix-independent group frames
TU/raw/ADD group caps
committed-history anchor state
byte-bounded retained G2 history
strict full completion
actual bounded RSS
```

## Retained evidence

On `ttuser@tt-quietbox2`:

```text
/tmp/issue16-grz-u64-localoracle-20260817/
  encode.tsv
  encode.stderr
  encode.time
  decode.tsv
  decode.stderr
  decode.time
  executable-source.sha256
  inputs.stat
  original.sha256
  decoded.sha256
  z19.sha256
  z19-decoded.sha256
  z19-decode-hash.time
  llvmfull.grz
```

The compact machine-readable row is retained in
`linecache/ml-artifacts/grz-u64-llvmfull-localoracle.tsv`.
