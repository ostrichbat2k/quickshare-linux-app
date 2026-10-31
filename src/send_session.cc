#include "send_session.h"

#include <openssl/rand.h>

#include <cstdio>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "sharing/advertisement.h"
#include "sharing/proto/enums.pb.h"
#include "proto/sharing_enums.pb.h"

namespace quickshare {

namespace {

using ::location::nearby::proto::sharing::OSType;
using ::nearby::sharing::Advertisement;
using ::nearby::sharing::AdvertisementCapabilities;
using ::nearby::sharing::NearbyConnection;
using ::nearby::sharing::Payload;
using ::nearby::sharing::PayloadStatus;
using ::nearby::sharing::ShareTargetType;
using ::nearby::sharing::Status;
using ::nearby::sharing::TransportType;
using ::nearby::sharing::service::proto::ConnectionResponseFrame;
using ::nearby::sharing::service::proto::FileMetadata;
using ::nearby::sharing::service::proto::IntroductionFrame;
using ::nearby::sharing::service::proto::PairedKeyEncryptionFrame;
using ::nearby::sharing::service::proto::PairedKeyResultFrame;

// See nearby's sharing/certificates/constants.h (not separately buildable,
// bundled into a monolithic target) -- values hardcoded here with a pointer
// back to the real source rather than guessed.
constexpr size_t kNearbyShareNumBytesAuthenticationTokenHash = 6;
// See sharing/paired_key_verification_runner.cc's
// kNearbyShareNumBytesRandomSignature.
constexpr size_t kNearbyShareNumBytesRandomSignature = 72;

constexpr absl::Duration kReadFrameTimeout = absl::Seconds(15);

std::vector<uint8_t> GenerateRandomBytes(size_t num_bytes) {
  std::vector<uint8_t> bytes(num_bytes);
  RAND_bytes(bytes.data(), static_cast<int>(bytes.size()));
  return bytes;
}

std::vector<uint8_t> BuildOwnEndpointInfo() {
  std::vector<uint8_t> salt(Advertisement::kSaltSize, 0);
  std::vector<uint8_t> encrypted_metadata_key(
      Advertisement::kMetadataEncryptionKeyHashByteSize, 0);
  std::unique_ptr<Advertisement> advertisement = Advertisement::NewInstance(
      std::move(salt), std::move(encrypted_metadata_key),
      ShareTargetType::kLaptop, std::string("quickshare-linux"),
      /*vendor_id=*/0, AdvertisementCapabilities{});
  return advertisement != nullptr ? advertisement->ToEndpointInfo()
                                  : std::vector<uint8_t>{};
}

}  // namespace

SendSession::SendSession(
    ::nearby::sharing::NearbyConnectionsManager& manager,
    ::nearby::FilePath file_to_send, std::string file_display_name,
    std::string mime_type, uint64_t file_size)
    : manager_(manager),
      file_to_send_(std::move(file_to_send)),
      file_display_name_(std::move(file_display_name)),
      mime_type_(std::move(mime_type)),
      file_size_(file_size) {}

void SendSession::OnEndpointDiscovered(
    absl::string_view endpoint_id, absl::Span<const uint8_t> endpoint_info) {
  if (connecting_or_connected_) return;
  connecting_or_connected_ = true;

  std::unique_ptr<Advertisement> peer_advertisement =
      Advertisement::FromEndpointInfo(endpoint_info);
  printf("Discovered endpoint %s (%s), connecting...\n",
         std::string(endpoint_id).c_str(),
         (peer_advertisement != nullptr &&
          peer_advertisement->device_name().has_value())
             ? peer_advertisement->device_name()->c_str()
             : "unknown device name");

  // Deliberately NOT calling manager_.StopDiscovery() here: for a
  // BLE-discovered endpoint, stopping discovery lets BlueZ garbage-collect
  // the (never-paired) device's D-Bus object before Connect() gets to use
  // it, producing "UnknownObject ... Device1 doesn't exist" from the BLE
  // GATT client. Keep discovery running until the connection attempt
  // settles; connecting_or_connected_ above already prevents acting on a
  // second discovery while one is in flight.
  std::shared_ptr<SendSession> self = shared_from_this();
  manager_.Connect(
      BuildOwnEndpointInfo(), endpoint_id,
      /*bluetooth_mac_address=*/std::nullopt,
      ::nearby::sharing::proto::DataUsage::ONLINE_DATA_USAGE,
      TransportType::kAny,
      [self](absl::string_view endpoint_id, NearbyConnection* connection,
             Status status) {
        self->OnConnectResult(std::string(endpoint_id), connection, status);
      });
}

void SendSession::OnEndpointLost(absl::string_view endpoint_id) {
  printf("Endpoint lost: %s\n", std::string(endpoint_id).c_str());
}

void SendSession::OnConnectResult(std::string endpoint_id,
                                   NearbyConnection* connection,
                                   Status status) {
  if (status != Status::kSuccess || connection == nullptr) {
    printf("Connect failed: status=%d\n", static_cast<int>(status));
    Finish(false);
    return;
  }
  printf("Connected to endpoint %s\n", endpoint_id.c_str());
  endpoint_id_ = std::move(endpoint_id);
  connection_ = connection;
  frames_reader_ = std::make_shared<::nearby::sharing::IncomingFramesReader>(
      frames_task_runner_, connection_);

  std::shared_ptr<SendSession> self = shared_from_this();
  connection_->SetDisconnectionListener([self]() {
    printf("Connection closed by peer.\n");
    self->Finish(false);
  });

  SendPairedKeyEncryptionFrame();
  frames_reader_->ReadFrame(
      V1Frame::PAIRED_KEY_ENCRYPTION,
      [self](bool is_timeout, std::optional<V1Frame> frame) {
        self->OnPairedKeyEncryptionFrame(is_timeout, std::move(frame));
      },
      kReadFrameTimeout);
}

void SendSession::WriteFrame(const Frame& frame) {
  std::vector<uint8_t> data(frame.ByteSizeLong());
  frame.SerializeToArray(data.data(), data.size());
  manager_.Send(
      endpoint_id_, std::make_unique<Payload>(std::move(data)),
      std::weak_ptr<
          ::nearby::sharing::NearbyConnectionsManager::PayloadStatusListener>());
}

void SendSession::SendPairedKeyEncryptionFrame() {
  std::vector<uint8_t> signature =
      GenerateRandomBytes(kNearbyShareNumBytesRandomSignature);
  std::vector<uint8_t> secret_id_hash =
      GenerateRandomBytes(kNearbyShareNumBytesAuthenticationTokenHash);

  Frame frame;
  frame.set_version(Frame::V1);
  V1Frame* v1_frame = frame.mutable_v1();
  v1_frame->set_type(V1Frame::PAIRED_KEY_ENCRYPTION);
  PairedKeyEncryptionFrame* encryption_frame =
      v1_frame->mutable_paired_key_encryption();
  encryption_frame->set_signed_data(
      reinterpret_cast<const char*>(signature.data()), signature.size());
  encryption_frame->set_secret_id_hash(
      reinterpret_cast<const char*>(secret_id_hash.data()),
      secret_id_hash.size());
  WriteFrame(frame);
  printf("Sent PairedKeyEncryptionFrame (random signature -- no "
         "certificate manager on this side).\n");
}

void SendSession::OnPairedKeyEncryptionFrame(bool is_timeout,
                                              std::optional<V1Frame> frame) {
  if (!frame.has_value()) {
    printf("Failed to read peer's PairedKeyEncryptionFrame (timeout=%d)\n",
           is_timeout);
    Finish(false);
    return;
  }
  printf("Received peer's PairedKeyEncryptionFrame (%zu-byte signature)\n",
         frame->paired_key_encryption().signed_data().size());

  Frame result_frame;
  result_frame.set_version(Frame::V1);
  V1Frame* v1_frame = result_frame.mutable_v1();
  v1_frame->set_type(V1Frame::PAIRED_KEY_RESULT);
  PairedKeyResultFrame* result = v1_frame->mutable_paired_key_result();
  // UNABLE, not SUCCESS/FAIL -- see the class-level comment in the header.
  result->set_status(PairedKeyResultFrame::UNABLE);
  result->set_os_type(OSType::UNKNOWN_OS_TYPE);
  WriteFrame(result_frame);
  printf("Sent PairedKeyResultFrame (status=UNABLE).\n");

  std::shared_ptr<SendSession> self = shared_from_this();
  frames_reader_->ReadFrame(
      V1Frame::PAIRED_KEY_RESULT,
      [self](bool is_timeout, std::optional<V1Frame> frame) {
        self->OnPairedKeyResultFrame(is_timeout, std::move(frame));
      },
      kReadFrameTimeout);
}

void SendSession::OnPairedKeyResultFrame(bool is_timeout,
                                          std::optional<V1Frame> frame) {
  if (!frame.has_value()) {
    printf("Failed to read peer's PairedKeyResultFrame (timeout=%d)\n",
           is_timeout);
    Finish(false);
    return;
  }
  printf("Peer's paired-key verification result: status=%d\n",
         static_cast<int>(frame->paired_key_result().status()));
  SendIntroduction();
}

void SendSession::SendIntroduction() {
  Payload file_payload(file_to_send_);
  file_payload_id_ = file_payload.id;

  Frame frame;
  frame.set_version(Frame::V1);
  V1Frame* v1_frame = frame.mutable_v1();
  v1_frame->set_type(V1Frame::INTRODUCTION);
  IntroductionFrame* introduction = v1_frame->mutable_introduction();
  introduction->set_use_case(IntroductionFrame::NEARBY_SHARE);
  introduction->set_start_transfer(true);
  FileMetadata* file_metadata = introduction->add_file_metadata();
  file_metadata->set_id(file_payload_id_);
  file_metadata->set_name(file_display_name_);
  file_metadata->set_payload_id(file_payload_id_);
  file_metadata->set_mime_type(mime_type_);
  file_metadata->set_size(file_size_);
  WriteFrame(frame);
  printf("Sent IntroductionFrame for \"%s\" (%llu bytes, payload id %lld), "
         "waiting for the peer to accept...\n",
         file_display_name_.c_str(),
         static_cast<unsigned long long>(file_size_),
         static_cast<long long>(file_payload_id_));

  pending_file_payload_ = std::make_unique<Payload>(std::move(file_payload));

  std::shared_ptr<SendSession> self = shared_from_this();
  frames_reader_->ReadFrame(
      V1Frame::RESPONSE,
      [self](bool is_timeout, std::optional<V1Frame> frame) {
        self->OnConnectionResponse(is_timeout, std::move(frame));
      },
      absl::ZeroDuration());
}

void SendSession::OnConnectionResponse(bool is_timeout,
                                        std::optional<V1Frame> frame) {
  if (!frame.has_value()) {
    printf("No ConnectionResponseFrame from peer (timeout=%d).\n",
           is_timeout);
    Finish(false);
    return;
  }
  ConnectionResponseFrame::Status status = frame->connection_response().status();
  printf("ConnectionResponseFrame: status=%d\n", static_cast<int>(status));
  if (status != ConnectionResponseFrame::ACCEPT) {
    printf("Peer did not accept the transfer.\n");
    Finish(false);
    return;
  }

  printf("Accepted! Sending file payload...\n");
  payload_listener_ = std::make_shared<PayloadListener>(shared_from_this());
  manager_.Send(endpoint_id_, std::move(pending_file_payload_),
                payload_listener_->GetWeakPtr());
}

void SendSession::OnPayloadUpdate(
    const ::nearby::sharing::PayloadTransferUpdate& update) {
  printf("Payload %lld: status=%d, %llu/%llu bytes\n",
         static_cast<long long>(update.payload_id),
         static_cast<int>(update.status),
         static_cast<unsigned long long>(update.bytes_transferred),
         static_cast<unsigned long long>(update.total_bytes));
  switch (update.status) {
    case PayloadStatus::kSuccess:
      printf("Transfer complete.\n");
      Finish(true);
      break;
    case PayloadStatus::kFailure:
    case PayloadStatus::kCanceled:
      printf("Transfer did not complete.\n");
      Finish(false);
      break;
    case PayloadStatus::kInProgress:
      break;
  }
}

void SendSession::Finish(bool ok) {
  if (done_) return;
  success_ = ok;
  done_ = true;
}

}  // namespace quickshare
