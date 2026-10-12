#!/bin/sh
# Runs the ThreadSanitizer build of the server (build-tsan/chat_server).
# setarch -R turns off address-space randomization for this one process:
# newer kernels randomize memory so widely that TSan can't start otherwise
# ("FATAL: ThreadSanitizer: unexpected memory mapping").
#
#   cmake -S . -B build-tsan -DCHAT_SANITIZER=thread && cmake --build build-tsan
#   TSAN_OPTIONS=halt_on_error=1 python3 tests/smoke_test.py tools/tsan_server.sh 4
exec setarch "$(uname -m)" -R "$(dirname "$0")/../build-tsan/chat_server" "$@"
