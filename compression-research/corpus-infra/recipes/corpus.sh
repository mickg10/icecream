# corpus (no suffix) -- LLVM, the reference corpus.
#
# ODD ONE OUT: LLVM is NOT preprocessed with a -E replay.  It is configured with
# -save-temps=obj so the real ninja build drops a .ii next to every object, and a
# time-bounded build harvests whatever it produced.  Consequence: the TU count is a
# function of how much of LLVM got built in BUILD_SECONDS, not of the project, so it
# is only approximately reproducible.  1238 TUs is what 720 s at -j12 yielded here.
PROJECT=LLVM
CHECKOUT=llvm-project
GIT_URL=https://github.com/llvm/llvm-project.git
GIT_REF=
CLONE_ARGS=
SRC_SUBDIR=llvm
MODE=savetemps
FILTER=0
PP_LIMIT=0
EXPECTED_TU=1238
BUILD_SECONDS=720
CMAKE_ARGS=(
  -DCMAKE_BUILD_TYPE=Release
  -DLLVM_TARGETS_TO_BUILD=X86
  -DLLVM_ENABLE_PROJECTS=""
  -DLLVM_ENABLE_ASSERTIONS=OFF
  -DCMAKE_CXX_COMPILER=g++ -DCMAKE_C_COMPILER=gcc
  -DCMAKE_CXX_FLAGS=-save-temps=obj
)
