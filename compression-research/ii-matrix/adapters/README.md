# Project adapter interface

`run_cell.sh` invokes one script from this directory inside the selected profile image.
See the environment and output contract in the parent README.

The committed `from-compile-commands.sh` adapter is the common final step: a project-specific
adapter may configure/code-generate first, set `II_COMPILE_COMMANDS`, and invoke it.  The 25
existing corpus recipes are being converted into thin profile-aware adapters; until a named
adapter exists here, that cell is not claimed runnable by this branch.
