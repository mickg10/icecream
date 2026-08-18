#!/usr/bin/env python3
"""Direct cold C-encode timestamps, observation-only.

Emits, from explicit clocks rather than by subtracting stages:

  C_input_ready_ns  : process start -> corpus bytes resident in memory
                      (the harness stand-in for "first raw byte at C")
  C_encode_ready_ns : corpus bytes in memory -> last selected payload fully encoded
                      spanning interning, S1 factorization, candidate selection,
                      materialization, entropy (z19/BSC on the literal groups) and the
                      selector
  C_total_ns        : process start -> same point (the two above, no subtraction)

Adds only timestamps and one fprintf. No control flow, no data, no output bytes change.
"""
import sys

SRC = sys.argv[1]
s = open(SRC).read()


def sub(old, new, why):
    assert s.count(old) == 1, f"anchor not unique ({why}): {s.count(old)}"
    return s.replace(old, new)


# process start
s = sub("    auto t0=Clock::now(); Corpus corpus=load_corpus(manifest,max_files); Interner dict;",
        "    auto _c_proc0=Clock::now();\n"
        "    auto t0=Clock::now(); Corpus corpus=load_corpus(manifest,max_files);\n"
        "    auto _c_mem0=Clock::now();   // corpus bytes resident: the in-memory producer hand-off\n"
        "    Interner dict;", "process start")

# last selected payload encoded == where the codec reports its own total
s = sub('    struct rusage ru{}; getrusage(RUSAGE_SELF,&ru); fprintf(stderr,"peak RSS=%.1f MiB total=%.1fs\\n",ru.ru_maxrss/1024.0,secs(t0));',
        '    {   auto _c_end=Clock::now();\n'
        '        double in_ready=std::chrono::duration<double>(_c_mem0-_c_proc0).count();\n'
        '        double enc_ready=std::chrono::duration<double>(_c_end-_c_mem0).count();\n'
        '        double total=std::chrono::duration<double>(_c_end-_c_proc0).count();\n'
        '        struct rusage _ru{}; getrusage(RUSAGE_SELF,&_ru);\n'
        '        fprintf(stderr,"CENCODE raw=%llu C_input_ready_s=%.6f C_encode_ready_s=%.6f"\n'
        '                       " C_total_s=%.6f encode_GBps=%.4f total_GBps=%.4f peakRSS_KiB=%ld\\n",\n'
        '                (unsigned long long)corpus.raw,in_ready,enc_ready,total,\n'
        '                corpus.raw/1e9/enc_ready,corpus.raw/1e9/total,_ru.ru_maxrss);\n'
        '    }\n'
        '    struct rusage ru{}; getrusage(RUSAGE_SELF,&ru); fprintf(stderr,"peak RSS=%.1f MiB total=%.1fs\\n",ru.ru_maxrss/1024.0,secs(t0));',
        "end timestamp")

open(SRC, "w").write(s)
print("instrumented for C-encode timestamps")
