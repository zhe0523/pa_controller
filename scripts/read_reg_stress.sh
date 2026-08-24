#!/bin/sh
#
# 使用 devmem 周期性读取 PA/PU AXI-lite 寄存器，用于单独压测寄存器读路径。
# 默认每 1ms 读取一次 0x40000000(INT_VECTOR)，count=0 表示一直读。

set -u

BASE_ADDR=0x40000000
OFFSET=0x0
ADDR=
WIDTH=32
INTERVAL_US=1000
COUNT=0
PRINT_EVERY=1

usage() {
  cat <<'EOF'
Usage:
  read_reg_stress.sh [options]

Options:
  --base HEX          PA/PU 基地址，默认 0x40000000
  --offset HEX        寄存器偏移，默认 0x0；最终地址 = base + offset
  --addr HEX          直接指定绝对地址；指定后忽略 base/offset
  --width N           devmem 访问宽度，默认 32
  --interval-us N     读取间隔，默认 1000，即 1ms
  --count N           读取次数，默认 0 表示一直循环
  --print-every N     每 N 次打印一次，默认 1；压测时可设为 100/1000 降低串口输出压力
  -h, --help          显示帮助

Examples:
  ./read_reg_stress.sh --offset 0x0 --interval-us 1000 --count 1000
  ./read_reg_stress.sh --addr 0x40000000 --interval-us 1000 --count 0 --print-every 100
  ./read_reg_stress.sh --offset 0x09a0 --interval-us 1000 --count 10000
EOF
}

hex_to_dec() {
  # busybox ash 支持 $((0x...))，这里统一从字符串转为十进制，方便做地址加法。
  printf '%u' "$(($1))"
}

sleep_interval() {
  if [ "$INTERVAL_US" -le 0 ]; then
    return 0
  fi

  if command -v usleep >/dev/null 2>&1; then
    usleep "$INTERVAL_US"
  else
    # 兼容没有 usleep 的系统；busybox sleep 是否支持小数取决于构建选项。
    sleep "$(awk "BEGIN { printf \"%.6f\", $INTERVAL_US / 1000000 }")"
  fi
}

while [ "$#" -gt 0 ]; do
  case "$1" in
    --base)
      BASE_ADDR="$2"
      shift 2
      ;;
    --offset)
      OFFSET="$2"
      shift 2
      ;;
    --addr)
      ADDR="$2"
      shift 2
      ;;
    --width)
      WIDTH="$2"
      shift 2
      ;;
    --interval-us)
      INTERVAL_US="$2"
      shift 2
      ;;
    --count)
      COUNT="$2"
      shift 2
      ;;
    --print-every)
      PRINT_EVERY="$2"
      shift 2
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      echo "unknown option: $1" >&2
      usage >&2
      exit 2
      ;;
  esac
done

if ! command -v devmem >/dev/null 2>&1; then
  echo "devmem not found" >&2
  exit 1
fi

if [ -z "$ADDR" ]; then
  BASE_DEC=$(hex_to_dec "$BASE_ADDR")
  OFFSET_DEC=$(hex_to_dec "$OFFSET")
  ADDR_DEC=$((BASE_DEC + OFFSET_DEC))
  ADDR=$(printf '0x%08x' "$ADDR_DEC")
fi

if [ "$PRINT_EVERY" -le 0 ]; then
  PRINT_EVERY=1
fi

echo "read_reg_stress start addr=$ADDR width=$WIDTH interval_us=$INTERVAL_US count=$COUNT print_every=$PRINT_EVERY"

i=0
while :; do
  i=$((i + 1))
  value=$(devmem "$ADDR" "$WIDTH")
  ret=$?
  if [ "$ret" -ne 0 ]; then
    echo "read_reg_stress devmem failed iteration=$i addr=$ADDR ret=$ret" >&2
    exit "$ret"
  fi

  if [ $((i % PRINT_EVERY)) -eq 0 ]; then
    printf 'iteration=%u addr=%s value=%s\n' "$i" "$ADDR" "$value"
  fi

  if [ "$COUNT" -ne 0 ] && [ "$i" -ge "$COUNT" ]; then
    break
  fi

  sleep_interval
done

echo "read_reg_stress done iteration=$i addr=$ADDR"
