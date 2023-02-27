#include <sys/time.h>


unsigned long long getCurrentMilliseconds() {
  timeval time;
  gettimeofday(&time, 0);
  return (time.tv_sec * (unsigned long long )1000) + (time.tv_usec / 1000);
}
