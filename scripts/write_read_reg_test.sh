#!/bin/sh
#
# 使用 devmem 对 PA/PU AXI-lite 寄存器做“先写后读”的一致性测试。
# 适合单独压测寄存器写通道、读通道以及写后生效延时；读回不一致时立即报错退出。

set -u

BASE_ADDR=0x40000000
OFFSET=0x0
ADDR=
WIDTH=32
VALUE=0x0
COUNT=1
READ_DELAY_US=1000
INTERVAL_US=0
PRINT_EVERY=1
CONTINUE_ON_ERROR=0

usage() {
  cat <<'EOF'
Usage:
  write_read_reg_test.sh [options]

Options:
  --base HEX              PA/PU 基地址，默认 0x40000000
  --offset HEX            寄存器偏移，默认 0x0；最终地址 = base + offset
  --addr HEX              直接指定绝对地址；指定后忽略 base/offset
  --width N               devmem 访问宽度，默认 32
  --value HEX|DEC         写入值，默认 0x0
  --count N               测试次数，默认 1；0 表示一直循环
  --read-delay-us N       写完到读回之间的延时，默认 1000，即 1ms
  --interval-us N         每轮测试之间的额外延时，默认 0
  --print-every N         每 N 次打印一次成功信息，默认 1
  --continue-on-error     读回不一致时继续测试，只统计错误数
  -h, --help              显示帮助

Examples:
  ./write_read_reg_test.sh --offset 0x0808 --value 0x0000b400 --count 1000 --read-delay-us 1000
  ./write_read_reg_test.sh --addr 0x40000810 --value 7680 --count 100 --read-delay-us 100
  ./write_read_reg_test.sh --offset 0x0818 --value 0x0c00 --count 0 --interval-us 1000 --print-every 100
EOF
}

hex_to_dec() {
  # busybox ash 支持 $((0x...))；这里统一转十进制，方便地址计算和读回比较。
  printf '%u' "$(($1))"
}

mask_for_width() {
  case "$WIDTH" in
    8)
      printf '%u' 0xff
      ;;
    16)
      printf '%u' 0xffff
      ;;
    32)
      printf '%u' 0xffffffff
      ;;
    *)
      # 非标准宽度不做额外掩码，交给 devmem 自身处理。
      printf '%u' 0xffffffffffffffff
      ;;
  esac
}

sleep_us() {
  delay_us="$1"
  if [ "$delay_us" -le 0 ]; then
    return 0
  fi

  if command -v usleep >/dev/null 2>&1; then
    usleep "$delay_us"
  else
    # 兼容没有 usleep 的系统；busybox sleep 是否支持小数取决于构建选项。
    sleep "$(awk "BEGIN { printf \"%.6f\", $delay_us / 1000000 }")"
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
    --value)
      VALUE="$2"
      shift 2
      ;;
    --count)
      COUNT="$2"
      shift 2
      ;;
    --read-delay-us)
      READ_DELAY_US="$2"
      shift 2
      ;;
    --interval-us)
      INTERVAL_US="$2"
      shift 2
      ;;
    --print-every)
      PRINT_EVERY="$2"
      shift 2
      ;;
    --continue-on-error)
      CONTINUE_ON_ERROR=1
      shift
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

MASK=$(mask_for_width)
EXPECTED_DEC=$(hex_to_dec "$VALUE")
EXPECTED_DEC=$((EXPECTED_DEC & MASK))
EXPECTED_HEX=$(printf '0x%08x' "$EXPECTED_DEC")

echo "write_read_reg_test start addr=$ADDR width=$WIDTH value=$EXPECTED_HEX count=$COUNT read_delay_us=$READ_DELAY_US interval_us=$INTERVAL_US print_every=$PRINT_EVERY continue_on_error=$CONTINUE_ON_ERROR"

i=0
errors=0
while :; do
  i=$((i + 1))

  devmem "$ADDR" "$WIDTH" "$VALUE" >/dev/null
  ret=$?
  if [ "$ret" -ne 0 ]; then
    echo "write_read_reg_test write failed iteration=$i addr=$ADDR value=$VALUE ret=$ret" >&2
    exit "$ret"
  fi

  sleep_us "$READ_DELAY_US"

  read_value=$(devmem "$ADDR" "$WIDTH")
  ret=$?
  if [ "$ret" -ne 0 ]; then
    echo "write_read_reg_test read failed iteration=$i addr=$ADDR ret=$ret" >&2
    exit "$ret"
  fi

  actual_dec=$(hex_to_dec "$read_value")
  actual_dec=$((actual_dec & MASK))
  actual_hex=$(printf '0x%08x' "$actual_dec")

  if [ "$actual_dec" -ne "$EXPECTED_DEC" ]; then
    errors=$((errors + 1))
    echo "write_read_reg_test mismatch iteration=$i addr=$ADDR width=$WIDTH expect=$EXPECTED_HEX actual=$actual_hex raw=$read_value errors=$errors" >&2
    if [ "$CONTINUE_ON_ERROR" -eq 0 ]; then
      exit 1
    fi
  elif [ $((i % PRINT_EVERY)) -eq 0 ]; then
    printf 'iteration=%u addr=%s value=%s ok errors=%u\n' "$i" "$ADDR" "$actual_hex" "$errors"
  fi

  if [ "$COUNT" -ne 0 ] && [ "$i" -ge "$COUNT" ]; then
    break
  fi

  sleep_us "$INTERVAL_US"
done

echo "write_read_reg_test done iteration=$i addr=$ADDR value=$EXPECTED_HEX errors=$errors"

if [ "$errors" -ne 0 ]; then
  exit 1
fi
