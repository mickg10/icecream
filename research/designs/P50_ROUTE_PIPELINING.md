# P50 Route Pipelining — Design Doc (DRAFT, WIP)

Status: DRAFT — skeleton, sections being filled in. No implementation code in this
tree change; this document only.

Branch: `perf-pipe`, off `a348414e` ("services/sys_close_range.h: guard the UAPI
include with `__has_include`"), the current farm "perf-combo" build
(instrumentation + robustness fixes + perf-opt).

Author: pipelining designer session, reviewed by sorbet-main before any code lands.

## 0. Problem statement and goal

(TODO)

## 1. C-side and F-side state-machine diffs

### 1.1 C side: predicted-state bookkeeping

(TODO)

### 1.2 F side: parking

(TODO)

## 2. Invariants that must still hold under K>1

(TODO)

## 3. Failure matrix

(TODO)

## 4. Interactions (ARM admission, route lock, deadlines, lifecycle RPCs, telemetry)

(TODO)

## 5. Test plan

### 5.1 Unit tests

(TODO)

### 5.2 Sim (cache/sim/p50sim.cpp)

(TODO)

### 5.3 TLA+

(TODO)

### 5.4 Farm A/B plan

(TODO)

## 6. LOC estimate and implementation order

(TODO)

## 7. Open questions for sorbet-main

(TODO)
