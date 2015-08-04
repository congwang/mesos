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

#ifndef __TRACE_HPP__
#define __TRACE_HPP__

#include <unistd.h>

#include <set>
#include <string>
#include <map>
#include <vector>

#include <process/future.hpp>

#include <stout/strings.hpp>
#include <stout/hashmap.hpp>

#include <linux/perf_event.h>

namespace trace {

enum SchedEvents {
  SCHED_SWITCH,
  SCHED_MIGRATE_TASK,
  SCHED_WAKEUP,
  SCHED_WAKEUP_NEW,
  SCHED_PROCESS_FORK,
  NR_EVENTS
};


std::vector<std::string> TraceEventFieldTypes = {
  "unsigned short",
  "unsigned char",
  "pid_t",
  "long",
  "char",
  "int",
};


struct TraceEventField {
  std::string name;
  int offset;
  int size;
};

class PerfEvent;

class TraceEvent {
public:
  int open(int cgroupFd);
  void close();
  PerfEvent parse(const char *);
  virtual int process(PerfEvent& event) = 0;

protected:
  void initialize(const std::string& _name);

private:
  inline std::string getFieldName(const std::string& field)
  {
    for (size_t i = 0; i < TraceEventFieldTypes.size(); i++) {
      if (strings::startsWith(field, TraceEventFieldTypes[i])) {
        return field.substr(TraceEventFieldTypes[i].length()+1);
      }
    }

    return "";
  }

  std::string name;
  int id;

  std::vector<TraceEventField> formats;
  std::map<int, struct perf_event_mmap_page*> perf_fds;
};


class PerfEvent {
public:
  PerfEvent(uint64_t _timestamp, int _cpu, const TraceEvent* _event)
  : timestamp(_timestamp), cpu(_cpu), event(_event) {}

  inline uint64_t& operator[](const std::string name)
  {
    return fields[name];
  }

private:
  uint64_t timestamp;
  int cpu;
  const TraceEvent *event;
  std::map<std::string, uint64_t> fields;
};


class TraceEventSchedSwitch: public TraceEvent {
public:
  TraceEventSchedSwitch()
  { initialize("sched_switch"); }

  int process(PerfEvent& event)
  {
    return 0;
  }
};


class TraceEventSchedMigrateTask: public TraceEvent {
public:
  TraceEventSchedMigrateTask()
  { initialize("sched_migrate_task"); }

  int process(PerfEvent& event)
  {
    return 0;
  }
};


class TraceEventSchedWakeup: public TraceEvent {
public:
  TraceEventSchedWakeup()
  { initialize("sched_wakeup"); }

  int process(PerfEvent& event)
  {
    return 0;
  }
};


class TraceEventSchedProcessFork: public TraceEvent {
public:
  TraceEventSchedProcessFork()
  { initialize("sched_process_fork"); }

  int process(PerfEvent& event)
  {
    return 0;
  }
};

} // namespace trace {

#endif // __TRACE_HPP__
