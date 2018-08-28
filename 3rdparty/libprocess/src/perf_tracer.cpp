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

#include <process/perf_tracer.hpp>
#include <process/process.hpp>

#include <stout/stringify.hpp>

#include <linux/perf_event.h>
#include <sys/syscall.h>
#include <sys/types.h>

#include <iostream>

namespace {

inline pid_t
sys_gettid()
{
    return ::syscall(__NR_gettid);
}

} // namespace {


namespace process {

LibprocessTracer::LibprocessTracer(
        const std::string& filename,
        size_t sample_period,
        observatory::CounterType type)
  : filename_(filename) // TODO(bevers): Print this in some VLOG().
  , recorder_(filename, sample_period, type)
{}


void LibprocessTracer::enable()
{
  recorder_.enable();
}

void LibprocessTracer::disable()
{
  std::string name = __process__
    ? stringify(__process__->self())
    : std::string("(no process)");

  VLOG(1) << "Disabling perf recorder for process " << name;

  recorder_.disable(name);
  recorder_.dump();
}

} // namespace process {
