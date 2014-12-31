#include "Scheduler.hxx"

#include <sys/mman.h>
#
#include <errno.h>
#
#include <utility>
#
#include "Error.hxx"
#include "Fiber.hxx"
#include "Log.hxx"
#include "RunFiber.hxx"
#include "UnwindStack.hxx"

#define TARA_FIBER_SIZE 65536

namespace Tara {

namespace {

Fiber *CreateFiber(const Coroutine &coroutine);
Fiber *CreateFiber(Coroutine &&coroutine);
void DestroyFiber(Fiber *fiber);
void FiberStart(Scheduler *scheduler) noexcept;

} // namespace

Scheduler::Scheduler()
  : fiberCount_(0), context_(nullptr), status_(0), runningFiber_(nullptr)
{
  QUEUE_INIT(&readyFiberQueue_);
  QUEUE_INIT(&deadFiberQueue_);
}

void Scheduler::callCoroutine(const Coroutine &coroutine)
{
  Fiber *fiber;
  if (!QUEUE_EMPTY(&deadFiberQueue_)) {
    fiber = QUEUE_DATA(QUEUE_HEAD(&deadFiberQueue_), Fiber, queueItem);
    QUEUE_REMOVE(&fiber->queueItem);
    const_cast<Coroutine &>(fiber->coroutine) = coroutine;
  } else {
    fiber = CreateFiber(coroutine);
    ++fiberCount_;
  }
  QUEUE_INSERT_TAIL(&readyFiberQueue_, &fiber->queueItem);
}

void Scheduler::callCoroutine(Coroutine &&coroutine)
{
  Fiber *fiber;
  if (!QUEUE_EMPTY(&deadFiberQueue_)) {
    fiber = QUEUE_DATA(QUEUE_HEAD(&deadFiberQueue_), Fiber, queueItem);
    QUEUE_REMOVE(&fiber->queueItem);
    const_cast<Coroutine &>(fiber->coroutine) = std::move(coroutine);
  } else {
    fiber = CreateFiber(std::move(coroutine));
    ++fiberCount_;
  }
  QUEUE_INSERT_TAIL(&readyFiberQueue_, &fiber->queueItem);
}

void Scheduler::run()
{
  assert(runningFiber_ == nullptr);
  if (fiberCount_ == 0) {
    return;
  }
  for (;;) {
    if (!QUEUE_EMPTY(&readyFiberQueue_)) {
      jmp_buf context;
      if (setjmp(context) != 0) {
        goto no_ready_fiber;
      }
      context_ = &context;
      status_ = 1;
      auto fiber = QUEUE_DATA(QUEUE_HEAD(&readyFiberQueue_), Fiber, queueItem);
      QUEUE_REMOVE(&fiber->queueItem);
      executeFiber(fiber);
    }
no_ready_fiber:
    if (!QUEUE_EMPTY(&deadFiberQueue_)) {
      QUEUE *q = QUEUE_HEAD(&deadFiberQueue_);
      do {
        auto fiber = QUEUE_DATA(q, Fiber, queueItem);
        q = QUEUE_NEXT(q);
        DestroyFiber(fiber);
        --fiberCount_;
      } while (q != &deadFiberQueue_);
      QUEUE_INIT(&deadFiberQueue_);
      if (fiberCount_ == 0) {
        break;
      }
    }
    {
      QUEUE fiberQueue;
      QUEUE_INIT(&fiberQueue);
      while (!ioPoll_.waitForEvents(timer_.calculateTimeout(), &fiberQueue));
      QUEUE *q;
      QUEUE_FOREACH(q, &fiberQueue) {
        auto fiber = QUEUE_DATA(q, Fiber, queueItem);
        timer_.removeItem(&fiber->timerItem);
      }
      if (!QUEUE_EMPTY(&fiberQueue)) {
        QUEUE_ADD(&readyFiberQueue_, &fiberQueue);
      }
    }
    {
      TimerItem *buffer[1024];
      unsigned int n = timer_.removeDueItems(buffer, TARA_LENGTH_OF(buffer));
      for (int i = 0; i < n; ++i) {
        auto fiber = TARA_CONTAINER_OF(buffer[i], Fiber, timerItem);
        if (fiber->fd >= 0) {
          ioPoll_.removeEventAwaiter(fiber->queueItem, fiber->fd);
          fiber->fd = -1;
          fiber->status = -ETIME;
        }
        QUEUE_INSERT_TAIL(&readyFiberQueue_, &fiber->queueItem);
      }
    }
  }
}

void Scheduler::execute()
{
  runningFiber_ = nullptr;
  assert(context_ != nullptr);
  assert(status_ != 0);
  longjmp(*context_, status_);
}

void Scheduler::executeFiber(Fiber *fiber)
{
  assert(fiber != nullptr);
  runningFiber_ = fiber;
  if (fiber->context == nullptr) {
    RunFiber(FiberStart, this, fiber->stack);
  }
  assert(fiber->status != 0);
  longjmp(*fiber->context, fiber->status);
}

void Scheduler::yieldCurrentFiber()
{
  assert(runningFiber_ != nullptr);
  if (QUEUE_EMPTY(&readyFiberQueue_)) {
    return;
  }
  jmp_buf context;
  if (setjmp(context) != 0) {
    return;
  }
  runningFiber_->context = &context;
  runningFiber_->status = 1;
  QUEUE_INSERT_TAIL(&readyFiberQueue_, &runningFiber_->queueItem);
  auto fiber = QUEUE_DATA(QUEUE_HEAD(&readyFiberQueue_), Fiber, queueItem);
  QUEUE_REMOVE(&fiber->queueItem);
  executeFiber(fiber);
}

void Scheduler::sleepCurrentFiber(int duration)
{
  assert(runningFiber_ != nullptr);
  jmp_buf context;
  if (setjmp(context) != 0) {
    return;
  }
  runningFiber_->context = &context;
  runningFiber_->status = 1;
  timer_.addItem(&runningFiber_->timerItem, duration);
  if (QUEUE_EMPTY(&readyFiberQueue_)) {
    execute();
  }
  auto fiber = QUEUE_DATA(QUEUE_HEAD(&readyFiberQueue_), Fiber, queueItem);
  QUEUE_REMOVE(&fiber->queueItem);
  executeFiber(fiber);
}

void Scheduler::exitCurrentFiber() const
{
  assert(runningFiber_ != nullptr);
  throw UnwindStack();
}

void Scheduler::killCurrentFiber()
{
  assert(runningFiber_ != nullptr);
  runningFiber_->context = nullptr;
  runningFiber_->status = 0;
  QUEUE_INSERT_TAIL(&deadFiberQueue_, &runningFiber_->queueItem);
  if (QUEUE_EMPTY(&readyFiberQueue_)) {
    execute();
  }
  auto fiber = QUEUE_DATA(QUEUE_HEAD(&readyFiberQueue_), Fiber, queueItem);
  QUEUE_REMOVE(&fiber->queueItem);
  executeFiber(fiber);
}

void Scheduler::unwatchIO(int fd)
{
  QUEUE fiberQueue;
  QUEUE_INIT(&fiberQueue);
  ioPoll_.removeEventAwaiters(fd, &fiberQueue);
  QUEUE *q;
  QUEUE_FOREACH(q, &fiberQueue) {
    auto fiber = QUEUE_DATA(q, Fiber, queueItem);
    timer_.removeItem(&fiber->timerItem);
    fiber->status = -EBADF;
  }
  if (!QUEUE_EMPTY(&fiberQueue)) {
    QUEUE_ADD(&readyFiberQueue_, &fiberQueue);
  }
}

int Scheduler::awaitIOEvent(int fd, IOEvent ioEvent, int timeout)
{
  assert(runningFiber_ != nullptr);
  jmp_buf context;
  int status = setjmp(context);
  if (status != 0) {
    if (status < 0) {
      errno = -status;
      return -1;
    }
    return 0;
  }
  runningFiber_->context = &context;
  runningFiber_->status = 1;
  runningFiber_->fd = fd;
  ioPoll_.addEventAwaiter(&runningFiber_->queueItem, fd, ioEvent);
  timer_.addItem(&runningFiber_->timerItem, timeout);
  if (QUEUE_EMPTY(&readyFiberQueue_)) {
    execute();
  }
  auto fiber = QUEUE_DATA(QUEUE_HEAD(&readyFiberQueue_), Fiber, queueItem);
  QUEUE_REMOVE(&fiber->queueItem);
  executeFiber(fiber);
}

namespace {

Fiber *CreateFiber(const Coroutine &coroutine)
{
  auto region = static_cast<char *>(mmap(nullptr, TARA_FIBER_SIZE,
                                         PROT_READ | PROT_WRITE,
                                         MAP_PRIVATE | MAP_ANONYMOUS
                                                     | MAP_GROWSDOWN
                                                     | MAP_STACK, -1, 0));
  if (region == nullptr) {
    TARA_FATALITY_LOG("mmap failed: ", Error(errno));
  }
  auto fiber = reinterpret_cast<Fiber *>(region + TARA_FIBER_SIZE
                                                - sizeof(Fiber));
  void *fiberStack = fiber;
  static_cast<void>(new (fiber) Fiber(coroutine, fiberStack));
  return fiber;
}

Fiber *CreateFiber(Coroutine &&coroutine)
{
  auto region = static_cast<char *>(mmap(nullptr, TARA_FIBER_SIZE,
                                         PROT_READ | PROT_WRITE,
                                         MAP_PRIVATE | MAP_ANONYMOUS
                                                     | MAP_GROWSDOWN
                                                     | MAP_STACK, -1, 0));
  if (region == nullptr) {
    TARA_FATALITY_LOG("mmap failed: ", Error(errno));
  }
  auto fiber = reinterpret_cast<Fiber *>(region + TARA_FIBER_SIZE
                                                - sizeof(Fiber));
  void *fiberStack = fiber;
  static_cast<void>(new (fiber) Fiber(std::move(coroutine), fiberStack));
  return fiber;
}

void DestroyFiber(Fiber *fiber)
{
  assert(fiber != nullptr);
  fiber->~Fiber();
  auto region = reinterpret_cast<char *>(fiber + 1) - TARA_FIBER_SIZE;
  if (munmap(region, TARA_FIBER_SIZE) < 0) {
    TARA_ERROR_LOG("munmap failed: ", Error(errno));
  }
}

void FiberStart(Scheduler *scheduler) noexcept
{
  assert(scheduler != nullptr);
  Fiber *fiber = scheduler->getCurrentFiber();
  try {
    fiber->coroutine();
  } catch (const UnwindStack &) {}
  scheduler->killCurrentFiber();
}

} // namespace

} // namespace Tara