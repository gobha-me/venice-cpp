#!/bin/bash
#
# ctest fixture CLEANUP for the `runners` fixture — the mirror of
# cmake/startup.sh. Tear down whatever that script brought up.
#
# Registered in test/CMakeLists.txt with FIXTURES_CLEANUP runners, so ctest runs
# it after the last test that required the fixture. Note it runs even when the
# suite went badly: if `startup` itself failed, the tests are skipped as
# `***Not Run` but this script is still executed.
#
# Which is exactly why it must be idempotent. It can run when startup never got
# as far as creating the thing being removed, so tolerate "not there" rather than
# failing on it:
#
#   docker compose -f test/docker-compose.yml down -v --remove-orphans || true
#
# A cleanup script that fails turns a green suite red for a reason that has
# nothing to do with the code under test.
#
# This ships as a deliberate no-op. Keep the executable bit (mode 100755): ctest
# runs it by path, with no interpreter. `check_artifacts.cmake` rule B5 enforces
# it.

set -euo pipefail

exit 0
