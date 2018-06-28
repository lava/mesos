// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License

#include "benchmarking/benchmarking.hpp"

#include <cstdio>
#include <cstring>
#include <ctime>
#include <unistd.h>

#include <process/http.hpp>

#include "logging/logging.hpp"

namespace {

std::string decorate(const char* basename)
{
  time_t now = ::time(nullptr);
  pid_t pid = ::getpid();

  return std::string(basename) +
    "-t" + std::to_string(now) +
    "-p" + std::to_string(pid);
}


class TimestampedFile {
public:
  TimestampedFile(const char* basename)
  {
    std::string path = decorate(basename);
    out = fopen(path.c_str(), "w+");
    if (!out) {
      // Don't throw so we don't impact other scale tests.
      LOG(WARNING)
        << "Couldn't open output file " << path << ": "
        << strerror(errno);
    }
  }

  ~TimestampedFile()
  {
    if (out) {
      fclose(out);
    }
  }

  bool append(const char* s) {
    if (!out) {
      return false;
    }

    int result = fputs(s, out);
    fflush(out);
    return result != EOF;
  }

private:
  // We use stdio over iostreams because the latter frequently
  // has a measurable impact on i/o operations, and we want to
  // minimize that as much as possible.
  FILE* out;
};


// Measurements for the /state endpoint
TimestampedFile& stateMeasurementsFile() {
  static TimestampedFile file("/tmp/state-benchmarking-old");
  return file;
}


// Measurements for the /state-copy endpoint
TimestampedFile& stateCopyMeasurementsFile() {
  static TimestampedFile file("/tmp/state-benchmarking-new");
  return file;
}

} // namespace {


namespace mesos {
namespace internal {
namespace benchmarking {
namespace state_json {

long long toMilliseconds(const struct timespec& ts)
{
  // TODO(bevers): We should probably adjust the origin
  // so it's measured in milliseconds since epoch.
  return ts.tv_sec * 1000ll + ts.tv_nsec / 1000000ll;
}

long long toMicroseconds(const struct timespec& ts)
{
  // TODO(bevers): We should probably adjust the origin
  // so it's measured in milliseconds since epoch.
  return ts.tv_sec * 1000000ll + ts.tv_nsec / 1000ll;
}

void logStateRequest(
    const process::http::Request& request,
    const process::http::Response& response)
{
  std::string line(128, '\0');

  sprintf(&line[0], "request %lx (%lu bytes) - %lld %lld %lld %lld\n",
      request.requestNumber,
      response.body.size(),
      toMilliseconds(request.received),
      toMilliseconds(request.masterEntered),
      toMilliseconds(request.masterExited),
      toMilliseconds(request.finished));

  if (strings::endsWith(request.url.path, "/state")) {
    stateMeasurementsFile().append(line.c_str());
  } else if (strings::endsWith(request.url.path, "/state-copy")) {
    stateCopyMeasurementsFile().append(line.c_str());
  } else {
    LOG(WARNING) << "Skipping benchmark for unknown path " << request.url.path;
  }
}

} // namespace state_json {
} // namespace benchmarking {
} // namespace internal {
} // namespace mesos {
