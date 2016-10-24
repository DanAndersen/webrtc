/*
 *  Copyright (c) 2012 The WebRTC project authors. All Rights Reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

#include <list>
#include <memory>

#include "testing/gmock/include/gmock/gmock.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "webrtc/modules/pacing/paced_sender.h"
#include "webrtc/system_wrappers/include/clock.h"

using testing::_;
using testing::Return;

namespace webrtc {
namespace test {

static const int kTargetBitrateBps = 800000;

class MockPacedSenderCallback : public PacedSender::PacketSender {
 public:
  MOCK_METHOD5(TimeToSendPacket,
               bool(uint32_t ssrc,
                    uint16_t sequence_number,
                    int64_t capture_time_ms,
                    bool retransmission,
                    int probe_cluster_id));
  MOCK_METHOD2(TimeToSendPadding, size_t(size_t bytes, int probe_cluster_id));
};

class PacedSenderPadding : public PacedSender::PacketSender {
 public:
  PacedSenderPadding() : padding_sent_(0) {}

  bool TimeToSendPacket(uint32_t ssrc,
                        uint16_t sequence_number,
                        int64_t capture_time_ms,
                        bool retransmission,
                        int probe_cluster_id) override {
    return true;
  }

  size_t TimeToSendPadding(size_t bytes, int probe_cluster_id) override {
    const size_t kPaddingPacketSize = 224;
    size_t num_packets = (bytes + kPaddingPacketSize - 1) / kPaddingPacketSize;
    padding_sent_ += kPaddingPacketSize * num_packets;
    return kPaddingPacketSize * num_packets;
  }

  size_t padding_sent() { return padding_sent_; }

 private:
  size_t padding_sent_;
};

class PacedSenderProbing : public PacedSender::PacketSender {
 public:
  PacedSenderProbing(const std::list<int>& expected_deltas, Clock* clock)
      : prev_packet_time_ms_(-1),
        expected_deltas_(expected_deltas),
        packets_sent_(0),
        clock_(clock) {}

  bool TimeToSendPacket(uint32_t ssrc,
                        uint16_t sequence_number,
                        int64_t capture_time_ms,
                        bool retransmission,
                        int probe_cluster_id) override {
    ExpectAndCountPacket();
    return true;
  }

  size_t TimeToSendPadding(size_t bytes, int probe_cluster_id) override {
    ExpectAndCountPacket();
    return bytes;
  }

  void ExpectAndCountPacket() {
    ++packets_sent_;
    EXPECT_FALSE(expected_deltas_.empty());
    if (expected_deltas_.empty())
      return;
    int64_t now_ms = clock_->TimeInMilliseconds();
    if (prev_packet_time_ms_ >= 0) {
      EXPECT_EQ(expected_deltas_.front(), now_ms - prev_packet_time_ms_);
      expected_deltas_.pop_front();
    }
    prev_packet_time_ms_ = now_ms;
  }

  int packets_sent() const { return packets_sent_; }

 private:
  int64_t prev_packet_time_ms_;
  std::list<int> expected_deltas_;
  int packets_sent_;
  Clock* clock_;
};

class PacedSenderTest : public ::testing::Test {
 protected:
  PacedSenderTest() : clock_(123456) {
    srand(0);
    // Need to initialize PacedSender after we initialize clock.
    send_bucket_.reset(new PacedSender(&clock_, &callback_));
    send_bucket_->CreateProbeCluster(900000, 6);
    send_bucket_->CreateProbeCluster(1800000, 5);
    // Default to bitrate probing disabled for testing purposes. Probing tests
    // have to enable probing, either by creating a new PacedSender instance or
    // by calling SetProbingEnabled(true).
    send_bucket_->SetProbingEnabled(false);
    send_bucket_->SetEstimatedBitrate(kTargetBitrateBps);

    clock_.AdvanceTimeMilliseconds(send_bucket_->TimeUntilNextProcess());
  }

  void SendAndExpectPacket(PacedSender::Priority priority,
                           uint32_t ssrc,
                           uint16_t sequence_number,
                           int64_t capture_time_ms,
                           size_t size,
                           bool retransmission) {
    send_bucket_->InsertPacket(priority, ssrc, sequence_number, capture_time_ms,
                               size, retransmission);
    EXPECT_CALL(callback_, TimeToSendPacket(ssrc, sequence_number,
                                            capture_time_ms, retransmission, _))
        .Times(1)
        .WillRepeatedly(Return(true));
  }

  SimulatedClock clock_;
  MockPacedSenderCallback callback_;
  std::unique_ptr<PacedSender> send_bucket_;
};

TEST_F(PacedSenderTest, QueuePacket) {
  uint32_t ssrc = 12345;
  uint16_t sequence_number = 1234;
  // Due to the multiplicative factor we can send 5 packets during a send
  // interval. (network capacity * multiplier / (8 bits per byte *
  // (packet size * #send intervals per second)
  const size_t packets_to_send =
      kTargetBitrateBps * PacedSender::kDefaultPaceMultiplier / (8 * 250 * 200);
  for (size_t i = 0; i < packets_to_send; ++i) {
    SendAndExpectPacket(PacedSender::kNormalPriority, ssrc, sequence_number++,
                        clock_.TimeInMilliseconds(), 250, false);
  }

  int64_t queued_packet_timestamp = clock_.TimeInMilliseconds();
  send_bucket_->InsertPacket(PacedSender::kNormalPriority, ssrc,
                             sequence_number, queued_packet_timestamp, 250,
                             false);
  EXPECT_EQ(packets_to_send + 1, send_bucket_->QueueSizePackets());
  send_bucket_->Process();
  EXPECT_EQ(5, send_bucket_->TimeUntilNextProcess());
  EXPECT_CALL(callback_, TimeToSendPadding(_, _)).Times(0);
  clock_.AdvanceTimeMilliseconds(4);
  EXPECT_EQ(1, send_bucket_->TimeUntilNextProcess());
  clock_.AdvanceTimeMilliseconds(1);
  EXPECT_EQ(0, send_bucket_->TimeUntilNextProcess());
  EXPECT_EQ(1u, send_bucket_->QueueSizePackets());
  EXPECT_CALL(callback_, TimeToSendPacket(ssrc, sequence_number++,
                                          queued_packet_timestamp, false, _))
      .Times(1)
      .WillRepeatedly(Return(true));
  send_bucket_->Process();
  sequence_number++;
  EXPECT_EQ(0u, send_bucket_->QueueSizePackets());

  // We can send packets_to_send -1 packets of size 250 during the current
  // interval since one packet has already been sent.
  for (size_t i = 0; i < packets_to_send - 1; ++i) {
    SendAndExpectPacket(PacedSender::kNormalPriority, ssrc, sequence_number++,
                        clock_.TimeInMilliseconds(), 250, false);
  }
  send_bucket_->InsertPacket(PacedSender::kNormalPriority, ssrc,
                             sequence_number++, clock_.TimeInMilliseconds(),
                             250, false);
  EXPECT_EQ(packets_to_send, send_bucket_->QueueSizePackets());
  send_bucket_->Process();
  EXPECT_EQ(1u, send_bucket_->QueueSizePackets());
}

TEST_F(PacedSenderTest, PaceQueuedPackets) {
  uint32_t ssrc = 12345;
  uint16_t sequence_number = 1234;

  // Due to the multiplicative factor we can send 5 packets during a send
  // interval. (network capacity * multiplier / (8 bits per byte *
  // (packet size * #send intervals per second)
  const size_t packets_to_send_per_interval =
      kTargetBitrateBps * PacedSender::kDefaultPaceMultiplier / (8 * 250 * 200);
  for (size_t i = 0; i < packets_to_send_per_interval; ++i) {
    SendAndExpectPacket(PacedSender::kNormalPriority, ssrc, sequence_number++,
                        clock_.TimeInMilliseconds(), 250, false);
  }

  for (size_t j = 0; j < packets_to_send_per_interval * 10; ++j) {
    send_bucket_->InsertPacket(PacedSender::kNormalPriority, ssrc,
                               sequence_number++, clock_.TimeInMilliseconds(),
                               250, false);
  }
  EXPECT_EQ(packets_to_send_per_interval + packets_to_send_per_interval * 10,
            send_bucket_->QueueSizePackets());
  send_bucket_->Process();
  EXPECT_EQ(packets_to_send_per_interval * 10,
            send_bucket_->QueueSizePackets());
  EXPECT_CALL(callback_, TimeToSendPadding(_, _)).Times(0);
  for (int k = 0; k < 10; ++k) {
    EXPECT_EQ(5, send_bucket_->TimeUntilNextProcess());
    clock_.AdvanceTimeMilliseconds(5);
    EXPECT_CALL(callback_, TimeToSendPacket(ssrc, _, _, false, _))
        .Times(packets_to_send_per_interval)
        .WillRepeatedly(Return(true));
    EXPECT_EQ(0, send_bucket_->TimeUntilNextProcess());
    send_bucket_->Process();
  }
  EXPECT_EQ(0u, send_bucket_->QueueSizePackets());
  EXPECT_EQ(5, send_bucket_->TimeUntilNextProcess());
  clock_.AdvanceTimeMilliseconds(5);
  EXPECT_EQ(0, send_bucket_->TimeUntilNextProcess());
  EXPECT_EQ(0u, send_bucket_->QueueSizePackets());
  send_bucket_->Process();

  for (size_t i = 0; i < packets_to_send_per_interval; ++i) {
    SendAndExpectPacket(PacedSender::kNormalPriority, ssrc, sequence_number++,
                        clock_.TimeInMilliseconds(), 250, false);
  }
  send_bucket_->InsertPacket(PacedSender::kNormalPriority, ssrc,
                             sequence_number, clock_.TimeInMilliseconds(), 250,
                             false);
  send_bucket_->Process();
  EXPECT_EQ(1u, send_bucket_->QueueSizePackets());
}

TEST_F(PacedSenderTest, PaceQueuedPacketsWithDuplicates) {
  uint32_t ssrc = 12345;
  uint16_t sequence_number = 1234;
  uint16_t queued_sequence_number;

  // Due to the multiplicative factor we can send 5 packets during a send
  // interval. (network capacity * multiplier / (8 bits per byte *
  // (packet size * #send intervals per second)
  const size_t packets_to_send_per_interval =
      kTargetBitrateBps * PacedSender::kDefaultPaceMultiplier / (8 * 250 * 200);
  for (size_t i = 0; i < packets_to_send_per_interval; ++i) {
    SendAndExpectPacket(PacedSender::kNormalPriority, ssrc, sequence_number++,
                        clock_.TimeInMilliseconds(), 250, false);
  }
  queued_sequence_number = sequence_number;

  for (size_t j = 0; j < packets_to_send_per_interval * 10; ++j) {
    // Send in duplicate packets.
    send_bucket_->InsertPacket(PacedSender::kNormalPriority, ssrc,
                               sequence_number, clock_.TimeInMilliseconds(),
                               250, false);
    send_bucket_->InsertPacket(PacedSender::kNormalPriority, ssrc,
                               sequence_number++, clock_.TimeInMilliseconds(),
                               250, false);
  }
  EXPECT_CALL(callback_, TimeToSendPadding(_, _)).Times(0);
  send_bucket_->Process();
  for (int k = 0; k < 10; ++k) {
    EXPECT_EQ(5, send_bucket_->TimeUntilNextProcess());
    clock_.AdvanceTimeMilliseconds(5);

    for (size_t i = 0; i < packets_to_send_per_interval; ++i) {
      EXPECT_CALL(callback_,
                  TimeToSendPacket(ssrc, queued_sequence_number++, _, false, _))
          .Times(1)
          .WillRepeatedly(Return(true));
    }
    EXPECT_EQ(0, send_bucket_->TimeUntilNextProcess());
    send_bucket_->Process();
  }
  EXPECT_EQ(5, send_bucket_->TimeUntilNextProcess());
  clock_.AdvanceTimeMilliseconds(5);
  EXPECT_EQ(0, send_bucket_->TimeUntilNextProcess());
  send_bucket_->Process();

  for (size_t i = 0; i < packets_to_send_per_interval; ++i) {
    SendAndExpectPacket(PacedSender::kNormalPriority, ssrc, sequence_number++,
                        clock_.TimeInMilliseconds(), 250, false);
  }
  send_bucket_->InsertPacket(PacedSender::kNormalPriority, ssrc,
                             sequence_number++, clock_.TimeInMilliseconds(),
                             250, false);
  send_bucket_->Process();
  EXPECT_EQ(1u, send_bucket_->QueueSizePackets());
}

TEST_F(PacedSenderTest, CanQueuePacketsWithSameSequenceNumberOnDifferentSsrcs) {
  uint32_t ssrc = 12345;
  uint16_t sequence_number = 1234;

  SendAndExpectPacket(PacedSender::kNormalPriority,
                      ssrc,
                      sequence_number,
                      clock_.TimeInMilliseconds(),
                      250,
                      false);

  // Expect packet on second ssrc to be queued and sent as well.
  SendAndExpectPacket(PacedSender::kNormalPriority,
                      ssrc + 1,
                      sequence_number,
                      clock_.TimeInMilliseconds(),
                      250,
                      false);

  clock_.AdvanceTimeMilliseconds(1000);
  send_bucket_->Process();
}

TEST_F(PacedSenderTest, Padding) {
  uint32_t ssrc = 12345;
  uint16_t sequence_number = 1234;

  send_bucket_->SetEstimatedBitrate(kTargetBitrateBps);
  send_bucket_->SetSendBitrateLimits(kTargetBitrateBps, kTargetBitrateBps);

  // Due to the multiplicative factor we can send 5 packets during a send
  // interval. (network capacity * multiplier / (8 bits per byte *
  // (packet size * #send intervals per second)
  const size_t packets_to_send_per_interval =
      kTargetBitrateBps * PacedSender::kDefaultPaceMultiplier / (8 * 250 * 200);
  for (size_t i = 0; i < packets_to_send_per_interval; ++i) {
    SendAndExpectPacket(PacedSender::kNormalPriority, ssrc, sequence_number++,
                        clock_.TimeInMilliseconds(), 250, false);
  }
  // No padding is expected since we have sent too much already.
  EXPECT_CALL(callback_, TimeToSendPadding(_, _)).Times(0);
  EXPECT_EQ(0, send_bucket_->TimeUntilNextProcess());
  send_bucket_->Process();
  EXPECT_EQ(0u, send_bucket_->QueueSizePackets());

  // 5 milliseconds later should not send padding since we filled the buffers
  // initially.
  EXPECT_CALL(callback_, TimeToSendPadding(250, _)).Times(0);
  EXPECT_EQ(5, send_bucket_->TimeUntilNextProcess());
  clock_.AdvanceTimeMilliseconds(5);
  EXPECT_EQ(0, send_bucket_->TimeUntilNextProcess());
  send_bucket_->Process();

  // 5 milliseconds later we have enough budget to send some padding.
  EXPECT_CALL(callback_, TimeToSendPadding(250, _))
      .Times(1)
      .WillOnce(Return(250));
  EXPECT_EQ(5, send_bucket_->TimeUntilNextProcess());
  clock_.AdvanceTimeMilliseconds(5);
  EXPECT_EQ(0, send_bucket_->TimeUntilNextProcess());
  send_bucket_->Process();
}

TEST_F(PacedSenderTest, NoPaddingBeforeNormalPacket) {
  send_bucket_->SetEstimatedBitrate(kTargetBitrateBps);
  send_bucket_->SetSendBitrateLimits(kTargetBitrateBps, kTargetBitrateBps);

  EXPECT_CALL(callback_, TimeToSendPadding(_, _)).Times(0);
  send_bucket_->Process();
  clock_.AdvanceTimeMilliseconds(send_bucket_->TimeUntilNextProcess());

  send_bucket_->Process();
  clock_.AdvanceTimeMilliseconds(send_bucket_->TimeUntilNextProcess());

  uint32_t ssrc = 12345;
  uint16_t sequence_number = 1234;
  int64_t capture_time_ms = 56789;

  SendAndExpectPacket(PacedSender::kNormalPriority, ssrc, sequence_number++,
                      capture_time_ms, 250, false);
  EXPECT_CALL(callback_, TimeToSendPadding(250, _))
      .Times(1)
      .WillOnce(Return(250));
  send_bucket_->Process();
}

TEST_F(PacedSenderTest, VerifyPaddingUpToBitrate) {
  uint32_t ssrc = 12345;
  uint16_t sequence_number = 1234;
  int64_t capture_time_ms = 56789;
  const int kTimeStep = 5;
  const int64_t kBitrateWindow = 100;
  send_bucket_->SetEstimatedBitrate(kTargetBitrateBps);
  send_bucket_->SetSendBitrateLimits(kTargetBitrateBps, kTargetBitrateBps);

  int64_t start_time = clock_.TimeInMilliseconds();
  while (clock_.TimeInMilliseconds() - start_time < kBitrateWindow) {
    SendAndExpectPacket(PacedSender::kNormalPriority,
                        ssrc,
                        sequence_number++,
                        capture_time_ms,
                        250,
                        false);
    EXPECT_CALL(callback_, TimeToSendPadding(250, _))
        .Times(1)
        .WillOnce(Return(250));
    send_bucket_->Process();
    clock_.AdvanceTimeMilliseconds(kTimeStep);
  }
}

TEST_F(PacedSenderTest, VerifyAverageBitrateVaryingMediaPayload) {
  uint32_t ssrc = 12345;
  uint16_t sequence_number = 1234;
  int64_t capture_time_ms = 56789;
  const int kTimeStep = 5;
  const int64_t kBitrateWindow = 10000;
  PacedSenderPadding callback;
  send_bucket_.reset(new PacedSender(&clock_, &callback));
  send_bucket_->SetProbingEnabled(false);
  send_bucket_->SetEstimatedBitrate(kTargetBitrateBps);

  send_bucket_->SetSendBitrateLimits(
      0 /*allocated_bitrate_bps*/,
      kTargetBitrateBps * 2 /* max_padding_bitrate_bps */);

  int64_t start_time = clock_.TimeInMilliseconds();
  size_t media_bytes = 0;
  while (clock_.TimeInMilliseconds() - start_time < kBitrateWindow) {
    int rand_value = rand();  // NOLINT (rand_r instead of rand)
    size_t media_payload = rand_value % 100 + 200;  // [200, 300] bytes.
    send_bucket_->InsertPacket(PacedSender::kNormalPriority, ssrc,
                               sequence_number++, capture_time_ms,
                               media_payload, false);
    media_bytes += media_payload;
    clock_.AdvanceTimeMilliseconds(kTimeStep);
    send_bucket_->Process();
  }
  EXPECT_NEAR(kTargetBitrateBps / 1000,
              static_cast<int>(8 * (media_bytes + callback.padding_sent()) /
                               kBitrateWindow),
              1);
}

TEST_F(PacedSenderTest, Priority) {
  uint32_t ssrc_low_priority = 12345;
  uint32_t ssrc = 12346;
  uint16_t sequence_number = 1234;
  int64_t capture_time_ms = 56789;
  int64_t capture_time_ms_low_priority = 1234567;

  // Due to the multiplicative factor we can send 5 packets during a send
  // interval. (network capacity * multiplier / (8 bits per byte *
  // (packet size * #send intervals per second)
  const size_t packets_to_send_per_interval =
      kTargetBitrateBps * PacedSender::kDefaultPaceMultiplier / (8 * 250 * 200);
  for (size_t i = 0; i < packets_to_send_per_interval; ++i) {
    SendAndExpectPacket(PacedSender::kNormalPriority, ssrc, sequence_number++,
                        clock_.TimeInMilliseconds(), 250, false);
  }
  send_bucket_->Process();
  EXPECT_EQ(0u, send_bucket_->QueueSizePackets());

  // Expect normal and low priority to be queued and high to pass through.
  send_bucket_->InsertPacket(PacedSender::kLowPriority, ssrc_low_priority,
                             sequence_number++, capture_time_ms_low_priority,
                             250, false);

  for (size_t i = 0; i < packets_to_send_per_interval; ++i) {
    send_bucket_->InsertPacket(PacedSender::kNormalPriority, ssrc,
                               sequence_number++, capture_time_ms, 250, false);
  }
  send_bucket_->InsertPacket(PacedSender::kHighPriority, ssrc,
                             sequence_number++, capture_time_ms, 250, false);

  // Expect all high and normal priority to be sent out first.
  EXPECT_CALL(callback_, TimeToSendPadding(_, _)).Times(0);
  EXPECT_CALL(callback_, TimeToSendPacket(ssrc, _, capture_time_ms, false, _))
      .Times(packets_to_send_per_interval + 1)
      .WillRepeatedly(Return(true));

  EXPECT_EQ(5, send_bucket_->TimeUntilNextProcess());
  clock_.AdvanceTimeMilliseconds(5);
  EXPECT_EQ(0, send_bucket_->TimeUntilNextProcess());
  send_bucket_->Process();
  EXPECT_EQ(1u, send_bucket_->QueueSizePackets());

  EXPECT_CALL(callback_,
              TimeToSendPacket(ssrc_low_priority, _,
                               capture_time_ms_low_priority, false, _))
      .Times(1)
      .WillRepeatedly(Return(true));

  EXPECT_EQ(5, send_bucket_->TimeUntilNextProcess());
  clock_.AdvanceTimeMilliseconds(5);
  EXPECT_EQ(0, send_bucket_->TimeUntilNextProcess());
  send_bucket_->Process();
}

TEST_F(PacedSenderTest, RetransmissionPriority) {
  uint32_t ssrc = 12345;
  uint16_t sequence_number = 1234;
  int64_t capture_time_ms = 45678;
  int64_t capture_time_ms_retransmission = 56789;

  // Due to the multiplicative factor we can send 5 packets during a send
  // interval. (network capacity * multiplier / (8 bits per byte *
  // (packet size * #send intervals per second)
  const size_t packets_to_send_per_interval =
      kTargetBitrateBps * PacedSender::kDefaultPaceMultiplier / (8 * 250 * 200);
  send_bucket_->Process();
  EXPECT_EQ(0u, send_bucket_->QueueSizePackets());

  // Alternate retransmissions and normal packets.
  for (size_t i = 0; i < packets_to_send_per_interval; ++i) {
    send_bucket_->InsertPacket(PacedSender::kNormalPriority, ssrc,
                               sequence_number++,
                               capture_time_ms_retransmission, 250, true);
    send_bucket_->InsertPacket(PacedSender::kNormalPriority, ssrc,
                               sequence_number++, capture_time_ms, 250, false);
  }
  EXPECT_EQ(2 * packets_to_send_per_interval, send_bucket_->QueueSizePackets());

  // Expect all retransmissions to be sent out first despite having a later
  // capture time.
  EXPECT_CALL(callback_, TimeToSendPadding(_, _)).Times(0);
  EXPECT_CALL(callback_, TimeToSendPacket(_, _, _, false, _)).Times(0);
  EXPECT_CALL(callback_, TimeToSendPacket(
                             ssrc, _, capture_time_ms_retransmission, true, _))
      .Times(packets_to_send_per_interval)
      .WillRepeatedly(Return(true));

  EXPECT_EQ(5, send_bucket_->TimeUntilNextProcess());
  clock_.AdvanceTimeMilliseconds(5);
  EXPECT_EQ(0, send_bucket_->TimeUntilNextProcess());
  send_bucket_->Process();
  EXPECT_EQ(packets_to_send_per_interval, send_bucket_->QueueSizePackets());

  // Expect the remaining (non-retransmission) packets to be sent.
  EXPECT_CALL(callback_, TimeToSendPadding(_, _)).Times(0);
  EXPECT_CALL(callback_, TimeToSendPacket(_, _, _, true, _)).Times(0);
  EXPECT_CALL(callback_, TimeToSendPacket(ssrc, _, capture_time_ms, false, _))
      .Times(packets_to_send_per_interval)
      .WillRepeatedly(Return(true));

  EXPECT_EQ(5, send_bucket_->TimeUntilNextProcess());
  clock_.AdvanceTimeMilliseconds(5);
  EXPECT_EQ(0, send_bucket_->TimeUntilNextProcess());
  send_bucket_->Process();

  EXPECT_EQ(0u, send_bucket_->QueueSizePackets());
}

TEST_F(PacedSenderTest, HighPrioDoesntAffectBudget) {
  uint32_t ssrc = 12346;
  uint16_t sequence_number = 1234;
  int64_t capture_time_ms = 56789;

  // As high prio packets doesn't affect the budget, we should be able to send
  // a high number of them at once.
  for (int i = 0; i < 25; ++i) {
    SendAndExpectPacket(PacedSender::kHighPriority, ssrc, sequence_number++,
                        capture_time_ms, 250, false);
  }
  send_bucket_->Process();
  // Low prio packets does affect the budget.
  // Due to the multiplicative factor we can send 5 packets during a send
  // interval. (network capacity * multiplier / (8 bits per byte *
  // (packet size * #send intervals per second)
  const size_t packets_to_send_per_interval =
      kTargetBitrateBps * PacedSender::kDefaultPaceMultiplier / (8 * 250 * 200);
  for (size_t i = 0; i < packets_to_send_per_interval; ++i) {
    SendAndExpectPacket(PacedSender::kLowPriority, ssrc, sequence_number++,
                        clock_.TimeInMilliseconds(), 250, false);
  }
  send_bucket_->InsertPacket(PacedSender::kLowPriority, ssrc, sequence_number,
                             capture_time_ms, 250, false);
  EXPECT_EQ(5, send_bucket_->TimeUntilNextProcess());
  clock_.AdvanceTimeMilliseconds(5);
  send_bucket_->Process();
  EXPECT_EQ(1u, send_bucket_->QueueSizePackets());
  EXPECT_CALL(callback_, TimeToSendPacket(ssrc, sequence_number++,
                                          capture_time_ms, false, _))
      .Times(1)
      .WillRepeatedly(Return(true));
  EXPECT_EQ(5, send_bucket_->TimeUntilNextProcess());
  clock_.AdvanceTimeMilliseconds(5);
  send_bucket_->Process();
  EXPECT_EQ(0u, send_bucket_->QueueSizePackets());
}

TEST_F(PacedSenderTest, Pause) {
  uint32_t ssrc_low_priority = 12345;
  uint32_t ssrc = 12346;
  uint32_t ssrc_high_priority = 12347;
  uint16_t sequence_number = 1234;
  int64_t capture_time_ms = clock_.TimeInMilliseconds();

  EXPECT_EQ(0, send_bucket_->QueueInMs());

  // Due to the multiplicative factor we can send 5 packets during a send
  // interval. (network capacity * multiplier / (8 bits per byte *
  // (packet size * #send intervals per second)
  const size_t packets_to_send_per_interval =
      kTargetBitrateBps * PacedSender::kDefaultPaceMultiplier / (8 * 250 * 200);
  for (size_t i = 0; i < packets_to_send_per_interval; ++i) {
    SendAndExpectPacket(PacedSender::kNormalPriority, ssrc, sequence_number++,
                        clock_.TimeInMilliseconds(), 250, false);
  }

  send_bucket_->Process();

  send_bucket_->Pause();

  for (size_t i = 0; i < packets_to_send_per_interval; ++i) {
    send_bucket_->InsertPacket(PacedSender::kLowPriority, ssrc_low_priority,
                               sequence_number++, capture_time_ms, 250, false);
    send_bucket_->InsertPacket(PacedSender::kNormalPriority, ssrc,
                               sequence_number++, capture_time_ms, 250, false);
    send_bucket_->InsertPacket(PacedSender::kHighPriority, ssrc_high_priority,
                               sequence_number++, capture_time_ms, 250, false);
  }
  clock_.AdvanceTimeMilliseconds(10000);
  int64_t second_capture_time_ms = clock_.TimeInMilliseconds();
  for (size_t i = 0; i < packets_to_send_per_interval; ++i) {
    send_bucket_->InsertPacket(PacedSender::kLowPriority, ssrc_low_priority,
                               sequence_number++, second_capture_time_ms, 250,
                               false);
    send_bucket_->InsertPacket(PacedSender::kNormalPriority, ssrc,
                               sequence_number++, second_capture_time_ms, 250,
                               false);
    send_bucket_->InsertPacket(PacedSender::kHighPriority, ssrc_high_priority,
                               sequence_number++, second_capture_time_ms, 250,
                               false);
  }

  // Expect everything to be queued.
  EXPECT_EQ(second_capture_time_ms - capture_time_ms,
            send_bucket_->QueueInMs());

  // Expect only high priority packets to come out while paused.
  EXPECT_CALL(callback_, TimeToSendPadding(_, _)).Times(0);
  EXPECT_CALL(callback_, TimeToSendPacket(_, _, _, _, _)).Times(0);
  EXPECT_CALL(callback_,
              TimeToSendPacket(ssrc_high_priority, _, capture_time_ms, _, _))
      .Times(packets_to_send_per_interval)
      .WillRepeatedly(Return(true));
  EXPECT_CALL(callback_, TimeToSendPacket(ssrc_high_priority, _,
                                          second_capture_time_ms, _, _))
      .Times(packets_to_send_per_interval)
      .WillRepeatedly(Return(true));

  for (int i = 0; i < 10; ++i) {
    clock_.AdvanceTimeMilliseconds(5);
    EXPECT_EQ(0, send_bucket_->TimeUntilNextProcess());
    send_bucket_->Process();
  }

  // Expect normal prio packets to come out first (in capture order)
  // followed by all low prio packets (in capture order).
  {
    ::testing::InSequence sequence;
    for (size_t i = 0; i < packets_to_send_per_interval; ++i) {
      EXPECT_CALL(callback_, TimeToSendPacket(ssrc, _, capture_time_ms, _, _))
          .Times(1)
          .WillRepeatedly(Return(true));
    }
    for (size_t i = 0; i < packets_to_send_per_interval; ++i) {
      EXPECT_CALL(callback_,
                  TimeToSendPacket(ssrc, _, second_capture_time_ms, _, _))
          .Times(1)
          .WillRepeatedly(Return(true));
    }
    for (size_t i = 0; i < packets_to_send_per_interval; ++i) {
      EXPECT_CALL(callback_,
                  TimeToSendPacket(ssrc_low_priority, _, capture_time_ms, _, _))
          .Times(1)
          .WillRepeatedly(Return(true));
    }
    for (size_t i = 0; i < packets_to_send_per_interval; ++i) {
      EXPECT_CALL(callback_, TimeToSendPacket(ssrc_low_priority, _,
                                              second_capture_time_ms, _, _))
          .Times(1)
          .WillRepeatedly(Return(true));
    }
  }
  send_bucket_->Resume();

  for (size_t i = 0; i < 4; i++) {
    EXPECT_EQ(5, send_bucket_->TimeUntilNextProcess());
    clock_.AdvanceTimeMilliseconds(5);
    EXPECT_EQ(0, send_bucket_->TimeUntilNextProcess());
    send_bucket_->Process();
  }

  EXPECT_EQ(0, send_bucket_->QueueInMs());
}

TEST_F(PacedSenderTest, ResendPacket) {
  uint32_t ssrc = 12346;
  uint16_t sequence_number = 1234;
  int64_t capture_time_ms = clock_.TimeInMilliseconds();
  EXPECT_EQ(0, send_bucket_->QueueInMs());

  send_bucket_->InsertPacket(PacedSender::kNormalPriority, ssrc,
                             sequence_number, capture_time_ms, 250, false);
  clock_.AdvanceTimeMilliseconds(1);
  send_bucket_->InsertPacket(PacedSender::kNormalPriority, ssrc,
                             sequence_number + 1, capture_time_ms + 1, 250,
                             false);
  clock_.AdvanceTimeMilliseconds(9999);
  EXPECT_EQ(clock_.TimeInMilliseconds() - capture_time_ms,
            send_bucket_->QueueInMs());
  // Fails to send first packet so only one call.
  EXPECT_CALL(callback_, TimeToSendPacket(ssrc, sequence_number,
                                          capture_time_ms, false, _))
      .Times(1)
      .WillOnce(Return(false));
  clock_.AdvanceTimeMilliseconds(10000);
  send_bucket_->Process();

  // Queue remains unchanged.
  EXPECT_EQ(clock_.TimeInMilliseconds() - capture_time_ms,
            send_bucket_->QueueInMs());

  // Fails to send second packet.
  EXPECT_CALL(callback_, TimeToSendPacket(ssrc, sequence_number,
                                          capture_time_ms, false, _))
      .Times(1)
      .WillOnce(Return(true));
  EXPECT_CALL(callback_, TimeToSendPacket(ssrc, sequence_number + 1,
                                          capture_time_ms + 1, false, _))
      .Times(1)
      .WillOnce(Return(false));
  clock_.AdvanceTimeMilliseconds(10000);
  send_bucket_->Process();

  // Queue is reduced by 1 packet.
  EXPECT_EQ(clock_.TimeInMilliseconds() - capture_time_ms - 1,
            send_bucket_->QueueInMs());

  // Send second packet and queue becomes empty.
  EXPECT_CALL(callback_, TimeToSendPacket(ssrc, sequence_number + 1,
                                          capture_time_ms + 1, false, _))
      .Times(1)
      .WillOnce(Return(true));
  clock_.AdvanceTimeMilliseconds(10000);
  send_bucket_->Process();
  EXPECT_EQ(0, send_bucket_->QueueInMs());
}

TEST_F(PacedSenderTest, ExpectedQueueTimeMs) {
  uint32_t ssrc = 12346;
  uint16_t sequence_number = 1234;
  const size_t kNumPackets = 60;
  const size_t kPacketSize = 1200;
  const int32_t kMaxBitrate = PacedSender::kDefaultPaceMultiplier * 30000;
  EXPECT_EQ(0, send_bucket_->ExpectedQueueTimeMs());

  send_bucket_->SetEstimatedBitrate(30000);
  for (size_t i = 0; i < kNumPackets; ++i) {
    SendAndExpectPacket(PacedSender::kNormalPriority, ssrc, sequence_number++,
                        clock_.TimeInMilliseconds(), kPacketSize, false);
  }

  // Queue in ms = 1000 * (bytes in queue) *8 / (bits per second)
  int64_t queue_in_ms =
      static_cast<int64_t>(1000 * kNumPackets * kPacketSize * 8 / kMaxBitrate);
  EXPECT_EQ(queue_in_ms, send_bucket_->ExpectedQueueTimeMs());

  int64_t time_start = clock_.TimeInMilliseconds();
  while (send_bucket_->QueueSizePackets() > 0) {
    int time_until_process = send_bucket_->TimeUntilNextProcess();
    if (time_until_process <= 0) {
      send_bucket_->Process();
    } else {
      clock_.AdvanceTimeMilliseconds(time_until_process);
    }
  }
  int64_t duration = clock_.TimeInMilliseconds() - time_start;

  EXPECT_EQ(0, send_bucket_->ExpectedQueueTimeMs());

  // Allow for aliasing, duration should be within one pack of max time limit.
  EXPECT_NEAR(duration, PacedSender::kMaxQueueLengthMs,
              static_cast<int64_t>(1000 * kPacketSize * 8 / kMaxBitrate));
}

TEST_F(PacedSenderTest, QueueTimeGrowsOverTime) {
  uint32_t ssrc = 12346;
  uint16_t sequence_number = 1234;
  EXPECT_EQ(0, send_bucket_->QueueInMs());

  send_bucket_->SetEstimatedBitrate(30000);
  SendAndExpectPacket(PacedSender::kNormalPriority,
                      ssrc,
                      sequence_number,
                      clock_.TimeInMilliseconds(),
                      1200,
                      false);

  clock_.AdvanceTimeMilliseconds(500);
  EXPECT_EQ(500, send_bucket_->QueueInMs());
  send_bucket_->Process();
  EXPECT_EQ(0, send_bucket_->QueueInMs());
}

TEST_F(PacedSenderTest, ProbingWithInitialFrame) {
  const int kNumPackets = 11;
  const int kNumDeltas = kNumPackets - 1;
  const size_t kPacketSize = 1200;
  const int kInitialBitrateBps = 300000;
  uint32_t ssrc = 12346;
  uint16_t sequence_number = 1234;

  const int expected_deltas[kNumDeltas] = {10, 10, 10, 10, 10, 5, 5, 5, 5, 5};
  std::list<int> expected_deltas_list(expected_deltas,
                                      expected_deltas + kNumDeltas);
  PacedSenderProbing callback(expected_deltas_list, &clock_);
  send_bucket_.reset(new PacedSender(&clock_, &callback));
  send_bucket_->CreateProbeCluster(900000, 6);
  send_bucket_->CreateProbeCluster(1800000, 5);
  send_bucket_->SetEstimatedBitrate(kInitialBitrateBps);

  for (int i = 0; i < kNumPackets; ++i) {
    send_bucket_->InsertPacket(PacedSender::kNormalPriority, ssrc,
                               sequence_number++, clock_.TimeInMilliseconds(),
                               kPacketSize, false);
  }

  while (callback.packets_sent() < kNumPackets) {
    int time_until_process = send_bucket_->TimeUntilNextProcess();
    if (time_until_process <= 0) {
      send_bucket_->Process();
    } else {
      clock_.AdvanceTimeMilliseconds(time_until_process);
    }
  }
}

TEST_F(PacedSenderTest, ProbingWithTooSmallInitialFrame) {
  const int kNumPackets = 11;
  const int kNumDeltas = kNumPackets - 1;
  const size_t kPacketSize = 1200;
  const int kInitialBitrateBps = 300000;
  uint32_t ssrc = 12346;
  uint16_t sequence_number = 1234;
  const int expected_deltas[kNumDeltas] = {10, 10, 10, 10, 10, 5, 5, 5, 5, 5};
  std::list<int> expected_deltas_list(expected_deltas,
                                      expected_deltas + kNumDeltas);
  PacedSenderProbing callback(expected_deltas_list, &clock_);
  send_bucket_.reset(new PacedSender(&clock_, &callback));
  send_bucket_->CreateProbeCluster(900000, 6);
  send_bucket_->CreateProbeCluster(1800000, 5);
  send_bucket_->SetEstimatedBitrate(kInitialBitrateBps);

  for (int i = 0; i < kNumPackets - 5; ++i) {
    send_bucket_->InsertPacket(PacedSender::kNormalPriority, ssrc,
                               sequence_number++, clock_.TimeInMilliseconds(),
                               kPacketSize, false);
  }
  while (callback.packets_sent() < kNumPackets) {
    int time_until_process = send_bucket_->TimeUntilNextProcess();
    if (time_until_process <= 0) {
      send_bucket_->Process();
    } else {
      clock_.AdvanceTimeMilliseconds(time_until_process);
    }
  }

  // Process one more time and make sure we don't send any more probes.
  int time_until_process = send_bucket_->TimeUntilNextProcess();
  clock_.AdvanceTimeMilliseconds(time_until_process);
  send_bucket_->Process();
  EXPECT_EQ(kNumPackets, callback.packets_sent());
}

TEST_F(PacedSenderTest, PriorityInversion) {
  uint32_t ssrc = 12346;
  uint16_t sequence_number = 1234;
  const size_t kPacketSize = 1200;

  send_bucket_->InsertPacket(
      PacedSender::kHighPriority, ssrc, sequence_number + 3,
      clock_.TimeInMilliseconds() + 33, kPacketSize, true);

  send_bucket_->InsertPacket(
      PacedSender::kHighPriority, ssrc, sequence_number + 2,
      clock_.TimeInMilliseconds() + 33, kPacketSize, true);

  send_bucket_->InsertPacket(PacedSender::kHighPriority, ssrc, sequence_number,
                             clock_.TimeInMilliseconds(), kPacketSize, true);

  send_bucket_->InsertPacket(PacedSender::kHighPriority, ssrc,
                             sequence_number + 1, clock_.TimeInMilliseconds(),
                             kPacketSize, true);

  // Packets from earlier frames should be sent first.
  {
    ::testing::InSequence sequence;
    EXPECT_CALL(callback_,
                TimeToSendPacket(ssrc, sequence_number,
                                 clock_.TimeInMilliseconds(), true, _))
        .WillOnce(Return(true));
    EXPECT_CALL(callback_,
                TimeToSendPacket(ssrc, sequence_number + 1,
                                 clock_.TimeInMilliseconds(), true, _))
        .WillOnce(Return(true));
    EXPECT_CALL(callback_,
                TimeToSendPacket(ssrc, sequence_number + 3,
                                 clock_.TimeInMilliseconds() + 33, true, _))
        .WillOnce(Return(true));
    EXPECT_CALL(callback_,
                TimeToSendPacket(ssrc, sequence_number + 2,
                                 clock_.TimeInMilliseconds() + 33, true, _))
        .WillOnce(Return(true));

    while (send_bucket_->QueueSizePackets() > 0) {
      int time_until_process = send_bucket_->TimeUntilNextProcess();
      if (time_until_process <= 0) {
        send_bucket_->Process();
      } else {
        clock_.AdvanceTimeMilliseconds(time_until_process);
      }
    }
  }
}

TEST_F(PacedSenderTest, PaddingOveruse) {
  uint32_t ssrc = 12346;
  uint16_t sequence_number = 1234;
  const size_t kPacketSize = 1200;

  send_bucket_->Process();
  send_bucket_->SetEstimatedBitrate(60000);
  send_bucket_->SetSendBitrateLimits(60000, 0);

  SendAndExpectPacket(PacedSender::kNormalPriority, ssrc, sequence_number++,
                      clock_.TimeInMilliseconds(), kPacketSize, false);
  send_bucket_->Process();

  // Add 30kbit padding. When increasing budget, media budget will increase from
  // negative (overuse) while padding budget will increase from 0.
  clock_.AdvanceTimeMilliseconds(5);
  send_bucket_->SetSendBitrateLimits(60000, 30000);

  SendAndExpectPacket(PacedSender::kNormalPriority, ssrc, sequence_number++,
                      clock_.TimeInMilliseconds(), kPacketSize, false);
  EXPECT_LT(5u, send_bucket_->ExpectedQueueTimeMs());
  // Don't send padding if queue is non-empty, even if padding budget > 0.
  EXPECT_CALL(callback_, TimeToSendPadding(_, _)).Times(0);
  send_bucket_->Process();
}

TEST_F(PacedSenderTest, AverageQueueTime) {
  uint32_t ssrc = 12346;
  uint16_t sequence_number = 1234;
  const size_t kPacketSize = 1200;
  const int kBitrateBps = 10 * kPacketSize * 8;  // 10 packets per second.

  send_bucket_->SetEstimatedBitrate(kBitrateBps);

  EXPECT_EQ(0, send_bucket_->AverageQueueTimeMs());

  int64_t first_capture_time = clock_.TimeInMilliseconds();
  send_bucket_->InsertPacket(PacedSender::kNormalPriority, ssrc,
                             sequence_number, first_capture_time, kPacketSize,
                             false);
  clock_.AdvanceTimeMilliseconds(10);
  send_bucket_->InsertPacket(PacedSender::kNormalPriority, ssrc,
                             sequence_number + 1, clock_.TimeInMilliseconds(),
                             kPacketSize, false);
  clock_.AdvanceTimeMilliseconds(10);

  EXPECT_EQ((20 + 10) / 2, send_bucket_->AverageQueueTimeMs());

  // Only first packet (queued for 20ms) should be removed, leave the second
  // packet (queued for 10ms) alone in the queue.
  EXPECT_CALL(callback_, TimeToSendPacket(ssrc, sequence_number,
                                          first_capture_time, false, _))
      .Times(1)
      .WillRepeatedly(Return(true));
  send_bucket_->Process();

  EXPECT_EQ(10, send_bucket_->AverageQueueTimeMs());

  clock_.AdvanceTimeMilliseconds(10);
  EXPECT_CALL(callback_, TimeToSendPacket(ssrc, sequence_number + 1,
                                          first_capture_time + 10, false, _))
      .Times(1)
      .WillRepeatedly(Return(true));
  for (int i = 0; i < 3; ++i) {
    clock_.AdvanceTimeMilliseconds(30);  // Max delta.
    send_bucket_->Process();
  }

  EXPECT_EQ(0, send_bucket_->AverageQueueTimeMs());
}

TEST_F(PacedSenderTest, ProbeClusterId) {
  uint32_t ssrc = 12346;
  uint16_t sequence_number = 1234;
  const size_t kPacketSize = 1200;

  send_bucket_->SetSendBitrateLimits(kTargetBitrateBps, kTargetBitrateBps);
  send_bucket_->SetProbingEnabled(true);
  for (int i = 0; i < 11; ++i) {
    send_bucket_->InsertPacket(PacedSender::kNormalPriority, ssrc,
                               sequence_number + i, clock_.TimeInMilliseconds(),
                               kPacketSize, false);
  }

  // First probing cluster.
  EXPECT_CALL(callback_, TimeToSendPacket(_, _, _, _, 0))
      .Times(6)
      .WillRepeatedly(Return(true));
  for (int i = 0; i < 6; ++i) {
    clock_.AdvanceTimeMilliseconds(20);
    send_bucket_->Process();
  }

  // Second probing cluster.
  EXPECT_CALL(callback_, TimeToSendPacket(_, _, _, _, 1))
      .Times(5)
      .WillRepeatedly(Return(true));
  for (int i = 0; i < 5; ++i) {
    clock_.AdvanceTimeMilliseconds(20);
    send_bucket_->Process();
  }

  // No more probing packets.
  EXPECT_CALL(callback_, TimeToSendPadding(_, PacketInfo::kNotAProbe))
        .Times(1)
        .WillRepeatedly(Return(500));
  send_bucket_->Process();
}

}  // namespace test
}  // namespace webrtc
