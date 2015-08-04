#include <stdlib.h>
#include <unistd.h>

#include <sys/prctl.h>
#include <sys/types.h>
#include <sys/wait.h>

#include <list>
#include <ostream>
#include <vector>

#include <stout/strings.hpp>

#include <stout/os/perf.hpp>
#include <sys/ioctl.h>
#include <sys/mman.h>

#include "linux/trace_event.hpp"


using namespace process;
using namespace std;

namespace trace {

static string TraceRootPath = "/sys/kernel/debug/tracing";

static const int buffer_pages = 64;

void TraceEvent::initialize(const string& _name)
{
  name = _name;

  const string IdPath = path::join(TraceRootPath, "events/sched", _name, "id");
  Try<string> _id = os::read(IdPath);
  Try<int> value = numify<int>(_id.get());
  id = value.get();

  const string FormatPath = path::join(TraceRootPath, "events/sched", _name, "format");
  Try<string>  _format = os::read(FormatPath);
  foreach (const string& line, strings::tokenize(_format.get(), "\n")) {
    map<string, vector<string>> tokens = strings::pairs(line, ";", ":");
    TraceEventField field;
    field.name = getFieldName(tokens["field"][0]);
    Try<int> size = numify<int>(tokens["size"][0]);
    field.size = size.get();
    Try<int> offset = numify<int>(tokens["offset"][0]);
    field.offset = offset.get();
    formats.push_back(field);
  }
}


int TraceEvent::open(int cgroup_fd)
{
  struct perf_event_attr attr;
  size_t pgsz = sysconf(_SC_PAGESIZE);
  int nr_cpus = sysconf(_SC_NPROCESSORS_ONLN);

  memset(&attr, 0, sizeof(attr));
  attr.type = PERF_TYPE_TRACEPOINT;
  attr.config = id;
  attr.wakeup_events = 1;
  attr.sample_type = PERF_SAMPLE_RAW | PERF_SAMPLE_TIME | PERF_SAMPLE_CPU;
  attr.sample_period = 1;
  attr.sample_freq = 1;
  attr.inherit = 1;
  attr.sample_id_all = 1;
  attr.exclude_guest = 1;
  attr.read_format = 0;

  // XXX: we don't support CPU hotplug
  for(int cpu = 0; cpu < nr_cpus; cpu++) {
    struct perf_event_mmap_page *buffer;
    int perf_fd;

    if (!cgroup_fd)
      perf_fd = os::perf_event_open(&attr, -1, cpu, -1, 0);
    else
      perf_fd = os::perf_event_open(&attr, cgroup_fd, cpu, -1, PERF_FLAG_PID_CGROUP);

    if (perf_fd < 0) {
      LOG(ERROR) << "can not attach event to cpu: " << cpu << endl;
      return -1;
    }

    buffer = (struct perf_event_mmap_page *)mmap(NULL, (buffer_pages + 1) * pgsz, PROT_READ | PROT_WRITE,
			      MAP_SHARED, perf_fd, 0);
    if (buffer == MAP_FAILED) {
      LOG(ERROR) << "cannot mmap buffer";
      return -1;
    }

    // Do not active the event now, defer it
    perf_fds[perf_fd] = buffer;
  }

  return 0;
}


void TraceEvent::close()
{
  int ret;
  size_t pgsz = sysconf(_SC_PAGESIZE);

  map<int, struct perf_event_mmap_page*>::iterator it;
  for (it = perf_fds.begin(); it != perf_fds.end(); it++) {
    ret = ioctl(it->first, PERF_EVENT_IOC_DISABLE, 0);
    if (ret)
      LOG(ERROR) << "perf cannot stop event";
    munmap(it->second, (buffer_pages + 1) * pgsz);
    os::close(it->first);
  }
}

} // namespace trace {
