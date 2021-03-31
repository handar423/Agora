/**
 * @file txrx_client.cc
 * @brief Implementation file for the radio txrx class
 */
#include "txrx_client.h"

#include <condition_variable>

#include "config.h"
#include "logger.h"

RadioTxRx::RadioTxRx(Config* const cfg, int n_threads, int in_core_id)
    : config_(cfg), thread_num_(n_threads), core_id_(in_core_id) {
  if ((kUseArgos == false) && (kUseUHD == false)) {
    udp_servers_.resize(config_->NumRadios());
    udp_clients_.resize(config_->NumRadios());
  } else {
    radioconfig_ = std::make_unique<ClientRadioConfig>(config_);
  }

  /* initialize random seed: */
  srand(time(nullptr));
  thread_sync_ = false;
}

RadioTxRx::RadioTxRx(Config* const config, int n_threads, int in_core_id,
                     moodycamel::ConcurrentQueue<EventData>* in_message_queue,
                     moodycamel::ConcurrentQueue<EventData>* in_task_queue,
                     moodycamel::ProducerToken** in_rx_ptoks,
                     moodycamel::ProducerToken** in_tx_ptoks)
    : RadioTxRx(config, n_threads, in_core_id) {
  message_queue_ = in_message_queue;
  task_queue_ = in_task_queue;
  rx_ptoks_ = in_rx_ptoks;
  tx_ptoks_ = in_tx_ptoks;
}

RadioTxRx::~RadioTxRx() {
  if (kUseArgos || kUseUHD) {
    radioconfig_->RadioStop();
  }

  for (auto& join_thread : txrx_threads_) {
    join_thread.join();
  }
}

bool RadioTxRx::StartTxRx(Table<char>& in_buffer, Table<int>& in_buffer_status,
                          size_t in_buffer_frame_num, size_t in_buffer_length,
                          char* in_tx_buffer, int* in_tx_buffer_status,
                          int in_tx_buffer_frame_num, int in_tx_buffer_length) {
  buffer_frame_num_ = in_buffer_frame_num;
  assert(in_buffer_length == config_->PacketLength() * buffer_frame_num_);
  buffer_length_ = in_buffer_length;
  buffer_ = &in_buffer;                // for save data
  buffer_status_ = &in_buffer_status;  // for save status

  tx_buffer_frame_num_ = in_tx_buffer_frame_num;
  tx_buffer_length_ = in_tx_buffer_length;
  tx_buffer_ = in_tx_buffer;
  tx_buffer_status_ = in_tx_buffer_status;

  if ((kUseArgos == true) || (kUseUHD == true)) {
    if (radioconfig_->RadioStart() == false) {
      return false;
    }
  }

  txrx_threads_.resize(thread_num_);

  for (int i = 0; i < thread_num_; i++) {
    // start socket thread
    if ((kUseArgos == true) && (config_->HwFramer() == true)) {
      txrx_threads_.at(i) = std::thread(&RadioTxRx::LoopTxRxArgos, this, i);
    } else if (kUseArgos || kUseUHD) {
      txrx_threads_.at(i) = std::thread(&RadioTxRx::LoopTxRxArgosSync, this, i);
    } else {
      txrx_threads_.at(i) = std::thread(&RadioTxRx::LoopTxRx, this, i);
    }
  }

  // give time for all threads to lock
  sleep(1);
  {
    std::unique_lock<std::mutex> locker(mutex_);
    this->thread_sync_ = true;
    this->cond_.notify_all();
  }
  return true;
}

struct Packet* RadioTxRx::RecvEnqueue(int tid, int radio_id, int rx_offset) {
  moodycamel::ProducerToken* local_ptok = rx_ptoks_[tid];
  char* rx_buffer = (*buffer_)[tid];
  int* rx_buffer_status = (*buffer_status_)[tid];
  size_t packet_length = config_->PacketLength();

  // if rx_buffer is full, exit
  if (rx_buffer_status[rx_offset] == 1) {
    std::printf("Receive thread %d rx_buffer full, offset: %d\n", tid,
                rx_offset);
    config_->Running(false);
    return (nullptr);
  }
  auto* pkt =
      reinterpret_cast<struct Packet*>(&rx_buffer[rx_offset * packet_length]);

  int rx_bytes = udp_servers_.at(radio_id)->Recv(
      reinterpret_cast<uint8_t*>(pkt), packet_length);

  if (0 > rx_bytes) {
    std::printf("RadioTxRx: receive failed\n");
    throw std::runtime_error("RadioTxRx: receive failed");
  } else if (static_cast<size_t>(rx_bytes) == packet_length) {
    // get the position in rx_buffer
    // move ptr & set status to full
    rx_buffer_status[rx_offset] = 1;

    // Push kPacketRX event into the queue.
    EventData rx_message(EventType::kPacketRX, rx_tag_t(tid, rx_offset).tag_);
    if (message_queue_->enqueue(*local_ptok, rx_message) == false) {
      std::printf("socket message enqueue failed\n");
      throw std::runtime_error("RadioTxRx: socket message enqueue failed");
    }
    return pkt;
  } else if (rx_bytes != 0) {
    std::printf(
        "RadioTxRx: receive failed on %d with less than full packet %d : "
        "%zu\n",
        radio_id, rx_bytes, packet_length);
    throw std::runtime_error(
        "RadioTxRx: receive failed with less than full packet");
  }
  return (nullptr);
}

int RadioTxRx::DequeueSend(int tid) {
  auto& c = config_;
  EventData event;
  if (!task_queue_->try_dequeue_from_producer(*tx_ptoks_[tid], event)) {
    return -1;
  }

  RtAssert(event.event_type_ == EventType::kPacketTX ||
               event.event_type_ == EventType::kPacketPilotTX,
           "RadioTxRx: Wrong Event Type in TX Queue!");

  // std::printf("tx queue length: %d\n", task_queue_->size_approx());
  size_t ant_id = gen_tag_t(event.tags_[0]).ant_id_;
  size_t frame_id = gen_tag_t(event.tags_[0]).frame_id_;
  std::vector<char> zeros(c->PacketLength(), 0);
  std::vector<char> pilot(c->PacketLength(), 0);
  std::memcpy(&pilot[Packet::kOffsetOfData], c->PilotCi16().data(),
              c->PacketLength() - Packet::kOffsetOfData);

  // Transmit uplink pilot
  for (size_t symbol_idx = 0; symbol_idx < c->Frame().NumPilotSyms();
       symbol_idx++) {
    if (kDebugPrintInTask) {
      std::printf(
          "In TX thread %d: Transmitted pilot in frame %zu, "
          "symbol %zu, "
          "ant %zu\n",
          tid, frame_id, c->Frame().GetPilotSymbol(symbol_idx), ant_id);
    }

    auto* pkt = (symbol_idx == ant_id) ? (struct Packet*)pilot.data()
                                       : (struct Packet*)zeros.data();
    new (pkt) Packet(frame_id, c->Frame().GetPilotSymbol(symbol_idx),
                     0 /* cell_id */, ant_id);

    udp_clients_.at(ant_id)->Send(
        config_->BsRruAddr(), config_->UeRruPort() + ant_id,
        reinterpret_cast<uint8_t*>(pkt), c->PacketLength());
  }
  if (event.event_type_ == EventType::kPacketPilotTX) {
    RtAssert(message_queue_->enqueue(
                 *rx_ptoks_[tid],
                 EventData(EventType::kPacketPilotTX, event.tags_[0])),
             "Socket message enqueue failed\n");
    return event.tags_[0];
  }

  // Transmit uplink Data
  for (size_t symbol_id = 0; symbol_id < c->Frame().NumULSyms(); symbol_id++) {
    size_t offset =
        (c->GetTotalDataSymbolIdxUl(frame_id, symbol_id) * c->UeAntNum()) +
        ant_id;

    if (kDebugPrintInTask) {
      std::printf(
          "In TX thread %d: Transmitted frame %zu, data symbol %zu, "
          "ant %zu, tag %zu, offset: %zu, msg_queue_length: %zu\n",
          tid, frame_id, c->Frame().GetULSymbol(symbol_id), ant_id,
          gen_tag_t(event.tags_[0]).tag_, offset,
          message_queue_->size_approx());
    }

    auto* pkt = (struct Packet*)(tx_buffer_ + offset * c->PacketLength());
    new (pkt) Packet(frame_id, c->Frame().GetULSymbol(symbol_id),
                     0 /* cell_id */, ant_id);

    // Send data (one OFDM symbol)
    udp_clients_.at(ant_id)->Send(
        config_->BsRruAddr(), config_->UeRruPort() + ant_id,
        reinterpret_cast<uint8_t*>(pkt), c->PacketLength());
  }
  if (event.event_type_ == EventType::kPacketTX) {
    RtAssert(
        message_queue_->enqueue(
            *rx_ptoks_[tid], EventData(EventType::kPacketTX, event.tags_[0])),
        "Socket message enqueue failed\n");
  }
  return event.tags_[0];
}

void* RadioTxRx::LoopTxRx(int tid) {
  PinToCoreWithOffset(ThreadType::kWorkerTXRX, core_id_, tid);
  size_t rx_offset = 0;
  size_t radio_lo = tid * config_->NumRadios() / thread_num_;
  size_t radio_hi = (tid + 1) * config_->NumRadios() / thread_num_;
  std::printf("Receiver thread %d has %zu radios\n", tid, radio_hi - radio_lo);

  size_t sock_buf_size = (1024 * 1024 * 64 * 8) - 1;
  for (size_t radio_id = radio_lo; radio_id < radio_hi; ++radio_id) {
    size_t local_port_id = config_->UeServerPort() + radio_id;
    udp_servers_.at(radio_id) =
        std::make_unique<UDPServer>(local_port_id, sock_buf_size);
    udp_clients_.at(radio_id) = std::make_unique<UDPClient>();
    MLPD_FRAME(
        "TXRX thread %d: set up UDP socket server listening to port %d"
        " with remote address %s:%d \n",
        tid, local_port_id, config_->BsRruAddr().c_str(),
        config_->UeRruPort() + radio_id);
  }

  size_t radio_id = radio_lo;
  while (config_->Running() == true) {
    if (-1 != DequeueSend(tid)) {
      continue;
    }
    // receive data
    struct Packet* pkt = RecvEnqueue(tid, radio_id, rx_offset);
    if (pkt == nullptr) {
      continue;
    }
    rx_offset = (rx_offset + 1) % buffer_frame_num_;

    if (++radio_id == radio_hi) {
      radio_id = radio_lo;
    }
  }
  return nullptr;
}

// dequeue_send_sdr
int RadioTxRx::DequeueSendArgos(int tid, long long time0) {
  auto& c = config_;
  auto& radio = radioconfig_;
  size_t packet_length = c->PacketLength();
  size_t num_samps = c->SampsPerSymbol();
  size_t frm_num_samps = num_samps * c->Frame().NumTotalSyms();

  // For UHD devices, first pilot should not be with the END_BURST flag
  // 1: HAS_TIME, 2: HAS_TIME | END_BURST
  int flags_tx_pilot = (kUseUHD && c->NumChannels() == 2) ? 1 : 2;

  EventData event;
  if (task_queue_->try_dequeue_from_producer(*tx_ptoks_[tid], event) == false) {
    return -1;
  }

  RtAssert(event.event_type_ == EventType::kPacketTX ||
               event.event_type_ == EventType::kPacketPilotTX,
           "Wrong Event Type in TX Queue!");

  size_t frame_id = gen_tag_t(event.tags_[0]).frame_id_;
  size_t ue_id = gen_tag_t(event.tags_[0]).ant_id_;
  size_t tx_frame_id = frame_id + TX_FRAME_DELTA;
  size_t ant_id = ue_id * c->NumChannels();
  long long tx_time(0);
  int r;

  // Transmit pilot
  if (c->HwFramer() == false) {
    size_t pilot_symbol_id = c->Frame().GetPilotSymbol(ant_id);

    tx_time = time0 + tx_frame_id * frm_num_samps +
              pilot_symbol_id * num_samps - c->ClTxAdvance().at(ue_id);
    r = radio->RadioTx(ue_id, pilot_buff0_.data(), num_samps, flags_tx_pilot,
                       tx_time);
    if (r < static_cast<int>(num_samps)) {
      std::cout << "BAD Write: (PILOT)" << r << "/" << num_samps << std::endl;
    }
    if (c->NumChannels() == 2) {
      pilot_symbol_id = c->Frame().GetPilotSymbol(ant_id + 1);
      tx_time = time0 + tx_frame_id * frm_num_samps +
                pilot_symbol_id * num_samps - c->ClTxAdvance().at(ue_id);
      r = radio->RadioTx(ue_id, pilot_buff1_.data(), num_samps, 2, tx_time);
      if (r < static_cast<int>(num_samps)) {
        std::cout << "BAD Write (PILOT): " << r << "/" << num_samps
                  << std::endl;
      }
    }
  }
  if (event.event_type_ == EventType::kPacketPilotTX) {
    RtAssert(message_queue_->enqueue(
                 *rx_ptoks_[tid],
                 EventData(EventType::kPacketPilotTX, event.tags_[0])),
             "Socket message enqueue failed\n");
    // kPacketPilotTX is scheduled when no Uplink data is present,
    // so return here!
    return event.tags_[0];
  }

  // Transmit data
  for (size_t symbol_id = 0; symbol_id < c->Frame().NumULSyms(); symbol_id++) {
    size_t tx_symbol_id = c->Frame().GetULSymbol(symbol_id);
    size_t offset =
        (c->GetTotalDataSymbolIdxUl(frame_id, symbol_id) * c->UeAntNum()) +
        ant_id;

    void* txbuf[2];
    for (size_t ch = 0; ch < c->NumChannels(); ++ch) {
      auto* pkt = reinterpret_cast<struct Packet*>(
          tx_buffer_ + (offset + ch) * packet_length);
      txbuf[ch] = (void*)pkt->data_;
      tx_buffer_status_[offset + ch] = 0;
    }
    tx_time = c->HwFramer()
                  ? ((long long)tx_frame_id << 32) | (tx_symbol_id << 16)
                  : time0 + tx_frame_id * frm_num_samps +
                        tx_symbol_id * num_samps - c->ClTxAdvance().at(ue_id);
    int flags_tx_symbol = 1;  // HAS_TIME
    if (tx_symbol_id == c->Frame().GetULSymbolLast()) {
      flags_tx_symbol = 2;  // HAS_TIME & END_BURST, fixme
    }
    r = radio->RadioTx(ue_id, txbuf, num_samps, flags_tx_symbol, tx_time);
    if (r < static_cast<int>(num_samps)) {
      std::cout << "BAD Write (UL): " << r << "/" << num_samps << std::endl;
    }
  }

  RtAssert(
      message_queue_->enqueue(*rx_ptoks_[tid],
                              EventData(EventType::kPacketTX, event.tags_[0])),
      "Socket message enqueue failed\n");
  return event.tags_[0];
}

struct Packet* RadioTxRx::RecvEnqueueArgos(int tid, size_t radio_id,
                                           size_t& frame_id, size_t& symbol_id,
                                           size_t rx_offset,
                                           bool dummy_enqueue) {
  auto& c = config_;
  moodycamel::ProducerToken* local_ptok = rx_ptoks_[tid];
  char* rx_buffer = (*buffer_)[tid];
  int* rx_buffer_status = (*buffer_status_)[tid];
  int num_samps = c->SampsPerSymbol();
  int packet_length = c->PacketLength();

  // if buffer is full, exit
  if (rx_buffer_status[rx_offset] == 1) {
    std::printf("RX thread %d at rx_offset %zu buffer full\n", tid, rx_offset);
    c->Running(false);
    return nullptr;
  }

  long long rx_time(0);
  struct Packet* pkt[c->NumChannels()];
  void* samp[c->NumChannels()];
  for (size_t ch = 0; ch < c->NumChannels(); ++ch) {
    pkt[ch] =
        (struct Packet*)&rx_buffer[((rx_offset + ch) % buffer_frame_num_) *
                                   packet_length];
    samp[ch] = pkt[ch]->data_;
  }
  if (dummy_enqueue == false) {
    ClientRadioConfig* radio = radioconfig_.get();
    int r = radio->RadioRx(radio_id, samp, num_samps, rx_time);
    if (r < num_samps) {
      std::cerr << "BAD Receive(" << r << "/" << num_samps << ") at Time "
                << rx_time << std::endl;
    }
    if (r < 0) {
      std::cerr << "Receive error! Stopping... " << std::endl;
      c->Running(false);
      return nullptr;
    }
    if (c->HwFramer()) {
      frame_id = (size_t)(rx_time >> 32);
      symbol_id = (size_t)((rx_time >> 16) & 0xFFFF);
    } else {
      // assert(c->HwFramer()
      //    || (((rxTime - time0) / (num_samps * c->frame().NumTotalSyms()))
      //           == frame_id));
    }
  }
  if (kDebugPrintInTask) {
    std::printf(
        "downlink receive: thread %d, frame_id %zu, symbol_id "
        "%zu, radio_id %zu "
        "rxtime %llx\n",
        tid, frame_id, symbol_id, radio_id, rx_time);
  }
  size_t ant_id = radio_id * c->NumChannels();
  for (size_t ch = 0; ch < c->NumChannels(); ++ch) {
    new (pkt[ch]) Packet(frame_id, symbol_id, 0 /* cell_id */, ant_id + ch);

    // get the position in rx_buffer
    // move ptr & set status to full
    rx_buffer_status[rx_offset + ch] = 1;

    EventData rx_message(EventType::kPacketRX,
                         rx_tag_t(tid, rx_offset + ch).tag_);

    RtAssert(message_queue_->enqueue(*local_ptok, rx_message),
             "socket message enqueue failed");
  }
  return pkt[0];
}

void* RadioTxRx::LoopTxRxArgos(int tid) {
  PinToCoreWithOffset(ThreadType::kWorkerTXRX, core_id_, tid);
  auto& c = config_;
  size_t num_radios = c->NumRadios();
  size_t radio_lo = tid * num_radios / thread_num_;
  size_t radio_hi = (tid + 1) * num_radios / thread_num_;
  std::printf("receiver thread %d has radios %zu to %zu (%zu)\n", tid, radio_lo,
              radio_hi - 1, radio_hi - radio_lo);

  // Use mutex to sychronize data receiving across threads
  {
    std::unique_lock<std::mutex> locker(mutex_);
    std::printf("Thread %d: waiting for release\n", tid);
    cond_.wait(locker, [this] { return this->thread_sync_; });
  }

  ClientRadioConfig* radio = radioconfig_.get();

  std::vector<int> all_trigs(radio_hi - radio_lo, 0);
  struct timespec tv;
  struct timespec tv2;
  clock_gettime(CLOCK_MONOTONIC, &tv);

  size_t rx_offset = 0;
  size_t frame_id(0);
  size_t symbol_id(0);
  size_t radio_id = radio_lo;
  while (c->Running() == true) {
    clock_gettime(CLOCK_MONOTONIC, &tv2);
    double diff =
        ((tv2.tv_sec - tv.tv_sec) * 1e9 + (tv2.tv_nsec - tv.tv_nsec)) / 1e9;
    if (diff > 2) {
      for (size_t it = radio_lo; it < radio_hi; it++) {
        int total_trigs = radio->Triggers(it);
        std::cout << "radio: " << it << ", new triggers: "
                  << total_trigs - all_trigs[it - radio_lo]
                  << ", total: " << total_trigs << std::endl;
        all_trigs[it - radio_lo] = total_trigs;
      }
      tv = tv2;
    }
    // transmit data
    if (-1 != DequeueSendArgos(tid, 0)) {
      continue;
    }

    struct Packet* pkt =
        RecvEnqueueArgos(tid, radio_id, frame_id, symbol_id, rx_offset, false);
    if (pkt == nullptr) {
      continue;
    }

    rx_offset += c->NumChannels();
    rx_offset %= buffer_frame_num_;
    if (++radio_id == radio_hi) {
      radio_id = radio_lo;
    }
  }
  return nullptr;
}

void* RadioTxRx::LoopTxRxArgosSync(int tid) {
  // FIXME: This only works when there is 1 radio per thread.
  PinToCoreWithOffset(ThreadType::kWorkerTXRX, core_id_, tid);
  auto& c = config_;
  size_t num_samps = c->SampsPerSymbol();
  size_t frm_num_samps = num_samps * c->Frame().NumTotalSyms();
  ClientRadioConfig* radio = radioconfig_.get();
  long long rx_time(0);
  int radio_id = tid;
  int sync_index(-1);
  int rx_offset(0);
  size_t cursor(0);
  std::stringstream sout;
  std::vector<std::complex<int16_t>> frm_buff0(frm_num_samps, 0);
  std::vector<std::complex<int16_t>> frm_buff1(frm_num_samps, 0);
  std::vector<void*> frm_rx_buff(2);
  std::vector<std::complex<int16_t>> zeros0(c->SampsPerSymbol(), 0);
  std::vector<std::complex<int16_t>> zeros1(c->SampsPerSymbol(), 0);

  // Use mutex to sychronize data receiving across threads
  {
    std::unique_lock<std::mutex> locker(mutex_);

    frm_rx_buff.at(0) = frm_buff0.data();
    pilot_buff0_.resize(2);
    pilot_buff1_.resize(2);
    pilot_buff0_.at(0) = c->PilotCi16().data();
    if (c->NumChannels() == 2) {
      pilot_buff0_.at(1) = zeros0.data();
      pilot_buff1_.at(0) = zeros1.data();
      pilot_buff1_.at(1) = c->PilotCi16().data();
      frm_rx_buff.at(1) = frm_buff1.data();
    }

    std::printf("Thread %d: waiting for release\n", tid);
    cond_.wait(locker, [this] { return this->thread_sync_; });
  }

  // Keep receiving one frame of data until a beacon is found
  // Perform initial beacon detection every kBeaconDetectInterval frames
  while ((c->Running() == true) && sync_index < 0) {
    int r;
    for (size_t find_beacon_retry = 0;
         find_beacon_retry < kBeaconDetectInterval; find_beacon_retry++) {
      r = radio->RadioRx(radio_id, frm_rx_buff.data(), frm_num_samps, rx_time);

      if (r != static_cast<int>(frm_num_samps)) {
        std::cerr << "BAD SYNC Receive(" << r << "/" << frm_num_samps
                  << ") at Time " << rx_time << std::endl;
        continue;
      }
    }

    // convert data to complex float for sync detection
    std::vector<std::complex<float>> sync_buff(frm_num_samps, 0);
    for (size_t i = 0; i < frm_num_samps; i++) {
      sync_buff[i] = (std::complex<float>(frm_buff0[i].real() / 32768.0,
                                          frm_buff0[i].imag() / 32768.0));
    }
    sync_index = CommsLib::FindBeaconAvx(sync_buff, c->GoldCf32());
    if (sync_index >= 0) {
      MLPD_INFO("Client %d: Beacon detected at Time %lld, sync_index: %d\n",
                radio_id, rx_time, sync_index);
      rx_offset = sync_index - c->BeaconLen() - c->OfdmTxZeroPrefix();
    }
  }

  // Read rx_offset to align with the begining of a frame
  if (rx_offset > 0) {
    radio->RadioRx(radio_id, frm_rx_buff.data(), rx_offset, rx_time);
  }

  long long time0(0);
  size_t frame_id(0);
  size_t symbol_id(0);

  bool resync = false;
  size_t resync_retry_cnt(0);
  size_t resync_retry_max(100);
  size_t resync_success(0);
  rx_offset = 0;
  while (c->Running() == true) {
    if (c->FramesToTest() > 0 && frame_id > c->FramesToTest()) {
      c->Running(false);
      break;
    }

    // recv corresponding to symbol_id = 0 (Beacon)
    int r = radio->RadioRx(radio_id, frm_rx_buff.data(), num_samps + rx_offset,
                           rx_time);
    if (r != static_cast<int>(num_samps + rx_offset)) {
      std::cerr << "BAD Beacon Receive(" << r << "/" << num_samps
                << ") at Time " << rx_time << std::endl;
    }
    if (r < 0) {
      std::cerr << "Receive error! Stopping... " << std::endl;
      c->Running(false);
      break;
    }
    if (frame_id == 0) {
      time0 = rx_time;
    }

    // Dummy enqueue for received beacon to use in scheduler
    auto* pkt =
        RecvEnqueueArgos(tid, radio_id, frame_id, symbol_id, cursor, true);
    if (pkt == nullptr) {
      break;
    }
    symbol_id++;
    cursor += c->NumChannels();
    cursor %= buffer_frame_num_;

    // resync every X=1000 frames:
    // TODO: X should be a function of sample rate and max CFO
    if (frame_id / 1000 > 0 && frame_id % 1000 == 0) {
      resync = true;
    }
    rx_offset = 0;
    if (resync) {
      // convert data to complex float for sync detection
      std::vector<std::complex<float>> sync_buff;
      sync_buff.reserve(num_samps);
      for (size_t i = 0; i < num_samps; i++) {
        sync_buff.emplace_back(frm_buff0[i].real() / 32768.0,
                               frm_buff0[i].imag() / 32768.0);
      }
      sync_index = CommsLib::FindBeaconAvx(sync_buff, c->GoldCf32());
      if (sync_index >= 0) {
        rx_offset = sync_index - c->BeaconLen() - c->OfdmTxZeroPrefix();
        time0 += rx_offset;
        resync = false;
        resync_retry_cnt = 0;
        resync_success++;
        MLPD_INFO(
            "Client %d: Re-syncing with offset: %d, after %zu "
            "tries, index: %d\n",
            radio_id, rx_offset, resync_retry_cnt + 1, sync_index);
      } else {
        resync_retry_cnt++;
      }
    }
    if (resync && resync_retry_cnt > resync_retry_max) {
      MLPD_ERROR(
          "Client %d: Exceeded resync retry limit (%zu) for client "
          "%d reached after %zu resync successes at "
          "frame: %zu.  Stopping!\n",
          radio_id, resync_retry_max, tid, resync_success, frame_id);
      c->Running(false);
      break;
    }

    // schedule transmit pilots and symbols
    while (-1 != DequeueSendArgos(tid, time0)) {
      ;
    }

    // receive the remaining of the frame
    for (; symbol_id < c->Frame().NumTotalSyms(); symbol_id++) {
      if (!config_->IsPilot(frame_id, symbol_id) &&
          !(config_->IsDownlink(frame_id, symbol_id))) {
        radio->RadioRx(radio_id, frm_rx_buff.data(), num_samps, rx_time);
        if (r < static_cast<int>(num_samps)) {
          std::cerr << "BAD Receive(" << r << "/" << num_samps << ") at Time "
                    << rx_time << std::endl;
        }
        if (r < 0) {
          std::cerr << "Receive error! Stopping... " << std::endl;
          c->Running(false);
          break;
        }
        if (kDebugPrintInTask) {
          std::printf(
              "idle receive: thread %d, frame_id %zu, symbol_id %zu, "
              "radio_id %d "
              "rxtime %llx\n",
              tid, frame_id, symbol_id, radio_id, rx_time);
        }
      } else {
        struct Packet* rx_pkt =
            RecvEnqueueArgos(tid, radio_id, frame_id, symbol_id, cursor, false);
        if (rx_pkt == nullptr) {
          break;
        }

        cursor += c->NumChannels();
        cursor %= buffer_frame_num_;
      }
    }
    frame_id++;
    symbol_id = 0;
  }
  return nullptr;
}
