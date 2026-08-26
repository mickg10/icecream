# S2 pre-bound listener and two-role prototype

The structured supervisor launch now allocates one private `0700` lease
directory and binds `cache.sock` before `fork()`.  The parent passes the
listener as `ICECC_CACHE_SERVICE_LISTENER_FD`; the pre-exec child clears
`FD_CLOEXEC` on that descriptor only, then the service drops its configured
UID/GID and adopts the socket with `fstat(2)`.  It does not bind or unlink the
pathname in this mode.  The supervisor remains the sole pathname/listener
owner and performs identity-checked deletion after the child group is proven
dead.  The old unstructured command-line mode remains for compatibility.

Structured `READY v2` contains one generation/attempt/PID/listener identity
and both nonzero, unequal domain-separated GUIDs:

```
READY v2 generation=... attempt=... pid=... C_STORE_GUID=... F_STORE_GUID=... PATH=... DIGEST=... DEV=... INO=...
```

`ClientRoleOwner` and `ServerRoleOwner` are deliberately small pre-adoption
skeletons.  Each has independent limits/counters, accepts only its typed
`RoleDiscriminator`, and requires the same C/F namespace before wrapping an
FD.  The focused role test proves C/F overlap between the two role domains
and closes a wrong-role descriptor before ownership is returned.  Full
CompileFile/session wiring is intentionally deferred.
