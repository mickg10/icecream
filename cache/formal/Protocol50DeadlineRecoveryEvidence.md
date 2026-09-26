# Deadline/recovery TLC evidence

Pinned TLC jar SHA-256: `936a262061c914694dfd669a543be24573c45d5aa0ff20a8b96b23d01e050e88`.

Exact 19-row invocation:

```sh
TLA2TOOLS_JAR=/tanksmall/scratch/icecream-formal-tools/tlc-1.7.4/tla2tools.jar \
TLC_STATE_ROOT=/tanksmall/scratch/tmp/p51-deadline-recovery/final-run3 \
sh cache/formal/run_deadline_recovery_tlc.sh
```

Runner exit: `0`. Final output: `DEADLINE-RECOVERY-TLC PASS rows=19`.
Model SHA-256: `3b77cecec7f0b809e28e77c6ee9e1fbee705e4347520d7887cf48992ba78a471`.
Runner SHA-256: `00f794fe8f7d040197b83e77141dd45dd9ea0e8f33ec10ca19d20486c7d9be58`.
Full retained logs: `/tanksmall/scratch/tmp/p51-deadline-recovery/final-run3/deadline-recovery/`.

Each table log is named `<row>.log` in that directory. The clean C-expiry-first
rows all have 59,253 generated / 13,700 distinct states at depth 26; clean
F-expiry-first rows have 31,857 / 7,548 at depth 24. The row-specific log
SHA-256 authenticates the TLC terminal result and statistics.

| Row | Config SHA-256 | Result | Log SHA-256 |
|---|---|---|---|
| clean-c1f2-c-expiry | `399e485f2f135aa2366261b219f20d8c6679cbac60bbf7724a5880df2e385e28` | PASS 59253/13700 d26 | `bdeaac131ea40e4e81955c029671bc7bf984743a88ebbc183420e4b777e605f9` |
| clean-c1f3-c-expiry | `64f83760365533d335d653bfb5d71a315961d2a068a7c872d1d88eb8b7f113df` | PASS 59253/13700 d26 | `3664ee6fd1878a4fa4f207ef0ad5099c4ec2b295d5fc30b41be693d7b8fc3613` |
| clean-c1f4-c-expiry | `5bf6b896886de47b0e1c7b346da0e40a507d2478ef64af9587f32612b8be9632` | PASS 59253/13700 d26 | `77f83f0b0a826e634cf08fceb7da36ef52ed388ab2aec40e5f678a5fd5634e18` |
| clean-c2f1-c-expiry | `ef5cc9262b50f1a71334c93c0d5a84863c829661a3a30a3755001c50253300ef` | PASS 59253/13700 d26 | `0b33b3c64b776018f2c7bb0cf9c788368bb3d23491451740a10fa87d49d602a6` |
| clean-c3f1-c-expiry | `b6e94558dc28d86da28ed31cc809260d4a3cd7dbbd9c8148834ababa4c915d15` | PASS 59253/13700 d26 | `93d449901cb9f40b3b293e2c4792fa290424f2f0c57f6cbedc4bbc933389676d` |
| clean-c4f1-c-expiry | `795af78585b4392e1e4435727e82917fa579e84dda6dbf9413fef844857ba1dd` | PASS 59253/13700 d26 | `2fdacbde2b4bf509ef337bc5cb3a7a07ac33ed929cfbadf71baee4fa960f916c` |
| clean-c1f2-f-expiry | `9a99ae5789a16082edb983c58120dd350e97e113ba518a5c01a00e31a31be021` | PASS 31857/7548 d24 | `fa3c1c47633d58c8f195e8985c6d84f607a7443535b8d5076be7b7dc13becc3a` |
| clean-c1f3-f-expiry | `a737b9d0c0a5d4f5cf4d160f9477ae15512efe39ad6e81327db733eb2e8b4f86` | PASS 31857/7548 d24 | `c3f9d7daeb0b98abc8c02736a434509b6dc32dea0b309315dca43e1269183a16` |
| clean-c1f4-f-expiry | `7918e581184130337a83bfc7930f9e51aaf05f133f12455815346275d1a5e443` | PASS 31857/7548 d24 | `cf78071d7e58e1834031fc7cf2077e7f1706748621cf118c1d3528c6a1e80e05` |
| clean-c2f1-f-expiry | `ff6efcebb989b564cb2b99df16e123e33301cdb9cb4d904ca453d3fa02610dc2` | PASS 31857/7548 d24 | `b550823b5bf2484f6f57b0e54cacd33f481b3a8db9990dba1dfa2ff8aca64640` |
| clean-c3f1-f-expiry | `417fe4347ee345142d1f97025f542919eb1f5610e6c8276bd1ba44bd28f4d363` | PASS 31857/7548 d24 | `af1c7e99cd872a0d95e7ab007e4509160fefc4d2611fccfc32ae57fba6abb990` |
| clean-c4f1-f-expiry | `6d15a671e086c2de1da50f718454c4bced0e8b429d3b350d2664b52ea46fede0` | PASS 31857/7548 d24 | `c2eef85c184c58e2b2f790ac7019f5aceafa4e276ac9fe7f2e2871731147ff77` |
| urgent-caller-expiry-admission | `abf775ec3c0ea909069c71948591a3cb1744472c81159296dedfd5ff14a84c37` | PASS 1477/448 d20 | `0df564a378f49a00bfd5864cb5abbdda17111708a23275c8c1928b35e594ad60` |
| mutant-drop-trigger | `67845b7fb05877fc665bfd5f562942e8037bb8ba0ed12f99940325ef3b80b5bf` | expected temporal violation, exit 13 | `b9e58780e7a783212094a0c769926065410210e3237d84b56b27a49065769b73` |
| mutant-disable-retirement | `155fb2508903d9c0df06d162cab0727221a5d17521875c198beb13044d4a02e6` | expected temporal violation, exit 13 | `d1dd27f3bb1e104ce94a466d06433da3ece09ed69eb16c351bc8368a802a1824` |
| mutant-renew-cleanup | `f2d19f2cfbec469dd0bb26ad52a815aa67faacfe22839207c2ce76bbe891974a` | expected `CleanupNotRenewed`, exit 12 | `3a3901d1c4ef8387f4b7c52c2346e823480cc0058adee0f214d70ba880ca0704` |
| mutant-replay-expired | `45ae70c79403a87cde28fb77f67a3b86ddebc54100413b04e49566e63f7ec5cc` | expected `NoExpiredReplay`, exit 12 | `9857a98cace91bfba09670898c0e8f643258499d46100831086c24519308d7d2` |
| mutant-write-after-expiry | `f8f6f4120627cd54023d632b363ed3b75a4db20ecb4ebc4a7a9fc96eb13d5455` | expected `NoExpiredReplay`, exit 12 | `8a4bf8817a2a74ce5262e820cbf1339bdca7f7b56bf62c05e8d294e31e90698b` |
| mutant-gate-sibling | `4ba8314a8bf414be461342e348de425f9d1e7bd2858dd5a2d4f5c8d9ef8b4062` | expected `SiblingProgressEnabled`, exit 12 | `f8d4b9d42c3ee9bc5f00e3caa860ad8fa6d231e374f25007f30f0f8ed4e16516` |

The post-expiry-write mutant trace is intentionally checked separately from
expired replay: it first binds a replay while C is live, advances past the C
deadline, then writes and violates `NoExpiredReplay`.
