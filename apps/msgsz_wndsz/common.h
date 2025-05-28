#ifndef MSGSZ_WNDSZ_COMMON_H_
#define MSGSZ_WNDSZ_COMMON_H_
#include <cstdint>

union tag_t {
  struct {
    uint64_t batch_i : 32;
    uint64_t msgbuf_i : 32;
  } s;

  void *_tag;

  tag_t(uint64_t batch_i, uint64_t msgbuf_i) {
    s.batch_i = batch_i;
    s.msgbuf_i = msgbuf_i;
  }
  tag_t(void *_tag) : _tag(_tag) {}
};

static_assert(sizeof(tag_t) == sizeof(void *), "");

// Per-batch context
class BatchContext {
 public:
  size_t num_resps_rcvd = 0;         // Number of responses received
  size_t req_tsc[kAppMaxBatchSize];  // Timestamp when request was issued
  erpc::MsgBuffer req_msgbuf[kAppMaxBatchSize];
  erpc::MsgBuffer resp_msgbuf[kAppMaxBatchSize];
};

struct app_stats_t {
  double mrps;
  size_t num_re_tx;

  // Used only if latency stats are enabled
  double lat_us_50;
  double lat_us_99;
  double lat_us_999;
  double lat_us_9999;
  size_t pad[2];

  app_stats_t() { memset(this, 0, sizeof(app_stats_t)); }

  static std::string get_template_str() {
    std::string ret = "mrps num_re_tx";
    if (kAppMeasureLatency) {
      ret += " lat_us_50 lat_us_99 lat_us_999 lat_us_9999";
    }
    return ret;
  }

  std::string to_string() {
    auto ret = std::to_string(mrps) + " " + std::to_string(num_re_tx);
    if (kAppMeasureLatency) {
      return ret + " " + std::to_string(lat_us_50) + " " +
             std::to_string(lat_us_99) + " " + std::to_string(lat_us_999) +
             " " + std::to_string(lat_us_9999);
    }

    return ret;
  }

  /// Accumulate stats
  app_stats_t &operator+=(const app_stats_t &rhs) {
    this->mrps += rhs.mrps;
    this->num_re_tx += rhs.num_re_tx;
    if (kAppMeasureLatency) {
      this->lat_us_50 += rhs.lat_us_50;
      this->lat_us_99 += rhs.lat_us_99;
      this->lat_us_999 += rhs.lat_us_999;
      this->lat_us_9999 += rhs.lat_us_9999;
    }
    return *this;
  }
};
static_assert(sizeof(app_stats_t) == 64, "");

// Per-thread application context
class AppContext : public BasicAppContext {
 public:
  struct timespec tput_t0;  // Start time for throughput measurement
  app_stats_t *app_stats;   // Common stats array for all threads

  size_t stat_resp_rx[kAppMaxConcurrency] = {0};  // Resps received for batch i
  size_t tx_pkts = 0;
  size_t tx_bytes = 0;
  size_t rx_pkts = 0;
  size_t rx_bytes = 0;
  size_t next_session_number = 0;

  std::array<BatchContext, kAppMaxConcurrency> batch_arr;  // Per-batch context
  erpc::Latency latency;  // Cold if latency measurement disabled

  ~AppContext() {}
};

static double sec_since(struct timespec &ts_origin)
{
  struct timespec now;
  double T_origin = static_cast<double>(ts_origin.tv_sec) + (static_cast<double>(ts_origin.tv_nsec) / 1000000000.0F);
  clock_gettime(CLOCK_REALTIME, &now);
  double T_now = static_cast<double>(now.tv_sec) + (static_cast<double>(now.tv_nsec) / 1000000000.0F);
  double T = T_now - T_origin;
  return T;
}

void print_stats(AppContext &c) {
  // double seconds = erpc::sec_since(c.tput_t0);
  double seconds = sec_since(c.tput_t0);

  // Min/max responses for a concurrent batch, to check for stagnated batches
  size_t max_resps = 0, min_resps = SIZE_MAX;
  for (size_t i = 0; i < FLAGS_concurrency; i++) {
    min_resps = std::min(min_resps, c.stat_resp_rx[i]);
    max_resps = std::max(max_resps, c.stat_resp_rx[i]);
    c.stat_resp_rx[i] = 0;
  }

  // Session throughput percentiles, used if rate computation is enabled
  std::vector<double> session_tput;
  if (erpc::kCcRateComp) {
    for (int session_num : c.session_num_vec_) {
      erpc::Timely *timely = c.rpc_->get_timely(session_num);
      session_tput.push_back(timely->get_rate_gbps());
    }
    std::sort(session_tput.begin(), session_tput.end());
  }

  double tput_mrps = c.rx_pkts / (seconds * 1000000);
  c.app_stats[c.thread_id_].mrps = tput_mrps;
  c.app_stats[c.thread_id_].num_re_tx = c.rpc_->pkt_loss_stats_.num_re_tx_;
  if (kAppMeasureLatency) {
    c.app_stats[c.thread_id_].lat_us_50 = c.latency.perc(0.50) / kAppLatFac;
    c.app_stats[c.thread_id_].lat_us_99 = c.latency.perc(0.99) / kAppLatFac;
    c.app_stats[c.thread_id_].lat_us_999 = c.latency.perc(0.999) / kAppLatFac;
    c.app_stats[c.thread_id_].lat_us_9999 = c.latency.perc(0.9999) / kAppLatFac;
  }

  size_t num_sessions = c.session_num_vec_.size();

  // Optional stats
  char lat_stat[100];
  sprintf(lat_stat, "[%.2f, %.2f us]", c.latency.perc(.50) / kAppLatFac,
      c.latency.perc(.99) / kAppLatFac);
  char rate_stat[100];
  if (session_tput.size() > 0) {
    sprintf(rate_stat, "[%.2f, %.2f, %.2f, %.2f Gbps]",
        erpc::kCcRateComp ? session_tput.at(num_sessions * 0.00) : -1,
        erpc::kCcRateComp ? session_tput.at(num_sessions * 0.05) : -1,
        erpc::kCcRateComp ? session_tput.at(num_sessions * 0.50) : -1,
        erpc::kCcRateComp ? session_tput.at(num_sessions * 0.95) : -1);
  }

  double rx_kpps = c.rx_pkts / (seconds * 1000000.f);
  double rx_gbps = c.rx_bytes * 8 / (seconds * 1000000000.0f);
  double tx_kpps = c.tx_pkts / (seconds * 1000000.f);
  double tx_gbps = c.tx_bytes * 8 / (seconds * 1000000000.0f);
  size_t re_tx = 0;
  if (!is_server)
    re_tx = c.rpc_->get_num_re_tx(c.session_num_vec_[0]);
  printf("Process %zu, thread %zu: "
      "Rx: %.3f (Kpps), %.3f (Gbps), Tx: %.3f (Kpps), %.3f (Gbps), Re_Tx: %zu\n",
      FLAGS_process_id, c.thread_id_, rx_kpps, rx_gbps, tx_kpps, tx_gbps, re_tx);

  /* printf( */
  /*     "Process %zu, thread %zu: %.3f Mrps, re_tx = %zu, still_in_wheel = %zu. " */
  /*     "RX: %zuK resps, %zuK reqs. Resps/batch: min %zuK, max %zuK. " */
  /*     "Latency: %s. Rate = %s.\n", */
  /*     FLAGS_process_id, c.thread_id_, tput_mrps, */
  /*     c.app_stats[c.thread_id_].num_re_tx, */
  /*     c.rpc_->pkt_loss_stats_.still_in_wheel_during_retx_, */
  /*     c.stat_resp_rx_tot / 1000, c.stat_req_rx_tot / 1000, min_resps / 1000, */
  /*     max_resps / 1000, kAppMeasureLatency ? lat_stat : "N/A", */
  /*     erpc::kCcRateComp ? rate_stat : "N/A"); */

  /* if (c.thread_id_ == 0) { */
  /*   app_stats_t accum; */
  /*   for (size_t i = 0; i < FLAGS_num_threads; i++) accum += c.app_stats[i]; */
  /*   if (kAppMeasureLatency) { */
  /*     accum.lat_us_50 /= FLAGS_num_threads; */
  /*     accum.lat_us_99 /= FLAGS_num_threads; */
  /*     accum.lat_us_999 /= FLAGS_num_threads; */
  /*     accum.lat_us_9999 /= FLAGS_num_threads; */
  /*   } */
  /*   c.tmp_stat_->write(accum.to_string()); */
  /* } */

  // clear stats for next measurement period
  c.rx_pkts = 0;
  c.rx_bytes = 0;
  c.tx_pkts = 0;
  c.tx_bytes = 0;
  c.rpc_->pkt_loss_stats_.num_re_tx_ = 0;
  c.latency.reset();
  // record the timestamp of the begining of next period
  clock_gettime(CLOCK_REALTIME, &c.tput_t0);
}

#endif
