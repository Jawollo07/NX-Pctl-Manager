# Plutonium's supported Nintendo Switch build uses the devkitPro Makefile.
# The former CMake configuration built Borealis and is intentionally retired.
message(FATAL_ERROR
    "NX-Pctl-Manager now uses Plutonium. Build with devkitPro by running 'make' from the repository root. See README.md for dependencies.")
