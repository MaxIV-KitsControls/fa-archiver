#!/bin/sh

# Wrapper script for launching fa_zoomer application build by Matlab compiler,
# derived from fa_zoomer_bin script generated by the same tool.

MCRROOT='@MCRROOT@'
ARCH='@ARCH@'

# Joins arguments into a : separated path expression
join() { sep=; for x; do echo -n "$sep$x"; sep=:; done; }

export LD_LIBRARY_PATH="$(join \
    "$MCRROOT"/runtime/$ARCH \
    "$MCRROOT"/bin/$ARCH \
    "$MCRROOT"/sys/os/$ARCH)"

exec "$(dirname "$0")"/fa_zoomer_bin "$@"
