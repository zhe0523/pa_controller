#!/bin/sh
set -eu

INSTALL_DIR="${1:-/root}"
SERVICE_FILE="/etc/systemd/system/pa_controller.service"

if [ ! -x "${INSTALL_DIR}/pa_controller" ]; then
  echo "找不到可执行文件: ${INSTALL_DIR}/pa_controller" >&2
  exit 1
fi

sed "s#^WorkingDirectory=.*#WorkingDirectory=${INSTALL_DIR}#; s#^ExecStart=.*#ExecStart=${INSTALL_DIR}/pa_controller#" \
  "$(dirname "$0")/pa_controller.service" > "${SERVICE_FILE}"
chmod 0644 "${SERVICE_FILE}"
systemctl daemon-reload
systemctl enable pa_controller.service
systemctl restart pa_controller.service
systemctl --no-pager --full status pa_controller.service || true
