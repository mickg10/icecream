# Icecream 1.5.0 release checklist

This checklist applies to the `sorbet_v1.5` checkout. A successful local
check does not authorize pushing a branch, creating a tag, or publishing a
GitHub release.

1. Start from a clean checkout and verify `configure.ac`, generated metadata,
   `S1B_EXIT_MANIFEST.md`, and installed-identity checks all report 1.5.0.
2. Run `./autogen.sh` in the source checkout. From a separate build directory
   on the selected scratch storage, invoke `/absolute/source/path/configure`,
   then `make -j2`, `make check`, and `make distcheck`; resolve every failure.
   See the [native build example](README.md#build-and-test).
   Set `ICEFARM_TMPDIR` and run native commands through
   `sh /absolute/source/path/dev/python.sh --exec COMMAND ...` so distribution
   checks and their Python subprocesses share the locked environment.
3. Run package-builder dependency-contract and release-manifest checks.
4. Run local integration checks with an explicit scratch directory. Keep
   Docker/farm qualification results separate from source-release checks.
5. Run `make dist` from the clean out-of-tree build and retain archive hashes.
6. Extract an archive into a separate directory and verify it independently:

   ```sh
   ./configure --prefix="$PWD/install"
   make -j2
   make check
   ```

7. Only after review approval, push the release branch, create the requested
   tag, and publish the GitHub release with exact archive hashes.

## Version policy

Official releases use `1.X.Y`. Development branches may use a `.90` suffix,
but this branch prepares version 1.5.0. Update `configure.ac` and all release
identity checks together; do not retain the old 1.4.90 example as an
instruction for this release.

## Distribution verification

`make distcheck` must run from the generated out-of-tree build. The package
archive must contain the explicit distribution manifest and no local build
outputs. Check both compressed archives, then perform the clean extraction
test above on at least one archive and record the compiler/platform used.
