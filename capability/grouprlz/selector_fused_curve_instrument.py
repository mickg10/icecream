#!/usr/bin/env python3
"""Observation-only per-TU cumulative-wire curve for production-fused.

Adds a --curve <file> option that logs, per TU, the bytes this codec would put on the
wire: the zstd-compressed body (new line definitions + the file-local occurrence stream).
Nothing else changes -- the body, its compression and the byte-exact reconstruction check
are untouched, so the summary totals must match the unpatched binary exactly.
"""
import sys

src, dst = sys.argv[1], sys.argv[2]
text = open(src).read()
applied = []


def sub(tag, old, new, count=1):
    global text
    if text.count(old) != count:
        sys.exit("anchor %s matched %d times, expected %d" % (tag, text.count(old), count))
    text = text.replace(old, new, count)
    applied.append(tag)


sub("opt", """        else if(!strcmp(argv[i],"--level")&&i+1<argc)only_level=atoi(argv[++i]); }""",
    """        else if(!strcmp(argv[i],"--level")&&i+1<argc)only_level=atoi(argv[++i]);
        else if(!strcmp(argv[i],"--curve")&&i+1<argc)curve_out=argv[++i]; }""")

sub("decl", """    const char* manifest=nullptr; size_t maxf=SIZE_MAX; int only_level=-1;""",
    """    const char* manifest=nullptr; size_t maxf=SIZE_MAX; int only_level=-1;
    const char* curve_out=nullptr;   // OBSERVATION-ONLY""")

sub("open", """        Stats st{}; bool ok=true;""",
    """        Stats st{}; bool ok=true;
        // OBSERVATION-ONLY per-TU curve; the cold (F-empty) pass is the learning curve.
        FILE* curve=nullptr; uint64_t cum_raw=0, cum_wire=0; long tu_index=0;
        if(curve_out && !warm){ curve=fopen(curve_out,"w");
            if(curve) fprintf(curve,"tu\\traw\\twire\\tcumulative_raw\\tcumulative_wire\\t"
                                    "keys\\tmissing\\toccurrences\\tbody_bytes\\n"); }""")

sub("row", """            st.rec+=seconds_since(t5); st.rbytes+=reconbuf.size(); st.keys+=nloc; st.miss+=nmiss; st.occ+=occ; st.tus++;""",
    """            st.rec+=seconds_since(t5); st.rbytes+=reconbuf.size(); st.keys+=nloc; st.miss+=nmiss; st.occ+=occ; st.tus++;
            if(curve){ cum_raw+=flen; cum_wire+=csz;   // OBSERVATION-ONLY
                fprintf(curve,"%ld\\t%u\\t%zu\\t%llu\\t%llu\\t%zu\\t%llu\\t%zu\\t%zu\\n",
                        tu_index++, flen, csz, (unsigned long long)cum_raw,
                        (unsigned long long)cum_wire, nloc, (unsigned long long)nmiss,
                        occ, body.size()); }""")

sub("close", """        if(!ok) return 1;""",
    """        if(curve){ if(fclose(curve)!=0){ fprintf(stderr,"curve write failed\\n"); return 2; } }
        if(!ok) return 1;""")

open(dst, "w").write(text)
print("applied:", " ".join(applied))
