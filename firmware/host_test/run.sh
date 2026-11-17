#!/bin/sh
# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 Jamie Saker
# Build and run the firmware's host tests with the system C compiler.
set -e
here=$(dirname "$0")
main=$here/../main
odid=$here/../components/opendroneid
out=${TMPDIR:-/tmp}

cc -std=gnu17 -Wall -Wextra -Werror -I "$main" "$here/test_session.c" "$main/session.c" -o "$out/chickadee_test_session"
"$out/chickadee_test_session"

# The vendored encoder is compiled without warnings-as-errors; our own code is not.
cc -std=gnu17 -w -DODID_DISABLE_PRINTF -c "$odid/opendroneid.c" -o "$out/chickadee_odid.o"
cc -std=gnu17 -Wall -Wextra -Werror -I "$main" -I "$odid" "$here/test_rid.c" "$main/rid.c" "$out/chickadee_odid.o" -lm -o "$out/chickadee_test_rid"
"$out/chickadee_test_rid"

# The command handler and control loop, against a stand-in radio. This one
# needs cJSON, which ships with ESP-IDF.
cjson=${CJSON_DIR:-${IDF_PATH:+$IDF_PATH/components/json/cJSON}}
if [ -n "$cjson" ] && [ -f "$cjson/cJSON.c" ]; then
    cc -std=gnu17 -w -c "$cjson/cJSON.c" -o "$out/chickadee_cjson.o"
    cc -std=gnu17 -Wall -Wextra -Werror -Wno-unused-function \
        -I "$here/stubs" -I "$main" -I "$odid" -I "$cjson" \
        "$here/test_control.c" "$main/session.c" "$main/rid.c" \
        "$out/chickadee_odid.o" "$out/chickadee_cjson.o" -lm -o "$out/chickadee_test_control"
    "$out/chickadee_test_control"
else
    echo "skipped the command and radio-control tests: activate ESP-IDF (or set CJSON_DIR) to run them"
fi
