#!/bin/sh
set -eu

BOARD_HOST="${BOARD_HOST:-192.168.3.17}"
BOARD_USER="${BOARD_USER:-root}"
BOARD_PASS="${BOARD_PASS:-root}"
BOARD_DIR="${BOARD_DIR:-/root}"
TARGET="${TARGET:-pa_controller}"
BOARD_RUN_ARGS="${BOARD_RUN_ARGS:--d /dev/ttyS1 -b 115200}"

make deploy-board BOARD_HOST="${BOARD_HOST}" BOARD_USER="${BOARD_USER}" BOARD_PASS="${BOARD_PASS}" BOARD_DIR="${BOARD_DIR}" TARGET="${TARGET}"
sshpass -p "${BOARD_PASS}" ssh -o StrictHostKeyChecking=no "${BOARD_USER}@${BOARD_HOST}" "'${BOARD_DIR}/${TARGET}' ${BOARD_RUN_ARGS}"
