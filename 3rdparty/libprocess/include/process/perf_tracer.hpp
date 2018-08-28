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

#pragma once

#include <fstream>

#include <observatory/instrumentation/perf_sampler.hpp>

namespace process {

// Appends to the given file on every `disable()`.
class LibprocessTracer {
public:
    LibprocessTracer(
        const std::string& filename,
        size_t sample_period,
        observatory::CounterType type);

    void enable();
    void disable();

private:
    std::string filename_;
    observatory::ThreadAwareRecorder recorder_;
};

} // namespace process {
