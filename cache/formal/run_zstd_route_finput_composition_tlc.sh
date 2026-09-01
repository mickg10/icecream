#!/bin/sh
# Bounded, manifest-authoritative TLC executor for the V5 composition model.
# Reproducibility/protocol evidence only; no product/profile disposition.
set -eu
D=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(CDPATH='' cd -- "$D/../.." && pwd)
MANIFEST=$D/s6_mutation_manifest.jsonl
SPEC=$(printenv S6_V5_SPEC_PATH 2>/dev/null || true)
test -n "$SPEC" || SPEC=/tmp/s6-zstd-route-finput-v5-correction-spec-20260828.md
SPEC_SHA=70051b06bbb9e74fc743696ce1387fafe39731fc47c2a4641bb7e634c3809dd0
PINNED=936a262061c914694dfd669a543be24573c45d5aa0ff20a8b96b23d01e050e88
JAR=$(printenv TLA2TOOLS_JAR 2>/dev/null || true)
TIMEOUT=$(printenv ROW_TIMEOUT_SECONDS 2>/dev/null || true); test -n "$TIMEOUT" || TIMEOUT=60
IDS=$(printenv ROW_IDS 2>/dev/null || true)
FULL_MATRIX=$(printenv FULL_MATRIX 2>/dev/null || true); test -n "$FULL_MATRIX" || FULL_MATRIX=0
TLC_WORKERS=$(printenv TLC_WORKERS 2>/dev/null || true); test -n "$TLC_WORKERS" || TLC_WORKERS=1
TLC_SEED=$(printenv TLC_SEED 2>/dev/null || true); test -n "$TLC_SEED" || TLC_SEED=1
TLC_FP_INDEX=$(printenv TLC_FP_INDEX 2>/dev/null || true); test -n "$TLC_FP_INDEX" || TLC_FP_INDEX=0
EXP=$(printenv ZSTD_ROUTE_EXPERIMENTS_ROOT 2>/dev/null || true)
test -n "$EXP" || EXP=/tanksmall/scratch/ictmp/experiments/icecream/zstd-route-finput-composition-v5
case "$TIMEOUT" in ''|*[!0-9]*) exit 2;; esac
test "$TIMEOUT" -gt 0 -a "$TIMEOUT" -le 300 || exit 2
test -n "$JAR" || { echo 'FAIL: TLA2TOOLS_JAR is required' >&2; exit 2; }
case "$JAR" in /*) ;; *) echo 'FAIL: TLA2TOOLS_JAR must be absolute' >&2; exit 2;; esac
test -f "$JAR" || { echo 'FAIL: pinned TLC JAR is absent' >&2; exit 2; }
sha256() { sha256sum "$1" | awk '{print $1}'; }
JSHA=$(sha256 "$JAR")
test "$JSHA" = "$PINNED" || { echo 'FAIL: pinned TLC digest mismatch' >&2; exit 2; }
test -f "$SPEC" || { echo 'FAIL: V5 correction spec is absent' >&2; exit 2; }
test "$(sha256 "$SPEC")" = "$SPEC_SHA" || { echo 'FAIL: V5 correction spec digest mismatch' >&2; exit 2; }
if test "${S6_TRANSPLANT_REVIEW:-0}" = 1; then
  test "$(git -C "$ROOT" merge-base HEAD b702a35cf4060a560135d488e51b2d39d2fd3526)" = b702a35cf4060a560135d488e51b2d39d2fd3526 || {
    echo 'FAIL: review transplant requires exact b702 ancestry' >&2; exit 2
  }
  test -f "$D/S6_V6_TRANSPLANT_AUTHORITY.md" || {
    echo 'FAIL: review transplant authority map is absent' >&2; exit 2
  }
else
  test "$(git -C "$ROOT" rev-parse HEAD^)" = dd649d40593b1b685c92ea62965072c63172af7b || {
    echo 'FAIL: exact V6 base required' >&2; exit 2
  }
fi
PARENTS=$(git -C "$ROOT" rev-list --parents -n1 HEAD)
set -- $PARENTS
test "$#" -eq 2 || { echo 'FAIL: reviewed commit must have one parent' >&2; exit 2; }
test -z "$(git -C "$ROOT" status --porcelain --untracked-files=all)" || {
  echo 'FAIL: subject worktree is not clean' >&2; exit 2
}
STATE=$(printenv ZSTD_ROUTE_FINPUT_COMPOSITION_STATE_ROOT 2>/dev/null || true)
test -n "$STATE" || STATE=$EXP/$(date -u +%Y%m%dT%H%M%SZ)
RESUME=$(printenv RESUME 2>/dev/null || true); test -n "$RESUME" || RESUME=0
if test -e "$STATE" && test "$RESUME" != 1; then echo 'FAIL: existing state path; use RESUME=1' >&2; exit 2; fi
mkdir -p "$STATE/rows" "$STATE/states" "$STATE/java-tmp" "$STATE/inputs/config"
COMMIT=$(git -C "$ROOT" rev-parse HEAD); TREE=$(git -C "$ROOT" rev-parse 'HEAD^{tree}'); PARENT=$(git -C "$ROOT" rev-parse HEAD^)
export COMMIT TREE PARENT TIMEOUT JAR JSHA SPEC SPEC_SHA PINNED RESUME TLC_WORKERS TLC_SEED TLC_FP_INDEX FULL_MATRIX IDS

python3 - "$MANIFEST" "$D" "$STATE/config.list" <<'PY'
import hashlib,json,pathlib,re,sys
m=pathlib.Path(sys.argv[1]); d=pathlib.Path(sys.argv[2]); out=pathlib.Path(sys.argv[3])
rows=[json.loads(x) for x in m.read_text().splitlines() if x.strip()]
need={'id','kind','module','config','module_sha256','config_sha256','expected_exit','expected_wait','expected','injection','injection_marker','target','antecedent','row_sha256','expected_phase'}
if len(rows)!=86: raise SystemExit('manifest must declare exactly 86 rows')
if any(need-set(r) for r in rows): raise SystemExit('manifest semantic fields incomplete')
if len({r['id'] for r in rows})!=len(rows): raise SystemExit('manifest duplicate identity')
def h(p): return hashlib.sha256(p.read_bytes()).hexdigest()
def rh(r): return hashlib.sha256(json.dumps({k:v for k,v in r.items() if k!='row_sha256'},sort_keys=True,separators=(',',':')).encode()).hexdigest()
cfgs=[]; hashes=[]
for r in rows:
    mp=d/r['module']; cp=d/r['config']
    if not mp.is_file() or not cp.is_file(): raise SystemExit('missing manifest file '+r['id'])
    if h(mp)!=r['module_sha256'] or h(cp)!=r['config_sha256'] or rh(r)!=r['row_sha256']:
        raise SystemExit('manifest digest mismatch '+r['id'])
    if r['expected_wait'] not in {'zero','nonzero'}: raise SystemExit('bad wait '+r['id'])
    if r['expected_phase'] not in {'clean','invariant','initial-invariant','temporal','deadlock'}: raise SystemExit('bad phase '+r['id'])
    if 'antecedent_state' in r:
        if not isinstance(r['antecedent_state'],dict) or not r['antecedent_state']: raise SystemExit('bad antecedent state '+r['id'])
        if r.get('antecedent_state_position','last') not in {'last','previous'}: raise SystemExit('bad antecedent state position '+r['id'])
        if any(not isinstance(k,str) or '.' not in k for k in r['antecedent_state']): raise SystemExit('bad antecedent state key '+r['id'])
    mt=mp.read_text(errors='replace'); ct=cp.read_text(errors='replace')
    marker=r['injection_marker']
    if marker!='none' and marker not in mt+ct: raise SystemExit('marker not source-bound '+r['id'])
    if ('mutant' in r['kind'] or r['injection']=='reachable') and (not r['target'] or not r['antecedent']):
        raise SystemExit('negative semantic fields incomplete '+r['id'])
    for k in ('required_properties','coverage_operators'):
        if k in r and (not isinstance(r[k],list) or any(not isinstance(x,str) or not x for x in r[k])):
            raise SystemExit('bad '+k+' '+r['id'])
    for prop in r.get('required_properties',[]):
        if not re.search(r'(?m)^\s*'+re.escape(prop)+r'\s*$',ct): raise SystemExit('required property not declared '+r['id']+'/'+prop)
    for op in r.get('coverage_operators',[]):
        if op not in mt+ct: raise SystemExit('coverage operator not source-bound '+r['id']+'/'+op)
    cfgs.append(r['config']); hashes.append(h(cp))
if len(set(hashes)) != len(hashes): raise SystemExit('manifest duplicate config bytes')
out.write_text(''.join(x+'\n' for x in sorted(set(cfgs))))
print('PREFLIGHT rows=%d unique_ids=%d unique_configs=%d unique_config_bytes=%d'%(len(rows),len({r['id'] for r in rows}),len(set(cfgs)),len(set(hashes))))
PY

cp "$D/Protocol50ZstdRoute.tla" "$STATE/inputs/Protocol50ZstdRoute.tla"
cp "$D/Protocol50ZstdRouteFInputInterface.tla" "$STATE/inputs/Protocol50ZstdRouteFInputInterface.tla"
cp "$D/Protocol50ZstdRouteFInputComposition.tla" "$STATE/inputs/Protocol50ZstdRouteFInputComposition.tla"
cp "$MANIFEST" "$STATE/inputs/s6_mutation_manifest.jsonl"
cp "$D/run_zstd_route_finput_composition_tlc.sh" "$STATE/inputs/run_zstd_route_finput_composition_tlc.sh"
cp "$SPEC" "$STATE/inputs/s6-zstd-route-finput-v5-correction-spec.md"
while IFS= read -r cfg; do test -z "$cfg" || cp "$D/$cfg" "$STATE/inputs/config/$cfg"; done <"$STATE/config.list"
printf '%s  %s\n' "$JSHA" "$JAR" >"$STATE/tool.sha256"
printf '%s  %s\n' "$SPEC_SHA" "$SPEC" >"$STATE/spec.sha256"
printf '%s\n' \
  "TLA2TOOLS_JAR=$JAR" "ROW_TIMEOUT_SECONDS=$TIMEOUT" "TLC_WORKERS=$TLC_WORKERS" \
  "TLC_SEED=$TLC_SEED" "TLC_FP_INDEX=$TLC_FP_INDEX" "FULL_MATRIX=$FULL_MATRIX" "ROW_IDS=$IDS" \
  "ROOT=$ROOT" "MANIFEST=$MANIFEST" \
  "COMMAND=(cd $D && timeout --signal=TERM --kill-after=2s TIMEOUTs java -XX:+UseParallelGC -cp TLA2TOOLS_JAR tlc2.TLC -workers TLC_WORKERS -seed TLC_SEED -fp TLC_FP_INDEX -coverage 1 -metadir STATE/states/ROW_ID -config ROW_CONFIG ROW_MODULE)" \
  >"$STATE/commands.txt"
printf '%s\n' 'BigOracle issue16 follow-up comments 5447981767 and 5448067827 were not available in the frozen local evidence set; interpretation remains HOLD pending external ruling.' >"$STATE/issue16-ruling.txt"

set +e
(cd "$D" && java -cp "$JAR" tla2sany.SANY Protocol50ZstdRouteFInputComposition.tla) >"$STATE/sany.stdout" 2>"$STATE/sany.stderr"
SANY_RC=$?
set -e
test "$SANY_RC" -eq 0 && SANY_STATUS=pass || SANY_STATUS=fail
export SANY_RC SANY_STATUS
python3 - "$STATE/provenance.json" "$D/run_zstd_route_finput_composition_tlc.sh" "$MANIFEST" <<'PY'
import hashlib,json,os,pathlib,subprocess,time,sys
def h(p): return hashlib.sha256(pathlib.Path(p).read_bytes()).hexdigest()
json.dump({'source_commit':os.environ['COMMIT'],'source_tree':os.environ['TREE'],'source_parent':os.environ['PARENT'],
 'subject_clean':True,'jar_path':os.environ['JAR'],'jar_sha256':os.environ['JSHA'],
 'spec_path':os.environ['SPEC'],'spec_sha256':os.environ['SPEC_SHA'],
 'runner_sha256':h(sys.argv[2]),'manifest_sha256':h(sys.argv[3]),
 'java':subprocess.run(['java','-version'],capture_output=True,text=True).stderr.splitlines()[:1],
 'tool_available':True,'sany_status':os.environ['SANY_STATUS'],'sany_raw_exit':int(os.environ['SANY_RC']),
 'utc_start':time.strftime('%Y-%m-%dT%H:%M:%SZ',time.gmtime()),'timeout_seconds':int(os.environ['TIMEOUT']),
 'workers':os.environ['TLC_WORKERS'],'seed':os.environ['TLC_SEED'],'fp_index':os.environ['TLC_FP_INDEX']},
 open(sys.argv[1],'w'),sort_keys=True,indent=2)
PY

selected() {
  id=$1
  if test -n "$IDS"; then case ",$IDS," in *,"$id",*) return 0;; *) return 1;; esac; fi
  test "$FULL_MATRIX" = 1 || test "$id" = composition-general-safety
}

runrow() {
  row=$1
  id=$(printf '%s' "$row" | python3 -c 'import json,sys; print(json.load(sys.stdin)["id"])')
  selected "$id" || return 0
  out=$STATE/rows/$id.json
  test -e "$out" && test "$RESUME" = 1 && return 0
  cfg=$(printf '%s' "$row" | python3 -c 'import json,sys; print(json.load(sys.stdin)["config"])')
  mod=$(printf '%s' "$row" | python3 -c 'import json,sys; print(json.load(sys.stdin)["module"])')
  so=$STATE/rows/$id.stdout; se=$STATE/rows/$id.stderr; resource=$STATE/rows/$id.resource
  start=$(date -u +%Y-%m-%dT%H:%M:%SZ); ms=$(python3 -c 'import time; print(time.monotonic())')
  rc=125; signal=none
  set +e
  (cd "$D" && /usr/bin/time -f 'elapsed_seconds=%e\nmax_rss_kb=%M\nuser_seconds=%U\nsys_seconds=%S' -o "$resource" \
    timeout --signal=TERM --kill-after=2s "$TIMEOUT"s java -XX:+UseParallelGC -cp "$JAR" \
    tlc2.TLC -workers "$TLC_WORKERS" -seed "$TLC_SEED" -fp "$TLC_FP_INDEX" -coverage 1 \
    -metadir "$STATE/states/$id" -config "$cfg" "$mod") >"$so" 2>"$se"
  rc=$?
  set -e
  case "$rc" in 124|137|143) signal=timeout;; esac
  end=$(date -u +%Y-%m-%dT%H:%M:%SZ); me=$(python3 -c 'import time; print(time.monotonic())')
  python3 - "$out" "$row" "$rc" "$signal" "$start" "$end" "$ms" "$me" "$so" "$se" "$resource" "$D" "$STATE" <<'PY'
import hashlib,json,pathlib,re,sys,os
out=pathlib.Path(sys.argv[1]); r=json.loads(sys.argv[2]); rc=int(sys.argv[3]); sig=sys.argv[4]
so=pathlib.Path(sys.argv[9]); se=pathlib.Path(sys.argv[10]); rp=pathlib.Path(sys.argv[11]); root=pathlib.Path(sys.argv[12]); state=pathlib.Path(sys.argv[13])
stdout=so.read_text(errors='replace'); stderr=se.read_text(errors='replace'); text=stdout+'\n'+stderr
def h(p): return hashlib.sha256(p.read_bytes()).hexdigest()
st=dict(r)
st.update(raw_exit=rc,raw_wait_status=rc,observed_wait_disposition='zero' if rc==0 else 'nonzero',
 wait_disposition='zero' if rc==0 else 'nonzero',timeout_signal=sig,utc_start=sys.argv[5],utc_end=sys.argv[6],
 monotonic_start=float(sys.argv[7]),monotonic_end=float(sys.argv[8]),
 elapsed_seconds=max(0.0,float(sys.argv[8])-float(sys.argv[7])),
 stdout_path=str(so.relative_to(state)),stderr_path=str(se.relative_to(state)),resource_path=str(rp.relative_to(state)),
 stdout_sha256=h(so),stderr_sha256=h(se))
resource={}
for line in rp.read_text(errors='replace').splitlines():
 if '=' in line:
  k,v=line.split('=',1)
  try: resource[k]=float(v) if '.' in v else int(v)
  except ValueError: resource[k]=v
st['resource']=resource
nums=re.findall(r'(?m)^\s*(\d+) states generated, (\d+) distinct states found, (\d+) states left on queue\.?',text)
if nums: st['generated_states'],st['distinct_states'],st['queue_depth']=map(int,nums[-1])
else: st['generated_states']=st['distinct_states']=0; st['queue_depth']=None
depths=re.findall(r'(?mi)^The depth of the complete state graph(?: search)? is (\d+)\.',text)
st['max_depth']=int(depths[-1]) if depths else None
clean='Model checking completed. No error has been found.' in text
errors=[x.strip() for x in text.splitlines() if x.strip().startswith('Error:')]
st['observed_diagnostics']=list(dict.fromkeys(errors+(['Model checking completed. No error has been found.'] if clean else [])))
st['first_failure_diagnostic']=errors[0] if errors else None
first=st['first_failure_diagnostic'] or ''
if not first: dclass='clean-completion' if clean else ('timeout' if sig=='timeout' else 'no-diagnostic')
elif first.startswith('Error: Invariant '): dclass='initial-invariant' if r.get('expected_phase')=='initial-invariant' or 'initial state' in first else 'invariant'
elif first.startswith('Error: Temporal properties were violated.'): dclass='temporal'
elif first.startswith('Error: Deadlock reached.'): dclass='deadlock'
elif 'Parsing or semantic analysis failed' in text or 'unexpected exception' in first: dclass='syntax-semantic'
else: dclass='tool-or-runtime'
st['diagnostic_class']=dclass; st['first_failure_phase']='timeout' if sig=='timeout' else ('tool-unavailable' if rc==125 else dclass)

op={}; source=[]
for line in text.splitlines():
 m=re.match(r'^\s*<([A-Za-z_][A-Za-z0-9_]*)\b([^>]*)>:\s*(\d+):\s*([^\s]+)',line)
 if m:
  name=m.group(1); meta=m.group(2); ent={'count':int(m.group(3)),'cost':m.group(4)}
  lm=re.search(r'line\s+(\d+).*?to line\s+(\d+).*?of module\s+([^>]+)',meta)
  if lm: ent.update(start_line=int(lm.group(1)),end_line=int(lm.group(2)),module=lm.group(3).strip())
  q=op.setdefault(name,{'count':0,'costs':[],'entries':[]}); q['count']+=ent['count']; q['costs'].append(ent['cost']); q['entries'].append(ent)
 m=re.match(r'^\s*\|*\s*line\s+(\d+),\s*col\s+\d+.*?to line\s+(\d+),\s*col\s+\d+\s+of module\s+([^:]+):\s*(\d+)\s*$',line)
 if m: source.append({'start_line':int(m.group(1)),'end_line':int(m.group(2)),'module':m.group(3).strip(),'count':int(m.group(4))})
if not op:
 for n,x in re.findall(r'(?m)^\s*(\d+):\s+([A-Za-z_][A-Za-z0-9_]*)\s*$',text): op[x]={'count':int(n),'costs':[],'entries':[]}
st['operator_coverage']=op; st['source_coverage']=source
trace=[]
for x in re.findall(r'(?m)^State\s+\d+:\s+<([A-Za-z_][A-Za-z0-9_]*)\b',text):
 if x not in trace: trace.append(x)
st['trace_actions']=trace
state_blocks=[]
heads=list(re.finditer(r'(?m)^State\s+(\d+):\s*(?:<([^>]+)>)?',text))
for j,hdr in enumerate(heads):
 end=heads[j+1].start() if j+1<len(heads) else text.find('\nThe coverage statistics',hdr.start())
 if end < 0: end=len(text)
 block=text[hdr.start():end]
 core_start=block.find('/\\ core = ['); owner_start=block.find('/\\ owner = [')
 core_part=block[core_start:owner_start] if core_start >= 0 and owner_start > core_start else ''
 owner_part=block[owner_start:] if owner_start >= 0 else ''
 def val(part,pattern,cast=lambda x:x):
  m=re.search(pattern,part,re.S)
  return cast(m.group(1)) if m else None
 def boolean(part,name):
  x=val(part,r'\b'+re.escape(name)+r' \|-> (TRUE|FALSE)')
  return None if x is None else x=='TRUE'
 pseq=val(core_part,r'pendingOp \|-> \[tuSeq \|-> (\d+)',int)
 ptx=val(core_part,r'pendingOp \|-> \[.*?transaction \|-> "([^"]+)"')
 c={
  'phase':val(core_part,r'\bphase \|-> "([^"]+)"'),
  'nextIndex':val(core_part,r'nextIndex \|-> (\d+)',int),
  'pendingTU':val(core_part,r'pendingTU \|-> ([^,}\]]+)'),
  'pendingOp_tuSeq':pseq,
  'pendingOp_transaction':ptx,
  'pendingOp':'empty' if pseq==3 and ptx=='no-transaction' else ('nonempty' if pseq is not None else None),
  'cLeaseState':val(core_part,r'cLeaseState \|-> "([^"]+)"'),
  'fLeaseState':val(core_part,r'fLeaseState \|-> "([^"]+)"'),
  'lastEvent':val(core_part,r'lastEvent \|-> "([^"]+)"'),
  'liability':val(core_part,r'liability \|-> "([^"]+)"'),
  'cCodec':val(core_part,r'cCodec \|-> "([^"]+)"'),
  'fCodec':val(core_part,r'fCodec \|-> "([^"]+)"'),
  'resetRequired':boolean(core_part,'resetRequired'),
  'oldRouteFenced':boolean(core_part,'oldRouteFenced'),
  'lossObserved':boolean(core_part,'lossObserved'),
  'commitOwed':boolean(core_part,'commitOwed'),
  'terminalReady':boolean(core_part,'terminalReady'),
  'preparedPresent':boolean(core_part,'preparedPresent'),
  'permitPresent':boolean(core_part,'permitPresent'),
  'permitConsumed':boolean(core_part,'permitConsumed'),
  'touchedFailurePending':boolean(core_part,'touchedFailurePending'),
  'settlementHighWater':val(core_part,r'settlementHighWater \|-> (\d+)',int),
 }
 o={
  'phase':val(owner_part,r'\bphase \|-> "([^"]+)"'),
  'stage':val(owner_part,r'\bstage \|-> "([^"]+)"'),
  'lastEvent':val(owner_part,r'lastEvent \|-> "([^"]+)"'),
  'durable':'empty' if re.search(r'durable \|-> no_bundle',owner_part) else ('nonempty' if re.search(r'durable \|-> \[',owner_part) else None),
  'cancelRequested':boolean(owner_part,'cancelRequested'),
 }
 label=re.match(r'(?m)^State\s+\d+:\s*(?:<([^>]+)>)?',block)
 state_blocks.append({'number':int(hdr.group(1)),'action':label.group(1) if label and label.group(1) else 'Initial predicate','core':c,'owner':o,'block_sha256':hashlib.sha256(block.encode()).hexdigest()})
st['trace_state_count']=len(state_blocks); st['trace_state_summaries']=state_blocks
st['final_state_summary']=state_blocks[-1] if state_blocks else None
st['pre_failure_state_summary']=state_blocks[-2] if len(state_blocks)>1 else (state_blocks[-1] if state_blocks else None)
def state_value(state,key):
 p=key.split('.',1)
 if len(p)!=2: return None
 return state.get(p[0],{}).get(p[1])
def state_match(state,spec):
 if not spec: return False
 if not state: return False
 for key,want in spec.items():
  got=state_value(state,key)
  if isinstance(want,list):
   if got not in want: return False
  elif want in {'any','present'}:
   if got is None: return False
  elif got != want:
   return False
 return True
if r.get('antecedent_state'):
 pos=r.get('antecedent_state_position','last')
 candidate=st['pre_failure_state_summary'] if pos=='previous' else st['final_state_summary']
 st['antecedent_state_position']=pos
 st['antecedent_state_observed']=state_match(candidate,r['antecedent_state'])
 st['antecedent_state_candidate']=candidate
else:
 st['antecedent_state_position']=None; st['antecedent_state_observed']=None; st['antecedent_state_candidate']=None
def definitions(path):
 lines=path.read_text(errors='replace').splitlines(); a=[]
 for i,line in enumerate(lines,1):
  m=re.match(r'^([A-Za-z_][A-Za-z0-9_]*)\s*==',line)
  if m: a.append((m.group(1),i))
 return [(n,x,(a[j+1][1]-1 if j+1<len(a) else len(lines))) for j,(n,x) in enumerate(a)]
def source_count(name):
 total=0
 for path in (root/'Protocol50ZstdRoute.tla',root/'Protocol50ZstdRouteFInputInterface.tla',root/'Protocol50ZstdRouteFInputComposition.tla'):
  if not path.exists(): continue
  for n,a,b in definitions(path):
   if n==name:
    total += sum(q['count'] for q in source if q['module']==path.stem and q['count']>0 and q['end_line']>=a and q['start_line']<=b)
 return total
def count(name):
 return op.get(name,{}).get('count',0) or source_count(name)
def detail(name): return {'count':count(name),'operator_count':op.get(name,{}).get('count',0),'source_count':source_count(name)}
target=r.get('target',''); target_name=target.split(':',1)[1] if target.startswith('temporal:') else (None if target=='Deadlock reached.' else target)
req=list(r.get('required_properties',[]))
if not req and target_name and r.get('expected_phase') in {'clean','temporal','invariant','initial-invariant'}: req=[target_name]
st['target_coverage']={x:detail(x) for x in req}
st['required_operator_coverage']={x:detail(x) for x in r.get('coverage_operators',[])}
st['coverage_operators_nonzero']={x:detail(x)['count']>0 for x in r.get('coverage_operators',[])}
ant=r.get('antecedent'); st['antecedent_action_coverage']=detail(ant) if ant not in {'positive-state','cancelled/reset-required','committed/settlement','context-loss/drain'} else {'count':None,'operator_count':0,'source_count':0}
marker=r.get('injection_marker','none'); st['injection_bytes_observed']=marker=='none' or marker in ((root/r['module']).read_text(errors='replace')+(root/r['config']).read_text(errors='replace'))
st['antecedent_observed']=st['antecedent_state_observed'] if r.get('antecedent_state') else (True if r.get('injection')=='static-contract' else (True if ant=='positive-state' else (False if ant in {'cancelled/reset-required','committed/settlement','context-loss/drain'} else st['antecedent_action_coverage']['count']>0)))
st['target_coverage_observed']=all(x['count']>0 for x in st['target_coverage'].values()) if req else (first=='Error: Deadlock reached.' if target=='Deadlock reached.' else False)
phase=r.get('expected_phase'); expected=r.get('expected','')
if phase == 'initial-invariant' and expected.startswith('initial:'):
 name=expected.split(':',1)[1]
 target_diag=bool(re.match(r'^Error: The invariant of '+re.escape(name)+r' is equal to FALSE$',first))
elif phase in {'invariant','initial-invariant'}: target_diag=bool(re.match(r'^Error: Invariant '+re.escape(expected)+r' is violated',first))
elif phase=='temporal': target_diag=first=='Error: Temporal properties were violated.'
elif phase=='deadlock': target_diag=first=='Error: Deadlock reached.'
else: target_diag=clean
st['diagnostic_result']=target_diag; st['expected_exit_match']=rc==int(r['expected_exit'])
st['complete_state_space']=bool(clean and st['queue_depth']==0 and nums)
st['required_properties_proven']=bool(clean and (not req or dclass=='clean-completion'))
if req and int(r['expected_exit'])==0:
 st['target_coverage_observed']=st['required_properties_proven']
operator_coverage_ok=(r.get('injection')=='static-contract' or all(st['required_operator_coverage'][x]['count']>0 for x in r.get('coverage_operators',[])))
# A failing invariant/property is identified by its first exact diagnostic and
# the exact antecedent state.  TLC normally cannot emit coverage for the
# failing target itself, so nonzero controls require only their declared
# executable witness operators; clean positive rows require the target/property
# proof as well.
st['required_coverage_ok']=operator_coverage_ok and (st['target_coverage_observed'] if int(r['expected_exit'])==0 else True)
st['first_relevant_failure']=first
reasons=[]
if sig=='timeout': st['status']='hold-timeout'; reasons.append('row timeout')
elif int(r['expected_exit'])==0:
 ok=rc==0 and dclass=='clean-completion' and st['complete_state_space'] and st['target_coverage_observed'] and st['required_coverage_ok'] and st['injection_bytes_observed']
 st['status']='pass' if ok else 'hold'
 if not ok: reasons.append('clean completion/queue/coverage obligation incomplete')
elif rc==125:
 st['status']='not-started'; reasons.append('tool unavailable')
else:
 ok=st['expected_exit_match'] and st['diagnostic_result'] and st['injection_bytes_observed'] and st['antecedent_observed'] and st['required_coverage_ok'] and sig!='timeout'
 if ok: st['status']='pass'
 elif st['antecedent_observed'] is False and st['diagnostic_result']: st['status']='hold'; reasons.append('expected failure without attributable antecedent reach')
 elif dclass in {'no-diagnostic','tool-or-runtime','syntax-semantic'}: st['status']='fail'; reasons.append('unexpected diagnostic class')
 else: st['status']='fail'; reasons.append('completed control did not match exact expected result')
st['status_reasons']=reasons
st['full_argv']=['java','-XX:+UseParallelGC','-cp',os.environ['JAR'],'tlc2.TLC','-workers',os.environ['TLC_WORKERS'],'-seed',os.environ['TLC_SEED'],'-fp',os.environ['TLC_FP_INDEX'],'-coverage','1','-metadir',str(state/'states'/r['id']),'-config',r['config'],r['module']]
st['cwd']=str(root/'cache/formal')
out.write_text(json.dumps(st,sort_keys=True)+'\n')
PY
}

python3 - "$MANIFEST" <<'PY' | while IFS= read -r row; do test -z "$row" || runrow "$row"; done
import json,sys
for x in open(sys.argv[1]):
 if x.strip(): print(json.dumps(json.loads(x),sort_keys=True))
PY

SELF=$(printenv RUNNER_SELF_CONTROLS 2>/dev/null || true); test -n "$SELF" || SELF=1
if test "$SELF" = 1; then
  python3 - "$STATE/self-controls.txt" <<'PY'
def decide(r,rc,signal,clean,queue,target,ant,coverage,first):
 if rc==125: return 'not-started'
 if signal=='timeout': return 'hold-timeout'
 if r['expected_exit']==0: return 'pass' if rc==0 and clean and queue==0 and target and ant and coverage and first=='clean' else 'hold'
 return 'pass' if rc==r['expected_exit'] and first==r['expected_phase'] and target and ant and coverage else 'fail'
cases=[
 ({'expected_exit':0,'expected_phase':'clean'},0,'none',True,0,True,True,True,'clean'),
 ({'expected_exit':0,'expected_phase':'clean'},1,'none',True,0,True,True,True,'clean'),
 ({'expected_exit':12,'expected_phase':'invariant'},12,'none',False,0,False,True,True,'temporal'),
 ({'expected_exit':12,'expected_phase':'invariant'},12,'none',False,0,True,True,False,'invariant'),
 ({'expected_exit':12,'expected_phase':'invariant'},12,'none',False,0,True,True,True,'invariant'),
 ({'expected_exit':12,'expected_phase':'invariant'},12,'none',False,0,True,True,True,'deadlock'),
 ({'expected_exit':12,'expected_phase':'invariant'},124,'timeout',False,None,False,False,False,'timeout'),
 ({'expected_exit':12,'expected_phase':'invariant'},125,'none',False,None,False,False,False,'tool')]
assert decide(*cases[0])=='pass'
assert decide(*cases[1])!='pass' and decide(*cases[2])!='pass' and decide(*cases[3])!='pass'
assert decide(*cases[4])=='pass' and decide(*cases[5])!='pass'
assert decide(*cases[6])=='hold-timeout' and decide(*cases[7])=='not-started'
open(__import__('sys').argv[1],'w').write('SELF_CONTROLS pass wrong-exit wrong-diagnostic zero-count-coverage unrelated-first timeout resume clean-success\n')
print('SELF_CONTROLS pass wrong-exit wrong-diagnostic zero-count-coverage unrelated-first timeout resume clean-success')
PY
fi

python3 - "$MANIFEST" "$STATE" <<'PY'
import json,os,pathlib,sys,time
m=pathlib.Path(sys.argv[1]); s=pathlib.Path(sys.argv[2]); rows=[]
for x in m.read_text().splitlines():
 if not x.strip(): continue
 r=json.loads(x); p=s/'rows'/(r['id']+'.json')
 if p.exists(): rows.append(json.loads(p.read_text()))
 else: rows.append({**r,'status':'not-started','raw_exit':None,'raw_wait_status':None,'wait_disposition':'not-started','observed_wait_disposition':'not-started','observed_diagnostics':[],'first_failure_diagnostic':None,'diagnostic_class':'not-started','first_failure_phase':'not-started','expected_exit_match':False,'diagnostic_result':False,'injection_bytes_observed':False,'antecedent_observed':False,'target_coverage_observed':False,'required_coverage_ok':False,'complete_state_space':False,'generated_states':0,'distinct_states':0,'queue_depth':None,'max_depth':None,'operator_coverage':{},'source_coverage':[],'trace_actions':[],'target_coverage':{},'required_operator_coverage':{},'antecedent_action_coverage':{'count':0},'status_reasons':['row not selected or not resumed']})
(s/'results.jsonl').write_text(''.join(json.dumps(r,sort_keys=True)+'\n' for r in rows))
c={x:sum(r['status']==x for r in rows) for x in ('pass','hold','hold-timeout','fail','not-started')}
selected=sum(r['status']!='not-started' for r in rows); full=selected==len(rows)
status='PASS' if full and c['hold']==c['hold-timeout']==c['fail']==c['not-started']==0 and os.environ['SANY_STATUS']=='pass' else 'HOLD'
json.dump({'status':status,'sany_status':os.environ['SANY_STATUS'],'sany_raw_exit':int(os.environ['SANY_RC']),'declared_rows':len(rows),'selected_rows':selected,'complete_matrix':full,'counts':c,'utc_end':time.strftime('%Y-%m-%dT%H:%M:%SZ',time.gmtime())},open(s/'suite-summary.json','w'),sort_keys=True,indent=2)
PY
python3 - "$STATE/REPORT.md" "$STATE/suite-summary.json" <<'PY'
import json,os,sys
s=json.load(open(sys.argv[2]))
open(sys.argv[1],'w').write('# S6 ZSTD_ROUTE/FInput composition V5 execution evidence\n\nNeutral formal-model, protocol-engineering, reproducibility and functional-QA evidence only. No S6/product/profile, candidate, merge, landing, tag, deployment or publication authority.\n\n- Commit: %s\n- Tree: %s\n- Sole direct parent: %s\n- Pinned TLC SHA-256: %s\n- Correction spec SHA-256: %s\n- SANY: %s (raw exit %s)\n- Declared/selected rows: %s/%s\n- Matrix status: %s\n- Counts: %s\n- BigOracle issue16 interpretation: HOLD; requested comments 5447981767 and 5448067827 were unavailable in frozen local evidence.\n\nPositive rows require clean completion, zero queue, exact target/property coverage and all declared witness coverage. Controls require exact exit/diagnostic order, source-bound changed marker, nonzero attributable antecedent coverage and target coverage. Timeout and incomplete rows remain HOLD.\n'%(os.environ['COMMIT'],os.environ['TREE'],os.environ['PARENT'],os.environ['JSHA'],os.environ['SPEC_SHA'],s['sany_status'],s['sany_raw_exit'],s['selected_rows'],s['declared_rows'],s['status'],json.dumps(s['counts'],sort_keys=True)))
PY

tar --sort=name --mtime='UTC 1970-01-01' --owner=0 --group=0 --numeric-owner -cf "$STATE/source-archive-1.tar" -C "$STATE/inputs" .
tar --sort=name --mtime='UTC 1970-01-01' --owner=0 --group=0 --numeric-owner -cf "$STATE/source-archive-2.tar" -C "$STATE/inputs" .
A1=$(sha256 "$STATE/source-archive-1.tar"); A2=$(sha256 "$STATE/source-archive-2.tar")
test "$A1" = "$A2" || { echo 'FAIL: deterministic archive mismatch' >&2; exit 2; }
python3 - "$STATE/archives.json" "$A1" "$A2" <<'PY'
import json,sys
json.dump({'format':'tar','sort':'name','mtime':'1970-01-01T00:00:00Z','owner':0,'group':0,'numeric_owner':True,'archive_1_sha256':sys.argv[2],'archive_2_sha256':sys.argv[3],'equal':sys.argv[2]==sys.argv[3]},open(sys.argv[1],'w'),sort_keys=True,indent=2)
PY
find "$STATE" -type f ! -name artifacts.sha256 -print0 | sort -z | xargs -0 sha256sum >"$STATE/artifacts.sha256"
printf '%s\n' "STATE_ROOT=$STATE" "RESULTS_JSONL=$STATE/results.jsonl" "SUMMARY=$(tr '\n' ' ' <"$STATE/suite-summary.json")"
chmod -R a-w "$STATE"
