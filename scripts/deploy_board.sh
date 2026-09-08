#!/bin/sh
set -eu

BOARD_HOST="${BOARD_HOST:-192.168.3.52}"
BOARD_USER="${BOARD_USER:-root}"
BOARD_PASS="${BOARD_PASS:-root}"
BOARD_DIR="${BOARD_DIR:-/root}"
TARGET="${TARGET:-pa_controller}"

make
sshpass -p "${BOARD_PASS}" scp -o StrictHostKeyChecking=no "build/bin/${TARGET}" "${BOARD_USER}@${BOARD_HOST}:${BOARD_DIR}/${TARGET}"
sshpass -p "${BOARD_PASS}" ssh -o StrictHostKeyChecking=no "${BOARD_USER}@${BOARD_HOST}" "chmod +x '${BOARD_DIR}/${TARGET}'"
