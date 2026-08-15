# corpus6 -- Godot game engine.
# GOTCHA: SCons, not CMake, and a FULL build is required first: Godot generates a large
# number of .gen.h files (shaders, gdvirtual, version, certs) that every TU includes, so
# -E fails wholesale until the build has run.  compile_commands.json comes from Godot's
# own `compiledb` target and lands in the checkout root, not in a build dir.
PROJECT=Godot
CHECKOUT=godot
GIT_URL=https://github.com/godotengine/godot.git
GIT_REF=
CLONE_ARGS=
SRC_SUBDIR=.
MODE=scons
FILTER=1
PP_LIMIT=0
EXPECTED_TU=2207
SCONS_ARGS=(platform=linuxbsd target=editor)
