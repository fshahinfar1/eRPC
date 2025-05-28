#ifndef MSGSZ_WNDSZ_SERVER_H_
#define MSGSZ_WNDSZ_SERVER_H_
#include <cstdio>
#include "./common.h"

// Send response of size kServerRespSize
void req_handler(erpc::ReqHandle *req_handle, void *_context) {
  auto *c = static_cast<AppContext *>(_context);

  const erpc::MsgBuffer *req_msgbuf = req_handle->get_req_msgbuf();
  // make sure we have received all the client data
  assert(req_msgbuf->get_data_size() == FLAGS_msg_size);
  c->rx_pkts++;
  c->rx_bytes += req_msgbuf->get_data_size();

  // RX ring request optimization knob
  if (kAppOptDisableRxRingReq) {
    // Simulate copying the request off the RX ring
    auto copy_msgbuf = c->rpc_->alloc_msg_buffer(FLAGS_msg_size);
    assert(copy_msgbuf.buf != nullptr);
    memcpy(copy_msgbuf.buf_, req_msgbuf->buf_, FLAGS_msg_size);
    c->rpc_->free_msg_buffer(copy_msgbuf);
  }

  // Preallocated response optimization knob
  if (kAppOptDisablePreallocResp) {
    erpc::MsgBuffer &resp_msgbuf = req_handle->dyn_resp_msgbuf_;
    resp_msgbuf = c->rpc_->alloc_msg_buffer(kServerRespSize);
    assert(resp_msgbuf.buf != nullptr);

    if (!kAppPayloadCheck) {
      resp_msgbuf.buf_[0] = req_msgbuf->buf_[0];
    } else {
      memcpy(resp_msgbuf.buf_, req_msgbuf->buf_, kServerRespSize);
    }
    c->rpc_->enqueue_response(req_handle, &req_handle->dyn_resp_msgbuf_);
  } else {
    erpc::Rpc<erpc::CTransport>::resize_msg_buffer(
        &req_handle->pre_resp_msgbuf_, kServerRespSize);

    if (!kAppPayloadCheck) {
      req_handle->pre_resp_msgbuf_.buf_[0] = req_msgbuf->buf_[0];
    } else {
      memcpy(req_handle->pre_resp_msgbuf_.buf_, req_msgbuf->buf_,
             kServerRespSize);
    }
    c->rpc_->enqueue_response(req_handle, &req_handle->pre_resp_msgbuf_);
  }
  c->tx_pkts += 1;
  c->tx_bytes += kServerRespSize;
}


void server_thread_func(size_t thread_id, app_stats_t *app_stats, erpc::Nexus *nexus)
{
  auto tmp_ = new AppContext();
  if (tmp_ == nullptr) {
    throw std::runtime_error("failed to allocate AppContext");
  }
  AppContext &c = *tmp_;
  c.thread_id_ = thread_id;
  c.app_stats = app_stats;

  std::vector<size_t> port_vec = flags_get_numa_ports(FLAGS_numa_node);
  erpc::rt_assert(port_vec.size() > 0);
  uint8_t phy_port = port_vec.at(thread_id % port_vec.size());

  erpc::Rpc<erpc::CTransport> rpc(nexus, static_cast<void *>(&c),
      static_cast<uint8_t>(thread_id),
      basic_sm_handler, phy_port);

  rpc.retry_connect_on_invalid_rpc_id_ = true;
  c.rpc_ = &rpc;

  // The server will run for a limited time
  for (size_t i = 0; i < FLAGS_test_ms; i += 1000) {
    rpc.run_event_loop(kAppEvLoopMs);  // 1 second
    if (ctrl_c_pressed == 1) break;
    print_stats(c);
  }
}

#endif
