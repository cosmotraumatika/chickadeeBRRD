#!/bin/sh
# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 Jamie Saker
# Build and run the firmware's host tests with the system C compiler.
set -e
here=$(dirname "$0")
out=${TMPDIR:-/tmp}/chickadee_test_session
cc -std=gnu17 -Wall -Wextra -Werror -I "$here/../main" "$here/test_session.c" "$here/../main/session.c" -o "$out"
"$out"
