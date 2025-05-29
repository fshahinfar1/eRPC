#ifndef MSGSZ_WNDSZ_SERVER_H_
#define MSGSZ_WNDSZ_SERVER_H_
#include <cstdio>
#include <optional>
#include "./common.h"

static size_t fib(size_t n) {
	size_t a, b, c = 4;
	a = b = 1;
	if (n == 2) return 1;
	for (size_t i = 2 ; i < n; i++) {
		c = a+b;
		a = b;
		b = c;
	}
	return c;
}

// Send response of size kServerRespSize
void req_handler(erpc::ReqHandle *req_handle, void *_context) {
  auto *c = static_cast<AppContext *>(_context);

  const erpc::MsgBuffer *req_msgbuf = req_handle->get_req_msgbuf();
  // make sure we have received all the client data
  assert(req_msgbuf->get_data_size() == FLAGS_msg_size);
  c->rx_pkts++;
  const size_t sz = req_msgbuf->get_data_size();
  c->rx_bytes += sz;

  if (kDoExtraWorkPerReq) {
	  volatile size_t x = 2048;
	  int fib_value = fib(x);
	  if (fib_value == 4) {
		  printf("we have captured the flag!");
	  }
  }

  std::optional<erpc::MsgBuffer> copy_msgbuf;
  // *copy_msgbuf = c->rpc_->alloc_msg_buffer(sz);

  // RX ring request optimization knob
  if (kAppOptDisableRxRingReq) {
    // Simulate copying the request off the RX ring
    copy_msgbuf = c->rpc_->alloc_msg_buffer(sz);
    assert(copy_msgbuf->buf_ != nullptr);
    if (copy_msgbuf->buf_ == nullptr) {
      printf("the buffer is null and it's bad!\n");
      throw std::runtime_error("hell ran out of memory!");
    }
    memcpy(copy_msgbuf->buf_, req_msgbuf->buf_, sz);
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

  // ..
  if (copy_msgbuf) {
    assert(copy_msgbuf->buf_ != nullptr);
    if (copy_msgbuf->buf_[32] == 'h'){
      printf("you discovered the easter egg! now let's capture the flag: ...\n");
    }
    c->rpc_->free_msg_buffer(*copy_msgbuf);
    // printf("are we freeing things?\n");
  }
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
