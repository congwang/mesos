/**
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <signal.h>
#include <stdlib.h>
#include <unistd.h>

#include <sys/prctl.h>
#include <sys/types.h>
#include <sys/wait.h>

#include <list>
#include <ostream>
#include <tuple>
#include <vector>

#include <process/clock.hpp>
#include <process/collect.hpp>
#include <process/defer.hpp>
#include <process/io.hpp>

#include <stout/strings.hpp>
#include <stout/unreachable.hpp>

#include "common/status_utils.hpp"

#include "linux/perf.hpp"

using namespace process;

using process::await;

using std::list;
using std::ostringstream;
using std::set;
using std::string;
using std::tuple;
using std::vector;

namespace sched {

namespace internal {


class SchedSampler : public Process<SchedSampler>
{
public:
  SchedSampler(const vector<string>& _argv, const Duration& _duration)
    : duration(_duration)
  {
    traceEvents["sched_switch"] = TraceEventSchedSwitch();
    traceEvents["sched_migrate_task"] = TraceEventSchedMigrateTask();
    traceEvents["sched_wakeup"] = TraceEventSchedWakeup();
    traceEvents["sched_wakeup_new"] = TraceEventSchedWakeup();
    traceEvents["sched_process_fork"] = TraceEventSchedProcessFork();
  }

  void initialize()
  {
    sample();
  }

private:

  char *perf_parse_raw(struct perf_event_mmap_page *hdr,
                       unsigned int pgmsk)
  {
    size_t sz = 0;
    uint32_t raw_sz, i;
    char *buf;
    int ret;

    ret = perf_read_buffer(hdr, pgmsk, (char *)&raw_sz, sizeof(uint32_t));
    if (ret) {
      return NULL;
    }

    sz += sizeof(raw_sz);
    buf = (char *)malloc(raw_sz);
    if (!buf) {
      return NULL;
    }

    ret = os::perf_read_buffer(hdr, pgmsk, buf, raw_sz);
    if (ret) {
      free(buf);
      return NULL;
    }
    return buf;
  }


  void perf_parse_trace_event(const char *raw, TraceEvent& event, int cpu, uint64_t timestamp)
  {
    uint64_t val = 0;
    PerfEvent e(cpu, timestamp, event);

    foreach(const TraceEventField field, event.format) {
      // We only care about integers.
      if (field.size == 4) {
        uint32_t tmp;
        memcpy(&tmp, raw + field.offset, field.size);
        val = tmp;
      } else if (field.size == 8) {
        memcpy(&val, raw + it->offset, it->size);
      } else {
        continue;
      }
      e[field.name] = val;
    }

    // Do NOT process the event here, instead save it to an ordered map
    // so that the time-order is guaranteed.
    results[timestamp] = e;
  }


  Option<TraceEvent&> fd2event(int fd)
  {
    foreachvalue(const TraceEvent& event, traceEvents) {
      if (eventperf_fds.find(fd) != event.perf_fds.end())
        return event;
    }

    return Nothing();
  }

  void handleEvent(int fd)
  {
    size_t pgsz = sysconf(_SC_PAGESIZE);
    size_t pgmsk = (buffer_pages * pgsz) - 1;
    struct perf_event_header ehdr;
    char *raw = NULL;
    uint64_t time;
    int cpu, res;
    int ret;

    Option<TraceEvent&> event = fd2event(fd);
    if (event.isNothing()) {
      return;
    }

    struct perf_event_mmap_page *buffer = event.get().perf_fds[fd];
    ehdr.size = 0;

    while (!(ret = os::perf_read_buffer(buffer, pgmsk, (char *)&ehdr, sizeof(ehdr)))) {

      if (ehdr.type == PERF_RECORD_LOST) {
        uint64_t id;
        os::perf_read_buffer(buffer, pgmsk, (char *)&id, sizeof(id));
        goto skip;
      }

      if (ehdr.type != PERF_RECORD_SAMPLE)
        goto skip;

      if (ehdr.size < sizeof(time) + sizeof(cpu) + sizeof(res))
        goto skip;

      os::perf_read_buffer(buffer, pgmsk, (char *)&time, sizeof(time));
      os::perf_read_buffer(buffer, pgmsk, (char *)&cpu, sizeof(cpu));
      os::perf_read_buffer(buffer, pgmsk, (char *)&res, sizeof(res));

      raw = perf_parse_raw(buffer, pgmsk);
      if (raw)
        perf_parse_trace_event(raw, event.get(), cpu, time);
      free(raw);
    }

skip:
    // mark sample as consumed
    os::perf_skip_buffer(buffer, ehdr.size);
  }


  void pollTimedout(Future<short> future)(
  {
    future.discard();

    for (auto it = results.begin(); it != results.end(); it++) {
      it->second->event.process(it->second);
    }
  }


  void sample()
  {
    start = Clock::now();

    foreachvalue(const TraceEvent& event, traceEvents) {
      foreachkey(int fd, event.perf_fds) {
        io::poll(fd, io::READ)
        .then(lambda::bind(&internal::handleEvent, fd));
      }
    }

    delay(duration, self(), &Self::pollTimedout)
  }

  hashmap<string, TraceEvent> traceEvents;
  const Duration duration;
  Time start;
  map<uint64_t, PerfEvent> results;
};

} // namespace internal {


Future<mesos::PerfStatistics> sample(
    const set<string>& events,
    pid_t pid,
    const Duration& duration)
{
  set<pid_t> pids;
  pids.insert(pid);
  return sample(events, pids, duration);
}


Future<mesos::PerfStatistics> sample(
    const set<string>& events,
    const set<pid_t>& pids,
    const Duration& duration)
{
  if (!supported()) {
    return Failure("Perf is not supported");
  }

  const vector<string> argv = internal::argv(events, pids, duration);
  internal::SchedSampler* sampler = new internal::SchedSampler(argv, duration);
  Future<hashmap<string, mesos::PerfStatistics>> future = sampler->future();
  spawn(sampler, true);
  return future
    .then(lambda::bind(&internal::select, PIDS_KEY, lambda::_1));
}


Future<mesos::PerfStatistics> sample(
    const set<string>& events,
    const string& cgroup,
    const Duration& duration)
{
  set<string> cgroups;
  cgroups.insert(cgroup);
  return sample(events, cgroups, duration)
    .then(lambda::bind(&internal::select, cgroup, lambda::_1));
}


Future<hashmap<string, mesos::PerfStatistics>> sample(
    const set<string>& events,
    const set<string>& cgroups,
    const Duration& duration)
{
  if (!supported()) {
    return Failure("Perf is not supported");
  }

  const vector<string> argv = internal::argv(events, cgroups, duration);
  internal::SchedSampler* sampler = new internal::SchedSampler(argv, duration);
  Future<hashmap<string, mesos::PerfStatistics>> future = sampler->future();
  spawn(sampler, true);
  return future;
}



} // namespace sched {
