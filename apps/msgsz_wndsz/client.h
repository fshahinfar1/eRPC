#ifndef MSGSZ_WNDSZ_CLIENT_H_
#define MSGSZ_WNDSZ_CLIENT_H_
#include <cstdio>
#include "./common.h"

void connect_sessions(AppContext &c) {
  assert(is_server == false);
  assert(FLAGS_process_id != 0); // assumption is that server is process id zero
  // Assumption is that we have one process for server with on thread.
  // Everything else is a client.

  size_t server_p_id = 0;
  uint8_t remote_rpc_id = 0; // we only have one thread for the server
  std::string remote_uri = erpc::get_uri_for_process(server_p_id);

  // Each thread may oppen multiple sessions (each client thread will call this
  // function)
  for (uint32_t s = 0; s < kSessionPerThread; s++) {
    if (FLAGS_sm_verbose == 1) {
      printf("Process %zu, thread %zu: Creating sessions to %s.\n",
             FLAGS_process_id, c.thread_id_, remote_uri.c_str());
    }
    int session_num = c.rpc_->create_session(remote_uri, remote_rpc_id);
    erpc::rt_assert(session_num >= 0, "Failed to create session");
    c.session_num_vec_.push_back(session_num);
  }

  // wait untill sessions are successfully opened
  while (c.num_sm_resps_ != c.session_num_vec_.size()) {
    c.rpc_->run_event_loop(kAppEvLoopMs);
    if (unlikely(ctrl_c_pressed == 1)) return;
  }
}

void app_cont_func(void *_context, void *_tag);

// Send all requests for a batch
void send_reqs(AppContext *c, size_t batch_i) {
  assert(batch_i < FLAGS_concurrency);
  BatchContext &bc = c->batch_arr[batch_i];

  for (size_t i = 0; i < FLAGS_batch_size; i++) {
    if (kAppVerbose) {
      printf("Process %zu, Rpc %u: Sending request for batch %zu.\n",
             FLAGS_process_id, c->rpc_->get_rpc_id(), batch_i);
    }

    if (!kAppPayloadCheck) {
      bc.req_msgbuf[i].buf_[0] = kAppDataByte;  // Touch req MsgBuffer
    } else {
      // Fill the request MsgBuffer with a checkable sequence
      uint8_t *buf = bc.req_msgbuf[i].buf_;
      buf[0] = c->fastrand_.next_u32();
      for (size_t j = 1; j < FLAGS_msg_size; j++) buf[j] = buf[0] + j;
    }

    if (kAppMeasureLatency) bc.req_tsc[i] = erpc::rdtsc();

    tag_t tag(batch_i, i);
    c->rpc_->enqueue_request(c->next_session_number, kAppReqType,
                             &bc.req_msgbuf[i], &bc.resp_msgbuf[i],
                             app_cont_func, reinterpret_cast<void *>(tag._tag));
    c->next_session_number = (c->next_session_number + 1) % kSessionPerThread;
  }

  c->tx_pkts += FLAGS_batch_size;
  c->tx_bytes += FLAGS_batch_size * FLAGS_msg_size;
}

void app_cont_func(void *_context, void *_tag) {
  auto *c = static_cast<AppContext *>(_context);
  auto tag = static_cast<tag_t>(_tag);

  /* printf("@app_cont_func: tag: %u:%u\n", tag.s.batch_i, tag.s.msgbuf_i); */

  BatchContext &bc = c->batch_arr[tag.s.batch_i];
  const erpc::MsgBuffer &resp_msgbuf = bc.resp_msgbuf[tag.s.msgbuf_i];
  assert(resp_msgbuf.get_data_size() == kServerRespSize);

  if (!kAppPayloadCheck) {
    // Do a cheap check, but touch the response MsgBuffer
    if (unlikely(resp_msgbuf.buf_[0] != kAppDataByte)) {
      fprintf(stderr, "Invalid response.\n");
      exit(-1);
    }
  } else {
    // Check the full response MsgBuffer
    for (size_t i = 0; i < FLAGS_msg_size; i++) {
      const uint8_t *buf = resp_msgbuf.buf_;
      if (unlikely(buf[i] != static_cast<uint8_t>(buf[0] + i))) {
        fprintf(stderr, "Invalid resp at %zu (%u, %u)\n", i, buf[0], buf[i]);
        exit(-1);
      }
    }
  }

  if (kAppVerbose) {
    printf("Received response for batch %u, msgbuf %u.\n", tag.s.batch_i,
           tag.s.msgbuf_i);
  }

  bc.num_resps_rcvd++;

  if (kAppMeasureLatency) {
    size_t req_tsc = bc.req_tsc[tag.s.msgbuf_i];
    double req_lat_us =
        erpc::to_usec(erpc::rdtsc() - req_tsc, c->rpc_->get_freq_ghz());
    c->latency.update(static_cast<size_t>(req_lat_us * kAppLatFac));
  }

  if (bc.num_resps_rcvd == FLAGS_batch_size) {
    // If we have a full batch, reset batch progress and send more requests
    bc.num_resps_rcvd = 0;
    send_reqs(c, tag.s.batch_i);
  }

  c->rx_pkts++;
  c->rx_bytes += resp_msgbuf.get_data_size();
  c->stat_resp_rx[tag.s.batch_i]++;
}

// The function executed by each thread in the cluster
void client_thread_func(size_t thread_id, app_stats_t *app_stats, erpc::Nexus *nexus) {
  // move context to heap to have larger batch and concurency
  auto tmp_ = new AppContext();
  if (tmp_ == nullptr) {
    throw std::runtime_error("failed to allocate AppContext");
  }
  AppContext &c = *tmp_;
  c.thread_id_ = thread_id;
  c.app_stats = app_stats;
  // if (thread_id == 0) c.tmp_stat_ = new TmpStat(app_stats_t::get_template_str());

  std::vector<size_t> port_vec = flags_get_numa_ports(FLAGS_numa_node);
  erpc::rt_assert(port_vec.size() > 0);
  uint8_t phy_port = port_vec.at(thread_id % port_vec.size());

  erpc::Rpc<erpc::CTransport> rpc(nexus, static_cast<void *>(&c),
      static_cast<uint8_t>(thread_id),
      basic_sm_handler, phy_port);

  rpc.retry_connect_on_invalid_rpc_id_ = true;
  c.rpc_ = &rpc;

  // Pre-allocate request and response MsgBuffers for each batch
  for (size_t i = 0; i < FLAGS_concurrency; i++) {
    BatchContext &bc = c.batch_arr[i];
    for (size_t j = 0; j < FLAGS_batch_size; j++) {
      bc.req_msgbuf[j] = rpc.alloc_msg_buffer_or_die(FLAGS_msg_size);
      bc.resp_msgbuf[j] = rpc.alloc_msg_buffer_or_die(FLAGS_msg_size);
    }
  }

  // Start work
  connect_sessions(c);
  if (ctrl_c_pressed == 1)
    return;

  printf("Process %zu, thread %zu: All sessions connected. Starting work.\n",
      FLAGS_process_id, thread_id);
  // Client
  clock_gettime(CLOCK_REALTIME, &c.tput_t0);
  for (size_t i = 0; i < FLAGS_concurrency; i++) send_reqs(&c, i);

  for (size_t i = 0; i < FLAGS_test_ms; i += 1000) {
    rpc.run_event_loop(kAppEvLoopMs);  // 1 second
    if (ctrl_c_pressed == 1) break;
    if (is_server) continue;
    print_stats(c);
  }
}

#endif
