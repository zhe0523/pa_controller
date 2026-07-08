#include "rs422.h"

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

#include "log.h"

static speed_t baud_to_speed(int baud) {
  switch (baud) {
  case 9600: return B9600;
  case 19200: return B19200;
  case 38400: return B38400;
  case 57600: return B57600;
  case 115200: return B115200;
  case 230400: return B230400;
  case 460800: return B460800;
  case 921600: return B921600;
  default: return B115200;
  }
}

int rs422_open(rs422_t* port, const char* device, int baud) {
  if (port == NULL || device == NULL) {
    return -1;
  }

  port->fd = -1;

  /* O_NOCTTY 避免该串口成为本进程控制终端，适合后台服务长期运行。 */
  port->fd = open(device, O_RDWR | O_NOCTTY | O_CLOEXEC);
  if (port->fd == -1) {
    log_error("open rs422 %s failed: %d", device, errno);
    return -1;
  }

  struct termios tio;
  memset(&tio, 0, sizeof(tio));
  if (tcgetattr(port->fd, &tio) != 0) {
    log_error("tcgetattr %s failed: %d", device, errno);
    rs422_close(port);
    return -1;
  }

  /* 422 使用原始 8N1 模式，不启用软件流控和行规程转换。 */
  cfmakeraw(&tio);
  tio.c_cflag |= CLOCAL | CREAD;
  tio.c_cflag &= (tcflag_t)~CSTOPB;
  tio.c_cflag &= (tcflag_t)~PARENB;
  tio.c_cflag &= (tcflag_t)~CRTSCTS;
  tio.c_cflag &= (tcflag_t)~CSIZE;
  tio.c_cflag |= CS8;
  tio.c_cc[VTIME] = 10; /* read 最长等待约 1 秒，便于主循环检查退出标志。 */
  tio.c_cc[VMIN] = 0;   /* 没有数据时允许 read 返回 0，而不是永久阻塞。 */

  speed_t speed = baud_to_speed(baud);
  cfsetispeed(&tio, speed);
  cfsetospeed(&tio, speed);

  if (tcsetattr(port->fd, TCSANOW, &tio) != 0) {
    log_error("tcsetattr %s failed: %d", device, errno);
    rs422_close(port);
    return -1;
  }

  tcflush(port->fd, TCIOFLUSH);
  log_info("rs422 opened %s baud=%d", device, baud);
  return 0;
}

void rs422_close(rs422_t* port) {
  if (port != NULL && port->fd >= 0) {
    close(port->fd);
    port->fd = -1;
  }
}

int rs422_read_line(rs422_t* port, char* buffer, size_t size) {
  if (port == NULL || port->fd < 0 || buffer == NULL || size < 2) {
    return -1;
  }

  size_t pos = 0;
  while (pos + 1 < size) {
    char ch;
    ssize_t n = read(port->fd, &ch, 1);
    if (n < 0) {
      if (errno == EINTR) {
        /* 被信号打断时重试，让 Ctrl+C 由 main 循环的 g_stop 处理。 */
        continue;
      }
      return -1;
    }
    if (n == 0) {
      continue;
    }
    if (ch == '\n') {
      break;
    }
    if (ch != '\r') {
      /* 协议允许 \n 或 \r\n 结尾，内部统一保存不带换行的命令字符串。 */
      buffer[pos++] = ch;
    }
  }

  buffer[pos] = '\0';
  return (int)pos;
}

int rs422_write_text(rs422_t* port, const char* text) {
  if (port == NULL || port->fd < 0 || text == NULL) {
    return -1;
  }

  size_t len = strlen(text);
  const char* p = text;
  while (len > 0) {
    ssize_t n = write(port->fd, p, len);
    if (n <= 0) {
      if (errno == EINTR) {
        /* 写串口时被信号打断，继续写剩余数据。 */
        continue;
      }
      return -1;
    }
    p += n;
    len -= (size_t)n;
  }
  return 0;
}
