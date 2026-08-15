# corpus4 -- Abseil + protobuf.  The only TWO-PROJECT corpus: two independent CMake
# trees are preprocessed into corpus4/abseil and corpus4/protobuf and share one
# manifest, so cross-project (not just cross-TU) redundancy is measurable.
#
#   abseil:   whole tree with tests, using a LOCAL googletest checkout
#             (ABSL_LOCAL_GOOGLETEST_DIR) so nothing is fetched at configure time.
#   protobuf: only entries under protobuf/src/ are kept -- upb, upb_generator and the
#             Abseil that protobuf FetchContent's into build/_deps are dropped, so
#             corpus4's protobuf half does not double-count corpus4's abseil half.
PROJECT=abseil+protobuf
CHECKOUT="abseil-cpp protobuf googletest"
GIT_URL="https://github.com/abseil/abseil-cpp.git https://github.com/protocolbuffers/protobuf.git https://github.com/google/googletest.git"
GIT_REF=
CLONE_ARGS=
SRC_SUBDIR=.
MODE=multi
FILTER=1
PP_LIMIT=0
EXPECTED_TU=700           # 430 abseil + 270 protobuf
ABSEIL_CMAKE_ARGS=(-DCMAKE_CXX_STANDARD=17 -DABSL_BUILD_TESTING=ON -DBUILD_TESTING=ON
                   -DABSL_USE_EXTERNAL_GOOGLETEST=OFF -DABSL_LOCAL_GOOGLETEST_DIR=@SRCROOT@/googletest)
PROTOBUF_CMAKE_ARGS=(-DCMAKE_CXX_STANDARD=17 -Dprotobuf_BUILD_TESTS=OFF
                     -Dprotobuf_BUILD_CONFORMANCE=OFF -Dprotobuf_BUILD_EXAMPLES=OFF
                     -Dprotobuf_BUILD_LIBPROTOC=OFF -DABSL_BUILD_TESTING=OFF)
# Keep only compile_commands entries under protobuf/src/.  Anchored at the checkout,
# not the bare substring "/src/", so a SRCROOT that itself contains a src/ component
# cannot accidentally match everything.
PROTOBUF_KEEP_PREFIX=@SRCROOT@/protobuf/src/
