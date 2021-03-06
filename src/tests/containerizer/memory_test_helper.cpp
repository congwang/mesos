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
 * limitations under the License.n
 */

#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sys/mman.h>
#include <sys/types.h>
#include <sys/wait.h>

#include <string>
#include <vector>

#include <stout/bytes.hpp>
#include <stout/error.hpp>
#include <stout/hashmap.hpp>
#include <stout/lambda.hpp>
#include <stout/os.hpp>
#include <stout/stringify.hpp>
#include <stout/strings.hpp>
#include <stout/try.hpp>

#include "tests/flags.hpp"

#include "tests/containerizer/memory_test_helper.hpp"

using process::Subprocess;

using std::cerr;
using std::cin;
using std::cout;
using std::endl;
using std::flush;
using std::getline;
using std::string;
using std::vector;

namespace mesos {
namespace internal {
namespace tests {

// Constants used to sync MemoryTestHelper and its subprocess.

// Used by the subprocess to inform that it has started.
const char STARTED = 'S';

// Used by the subprocess to inform that the work requested is done.
const char DONE = 'D';

// Used to signal an increaseRSS request.
const char INCREASE_RSS[] = "INCREASE_RSS";

// Used to signal an increasePageCache request.
const char INCREASE_PAGE_CACHE[] = "INCREASE_PAGE_CACHE";


// This helper allocates and locks specified anonymous memory (RSS).
// It uses mlockall() and memset() to make sure allocated memory is
// mapped.
static Try<void*> allocateRSS(const Bytes& size)
{
  void* rss = NULL;

  // Make sure that all pages that are going to be mapped into the
  // address space of this process become unevictable. This is needed
  // for testing cgroup oom killer.
  if (mlockall(MCL_FUTURE) != 0) {
    return ErrnoError("Failed to make pages to be mapped unevictable");
  }

  if (posix_memalign(&rss, getpagesize(), size.bytes()) != 0) {
    return ErrnoError("Failed to increase RSS memory, posix_memalign");
  }

  // Use memset to map pages into the memory space of this process.
  memset(rss, 1, size.bytes());

  return rss;
}


static Try<Nothing> increaseRSS(const vector<string>& tokens)
{
  if (tokens.size() < 2) {
    return Error("Expect at least one argument");
  }

  Try<Bytes> size = Bytes::parse(tokens[1]);
  if (size.isError()) {
    return Error("The first argument '" + tokens[1] + "' is not a byte size");
  }

  Try<void*> memory = allocateRSS(size.get());
  if (memory.isError()) {
    return Error("Failed to allocate RSS memory: " + memory.error());
  }

  return Nothing();
}


static Try<Nothing> increasePageCache(const vector<string>& tokens)
{
  const Bytes UNIT = Megabytes(1);

  if (tokens.size() < 2) {
    return Error("Expect at least one argument");
  }

  Try<Bytes> size = Bytes::parse(tokens[1]);
  if (size.isError()) {
    return Error("The first argument '" + tokens[1] + "' is not a byte size");
  }

  // TODO(chzhcn): Currently, we assume the current working directory
  // is a temporary directory and will be cleaned up when the test
  // finishes. Since the child process will inherit the current
  // working directory from the parent process, that means the test
  // that uses this helper probably needs to inherit from
  // TemporaryDirectoryTest. Consider relaxing this constraint.
  Try<string> path = os::mktemp(path::join(os::getcwd(), "XXXXXX"));
  if (path.isError()) {
    return Error("Failed to create a temporary file: " + path.error());
  }

  Try<int> fd = os::open(path.get(), O_WRONLY);
  if (fd.isError()) {
    return Error("Failed to open file: " + fd.error());
  }

  // NOTE: We are doing round-down here to calculate the number of
  // writes to do.
  for (uint64_t i = 0; i < size.get().bytes() / UNIT.bytes(); i++) {
    // Write UNIT size to disk at a time. The content isn't important.
    Try<Nothing> write = os::write(fd.get(), string(UNIT.bytes(), 'a'));
    if (write.isError()) {
      os::close(fd.get());
      return Error("Failed to write file: " + write.error());
    }

    // Use fsync to make sure data is written to disk.
    if (fsync(fd.get()) == -1) {
      // Save the error message because os::close below might
      // overwrite the errno.
      const string message = strerror(errno);

      os::close(fd.get());
      return Error("Failed to fsync: " + message);
    }
  }

  os::close(fd.get());
  return Nothing();
}


MemoryTestHelper::~MemoryTestHelper()
{
  cleanup();
}


Try<Nothing> MemoryTestHelper::spawn()
{
  if (s.isSome()) {
    return Error("A subprocess has been spawned already");
  }

  vector<string> argv;
  argv.push_back("memory-test-helper");
  argv.push_back(MemoryTestHelperMain::NAME);

  Try<Subprocess> process = subprocess(
      path::join(flags.build_dir,
                 "src",
                 "memory-test-helper"),
      argv,
      Subprocess::PIPE(),
      Subprocess::PIPE(),
      Subprocess::FD(STDERR_FILENO));

  if (process.isError()) {
    return Error("Failed to spawn a subprocess: " + process.error());
  }

  s = process.get();

  // Wait for the child to inform it has started before returning.
  // Otherwise, the user might set the memory limit too earlier, and
  // cause the child oom-killed because 'ld' could use a lot of
  // memory.
  Result<string> read = os::read(s.get().out().get(), sizeof(STARTED));
  if (!read.isSome() || read.get() != string(sizeof(STARTED), STARTED)) {
    cleanup();
    return Error("Failed to sync with the subprocess");
  }

  return Nothing();
}


void MemoryTestHelper::cleanup()
{
  if (s.isSome()) {
    // We just want to make sure the subprocess is terminated in case
    // it's stuck, but we don't care about its status. Any error
    // should have been logged in the subprocess directly.
    ::kill(s.get().pid(), SIGKILL);
    ::waitpid(s.get().pid(), NULL, 0);
    s = None();
  }
}


Try<pid_t> MemoryTestHelper::pid()
{
  if (s.isNone()) {
    return Error("The subprocess has not been spawned yet");
  }

  return s.get().pid();
}


// Send a request to the subprocess and wait for its signal that the
// work has been done.
Try<Nothing> MemoryTestHelper::requestAndWait(const string& request)
{
  if (s.isNone()) {
    return Error("The subprocess has not been spawned yet");
  }

  Try<Nothing> write = os::write(s.get().in().get(), request + "\n");
  if (write.isError()) {
    cleanup();
    return Error("Fail to sync with the subprocess: " + write.error());
  }

  Result<string> read = os::read(s.get().out().get(), sizeof(DONE));
  if (!read.isSome() || read.get() != string(sizeof(DONE), DONE)) {
    cleanup();
    return Error("Failed to sync with the subprocess");
  }

  return Nothing();
}


Try<Nothing> MemoryTestHelper::increaseRSS(const Bytes& size)
{
  return requestAndWait(string(INCREASE_RSS) + " " + stringify(size));
}


Try<Nothing> MemoryTestHelper::increasePageCache(const Bytes& size)
{
  return requestAndWait(string(INCREASE_PAGE_CACHE) + " " + stringify(size));
}


const char MemoryTestHelperMain::NAME[] = "MemoryTestHelperMain";


int MemoryTestHelperMain::execute()
{
  hashmap<string, Try<Nothing>(*)(const vector<string>&)> commands;
  commands[INCREASE_RSS] = &increaseRSS;
  commands[INCREASE_PAGE_CACHE] = &increasePageCache;

  // Tell the parent that child has started.
  cout << STARTED << flush;

  string line;
  while(cin.good()) {
    getline(cin, line);
    vector<string> tokens = strings::tokenize(line, " ");

    if (tokens.empty()) {
      cerr << "No command from the parent" << endl;
      return 1;
    }

    if (!commands.contains(tokens[0])) {
      cerr << "Unknown command from the parent '" << tokens[0] << "'" << endl;
      return 1;
    }

    Try<Nothing> result = commands[tokens[0]](tokens);
    if (result.isError()) {
      cerr << result.error();
      return 1;
    }

    cout << DONE << flush;
  }

  if (!cin) {
    cerr << "Failed to sync with the parent" << endl;
    return 1;
  }

  return 0;
}

} // namespace tests {
} // namespace internal {
} // namespace mesos {
