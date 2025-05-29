#include <gflags/gflags.h>
#include <signal.h>
#include <cstring>
#include "../apps_common.h"
#include "rpc.h"
#include "util/autorun_helpers.h"
#include "util/latency.h"
#include "util/numautils.h"

static constexpr size_t kAppEvLoopMs = 1000;  // Duration of event loop
static constexpr bool kAppVerbose = false;    // Print debug info on datapath
static constexpr bool kAppMeasureLatency = false;
static constexpr double kAppLatFac = 3.0;        // Precision factor for latency
static constexpr bool kAppPayloadCheck = true;  // Check full request/response
static constexpr bool kDoExtraWorkPerReq = false;

// Optimization knobs. Set to true to disable optimization.
static constexpr bool kAppOptDisablePreallocResp = false;
static constexpr bool kAppOptDisableRxRingReq = true;

static constexpr size_t kAppReqType = 1;    // eRPC request type
static constexpr uint8_t kAppDataByte = 3;  // Data transferred in req & resp
static constexpr size_t kAppMaxBatchSize = 512;
static constexpr size_t kAppMaxConcurrency = 1024; // number of batches that are sent
static const size_t kServerRespSize = 64;
static bool is_server = false;
static uint32_t kSessionPerThread = 16; // number of sessions each client thread opens

DEFINE_uint64(batch_size, 0, "Request batch size");
DEFINE_uint64(msg_size, 0, "Request and response size");
DEFINE_uint64(num_threads, 0, "Number of foreground threads per machine");
DEFINE_uint64(concurrency, 0, "Concurrent batches per thread");

volatile sig_atomic_t ctrl_c_pressed = 0;
void ctrl_c_handler(int) { ctrl_c_pressed = 1; }

#include "./common.h"
#include "./server.h"
#include "./client.h"

int main(int argc, char **argv) {
  signal(SIGINT, ctrl_c_handler);
  gflags::ParseCommandLineFlags(&argc, &argv, true);

  erpc::rt_assert(FLAGS_batch_size <= kAppMaxBatchSize, "Invalid batch size");
  erpc::rt_assert(FLAGS_concurrency <= kAppMaxConcurrency, "Invalid cncrrncy.");
  erpc::rt_assert(FLAGS_numa_node <= 1, "Invalid NUMA node");

  // We create a bit fewer sessions
  const size_t num_sessions = 2 * FLAGS_num_processes * FLAGS_num_threads;
  erpc::rt_assert(num_sessions * erpc::kSessionCredits <=
      erpc::Transport::kNumRxRingEntries,
      "Too few ring buffers");

  erpc::Nexus nexus(erpc::get_uri_for_process(FLAGS_process_id),
      FLAGS_numa_node, 0);
  nexus.register_req_func(kPingReqHandlerType, ping_req_handler);

  // the first process is the server, waiting for request and send fix size
  // responses.
  is_server = FLAGS_process_id == 0;

  if (is_server) {
    nexus.register_req_func(kAppReqType, req_handler);
    // server has one thread
    auto *app_stats = new app_stats_t[1];
    server_thread_func(0, app_stats, &nexus);
    delete[] app_stats;
  } else {
    // client may have multiple threads
    auto *app_stats = new app_stats_t[FLAGS_num_threads];
    std::vector<std::thread> threads(FLAGS_num_threads);
    for (size_t i = 0; i < FLAGS_num_threads; i++) {
      threads[i] = std::thread(client_thread_func, i, app_stats, &nexus);
      erpc::bind_to_core(threads[i], FLAGS_numa_node, i);
    }

    for (auto &thread : threads) thread.join();
    delete[] app_stats;
  }
}
