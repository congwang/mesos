/**
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *  http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#ifndef __STOUT_OS_PERF_HPP__
#define __STOUT_OS_PERF_HPP__

// This file contains Linux-only OS utilities.
#ifndef __linux__
#error "stout/os/perf.hpp is only available on Linux systems."
#endif

#include <stout/try.hpp>

#include <sys/syscall.h>
#include <linux/perf_event.h>

namespace os {
#if defined(__i386__)
#define rmb()           asm volatile("lock; addl $0,0(%%esp)" ::: "memory")
#ifndef __NR_perf_event_open
# define __NR_perf_event_open 336
#endif
#endif

#if defined(__x86_64__)
#define rmb()           asm volatile("lfence" ::: "memory")
#ifndef __NR_perf_event_open
# define __NR_perf_event_open 298
#endif
#endif

inline int perf_event_open(struct perf_event_attr *event_attr,
    pid_t pid,
    int cpu,
    int group_fd,
    unsigned long flags)
{
  int ret;

  ret = syscall(__NR_perf_event_open, event_attr, pid, cpu, group_fd, flags);
#if defined(__x86_64__) || defined(__i386__)
  if (ret < 0 && ret > -4096) {
    errno = -ret;
    ret = -1;
  }
#endif
  return ret;
}

inline Try<int> perf_read_buffer(struct perf_event_mmap_page *header,
    size_t pgmsk,
    char *buf,
    size_t size)
{
  char *data;
  uint64_t data_head, data_tail;
  uint64_t data_size, ncopies;

  // The first page is a meta-data page (struct perf_event_mmap_page),
  // so move to the second page which contains the perf data.
  data = ((char *)header) + sysconf(_SC_PAGESIZE);

  // data_tail points to the position where userspace last read,
  // data_head points to the position where kernel last add.
  // After read data_head value, need to issue a rmb().
  data_tail = header->data_tail;
  data_head = header->data_head;
  rmb();

  // The kernel function "perf_output_space()" guarantees no data_head can
  // wrap over the data_tail.
  if ((data_size = data_head - data_tail) < size) {
    return Error("perf_read_buffer: avaible size less than needed size");
  }

  data_tail &= pgmsk;

  // Need to consider if data_head is wrapped when copy data.
  if ((ncopies = (pgmsk + 1 - data_tail)) < size) {
    memcpy(buf, data + data_tail, ncopies);
    memcpy(buf + ncopies, data, size - ncopies);
  } else {
    memcpy(buf, data + data_tail, size);
  }

  header->data_tail += size;
  return 0;
}

inline void perf_skip_buffer(struct perf_event_mmap_page *header, int size)
{
  uint64_t data_head;

  data_head = header->data_head;
  rmb();

  if ((header->data_tail + size) > data_head) {
    size = data_head - header->data_tail;
  }

  header->data_tail += size;
}

} // namespace os {

#endif // __STOUT_OS_PERF_HPP__
