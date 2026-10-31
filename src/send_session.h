#ifndef QUICKSHARE_LINUX_APP_SRC_SEND_SESSION_H_
#define QUICKSHARE_LINUX_APP_SRC_SEND_SESSION_H_

#include <cstdint>
#include <memory>
#include <optional>
#include <string>

#include "internal/base/file_path.h"
#include "internal/platform/task_runner_impl.h"
#include "sharing/incoming_frames_reader.h"
#include "sharing/nearby_connections_manager.h"
#include "sharing/nearby_connections_types.h"
#include "sharing/proto/wire_format.pb.h"

namespace quickshare {

// Drives one discover -> connect -> verify -> introduce -> transfer session
// against the first endpoint found, using the real production
// NearbyConnectionsManager. Lives as long as the caller holds a shared_ptr
// to it; async callbacks capture shared_from_this() so the session survives
// until it is explicitly finished.
//
// Paired-key verification: this app has no NearbyShareCertificateManager (no
// account/certificate backend), so it deliberately takes the same fallback
// path the real algorithm itself documents for this exact situation (see
// nearby's sharing/paired_key_verification_runner.cc, SendPairedKeyEncryptionFrame:
// "if a valid signature cannot be generated" -> random bytes, "consistent
// with the GmsCore implementation"). As the *sender* with no certificate,
// verification settles on UNABLE, the same outcome real Quick Share reaches
// when sending to a device that isn't one of your paired contacts.
class SendSession
    : public ::nearby::sharing::NearbyConnectionsManager::DiscoveryListener,
      public std::enable_shared_from_this<SendSession> {
 public:
  SendSession(::nearby::sharing::NearbyConnectionsManager& manager,
              ::nearby::FilePath file_to_send, std::string file_display_name,
              std::string mime_type, uint64_t file_size);

  void OnEndpointDiscovered(absl::string_view endpoint_id,
                            absl::Span<const uint8_t> endpoint_info) override;
  void OnEndpointLost(absl::string_view endpoint_id) override;

  bool done() const { return done_; }
  bool success() const { return success_; }

 private:
  using V1Frame = ::nearby::sharing::service::proto::V1Frame;
  using Frame = ::nearby::sharing::service::proto::Frame;

  void OnConnectResult(std::string endpoint_id,
                       ::nearby::sharing::NearbyConnection* connection,
                       ::nearby::sharing::Status status);
  void WriteFrame(const Frame& frame);
  void SendPairedKeyEncryptionFrame();
  void OnPairedKeyEncryptionFrame(bool is_timeout, std::optional<V1Frame> frame);
  void OnPairedKeyResultFrame(bool is_timeout, std::optional<V1Frame> frame);
  void SendIntroduction();
  void OnConnectionResponse(bool is_timeout, std::optional<V1Frame> frame);
  void OnPayloadUpdate(const ::nearby::sharing::PayloadTransferUpdate& update);
  void Finish(bool ok);

  class PayloadListener
      : public ::nearby::sharing::NearbyConnectionsManager::PayloadStatusListener {
   public:
    explicit PayloadListener(std::shared_ptr<SendSession> session)
        : session_(std::move(session)) {}
    void OnStatusUpdate(
        std::unique_ptr<::nearby::sharing::PayloadTransferUpdate> update)
        override {
      session_->OnPayloadUpdate(*update);
    }

   private:
    std::shared_ptr<SendSession> session_;
  };

  ::nearby::sharing::NearbyConnectionsManager& manager_;
  ::nearby::FilePath file_to_send_;
  std::string file_display_name_;
  std::string mime_type_;
  uint64_t file_size_;

  bool connecting_or_connected_ = false;
  bool done_ = false;
  bool success_ = false;

  std::string endpoint_id_;
  ::nearby::sharing::NearbyConnection* connection_ = nullptr;
  ::nearby::TaskRunnerImpl frames_task_runner_{/*runner_count=*/1};
  std::shared_ptr<::nearby::sharing::IncomingFramesReader> frames_reader_;
  int64_t file_payload_id_ = 0;
  std::unique_ptr<::nearby::sharing::Payload> pending_file_payload_;
  std::shared_ptr<PayloadListener> payload_listener_;
};

}  // namespace quickshare

#endif  // QUICKSHARE_LINUX_APP_SRC_SEND_SESSION_H_
