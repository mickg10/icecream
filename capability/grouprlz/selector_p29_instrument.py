#!/usr/bin/env python3
"""Observation-only stage timers inside codec50's Interner::process.

Splits the measured 68.4% load+intern share into:
  predict  region successor fast path (next1/next2 + memcmp)
  scan     next_region + sampled_hash + region index probe
  lines    the per-line intern_line loop for a NEW region   <- the intern_line seam
  store    region byte-store insert + record push + index insert

Adds ONLY counters and Clock::now() calls. No control flow, no data, no output changes,
so the wire must stay byte-identical -- which the caller verifies against the gated
identity wire before any number here is used.
"""
import sys

SRC = sys.argv[1]
s = open(SRC).read()

# --- accumulators on the Interner, plus a reporter -----------------------------------
anchor = "    uint32_t distinct() const { return next_id_-1; }"
assert s.count(anchor) == 1, "distinct() anchor not unique"
s = s.replace(anchor, """    // observation-only stage clocks (no effect on emitted bytes)
    double t_predict_=0,t_scan_=0,t_lines_=0,t_store_=0;
    uint64_t n_iter_=0,n_predict_hit_=0,n_scan_hit_=0,n_new_=0,n_lines_=0;
    void stage_report(const char*tag) const {
        double tot=t_predict_+t_scan_+t_lines_+t_store_;
        fprintf(stderr,"STAGECLOCK %s process_total=%.4f predict=%.4f scan=%.4f lines=%.4f store=%.4f"
                " pct_predict=%.1f pct_scan=%.1f pct_lines=%.1f pct_store=%.1f"
                " iters=%llu predict_hit=%llu scan_hit=%llu new_regions=%llu lines_interned=%llu\\n",
                tag,tot,t_predict_,t_scan_,t_lines_,t_store_,
                tot?100*t_predict_/tot:0.0,tot?100*t_scan_/tot:0.0,
                tot?100*t_lines_/tot:0.0,tot?100*t_store_/tot:0.0,
                (unsigned long long)n_iter_,(unsigned long long)n_predict_hit_,
                (unsigned long long)n_scan_hit_,(unsigned long long)n_new_,
                (unsigned long long)n_lines_);
    }
""" + anchor)

# --- phase boundaries inside the process loop ----------------------------------------
def sub(old, new, why):
    assert s.count(old) == 1, f"anchor not unique ({why}): {s.count(old)}"
    return s.replace(old, new)

s = sub("        while(p<end){ bool found=false; uint32_t region_id=UINT32_MAX,n=0; uint64_t h=0;",
        "        while(p<end){ bool found=false; uint32_t region_id=UINT32_MAX,n=0; uint64_t h=0;\n"
        "            ++n_iter_; auto _s0=Clock::now();", "loop head")

s = sub("            if(!found){ const char*q=next_region(p,end); n=uint32_t(q-p);",
        "            { auto _s1=Clock::now(); t_predict_+=std::chrono::duration<double>(_s1-_s0).count(); if(found)++n_predict_hit_; _s0=_s1; }\n"
        "            if(!found){ const char*q=next_region(p,end); n=uint32_t(q-p);", "phase A end")

s = sub("            if(!found){ const char*q=p+n; uint32_t ids_off=uint32_t(region_ids_.size()); const char*lp=p;",
        "            { auto _s2=Clock::now(); t_scan_+=std::chrono::duration<double>(_s2-_s0).count(); if(found)++n_scan_hit_; _s0=_s2; }\n"
        "            if(!found){ ++n_new_; const char*q=p+n; uint32_t ids_off=uint32_t(region_ids_.size()); const char*lp=p;", "phase B end")

s = sub("                uint32_t raw_off=uint32_t(region_bytes_.size()); region_bytes_.insert(region_bytes_.end(),p,q);",
        "                { auto _s3=Clock::now(); t_lines_+=std::chrono::duration<double>(_s3-_s0).count(); _s0=_s3; }\n"
        "                uint32_t raw_off=uint32_t(region_bytes_.size()); region_bytes_.insert(region_bytes_.end(),p,q);", "phase C1 end")

s = sub("insert_region_index(region_id); }\n            if(rout) rout->push_back(region_id);",
        "insert_region_index(region_id);\n"
        "                t_store_+=std::chrono::duration<double>(Clock::now()-_s0).count(); }\n"
        "            if(rout) rout->push_back(region_id);", "phase C2 end")

s = sub("uint32_t id=intern_line(lp,uint32_t(le-lp)); region_ids_.push_back(id);",
        "uint32_t id=intern_line(lp,uint32_t(le-lp)); ++n_lines_; region_ids_.push_back(id);", "line counter")

# --- emit the report right where load+intern is already reported ----------------------
s = sub('    fprintf(stderr,"loaded+interned %.1fs',
        '    dict.stage_report(manifest);\n'
        '    fprintf(stderr,"loaded+interned %.1fs', "report site")

open(SRC, "w").write(s)
print("instrumented")
