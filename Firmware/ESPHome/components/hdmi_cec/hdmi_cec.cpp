#include "hdmi_cec.h"
#include "esphome/core/log.h"

#ifdef USE_CEC_DECODER
#include "cec_decoder.h"
#endif

#ifdef USE_ESP32
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#endif

namespace esphome {
namespace hdmi_cec {

static const char *const TAG = "hdmi_cec";
// receiver constants
static const uint32_t START_BIT_MIN_US = 3500;
static const uint32_t HIGH_BIT_MIN_US = 400;
static const uint32_t HIGH_BIT_MAX_US = 800;
// transmitter constants
static const uint32_t TOTAL_BIT_US = 2400;
static const uint32_t HIGH_BIT_US = 600;
static const uint32_t LOW_BIT_US = 1500;
static const uint32_t START_BIT_LOW_US = 3700;
static const uint32_t START_BIT_TOTAL_US = 4500;
// Longest low pulse a follower still decodes as the intended bit value. These
// are the RECEIVER's decision points, not the tighter transmit tolerances: a
// logical 1 flips once it reaches the 1.05 ms sample instant, a logical 0 only
// once it is long enough to pass for a start bit. Overrunning by less than that
// is out of spec but still decodes correctly, and aborting on it costs more
// retries than it saves.
static const uint32_t HIGH_BIT_MAX_LOW_US = 1000;
static const uint32_t LOW_BIT_MAX_LOW_US = 3400;
static const uint32_t START_BIT_MAX_LOW_US = 4200;
// "Safe sample period" for reading the bus back during a transmitted logical 1.
// The end is stretched past the spec's 1.25 ms so that a late release still
// leaves a usable window: whoever answers holds the line down until 1.5 ms, and
// the reading latches, so a wider window can only add evidence.
static const uint32_t SAMPLE_START_US = 850;
static const uint32_t SAMPLE_END_US = 1400;
static const uint32_t LEGACY_SAMPLE_US = 1050;
static const uint32_t START_SAMPLE_START_US = 4000;
static const uint32_t START_SAMPLE_END_US = 4400;
// An acknowledging follower keeps the line down for a full logical 0.
static const uint32_t ACK_HOLD_US = 1500;
// Let the line rise through the bus capacitance before believing a read.
static const uint32_t RISE_SETTLE_US = 250;
// arbitration and retransmission
static const size_t MAX_ATTEMPTS = 5;
// Yield interval for bus-free wait loop: break long waits into chunks of this
// duration and call yield() between each, so the FreeRTOS scheduler can run
// other tasks and the Task Watchdog Timer is not triggered.
// 1ms is a good trade-off: short enough to maintain CEC timing accuracy
// (bus-free periods are 7200-16800µs), yet long enough to avoid excessive
// context-switch overhead from yielding on every microsecond-scale iteration.
static const uint32_t YIELD_INTERVAL_US = 1000;

static const gpio::Flags INPUT_MODE_FLAGS = gpio::FLAG_INPUT | gpio::FLAG_PULLUP;
static const gpio::Flags OUTPUT_MODE_FLAGS = gpio::FLAG_OUTPUT | gpio::FLAG_OPEN_DRAIN;
// Note: the esp8266 does NOT support 'FLAG_OUTPUT | FLAG_OPEN_DRAIN | FLAG_PULLUP' as opposed to the esp32 and rp2040.
// (see 'flags_to_mode' in its esphome gpio.cpp).
// So, unfortunately, in 'OPEN_DRAIN' mode, the required 'PULLUP' cannot be activated.
// Therefor, 'OUTPUT' will be used only to write '0': For writing a '1' the mode is switched to 'INPUT | PULLUP'.
// That allows to safely check for cec bus conflicts on writing '1' (avoid short-circuit with other bus initiators).

Frame::Frame(uint8_t initiator_addr, uint8_t target_addr, const std::vector<uint8_t> &payload)
    : std::vector<uint8_t>(1 + payload.size(), (uint8_t) (0)) {
  this->at(0) = ((initiator_addr & 0xf) << 4) | (target_addr & 0xf);
  std::memcpy(this->data() + 1, payload.data(), payload.size());
}

std::string Frame::to_string(bool skip_decode) const {
  std::string result;
  char part_buffer[3];
  for (auto it = this->cbegin(); it != this->cend(); it++) {
    uint8_t byte_value = *it;
    sprintf(part_buffer, "%02X", byte_value);
    result += part_buffer;

    if (it != (this->end() - 1)) {
      result += ":";
    }
  }
#ifdef USE_CEC_DECODER
  if (!skip_decode) {
    Decoder decoder(*this);
    result += " => " + decoder.decode();
  }
#endif
  return result;
}

inline void IRAM_ATTR HDMICEC::set_pin_input_high() {
  pin_->pin_mode(INPUT_MODE_FLAGS);
}

inline void IRAM_ATTR HDMICEC::set_pin_output_low() {
  pin_->pin_mode(OUTPUT_MODE_FLAGS);
  pin_->digital_write(false);
}

// Busy-wait until offset_us after start_us. The signed comparison is both
// rollover-safe and returns immediately when the instant already passed, so a
// preemption can never turn into a multi-minute spin (upstream issue #50).
static inline void busy_wait_until(uint32_t start_us, uint32_t offset_us) {
  while ((int32_t) (micros() - (start_us + offset_us)) < 0) {
  }
}

// Raises the calling task's priority for the duration of one bit-banged frame.
// The ESPHome main task runs at priority 1, so on the single-core C3 the WiFi
// stack preempts it mid-bit and stretches low pulses past the point where the
// follower decodes the wrong value: that is why transmits fail while receive
// (interrupt driven) stays reliable. Measured on a live bus, 200 frames each:
// priority 1 and priority 19 both leave ~6% of frames unacknowledged with low
// pulses overrunning by up to 954 us, while 24 (above the WiFi task at 23)
// gives 0 retries and a worst-case overrun of 13 us. Anything at or below the
// WiFi task's priority is not worth enabling.
class PriorityBoost {
 public:
  explicit PriorityBoost(uint8_t target) {
#ifdef USE_ESP32
    if (target == 0) {
      return;
    }
    TaskHandle_t task = xTaskGetCurrentTaskHandle();
    UBaseType_t current = uxTaskPriorityGet(task);
    if (target <= current) {
      return;
    }
    task_ = task;
    previous_ = current;
    vTaskPrioritySet(task_, target);
#endif
  }
  ~PriorityBoost() {
#ifdef USE_ESP32
    if (task_ != nullptr) {
      vTaskPrioritySet(task_, previous_);
    }
#endif
  }

 private:
#ifdef USE_ESP32
  TaskHandle_t task_{nullptr};
  UBaseType_t previous_{0};
#endif
};

void HDMICEC::setup() {
  this->pin_->setup();
  isr_pin_ = pin_->to_isr();
  frames_queue_.reset();
  pin_->attach_interrupt(HDMICEC::gpio_intr_, this, gpio::INTERRUPT_ANY_EDGE);
  set_pin_input_high();
}

void HDMICEC::dump_config() {
  ESP_LOGCONFIG(TAG, "HDMI-CEC");
  LOG_PIN("  pin: ", pin_);
  ESP_LOGCONFIG(TAG, "  address: %x", address_);
  ESP_LOGCONFIG(TAG, "  promiscuous mode: %s", (promiscuous_mode_ ? "yes" : "no"));
  ESP_LOGCONFIG(TAG, "  monitor mode: %s", (monitor_mode_ ? "yes" : "no"));
  ESP_LOGCONFIG(TAG, "  tx priority boost: %u", tx_priority_);
  ESP_LOGCONFIG(TAG, "  tx window sampling: %s", (tx_window_sampling_ ? "yes" : "no"));
  ESP_LOGCONFIG(TAG, "  tx strict timing: %s", (tx_strict_timing_ ? "yes" : "no"));
}

void HDMICEC::loop() {
  while (const Frame *frame = frames_queue_.front()) {
    uint8_t header = frame->front();
    uint8_t src_addr = ((header & 0xF0) >> 4);
    uint8_t dest_addr = (header & 0x0F);

    if (!promiscuous_mode_ && (dest_addr != 0x0F) && (dest_addr != address_)) {
      // ignore frames not meant for us, recycle frame buffer
      frames_queue_.push_front();
      continue;
    }

    if (frame->size() == 1) {
      // don't process pings. they're already dealt with by the acknowledgement mechanism
      ESP_LOGV(TAG, "ping received: 0x%01X -> 0x%01X", src_addr, dest_addr);
      frames_queue_.push_front();
      continue;
    }

    ESP_LOGD(TAG, "[received] %s", frame->to_string().c_str());

    std::vector<uint8_t> data(frame->begin() + 1, frame->end());

    // recycle received frame buffer
    frames_queue_.push_front();

    // Process on_message triggers
    bool handled_by_trigger = false;
    uint8_t opcode = data[0];
    for (auto trigger : message_triggers_) {
      bool can_trigger = (
        (!trigger->source_.has_value()      || (trigger->source_ == src_addr)) &&
        (!trigger->destination_.has_value() || (trigger->destination_ == dest_addr)) &&
        (!trigger->opcode_.has_value()      || (trigger->opcode_ == opcode)) &&
        (!trigger->data_.has_value() ||
          (data.size() == trigger->data_->size() && std::equal(trigger->data_->begin(), trigger->data_->end(), data.begin()))
        )
      );
      if (can_trigger) {
        trigger->trigger(src_addr, dest_addr, data);
        handled_by_trigger = true;
      }
    }

    // If nothing in on_message handled this message, we try to run the built-in handlers
    bool is_directly_addressed = (dest_addr != 0xF && dest_addr == address_);
    if (is_directly_addressed && !handled_by_trigger) {
      try_builtin_handler_(src_addr, dest_addr, data);
    }
  }
}

uint8_t logical_address_to_device_type(uint8_t logical_address) {
  switch (logical_address) {
    // "TV"
    case 0x0:
      return 0x00; // "TV"

    // "Audio System"
    case 0x5:
      return 0x05; // "Audio System"

    // "Recording 1"
    case 0x1:
    // "Recording 2"
    case 0x2:
    // "Recording 3"
    case 0x9:
      return 0x01; // "Recording Device"

    // "Tuner 1"
    case 0x3:
    // "Tuner 2"
    case 0x6:
    // "Tuner 3"
    case 0x7:
    // "Tuner 4"
    case 0xA:
      return 0x03; // "Tuner"

    default:
      return 0x04; // "Playback Device"
  }
}

void HDMICEC::try_builtin_handler_(uint8_t source, uint8_t destination, const std::vector<uint8_t> &data) {
  if (data.empty()) {
    return;
  }

  uint8_t opcode = data[0];
  switch (opcode) {
    // "Get CEC Version" request
    case 0x9F: {
      // reply with "CEC Version" (0x9E)
      send(address_, source, {0x9E, 0x04});
      break;
    }

    // "Give Device Power Status" request
    case 0x8F: {
      // reply with "Report Power Status" (0x90)
      send(address_, source, {0x90, 0x00}); // "On"
      break;
    }

    // "Give OSD Name" request
    case 0x46: {
      // reply with "Set OSD Name" (0x47)
      std::vector<uint8_t> data = { 0x47 };
      data.insert(data.end(), osd_name_bytes_.begin(), osd_name_bytes_.end());
      send(address_, source, data);
      break;
    }

    // "Give Physical Address" request
    case 0x83: {
      // reply with "Report Physical Address" (0x84)
      auto physical_address_bytes = decode_value(physical_address_);
      std::vector<uint8_t> data = { 0x84 };
      data.insert(data.end(), physical_address_bytes.begin(), physical_address_bytes.end());
      // Device Type
      data.push_back(logical_address_to_device_type(address_));
      // Broadcast Physical Address
      send(address_, 0xF, data);
      break;
    }

    // Ignore "Feature Abort" opcode responses
    case 0x00:
      // no-op
      break;

    // default case (no built-in handler + no on_message handler) => message not supported => send "Feature Abort"
    default:
      send(address_, source, {0x00, opcode, 0x00});
      break;
  }
}

bool HDMICEC::send(uint8_t source, uint8_t destination, const std::vector<uint8_t> &data_bytes, bool count_stats) {
  if (monitor_mode_) return false;

  bool is_broadcast = (destination == 0xF);

  // Occupancy probes (count_stats=false) must not pollute the reliability
  // counters: polling a free address deliberately exhausts every retry with
  // no ack, which would read as a failing transmitter on a perfect one.
  // Snapshot the whole stats block and restore it on every exit path, so a
  // probe (including its deep timing/sample counters) leaves stats untouched.
  const CecTxStats stats_snapshot = tx_stats_;
  auto finish = [&](bool result) {
    if (!count_stats) tx_stats_ = stats_snapshot;
    return result;
  };

  // prepare the bytes to send
  Frame frame(source, destination, data_bytes);
  ESP_LOGD(TAG, "[sending] %s", frame.to_string().c_str());
  tx_stats_.frames++;

  {
    LockGuard send_lock(send_mutex_);
    // Bus 'Signal Free' time between transmissions, according to the HDMI-CEC standard, shall be a minimum of:
    //  - 7 bit periods between successive transmissions of same sender
    //  - 5 bit periods between transmissions of different senders
    //  - 3 bit periods for resend of a failed transmission attempt
    uint8_t free_bit_periods = (last_sent_us_ > last_falling_edge_us_) ? 7 : 5;

    // Total timeout: abort if we can't send within 2 seconds (prevents infinite blocking on busy bus)
    static const uint32_t SEND_TIMEOUT_US = 2000000;
    const uint32_t send_start_us = micros();

    for (size_t i = 0; i < MAX_ATTEMPTS; i++) {
      int32_t delay = 0;
      // Per-attempt timeout for bus-free wait: 200ms max per attempt
      const uint32_t attempt_start_us = micros();
      static const uint32_t ATTEMPT_TIMEOUT_US = 200000;

      while (true) {
        // Wrap-safe bus-free check: measure time since the last bus activity instead of
        // computing an absolute deadline, which goes wrong when the last-activity
        // timestamps are stale or micros() rolls over (upstream issue #50).
        const uint32_t required_free_us = (uint32_t) free_bit_periods * TOTAL_BIT_US;
        const uint32_t since_activity_us = micros() - std::max(last_sent_us_, (uint32_t) last_falling_edge_us_);
        if (since_activity_us >= required_free_us) {
          break;
        }
        delay = (int32_t) (required_free_us - since_activity_us);
        // Check total timeout
        if ((micros() - send_start_us) > SEND_TIMEOUT_US) {
          ESP_LOGW(TAG, "HDMICEC::send(): total timeout reached (2s), aborting");
          tx_stats_.bus_timeouts++;
          tx_stats_.failed++;
          return finish(false);
        }
        // Check per-attempt timeout (bus constantly busy)
        if ((micros() - attempt_start_us) > ATTEMPT_TIMEOUT_US) {
          ESP_LOGW(TAG, "HDMICEC::send(): attempt %d bus-wait timeout (200ms), retrying", i + 1);
          break;
        }
        ESP_LOGV(TAG, "HDMICEC::send(): waiting %d usec for bus free period", delay);
        if (delay >= (int32_t) YIELD_INTERVAL_US) {
          delay_microseconds_safe(YIELD_INTERVAL_US);
          yield();
        } else {
          delay_microseconds_safe(delay);
        }
        // Note: during this delay, the 'last_falling_edge_us_' might be incremented by 'gpio_intr_', requiring further wait
        free_bit_periods = 5;
      }

      // Skip frame send if we broke out due to per-attempt timeout
      if ((micros() - attempt_start_us) > ATTEMPT_TIMEOUT_US) {
        tx_stats_.bus_timeouts++;
        free_bit_periods = 3;
        yield();
        continue;
      }

      ESP_LOGV(TAG, "HDMICEC::send(): bus available, sending frame...");

      tx_stats_.attempts++;
      auto result = send_frame_(frame, is_broadcast);
      if (result == SendResult::Success) {
        ESP_LOGD(TAG, "frame sent and acknowledged");
        tx_stats_.ok++;
        return finish(true);
      }
      const char *reason = "No Ack received";
      if (result == SendResult::BusCollision) {
        tx_stats_.collisions++;
        reason = "Bus Collision";
      } else if (result == SendResult::TimingFault) {
        reason = "Bit Timing Fault";
      } else {
        tx_stats_.no_ack++;
      }
      ESP_LOGI(TAG, "HDMICEC::send(): frame not sent: %s", reason);
      // attempt retransmission with smaller free time gap
      free_bit_periods = 3;
      yield();
    }
  }

  ESP_LOGE(TAG, "HDMICEC::send(): send failed after %d attempts", MAX_ATTEMPTS);
  tx_stats_.failed++;
  return finish(false);
}

SendResult HDMICEC::send_frame_(const Frame &frame, bool is_broadcast) {
  pin_->detach_interrupt();  // do NOT listen for pin changes while sending
  PriorityBoost boost(tx_priority_);
  auto result = SendResult::Success;

  BitSample sample = send_start_bit_();
  if (sample == BitSample::Fault) {
    result = SendResult::TimingFault;
  } else if (sample == BitSample::Low) {
    result = SendResult::BusCollision;
  }

  // for each byte of the frame:
  for (auto it = frame.begin(); (result == SendResult::Success) && it != frame.end(); ++it) {
    uint8_t current_byte = *it;

    // 1. send the current byte
    for (int8_t i = 7; i >= 0; i--) {
      bool bit_value = ((current_byte >> i) & 0b1);
      if ((it == frame.begin()) && i >= 4 && bit_value) {
        // my initiator address bit is 1: test for bus collision
        // see the specification in the HDMI standard, section "CEC Arbitration"
        sample = send_high_and_test_();
        if (sample == BitSample::Fault) {
          result = SendResult::TimingFault;
          break;
        }
        if (sample == BitSample::Low) {
          // immediatly stop sending bits due to bus collision:
          // the other concurrent initiator with lower address might not have detected the conflict
          result = SendResult::BusCollision;
          break;
        }
      } else if (!send_bit_(bit_value)) {
        result = SendResult::TimingFault;
        break;
      }
    }
    if (result != SendResult::Success) {
      break;
    }

    // 2. send EOM bit (logic 1 if this is the last byte of the frame)
    bool is_eom = (it == (frame.end() - 1));
    if (!send_bit_(is_eom)) {
      result = SendResult::TimingFault;
      break;
    }

    // 3. send ack bit and test bit value from destination(s)
    sample = send_high_and_test_();
    if (sample == BitSample::Fault) {
      result = SendResult::TimingFault;
      break;
    }
    // a follower acknowledges by holding the line low; on a broadcast a low
    // instead means somebody rejected the frame
    bool acked = (sample == BitSample::Low);
    if (acked == is_broadcast) {
      result = SendResult::NoAck;
      break;
    }
  }
  // capture last bus busy time also for bus writes (with interrupts off)
  last_sent_us_ = micros();
  // the line is idle high again: keep the edge detector in step, otherwise the
  // next falling edge is discarded as spurious
  last_level_ = true;
  pin_->attach_interrupt(HDMICEC::gpio_intr_, this, gpio::INTERRUPT_ANY_EDGE);
  return result;
}

// Reads the bus back across a window instead of at a single instant. A single
// read that preemption pushes past the follower's release reads high and looks
// like a missing acknowledgement, which is the dominant cause of spurious
// retries on a bit-banged transmitter sharing a core with the WiFi stack.
void HDMICEC::sample_line_(uint32_t start_us, uint32_t release_us, uint32_t window_start_us, uint32_t window_end_us,
                           uint32_t legacy_us, LineSample *out) {
  *out = LineSample{false, false, false, false};

  uint32_t window_start = start_us + window_start_us;
  if ((int32_t) (window_start - (release_us + RISE_SETTLE_US)) < 0) {
    window_start = release_us + RISE_SETTLE_US;
  }
  const uint32_t window_end = start_us + window_end_us;
  const uint32_t legacy_at = start_us + legacy_us;

  while ((int32_t) (micros() - window_end) < 0) {
    const uint32_t now = micros();
    if ((int32_t) (now - window_start) < 0) {
      continue;
    }
    const bool low = !pin_->digital_read();
    out->sampled = true;
    // confirm a low before believing it, so a glitch cannot fake an ack
    if (low && !pin_->digital_read()) {
      out->low_seen = true;
    }
    if (!out->legacy_valid && (int32_t) (now - legacy_at) >= 0) {
      out->legacy_valid = true;
      out->legacy_low = low;
    }
  }
}

BitSample HDMICEC::send_start_bit_() {
  const uint32_t start_us = micros();
  set_pin_output_low();
  busy_wait_until(start_us, START_BIT_LOW_US);
  set_pin_input_high();
  const uint32_t release_us = micros();
  const uint32_t low_us = release_us - start_us;

  BitSample result = BitSample::High;
  if (low_us > START_BIT_MAX_LOW_US) {
    tx_stats_.timing_faults++;
    if (tx_strict_timing_) {
      result = BitSample::Fault;
    }
  } else {
    // no other initiator may pull the line down during the high half
    LineSample sample;
    sample_line_(start_us, release_us, START_SAMPLE_START_US, START_SAMPLE_END_US, START_SAMPLE_START_US, &sample);
    if (sample.low_seen) {
      result = BitSample::Low;
    }
  }

  // total duration of start bit: 4500 us
  busy_wait_until(start_us, START_BIT_TOTAL_US);
  return result;
}

// Returns false when our own low pulse overran the spec tolerance: the follower
// will have decoded the wrong bit value, so there is no point transmitting the
// rest of a frame that can only be dropped.
bool HDMICEC::send_bit_(bool bit_value) {
  // total bit duration:
  // logic 1: pull low for 600 us, then pull high for 1800 us
  // logic 0: pull low for 1500 us, then pull high for 900 us

  const uint32_t low_duration_us = (bit_value ? HIGH_BIT_US : LOW_BIT_US);
  const uint32_t max_low_us = (bit_value ? HIGH_BIT_MAX_LOW_US : LOW_BIT_MAX_LOW_US);

  const uint32_t start_us = micros();
  set_pin_output_low();
  busy_wait_until(start_us, low_duration_us);
  set_pin_input_high();
  const uint32_t low_us = micros() - start_us;

  busy_wait_until(start_us, TOTAL_BIT_US);

  if (low_us > low_duration_us) {
    const uint32_t overrun_us = low_us - low_duration_us;
    if (overrun_us > tx_stats_.max_overrun_us) {
      tx_stats_.max_overrun_us = overrun_us;
    }
  }
  if (low_us > max_low_us) {
    tx_stats_.timing_faults++;
    return !tx_strict_timing_;
  }
  return true;
}

BitSample HDMICEC::send_high_and_test_() {
  const uint32_t start_us = micros();

  // send a Logical 1
  set_pin_output_low();
  busy_wait_until(start_us, HIGH_BIT_US);
  set_pin_input_high();
  const uint32_t release_us = micros();
  const uint32_t low_us = release_us - start_us;

  tx_stats_.samples++;
  if (low_us > HIGH_BIT_US) {
    const uint32_t overrun_us = low_us - HIGH_BIT_US;
    if (overrun_us > tx_stats_.max_overrun_us) {
      tx_stats_.max_overrun_us = overrun_us;
    }
  }
  if (low_us > HIGH_BIT_MAX_LOW_US) {
    // we were still driving the line at the sample point: whatever we read back
    // would be our own pulse, not the follower's answer
    tx_stats_.timing_faults++;
    if (tx_strict_timing_) {
      busy_wait_until(start_us, TOTAL_BIT_US);
      return BitSample::Fault;
    }
  }

  LineSample sample;
  sample_line_(start_us, release_us, SAMPLE_START_US, SAMPLE_END_US, LEGACY_SAMPLE_US, &sample);

  BitSample result;
  if (!sample.sampled) {
    // preempted clean through the safe sample period: an acknowledging follower
    // still holds the line down for a full logical 0, so try one late read
    tx_stats_.windows_missed++;
    const bool late_low = (((int32_t) (micros() - (start_us + ACK_HOLD_US)) < 0) && !pin_->digital_read());
    result = late_low ? BitSample::Low : BitSample::Fault;
  } else if (tx_window_sampling_) {
    result = sample.low_seen ? BitSample::Low : BitSample::High;
  } else {
    result = (sample.legacy_valid && sample.legacy_low) ? BitSample::Low : BitSample::High;
  }

  if (sample.low_seen && sample.legacy_valid && !sample.legacy_low) {
    tx_stats_.window_rescues++;
  }

  // sleep for the rest of the bit period
  busy_wait_until(start_us, TOTAL_BIT_US);
  return result;
}

void IRAM_ATTR HDMICEC::gpio_intr_(HDMICEC *self) {
  const uint32_t now = micros();
  const bool level = self->isr_pin_.digital_read();

  if (level == self->last_level_) {
    // spurious interrupt, probably resulting from a pin mode change
    return;
  }
  self->last_level_ = level;

  // on falling edge, store current time as the start of the low pulse
  if (level == false) {
    self->last_falling_edge_us_ = now;

    if (self->recv_ack_queued_ && !self->monitor_mode_) {
      self->recv_ack_queued_ = false;
      {
        InterruptLock interrupt_lock;
        self->set_pin_output_low();
        delay_microseconds_safe(LOW_BIT_US);
        self->set_pin_input_high();
      }
    }
    return;
  }
  // otherwise, it's a rising edge, so it's time to process the pulse length

  auto pulse_duration = (now - self->last_falling_edge_us_);

  if (pulse_duration > START_BIT_MIN_US) {
    // start bit detected. reset everything and start receiving
    self->receiver_state_ = ReceiverState::ReceivingByte;
    reset_state_variables_(self);
    self->recv_ack_queued_ = false;
    // pick frame receive buffer to fill, if available.
    self->frame_receive_ = self->frames_queue_.back();
    return;
  } else if (pulse_duration < (HIGH_BIT_MIN_US / 4)) {
    // short glitch on the line: ignore
    return;
  }

  bool value = (pulse_duration >= HIGH_BIT_MIN_US && pulse_duration <= HIGH_BIT_MAX_US);

  switch (self->receiver_state_) {
    case ReceiverState::ReceivingByte: {
      // write bit to the current byte
      self->recv_byte_buffer_ = (self->recv_byte_buffer_ << 1) | (value & 0b1);

      self->recv_bit_counter_++;
      if (self->recv_bit_counter_ >= 8) {
        // if we reached eight bits, push the current byte to the frame buffer
        if (self->frame_receive_) {
          self->frame_receive_->push_back(self->recv_byte_buffer_);
        }

        self->recv_bit_counter_ = 0;
        self->recv_byte_buffer_ = 0;

        self->receiver_state_ = ReceiverState::WaitingForEOM;
      } else {
        self->receiver_state_ = ReceiverState::ReceivingByte;
      }
      break;
    }

    case ReceiverState::WaitingForEOM: {
      // check if we need to acknowledge this byte on the next bit
      uint8_t destination_address = self->frame_receive_ ? (self->frame_receive_->front() & 0x0F) : 0xF;
      if (destination_address != 0xF && destination_address == self->address_) {
        self->recv_ack_queued_ = true;
      }

      bool isEOM = (value == 1);
      if (isEOM) {
        // pass frame to app
        if (self->frame_receive_ && self->frame_receive_->size() > 0) {
          self->frames_queue_.push_back();
          self->frame_receive_ = nullptr;
        }
        reset_state_variables_(self);
      }

      self->receiver_state_ = (
        isEOM
        ? ReceiverState::WaitingForEOMAck
        : ReceiverState::WaitingForAck
      );
      break;
    }

    case ReceiverState::WaitingForAck: {
      self->receiver_state_ = ReceiverState::ReceivingByte;
      break;
    }

    case ReceiverState::WaitingForEOMAck: {
      self->receiver_state_ = ReceiverState::Idle;
      break;
    }

    default: {
      break;
    }
  }
}

void IRAM_ATTR HDMICEC::reset_state_variables_(HDMICEC *self) {
  self->recv_bit_counter_ = 0;
  self->recv_byte_buffer_ = 0x0;
}

}
}
