# corpus13 -- RE2 regular-expression engine.
# GOTCHA: RE2 is PINNED to the last pre-Abseil commit.  Current RE2 hard-depends on
# Abseil, which would drag another project's headers into this corpus (and needs a
# separate Abseil install to configure at all).  Do not un-pin without re-checking
# what ends up in the .ii.
PROJECT=re2
CHECKOUT=re2
GIT_URL=https://github.com/google/re2.git
GIT_REF=0dade9ff39bb6276f18dd6d4bc12d3c20479ee24   # last pre-Abseil RE2 (2021-10-28)
CLONE_ARGS=
SRC_SUBDIR=.
MODE=ccjson
FILTER=1
PP_LIMIT=0
EXPECTED_TU=72
CMAKE_ARGS=(-DCMAKE_CXX_STANDARD=17 -DRE2_BUILD_TESTING=ON)
