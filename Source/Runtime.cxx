#include "Runtime.hxx"

#include <fcntl.h>
#include <sys/eventfd.h>
#include <unistd.h>
#
#include <errno.h>
#
#include <utility>
#
#include "IOEvent.hxx"
#include "Log.hxx"
#include "Scheduler.hxx"

namespace Tara {

thread_local Scheduler *TheScheduler;

void Call(const Coroutine &coroutine)
{
  if (TheScheduler == nullptr) {
    TARA_FATALITY_LOG("No scheduler");
  }
  if (coroutine != nullptr) {
    TheScheduler->callCoroutine(coroutine);
  }
}

void Call(Coroutine &&coroutine)
{
  if (TheScheduler == nullptr) {
    TARA_FATALITY_LOG("No scheduler");
  }
  if (coroutine != nullptr) {
    TheScheduler->callCoroutine(std::move(coroutine));
  }
}

void Yield()
{
  if (TheScheduler == nullptr) {
    TARA_FATALITY_LOG("No scheduler");
  }
  TheScheduler->yieldCurrentFiber();
}

void Sleep(int duration)
{
  if (TheScheduler == nullptr) {
    TARA_FATALITY_LOG("No scheduler");
  }
  TheScheduler->sleepCurrentFiber(duration);
}

void Exit()
{
  if (TheScheduler == nullptr) {
    TARA_FATALITY_LOG("No scheduler");
  }
  TheScheduler->exitCurrentFiber();
}

int Open(const char *path, int flags, mode_t mode)
{
  if (TheScheduler == nullptr) {
    TARA_FATALITY_LOG("No scheduler");
  }
  int fd;
  do {
    fd = open(path, flags | O_NONBLOCK, mode);
    if (fd >= 0) {
      break;
    }
  } while (errno == EINTR);
  if (fd < 0) {
    return -1;
  }
  TheScheduler->watchIO(fd);
  return fd;
}

int Pipe2(int *fds, int flags)
{
  if (TheScheduler == nullptr) {
    TARA_FATALITY_LOG("No scheduler");
  }
  if (pipe2(fds, flags | O_NONBLOCK) < 0) {
    return -1;
  }
  TheScheduler->watchIO(fds[0]);
  TheScheduler->watchIO(fds[1]);
  return 0;
}

int Socket(int domain, int type, int protocol)
{
  if (TheScheduler == nullptr) {
    TARA_FATALITY_LOG("No scheduler");
  }
  int fd = socket(domain, type | SOCK_NONBLOCK, protocol);
  if (fd < 0) {
    return -1;
  }
  TheScheduler->watchIO(fd);
  return fd;
}

int EventFD(unsigned int initval, int flags)
{
  if (TheScheduler == nullptr) {
    TARA_FATALITY_LOG("No scheduler");
  }
  int fd = eventfd(initval, flags | EFD_NONBLOCK);
  if (fd < 0) {
    return -1;
  }
  TheScheduler->watchIO(fd);
  return fd;
}

int Close(int fd)
{
  if (TheScheduler == nullptr) {
    TARA_FATALITY_LOG("No scheduler");
  }
  if (!TheScheduler->ioIsWatched(fd)) {
    errno = EBADF;
    return -1;
  }
  int result;
  do {
    result = close(fd);
    if (result >= 0) {
      break;
    }
  } while (errno == EINTR);
  if (result < 0) {
    TheScheduler->unwatchIO(fd);
    return -1;
  }
  TheScheduler->unwatchIO(fd);
  return 0;
}

ssize_t Read(int fd, void *buf, size_t buflen, int timeout)
{
  if (TheScheduler == nullptr) {
    TARA_FATALITY_LOG("No scheduler");
  }
  if (!TheScheduler->ioIsWatched(fd)) {
    errno = EBADF;
    return -1;
  }
  ssize_t n;
  for (;;) {
    n = read(fd, buf, buflen);
    if (n >= 0) {
      break;
    }
    if (errno == EWOULDBLOCK) {
      if (TheScheduler->awaitIOEvent(fd, IOEvent::Readability, timeout) < 0) {
        break;
      }
      continue;
    }
    if (errno == EINTR) {
      continue;
    }
    break;
  }
  if (n < 0) {
    return -1;
  }
  return n;
}

ssize_t Write(int fd, const void *buf, size_t buflen, int timeout)
{
  if (TheScheduler == nullptr) {
    TARA_FATALITY_LOG("No scheduler");
  }
  if (!TheScheduler->ioIsWatched(fd)) {
    errno = EBADF;
    return -1;
  }
  ssize_t n;
  for (;;) {
    n = write(fd, buf, buflen);
    if (n >= 0) {
      break;
    }
    if (errno == EWOULDBLOCK) {
      if (TheScheduler->awaitIOEvent(fd, IOEvent::Writability, timeout) < 0) {
        break;
      }
      continue;
    }
    if (errno == EINTR) {
      continue;
    }
    break;
  }
  if (n < 0) {
    return -1;
  }
  return n;
}

int Accept4(int fd, sockaddr *addr, socklen_t *addrlen, int flags, int timeout)
{
  if (TheScheduler == nullptr) {
    TARA_FATALITY_LOG("No scheduler");
  }
  if (!TheScheduler->ioIsWatched(fd)) {
    errno = EBADF;
    return -1;
  }
  int subfd;
  for (;;) {
    subfd = accept4(fd, addr, addrlen, flags);
    if (subfd >= 0) {
      break;
    }
    if (errno == EWOULDBLOCK) {
      if (TheScheduler->awaitIOEvent(fd, IOEvent::Readability, timeout) < 0) {
        break;
      }
      continue;
    }
    if (errno == EINTR) {
      continue;
    }
    break;
  }
  if (subfd < 0) {
    return -1;
  }
  return subfd;
}

int Connect(int fd, const sockaddr *addr, socklen_t addrlen, int timeout)
{
  if (TheScheduler == nullptr) {
    TARA_FATALITY_LOG("No scheduler");
  }
  if (!TheScheduler->ioIsWatched(fd)) {
    errno = EBADF;
    return -1;
  }
  if (connect(fd, addr, addrlen) < 0) {
    if (errno != EINTR && errno != EINPROGRESS) {
      return -1;
    }
    if (TheScheduler->awaitIOEvent(fd, IOEvent::Writability, timeout) < 0) {
      return -1;
    }
    int optval;
    socklen_t optlen = sizeof optval;
    if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &optval, &optlen) < 0) {
      return -1;
    }
    if (optval != 0) {
      errno = optval;
      return -1;
    }
  }
  return 0;
}

} // namespace Tara
