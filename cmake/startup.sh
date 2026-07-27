#!/bin/bash
#
# ctest fixture SETUP for the `runners` fixture.
#
# Intended use: bring up the service dependencies the suite needs — a database, a
# message broker, a stub HTTP server, a container — once, before any discovered
# test runs, rather than from inside every test binary. Per-binary setup belongs
# in test/main.cpp instead; this script is for state shared by the whole run.
#
# The wiring lives in test/CMakeLists.txt: this script is registered as the
# `startup` test with FIXTURES_SETUP runners, and every discovered test carries
# FIXTURES_REQUIRED runners. So ctest schedules it first and re-adds it even when
# you filter — `ctest -R <name>` reports three tests, not one, and `-FS . -FC .`
# excludes the fixtures again.
#
# Contract: exit 0 means the fixture is up. A nonzero exit fails `startup`, and
# ctest then reports every test that requires the fixture as `***Not Run` with
# "Failed test dependencies: startup" rather than running and failing them — one
# honest error instead of N confusing ones. (The cleanup fixture still runs.)
#
# This ships as a deliberate no-op so a fresh clone is green with nothing
# installed. Replace the body with the real thing, e.g.:
#
#   docker compose -f test/docker-compose.yml up -d --wait
#   ./scripts/wait-for-port.sh localhost 5432 30
#
# Keep the executable bit (mode 100755): ctest runs this by path, with no
# interpreter. `check_artifacts.cmake` rule B5 enforces it.

set -euo pipefail

exit 0
