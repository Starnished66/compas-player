/* Force-included into charge_limiter.c by the USB tests: its AXP I2C transfers
 * go to the test's fake register file, everything else to the real calls. */
#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>
int fake_i2c_open(const char *path, int flags, ...);
int fake_i2c_ioctl(int fd, unsigned long request, ...);
int fake_i2c_close(int fd);
#define open fake_i2c_open
#define ioctl fake_i2c_ioctl
#define close fake_i2c_close
