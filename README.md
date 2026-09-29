# c-concurrency-kit

[![CI](https://github.com/codissa/c-concurrency-kit/actions/workflows/ci.yml/badge.svg)](https://github.com/codissa/c-concurrency-kit/actions/workflows/ci.yml)

Lock-free and low-level concurrency primitives in portable **C11** (`<stdatomic.h>` + pthreads),
with explicit memory ordering, cache-line aware layouts, stress tests, and benchmarks.
Every commit is tested under **AddressSanitizer, UndefinedBehaviorSanitizer and ThreadSanitizer**
with both GCC and Clang.

| Component | Header | Progress guarantee | Notes |
|---|---|---|---|
| SPSC ring buffer | `ck/spsc_ring.h` | wait-free | cached indices, head/tail on separate cache lines |
| MPMC bounded queue | `ck/mpmc_queue.h` | lock-free | Vyukov per-cell sequence numbers, one CAS per op |
| TTAS spinlock | `ck/spinlock.h` | blocking | test-and-test-and-set + exponential backoff |
| Ticket lock | `ck/spinlock.h` | blocking, FIFO-fair | no starvation |
| Thread pool | `ck/thread_pool.h` | blocking | mutex/condvar work queue, nested submit, `wait()` barrier |
| Slab allocator | `ck/slab.h` | lock-free | Treiber free-list with 32-bit ABA tag packed in a 64-bit CAS |

## Build

Requires Linux (or WSL) with GCC or Clang.

```sh
make            # library + tests + benchmark
make test       # run tests
make asan       # tests under ASan + UBSan
make tsan       # tests under ThreadSanitizer
make bench      # throughput benchmarks
```

> On recent kernels TSan may abort with `unexpected memory mapping`; run
> `sudo sysctl vm.mmap_rnd_bits=28` (or `setarch $(uname -m) -R make tsan`).

## Design notes

### SPSC ring: `ck_spsc_push`/`ck_spsc_pop`
* `head` (consumer-owned) and `tail` (producer-owned) are `alignas(64)` so the two cores never
  false-share a cache line.
* Indices are free-running `size_t` counters masked with `capacity - 1`. Full is
  `tail - head == capacity` and empty is `head == tail`, so no slot is wasted.
* The producer writes the slot, then publishes it with a **release** store to `tail`. The
  consumer's **acquire** load of `tail` makes the slot contents visible. The reverse
  handshake on `head` stops the producer from overwriting a slot that is still being read.
* Each side caches the other side's index (`cached_head` / `cached_tail`) and re-reads the
  shared atomic only when the cached value says full or empty. In steady state most
  operations touch no shared cache line at all.

### MPMC queue: Vyukov's bounded queue
Each cell holds a `seq` counter. A producer at position `pos` may write a cell only when
`seq == pos`, and a consumer may read it only when `seq == pos + 1`. Claiming a position is one
CAS on `enqueue_pos` / `dequeue_pos`, and handing the cell over is a release store to `seq`.
Because `seq` grows by `capacity` every lap, stale observers can always tell whether a
cell belongs to them, which rules out ABA.

### Slab allocator: ABA-safe Treiber stack in 64 bits
The free-list head packs `{tag:32 | index:32}`. Every push and pop increments the tag, so the
classic ABA interleaving (A reads head=X, next=Y; B pops X, pops Y, pushes X; A's CAS succeeds
with a dangling Y) fails because the tag has changed. Next-links live in a separate
`_Atomic uint32_t` array, **not inside the free objects**. A thread holding a stale index can
therefore read a link while another thread is writing user data into the object, and there is
still no data race.

### Spinlocks
* **TTAS + backoff** spins on a relaxed load, which keeps the line in Shared state, and only
  attempts the `xchg` when the lock looks free.
* The **ticket lock** is FIFO-fair. The benchmark shows the cost of that fairness: when
  threads outnumber free cores (for example under virtualization), a preempted ticket holder
  blocks everyone queued behind it.

## Benchmarks

`make bench`, 1M items per producer, on an 8-vCPU laptop VM (Docker on Windows). Numbers
are indicative only.

```
Queue throughput (Mops/s, higher is better)
queue           1P/1C    2P/2C    4P/4C    8P/1C
spsc_ring        26.2      n/a      n/a      n/a
mpmc_queue       13.1      8.3      5.9      5.0
mutex_ring        2.8      1.9      3.8      1.1

Lock throughput (M acquisitions/s)
lock               1T       4T       8T
tas_backoff     131.3    134.5   106.4
ticket          113.3      4.2      1.1
pthread          79.1     20.6     19.4
```

Takeaways:
* The SPSC ring is about 9x faster than a mutex-protected ring.
* The lock-free MPMC queue is 2-5x faster than the mutex ring at every thread count.
* The backoff TTAS lock gets the highest throughput because it is unfair: the thread that
  already holds the cache line tends to win again. The ticket lock falls off sharply once
  threads are preempted while waiting in line.

## Tests

* `tests/test_queues.c`: FIFO semantics and wrap-around. A 2-thread SPSC ordering test over
  1M items. A 4P/4C MPMC stress test that checks every item is delivered exactly once and that
  each consumer sees each producer's items in order.
* `tests/test_sync.c`: mutual exclusion for both locks, trylock semantics, the thread pool
  (20k tasks, recursive task spawning, draining on destroy), and the slab allocator
  (exhaustion, LIFO reuse, and an 8-thread alloc/free storm that stamps each object to
  detect double allocation).

## License

MIT
