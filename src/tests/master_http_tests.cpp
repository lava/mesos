// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

// This file contains a set of basic sanity check against the Mesos v0 API,
// mostly written with the intention of ensuring that requests still work
// as expected in the presence of caching.

#include <process/clock.hpp>
#include <process/event.hpp>
#include <process/future.hpp>
#include <process/http.hpp>
#include <process/owned.hpp>
#include <process/process.hpp>

#include <stout/try.hpp>

#include <gtest/gtest.h>

#include "common/build.hpp"
#include "tests/mesos.hpp"

using process::Clock;
using process::Future;
using process::Owned;
using process::ProcessBase;

using process::http::OK;
using process::http::Response;

namespace mesos {
namespace internal {
namespace tests {

class MasterHttpTest : public MesosTest
{};


// Check that a call to the `/flags` endpoint returns valid JSON.
TEST_F(MasterHttpTest, GetFlags)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  Future<Response> response = process::http::get(
      master.get()->pid,
      "flags",
      None(),
      createBasicAuthHeaders(DEFAULT_CREDENTIAL));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response);
  AWAIT_EXPECT_RESPONSE_HEADER_EQ(APPLICATION_JSON, "Content-Type", response);

  Try<JSON::Value> parsed = JSON::parse<JSON::Value>(response->body);
  ASSERT_SOME(parsed);

  // Use some known value to sanity-check flags response.
  JSON::Value flagsSubset = JSON::parse(
      "{"
        "\"git_sha\":\"" + build::GIT_SHA.get() + "\"" +
        "\"port\":\"" + stringify(master.get()->getMasterInfo().port()) + "\","
      "}").get();

  ASSERT_TRUE(parsed->contains(flagsSubset));
}


// Check that three identical parallel calls to the `/state` endpoint all
// returns the same, valid response.
TEST_F(MasterHttpTest, ParallelState)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  ProcessBase* masterProcess = master.get()->master.get();

  Clock::pause();

  // TODO(bevers): Hide this monstrosity in a function somewhere.
  auto method = &master::Master::Http::deferBatchedRequest;
  auto arg1 = &master::Master::ReadOnlyHandler::state;
  auto arg2 = None(); // Option<Principal>
  auto arg3 = hashmap<std::string, std::string>();
  auto arg4 = ObjectApprovers::create(None(), None(), {}).get();
  auto ptr = std::unique_ptr<lambda::CallableOnce<void(ProcessBase*)>>(
      new lambda::CallableOnce<void(ProcessBase*)>(
          [&](ProcessBase* process) {
            master::Master::Http* t = dynamic_cast<master::Master::Http*>(
                process);
            (t->*method)(arg1, arg2, arg3, arg4);
          }));

  // Second argument is `const Option<const std::type_info*>&`.
  process::DispatchEvent request1(std::move(ptr), None());

  masterProcess->consume(std::move(request1));

  // TODO(bevers): How do we actually get a result now?
}


// Check that three identical parallel calls to the `/state` with different
// `Accept-Encoding` values produce the correct {non-,}gzipped response.
//
// In this test, we need to make an end-to-end connection over an actual socket,
// because the HTTP header handling and gzip encoding is done by libprocess
// after the `Response` has been created.
TEST_F(MasterHttpTest, AcceptEncoding)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  process::http::Headers headersGzip, headersRaw;
  headersGzip["Accept-Encoding"] = "gzip";
  headersRaw["Accept-Encoding"] = "raw";

  // The following sequence is still flaky, but in a benign way: There is a
  // chance that the test succeeds even if things are broken, e.g. if the
  // timing works out such that we never hit the caching code paths.
  // However, *if* the test breaks, it's probably a real bug.
  Clock::pause();

  Future<Response> responseGzip = process::http::get(
      master.get()->pid,
      "flags",
      None(),
      headersGzip);

  Future<Response> responseRaw = process::http::get(
      master.get()->pid,
      "flags",
      None(),
      headersRaw);

  Clock::resume();

  AWAIT_READY(responseGzip);
  AWAIT_READY(responseRaw);

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, responseGzip);
  AWAIT_EXPECT_RESPONSE_HEADER_EQ(
      APPLICATION_JSON, "Content-Encoding", responseGzip);

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, responseRaw);
  AWAIT_EXPECT_RESPONSE_HEADER_EQ(
      APPLICATION_JSON, "Content-Encoding", responseRaw);
}


// Check that parallel calls to the `/frameworks` endpoint with different
// framework id's and parallel calls to the `/slaves` endpoint with different
// slave id's return different JSON.
TEST_F(MasterHttpTest, ParallelFrameworks)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  // TODO(bevers): Create the actual test.
}


// Check that two parallel calls to the `/state` endpoint with different
// `jsonp` return the different responses.
TEST_F(MasterHttpTest, Jsonp)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  // TODO(bevers): Create the actual test.
}


// Check that parallel calls to the `/tasks` endpoint with different
// values for `limit`, `offset` or `order` parameters return different results.
TEST_F(MasterHttpTest, QueryParameters)
{
  /* (query processing in request handler)
  Result<int> result = numify<int>(query.get("limit"));
  size_t limit = result.isSome() ? result.get() : TASK_LIMIT;

  result = numify<int>(query.get("offset"));
  size_t offset = result.isSome() ? result.get() : 0;

  Option<string> order = query.get("order");
  string _order = order.isSome() && (order.get() == "asc") ? "asc" : "des";
  */

  // TODO(bevers): Create the actual test.
}

} // namespace tests {
} // namespace internal {
} // namespace mesos {
