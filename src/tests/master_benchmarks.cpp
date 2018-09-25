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

#include <atomic>
#include <limits>
#include <memory>
#include <string>
#include <tuple>
#include <vector>
#include <iomanip>

#include <mesos/resources.hpp>
#include <mesos/version.hpp>

#include <process/async.hpp>
#include <process/clock.hpp>
#include <process/collect.hpp>
#include <process/future.hpp>
#include <process/loop.hpp>
#include <process/pid.hpp>
#include <process/process.hpp>
#include <process/protobuf.hpp>
#include <process/statistics.hpp>

#include <stout/duration.hpp>
#include <stout/os.hpp>
#include <stout/stopwatch.hpp>

#include <observatory/instrumentation/pyplot_sink.hpp>

#include "common/protobuf_utils.hpp"

#include "tests/mesos.hpp"

namespace http = process::http;

using process::async;
using process::await;
using process::collect;
using process::Break;
using process::Clock;
using process::Continue;
using process::ControlFlow;
using process::Failure;
using process::Future;
using process::loop;
using process::Owned;
using process::PID;
using process::ProcessBase;
using process::Promise;
using process::spawn;
using process::Statistics;
using process::terminate;
using process::UPID;
using process::wait;

using std::atomic;
using std::cout;
using std::endl;
using std::make_tuple;
using std::numeric_limits;
using std::shared_ptr;
using std::string;
using std::tie;
using std::tuple;
using std::vector;

using testing::WithParamInterface;

namespace mesos {
namespace internal {
namespace tests {

static SlaveInfo createSlaveInfo(const SlaveID& slaveId)
{
  // Using a static local variable to avoid the cost of re-parsing.
  static const Resources resources =
    Resources::parse("cpus:20;mem:10240").get();

  SlaveInfo slaveInfo;
  *(slaveInfo.mutable_resources()) = resources;
  *(slaveInfo.mutable_id()) = slaveId;
  *(slaveInfo.mutable_hostname()) = slaveId.value(); // Simulate the hostname.

  return slaveInfo;
}


static FrameworkInfo createFrameworkInfo(const FrameworkID& frameworkId)
{
  FrameworkInfo frameworkInfo = DEFAULT_FRAMEWORK_INFO;
  *(frameworkInfo.mutable_id()) = frameworkId;

  return frameworkInfo;
}


static TaskInfo createTaskInfo(const SlaveID& slaveId)
{
  // Using a static local variable to avoid the cost of re-parsing.
  static const Resources resources = Resources::parse("cpus:0.1;mem:5").get();

  TaskInfo taskInfo = createTask(
      slaveId,
      resources,
      "dummy command");

  Labels* labels = taskInfo.mutable_labels();

  for (size_t i = 0; i < 10; i++) {
    const string index = stringify(i);
    *(labels->add_labels()) =
      protobuf::createLabel("key" + index, "value" + index);
  }

  return taskInfo;
}


// A fake agent currently just for testing reregisterations.
class TestSlaveProcess : public ProtobufProcess<TestSlaveProcess>
{
public:
  TestSlaveProcess(
      const UPID& _masterPid,
      const SlaveID& _slaveId,
      size_t _frameworksPerAgent,
      size_t _tasksPerFramework,
      size_t _completedFrameworksPerAgent,
      size_t _tasksPerCompletedFramework)
    : ProcessBase(process::ID::generate("test-slave")),
      masterPid(_masterPid),
      slaveId(_slaveId),
      frameworksPerAgent(_frameworksPerAgent),
      tasksPerFramework(_tasksPerFramework),
      completedFrameworksPerAgent(_completedFrameworksPerAgent),
      tasksPerCompletedFramework(_tasksPerCompletedFramework) {}

  void initialize() override
  {
    install<SlaveReregisteredMessage>(&Self::reregistered);
    install<PingSlaveMessage>(
        &Self::ping,
        &PingSlaveMessage::connected);

    // Prepare `ReregisterSlaveMessage` which simulates the real world scenario:
    // TODO(xujyan): Notable things missing include:
    // - `ExecutorInfo`s
    // - Task statuses
    SlaveInfo slaveInfo = createSlaveInfo(slaveId);
    message.mutable_slave()->Swap(&slaveInfo);
    message.set_version(MESOS_VERSION);

    // Used for generating framework IDs.
    size_t id = 0;
    for (; id < frameworksPerAgent; id++) {
      FrameworkID frameworkId;
      frameworkId.set_value("framework" + stringify(id));

      FrameworkInfo framework = createFrameworkInfo(frameworkId);
      message.add_frameworks()->Swap(&framework);

      for (size_t j = 0; j < tasksPerFramework; j++) {
        Task task = protobuf::createTask(
            createTaskInfo(slaveId),
            TASK_RUNNING,
            frameworkId);
        message.add_tasks()->Swap(&task);
      }
    }

    for (; id < frameworksPerAgent + completedFrameworksPerAgent; id++) {
      Archive::Framework* completedFramework =
        message.add_completed_frameworks();

      FrameworkID frameworkId;
      frameworkId.set_value("framework" + stringify(id));

      FrameworkInfo framework = createFrameworkInfo(frameworkId);
      completedFramework->mutable_framework_info()->Swap(&framework);

      for (size_t j = 0; j < tasksPerCompletedFramework; j++) {
        Task task = protobuf::createTask(
            createTaskInfo(slaveId),
            TASK_FINISHED,
            frameworkId);
        completedFramework->add_tasks()->Swap(&task);
      }
    }
  }

  Future<Nothing> reregister()
  {
    send(masterPid, message);
    return promise.future();
  }

  TestSlaveProcess(const TestSlaveProcess& other) = delete;
  TestSlaveProcess& operator=(const TestSlaveProcess& other) = delete;

private:
  void reregistered(const SlaveReregisteredMessage&)
  {
    promise.set(Nothing());
  }

  // We need to answer pings to keep the agent registered.
  void ping(const UPID& from, bool)
  {
    send(from, PongSlaveMessage());
  }

  const UPID masterPid;
  const SlaveID slaveId;
  const size_t frameworksPerAgent;
  const size_t tasksPerFramework;
  const size_t completedFrameworksPerAgent;
  const size_t tasksPerCompletedFramework;

  ReregisterSlaveMessage message;
  Promise<Nothing> promise;
};


class TestSlave
{
public:
  TestSlave(
      const UPID& masterPid,
      const SlaveID& slaveId,
      size_t frameworksPerAgent,
      size_t tasksPerFramework,
      size_t completedFrameworksPerAgent,
      size_t tasksPerCompletedFramework)
    : process(new TestSlaveProcess(
          masterPid,
          slaveId,
          frameworksPerAgent,
          tasksPerFramework,
          completedFrameworksPerAgent,
          tasksPerCompletedFramework))
  {
    spawn(process.get());
  }

  ~TestSlave()
  {
    terminate(process.get());
    wait(process.get());
  }

  Future<Nothing> reregister()
  {
    return dispatch(process.get(), &TestSlaveProcess::reregister);
  }

private:
  Owned<TestSlaveProcess> process;
};


class MasterFailover_BENCHMARK_Test
  : public MesosTest,
    public WithParamInterface<tuple<size_t, size_t, size_t, size_t, size_t>> {};


// The value tuples are defined as:
// - agentCount
// - frameworksPerAgent
// - tasksPerFramework (per agent)
// - completedFrameworksPerAgent
// - tasksPerCompletedFramework (per agent)
INSTANTIATE_TEST_CASE_P(
    AgentFrameworkTaskCount,
    MasterFailover_BENCHMARK_Test,
    ::testing::Values(
        make_tuple(2000, 5, 10, 5, 10),
        make_tuple(2000, 5, 20, 0, 0),
        make_tuple(20000, 1, 5, 0, 0)));


// This test measures the time from all agents start to reregister to
// to when all have received `SlaveReregisteredMessage`.
TEST_P(MasterFailover_BENCHMARK_Test, AgentReregistrationDelay)
{
  size_t agentCount;
  size_t frameworksPerAgent;
  size_t tasksPerFramework;
  size_t completedFrameworksPerAgent;
  size_t tasksPerCompletedFramework;

  tie(agentCount,
      frameworksPerAgent,
      tasksPerFramework,
      completedFrameworksPerAgent,
      tasksPerCompletedFramework) = GetParam();

  master::Flags masterFlags = CreateMasterFlags();
  masterFlags.authenticate_agents = false;

  // Use replicated log so it better simulates the production scenario.
  masterFlags.registry = "replicated_log";

  Try<Owned<cluster::Master>> master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  vector<TestSlave> slaves;

  for (size_t i = 0; i < agentCount; i++) {
    SlaveID slaveId;
    slaveId.set_value("agent" + stringify(i));

    slaves.emplace_back(
        master.get()->pid,
        slaveId,
        frameworksPerAgent,
        tasksPerFramework,
        completedFrameworksPerAgent,
        tasksPerCompletedFramework);
  }

  // Make sure all agents are ready to reregister before we start the stopwatch.
  Clock::pause();
  Clock::settle();
  Clock::resume();

  vector<Future<Nothing>> reregistered;

  // Measure the time for all agents to receive `SlaveReregisteredMessage`.
  Stopwatch watch;
  watch.start();

  foreach (TestSlave& slave, slaves) {
    reregistered.push_back(slave.reregister());
  }

  await(reregistered).await();

  watch.stop();

  cout << "Reregistered " << agentCount << " agents with a total of "
       << frameworksPerAgent * tasksPerFramework * agentCount
       << " running tasks and "
       << completedFrameworksPerAgent * tasksPerCompletedFramework * agentCount
       << " completed tasks in "
       << watch.elapsed() << endl;
}


class MasterStateQuery_BENCHMARK_Test
  : public MesosTest,
    public WithParamInterface<tuple<
      size_t, size_t, size_t, size_t, size_t>> {};


INSTANTIATE_TEST_CASE_P(
    AgentFrameworkTaskCountContentType,
    MasterStateQuery_BENCHMARK_Test,
    ::testing::Values(
        make_tuple(1000, 5, 2, 5, 2),
        make_tuple(10000, 5, 2, 5, 2),
        make_tuple(20000, 5, 2, 5, 2),
        make_tuple(40000, 5, 2, 5, 2)));


// This test measures the performance of the `master::call::GetState`
// v1 api (and also measures master v0 '/state' endpoint as the
// baseline). We set up a lot of master state from artificial agents
// similar to the master failover benchmark.
TEST_P(MasterStateQuery_BENCHMARK_Test, GetState)
{
  size_t agentCount;
  size_t frameworksPerAgent;
  size_t tasksPerFramework;
  size_t completedFrameworksPerAgent;
  size_t tasksPerCompletedFramework;

  tie(agentCount,
    frameworksPerAgent,
    tasksPerFramework,
    completedFrameworksPerAgent,
    tasksPerCompletedFramework) = GetParam();

  // Disable authentication to avoid the overhead, since we don't care about
  // it in this test.
  master::Flags masterFlags = CreateMasterFlags();
  masterFlags.authenticate_agents = false;

  Try<Owned<cluster::Master>> master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  vector<Owned<TestSlave>> slaves;

  for (size_t i = 0; i < agentCount; i++) {
    SlaveID slaveId;
    slaveId.set_value("agent" + stringify(i));

    slaves.push_back(Owned<TestSlave>(new TestSlave(
        master.get()->pid,
        slaveId,
        frameworksPerAgent,
        tasksPerFramework,
        completedFrameworksPerAgent,
        tasksPerCompletedFramework)));
  }

  cout << "Test setup: "
       << agentCount << " agents with a total of "
       << frameworksPerAgent * tasksPerFramework * agentCount
       << " running tasks and "
       << completedFrameworksPerAgent * tasksPerCompletedFramework * agentCount
       << " completed tasks" << endl;

  vector<Future<Nothing>> reregistered;

  foreach (const Owned<TestSlave>& slave, slaves) {
    reregistered.push_back(slave->reregister());
  }

  // Wait all agents to finish reregistration.
  await(reregistered).await();

  Clock::pause();
  Clock::settle();
  Clock::resume();

  Stopwatch watch;
  watch.start();

  // We first measure v0 "state" endpoint performance as the baseline.
  Future<http::Response> v0Response = http::get(
      master.get()->pid,
      "state",
      None(),
      createBasicAuthHeaders(DEFAULT_CREDENTIAL));

  v0Response.await();

  watch.stop();

  ASSERT_EQ(v0Response->status, http::OK().status);

  cout << "v0 '/state' response took " << watch.elapsed() << endl;

  // Helper function to post a request to '/api/v1' master endpoint
  // and return the response.
  auto post = [](
      const process::PID<master::Master>& pid,
      const v1::master::Call& call,
      const ContentType& contentType)
  {
    http::Headers headers = createBasicAuthHeaders(DEFAULT_CREDENTIAL);
    headers["Accept"] = stringify(contentType);

    return http::post(
        pid,
        "api/v1",
        headers,
        serialize(contentType, call),
        stringify(contentType));
  };

  // We measure both JSON and protobuf formats.
  const ContentType contentTypes[] =
    { ContentType::PROTOBUF, ContentType::JSON };

  for (ContentType contentType : contentTypes){
    v1::master::Call v1Call;
    v1Call.set_type(v1::master::Call::GET_STATE);

    watch.start();

    Future<http::Response> response =
      post(master.get()->pid, v1Call, contentType);

    response.await();

    watch.stop();

    ASSERT_EQ(response->status, http::OK().status);

    Future<v1::master::Response> v1Response =
      deserialize<v1::master::Response>(contentType, response->body);

    ASSERT_TRUE(v1Response->IsInitialized());
    EXPECT_EQ(v1::master::Response::GET_STATE, v1Response->type());

    cout << "v1 'master::call::GetState' "
         << contentType << " response took " << watch.elapsed() << endl;
  }
}

static int testRunCounter = 0;

class MasterActorResponsivenessDelay_BENCHMARK_Test
  : public MesosTest,
    public WithParamInterface<tuple<
      size_t, size_t, size_t, size_t, size_t, size_t, Duration>> {};


INSTANTIATE_TEST_CASE_P(
    AgentFrameworkTaskCount,
    MasterActorResponsivenessDelay_BENCHMARK_Test,
    ::testing::Values(
        make_tuple(100, 10, 10, 10, 10, 50, Milliseconds(200)),
        make_tuple(1000, 10, 10, 10, 10, 10, Milliseconds(200))));


// This test indirectly measures how the Master actor is affected by serving
// '/state' requests. The response time for a lightweight '/flags' endpoint
// is taken as a load indicator. We set up a lot of master state from artificial
// agents and send multiple '/state' and '/flags' queries in parallel with some
// interval. As the baseline only '/flags' is queried.
TEST_P(MasterActorResponsivenessDelay_BENCHMARK_Test, WithV0StateLoad)
{
  size_t agentCount;
  size_t frameworksPerAgent;
  size_t tasksPerFramework;
  size_t completedFrameworksPerAgent;
  size_t tasksPerCompletedFramework;
  size_t numRequests;
  Duration requestDelay;

  tie(agentCount,
    frameworksPerAgent,
    tasksPerFramework,
    completedFrameworksPerAgent,
    tasksPerCompletedFramework,
    numRequests,
    requestDelay) = GetParam();

  ++testRunCounter;
  string benchmarkPhase = "single";

  const string indicatorEndpoint = "flags";
  const string stateEndpoint = "state";

  // Disable authentication to avoid the overhead, since we don't care about
  // it in this test.
  master::Flags masterFlags = CreateMasterFlags();
  masterFlags.authenticate_agents = false;

  Try<Owned<cluster::Master>> master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  vector<Owned<TestSlave>> slaves;

  for (size_t i = 0; i < agentCount; i++) {
    SlaveID slaveId;
    slaveId.set_value("agent" + stringify(i));

    slaves.push_back(Owned<TestSlave>(new TestSlave(
        master.get()->pid,
        slaveId,
        frameworksPerAgent,
        tasksPerFramework,
        completedFrameworksPerAgent,
        tasksPerCompletedFramework)));
  }

  cout << "Test setup: "
       << agentCount << " agents with a total of "
       << frameworksPerAgent * tasksPerFramework * agentCount
       << " running tasks and "
       << completedFrameworksPerAgent * tasksPerCompletedFramework * agentCount
       << " completed tasks; " << numRequests << " '/" << stateEndpoint
       << "' and '/" << indicatorEndpoint << "' requests will be sent"
       << " with " << requestDelay << " interval" << endl;

  vector<Future<Nothing>> reregistered;

  foreach (const Owned<TestSlave>& slave, slaves) {
    reregistered.push_back(slave->reregister());
  }

  // Wait all agents to finish reregistration.
  await(reregistered).await();

  Clock::pause();
  Clock::settle();
  Clock::resume();

  // A helper sending a single request and measuring the time it takes to
  // receive a response.
  auto singleRequest = [master](const string& endpoint) -> Future<Duration> {
    shared_ptr<Stopwatch> watch(new Stopwatch);
    watch->start();

    Future<http::Response> response = http::get(
        master.get()->pid,
        endpoint,
        None(),
        createBasicAuthHeaders(DEFAULT_CREDENTIAL));

    return response.then([watch](const http::Response& r) -> Future<Duration> {
      watch->stop();
      EXPECT_EQ(r.status, http::OK().status);
      return watch->elapsed();
    });
  };

  // A helper sending `numRequests` requests with `requestDelayMs` interval.
  // It prints statistics for the response times.
  auto repeatedRequests =
    [singleRequest, numRequests, requestDelay, &benchmarkPhase](
        const string& endpoint) {
    vector<Future<Duration>> responses;
    responses.reserve(numRequests);

    for (size_t i = 0; i < numRequests; ++i) {
      auto f = singleRequest(endpoint);
      responses.push_back(f);
      os::sleep(requestDelay);
    }

    Future<vector<Duration>> durations = process::collect(responses);
    durations.await();

    Option<Statistics<Duration>> s = Statistics<Duration>::from(
        durations->cbegin(), durations->cend());
    EXPECT_SOME(s);

    cout << "'/" << endpoint << "' response [min, p25, p50, p75, p90, max]:"
         << " [" << s->min << ", " << s->p25 << ", " << s->p50 << ", "
         << s->p75 << ", " << s->p90 << ", " << s->max << "]"
         << " from " << durations->size() << " measurements" << endl;

    for (auto d : durations.get()) {
      std::string tag =
        //::testing::UnitTest::GetInstance()->current_test_info()->name() +
	"batched_delay" +
        std::string("__") + endpoint + "_" + benchmarkPhase;

      observatory::PyplotSink::datapoint("/tmp/responsiveness_benchmarks.py", tag,
          testRunCounter, d.secs());
    }
  };

  // First measure the average response time for the `indicatorEndpoint` only
  // as the baseline.
  cout << "Launching " << numRequests << " '/" << indicatorEndpoint << "'"
       << " requests" << endl;

  Future<Nothing> indicatorFinished = async(
      repeatedRequests, indicatorEndpoint);
  indicatorFinished.await();

  Clock::pause();
  Clock::settle();
  Clock::resume();

  benchmarkPhase = "parallel";

  // Now measure the average response times when request for both
  // `indicatorEndpoint` and `stateEndpoint` are sent in parallel.
  cout << "Launching " << numRequests << " '/" << stateEndpoint << "'"
       << " requests in background" << endl
       << "Launching " << numRequests << " '/" << indicatorEndpoint << "'"
       << " requests" << endl;

  Future<Nothing> stateFinished = async(repeatedRequests, stateEndpoint);
  indicatorFinished = async(repeatedRequests, indicatorEndpoint);

  indicatorFinished.await();
  stateFinished.await();
}


class MasterActorResponsivenessMulticlient_BENCHMARK_Test
  : public MesosTest,
    public WithParamInterface<tuple<
      size_t, size_t, size_t, size_t, size_t, size_t, size_t>> {};


INSTANTIATE_TEST_CASE_P(
    AgentFrameworkTaskCount,
    MasterActorResponsivenessMulticlient_BENCHMARK_Test,
    ::testing::Values(
        make_tuple(100, 10, 10, 10, 10, 50, 5),
        make_tuple(1000, 10, 10, 10, 10, 10, 5)));


// This test indirectly measures how the Master actor is affected by serving
// '/state' requests. The response time for a lightweight '/flags' endpoint
// is taken as a load indicator. We set up a lot of master state from artificial
// agents and send multiple '/state' queries while constantly probing '/flags'.
// As the baseline only '/flags' is queried.
TEST_P(MasterActorResponsivenessMulticlient_BENCHMARK_Test, WithV0StateLoad)
{
  size_t agentCount;
  size_t frameworksPerAgent;
  size_t tasksPerFramework;
  size_t completedFrameworksPerAgent;
  size_t tasksPerCompletedFramework;
  size_t numRequests;
  size_t numClients;

  tie(agentCount,
    frameworksPerAgent,
    tasksPerFramework,
    completedFrameworksPerAgent,
    tasksPerCompletedFramework,
    numRequests,
    numClients) = GetParam();

  const string indicatorEndpoint = "flags";
  const string stateEndpoint = "state";

  ++testRunCounter;
  string benchmarkPhase = "single";

  // Disable authentication to avoid the overhead, since we don't care about
  // it in this test.
  master::Flags masterFlags = CreateMasterFlags();
  masterFlags.authenticate_agents = false;

  Try<Owned<cluster::Master>> master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  vector<Owned<TestSlave>> slaves;

  for (size_t i = 0; i < agentCount; i++) {
    SlaveID slaveId;
    slaveId.set_value("agent" + stringify(i));

    slaves.push_back(Owned<TestSlave>(new TestSlave(
        master.get()->pid,
        slaveId,
        frameworksPerAgent,
        tasksPerFramework,
        completedFrameworksPerAgent,
        tasksPerCompletedFramework)));
  }

  cout << "Test setup: " << agentCount << " agents with a total of "
       << frameworksPerAgent * tasksPerFramework * agentCount
       << " running tasks and "
       << completedFrameworksPerAgent * tasksPerCompletedFramework * agentCount
       << " completed tasks; " << numRequests << " '/" << stateEndpoint
       << "' requests will be sent while constantly probing '/"
       << indicatorEndpoint << "'" << endl;

  vector<Future<Nothing>> reregistered;

  foreach (const Owned<TestSlave>& slave, slaves) {
    reregistered.push_back(slave->reregister());
  }

  // Wait all agents to finish reregistration.
  await(reregistered).await();

  Clock::pause();
  Clock::settle();
  Clock::resume();

  // A helper sending a single request and measuring the time it takes to
  // receive a response.
  auto singleRequest = [master](const string& endpoint) -> Future<Duration> {
    shared_ptr<Stopwatch> watch(new Stopwatch);
    watch->start();

    Future<http::Response> response = http::get(
        master.get()->pid,
        endpoint,
        None(),
        createBasicAuthHeaders(DEFAULT_CREDENTIAL));

    return response.then([watch](const http::Response& r) -> Future<Duration> {
      watch->stop();
      EXPECT_EQ(r.status, http::OK().status);
      return watch->elapsed();
    });
  };

  // Synchronizes completion of all lambdas sending requests.
  atomic<bool> stop = ATOMIC_VAR_INIT(false);

  // A helper sending `numRequests` requests to `endpoint`. An early exist is
  // possible if `stop` is set. Note that this lambda sets `stop` once
  // `numRequests` requests have been sent. The intention is to synchronize
  // completion across all running lambdas. Also prints statistics for the
  // response times.
  auto repeateRequests = [singleRequest, &stop](
      const string& endpoint, size_t numRequests) -> vector<Duration> {
    vector<Duration> durations;

    size_t remaining = numRequests;
    auto f = loop(
        None(),
        [=]() {
          return singleRequest(endpoint);
        },
        [&remaining, &durations, &stop](
            const Duration& d) -> ControlFlow<Nothing> {
          durations.push_back(d);

          if (--remaining <= 0) {
            stop.store(true);
          }

          if (stop.load()) {
            return Break();
          } else {
            return Continue();
          }
        });

    f.await();
    EXPECT_TRUE(f.isReady());

    return durations;
  };

  auto printStats = [&benchmarkPhase](
      const vector<Duration>& durations, const string& endpoint) {
    Option<Statistics<Duration>> s = Statistics<Duration>::from(
          durations.cbegin(), durations.cend());
    EXPECT_SOME(s);

    cout << "'/" << endpoint << "' response [min, p25, p50, p75, p90, max]:"
         << " [" << s->min << ", " << s->p25 << ", " << s->p50 << ", "
         << s->p75 << ", " << s->p90 << ", " << s->max << "]"
         << " from " << s->count << " measurements" << endl;

    for (auto d : durations) {
      std::string tag =
        "batched_multiclient" +
        std::string("__") + endpoint + "_" + benchmarkPhase;

      observatory::PyplotSink::datapoint("/tmp/responsiveness_benchmarks.py", tag,
          testRunCounter, d.secs());
    }
  };

  // First measure the average response time for the `indicatorEndpoint` only
  // as the baseline.
  cout << "Launching " << numRequests << " '/" << indicatorEndpoint << "'"
       << " requests" << endl;

  Future<vector<Duration>> indicatorFinished = async(
      repeateRequests, indicatorEndpoint, numRequests);
  indicatorFinished.await();

  printStats(indicatorFinished.get(), indicatorEndpoint);

  Clock::pause();
  Clock::settle();
  Clock::resume();

  // Now measure the average response times when request for both
  // `indicatorEndpoint` and `stateEndpoint` are sent in parallel.
  // Stop when `numRequests` to `stateEndpoint` have been sent.
  benchmarkPhase = "parallel";
  stop.store(false);

  cout << "Launching " << numClients << " * " << numRequests << " '/"
       << stateEndpoint << "'" << " requests in background" << endl
       << "Launching " << numRequests << " '/" << indicatorEndpoint << "'"
       << " requests" << endl;

  vector<Future<vector<Duration>>> stateFinished;
  while (numClients-- > 0) {
    stateFinished.push_back(async(repeateRequests, stateEndpoint, numRequests));
  }

  indicatorFinished = async(
      repeateRequests, indicatorEndpoint, numeric_limits<size_t>::max());

  auto collected = collect(stateFinished);
  collected.await();
  indicatorFinished.await();

  printStats(indicatorFinished.get(), indicatorEndpoint);

  vector<Duration> res;
  foreach (const auto& v, collected.get()) {
    res.insert(res.end(), v.cbegin(), v.cend());
  }
  printStats(res, stateEndpoint);
}


class MasterMetricsQuery_BENCHMARK_Test
  : public MesosTest,
    public WithParamInterface<tuple<
      size_t, size_t, size_t, size_t, size_t>> {};


INSTANTIATE_TEST_CASE_P(
    AgentFrameworkTaskCountContentType,
    MasterMetricsQuery_BENCHMARK_Test,
    ::testing::Values(
        make_tuple(1, 90, 2, 10, 2),
        make_tuple(1, 900, 2, 100, 2),
        make_tuple(1, 9000, 2, 1000, 2),
        make_tuple(1, 18000, 2, 2000, 2)));


// This test measures the performance of the `master::call::GetMetrics` v1 API
// and the unversioned '/metrics/snapshot' endpoint. Frameworks are added to the
// test agents in order to test the performance when large numbers of
// per-framework metrics are present.
TEST_P(MasterMetricsQuery_BENCHMARK_Test, GetMetrics)
{
  size_t agentCount;
  size_t activeFrameworkCount;
  size_t tasksPerActiveFramework;
  size_t completedFrameworkCount;
  size_t tasksPerCompletedFramework;

  tie(agentCount,
    activeFrameworkCount,
    tasksPerActiveFramework,
    completedFrameworkCount,
    tasksPerCompletedFramework) = GetParam();

  // Disable authentication to avoid the overhead, since we don't care about
  // it in this test.
  master::Flags masterFlags = CreateMasterFlags();
  masterFlags.authenticate_agents = false;
  masterFlags.authenticate_http_readwrite = false;
  masterFlags.authenticate_http_readonly = false;

  Try<Owned<cluster::Master>> master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  vector<Owned<TestSlave>> slaves;

  for (size_t i = 0; i < agentCount; i++) {
    SlaveID slaveId;
    slaveId.set_value("agent" + stringify(i));

    slaves.push_back(Owned<TestSlave>(new TestSlave(
        master.get()->pid,
        slaveId,
        activeFrameworkCount,
        tasksPerActiveFramework,
        completedFrameworkCount,
        tasksPerCompletedFramework)));
  }

  cout << "Test setup: "
       << agentCount << " agents with a total of "
       << activeFrameworkCount + completedFrameworkCount
       << " frameworks" << endl;

  vector<Future<Nothing>> reregistered;

  foreach (const Owned<TestSlave>& slave, slaves) {
    reregistered.push_back(slave->reregister());
  }

  // Wait for all agents to finish reregistration.
  await(reregistered).await();

  Clock::pause();
  Clock::settle();
  Clock::resume();

  UPID upid("metrics", process::address());

  Stopwatch watch;

  // We first measure v0 "metrics/snapshot" response time.
  watch.start();
  Future<http::Response> v0Response = http::get(upid, "snapshot");
  v0Response.await();
  watch.stop();

  ASSERT_EQ(v0Response->status, http::OK().status);

  cout << "unversioned /metrics/snapshot' response took "
       << watch.elapsed() << endl;

  // Helper function to post a request to '/api/v1' master endpoint
  // and return the response.
  auto post = [](
      const process::PID<master::Master>& pid,
      const v1::master::Call& call,
      const ContentType& contentType)
  {
    const http::Headers headers{{"Accept", stringify(contentType)}};

    return http::post(
        pid,
        "api/v1",
        headers,
        serialize(contentType, call),
        stringify(contentType));
  };

  // We measure both JSON and protobuf formats.
  const ContentType contentTypes[] =
    { ContentType::PROTOBUF, ContentType::JSON };

  for (ContentType contentType : contentTypes){
    v1::master::Call v1Call;
    v1Call.set_type(v1::master::Call::GET_METRICS);
    v1Call.mutable_get_metrics();

    // Measure the response time.
    watch.start();
    Future<http::Response> response =
      post(master.get()->pid, v1Call, contentType);
    response.await();
    watch.stop();

    ASSERT_EQ(response->status, http::OK().status);

    Future<v1::master::Response> v1Response =
      deserialize<v1::master::Response>(contentType, response->body);

    ASSERT_TRUE(v1Response->IsInitialized());
    EXPECT_EQ(v1::master::Response::GET_METRICS, v1Response->type());

    cout << "v1 'master::call::GetMetrics' "
         << contentType << " response took " << watch.elapsed() << endl;
  }
}

} // namespace tests {
} // namespace internal {
} // namespace mesos {
