#include <chrono>
#include <cstdio>
#include <memory>
#include <optional>
#include <string>
#include <thread>

#include "internal/base/file_path.h"
#include "internal/base/files.h"
#include "internal/platform/implementation/linux/bluetooth_adapter.h"
#include "internal/platform/implementation/linux/device_info.h"
#include "internal/platform/implementation/linux/sharing_context.h"
#include "internal/flags/nearby_flags.h"
#include "internal/platform/task_runner_impl.h"
#include "sharing/flags/generated/nearby_sharing_feature_flags.h"
#include "sharing/nearby_connections_manager.h"
#include "sharing/nearby_connections_manager_impl.h"
#include "sharing/nearby_connections_service_impl.h"
#include "sharing/nearby_connections_types.h"
#include "send_session.h"

namespace {

void PrintUsage() {
  std::fprintf(stderr, "usage: quickshare send <path>\n");
}

int RunSend(const std::string& path) {
  // Without this, NearbyConnectionsManagerImpl::Connect() excludes BLE from
  // the connect-allowed mediums (ShouldEnableBleForTransfers() reads this
  // flag, default off upstream -- it's meant to be server-controlled on
  // real Android). If BLE discovers the peer before WiFi LAN does, Connect
  // then fails outright ("Endpoint channel not available") because the
  // only medium the endpoint was found on isn't in the allowed set. This
  // app has no WiFi-LAN-only guarantee, so BLE-as-transfer-medium has to
  // stay enabled.
  ::nearby::NearbyFlags::GetInstance().OverrideBoolFlagValue(
      ::nearby::sharing::config_package_nearby::nearby_sharing_feature::
          kEnableBleForTransfer,
      true);

  ::nearby::FilePath file_path(path);
  std::optional<uintmax_t> file_size = ::nearby::Files::GetFileSize(file_path);
  if (!file_size.has_value()) {
    std::printf("File not found or unreadable: %s\n", path.c_str());
    return 1;
  }
  std::string display_name = file_path.GetFileName().ToString();
  std::printf("Will send: %s (%ju bytes) as \"%s\"\n", path.c_str(),
              *file_size, display_name.c_str());

  ::nearby::linux_platform::BluetoothAdapter adapter;
  ::nearby::linux_platform::SharingContext context(adapter);
  ::nearby::linux_platform::DeviceInfo device_info;
  ::nearby::linux_platform::NoOpConnectivityManager connectivity_manager;
  ::nearby::TaskRunnerImpl callback_task_runner(/*runner_count=*/1);

  auto connections_service =
      std::make_unique<::nearby::sharing::NearbyConnectionsServiceImpl>(
          &connectivity_manager, /*event_logger=*/nullptr);
  ::nearby::sharing::NearbyConnectionsManagerImpl manager(
      &callback_task_runner, &context, connectivity_manager, device_info,
      std::move(connections_service));

  auto session = std::make_shared<quickshare::SendSession>(
      manager, file_path, display_name, "application/octet-stream",
      *file_size);

  bool discovery_result_known = false;
  bool discovery_ok = false;
  manager.StartDiscovery(
      session.get(), ::nearby::sharing::proto::DataUsage::ONLINE_DATA_USAGE,
      /*alternate_service_uuid=*/std::nullopt,
      [&](::nearby::sharing::Status status) {
        discovery_ok = status == ::nearby::sharing::Status::kSuccess;
        discovery_result_known = true;
      });

  for (int i = 0; i < 300 && !discovery_result_known; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  if (!discovery_result_known || !discovery_ok) {
    std::printf("Failed to start discovery.\n");
    manager.Shutdown();
    return 1;
  }

  std::printf(
      "Discovering for up to 60s -- make sure the receiving device's Quick "
      "Share \"ready to receive\" screen is open...\n");
  for (int i = 0; i < 600 && !session->done(); ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }

  bool ok = session->done() && session->success();
  std::printf(ok ? "\nSUCCESS: file transfer completed.\n"
                 : "\nFAILED: see log above.\n");

  manager.Shutdown();
  return ok ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    PrintUsage();
    return 1;
  }
  std::string command = argv[1];
  if (command == "send") {
    if (argc < 3) {
      PrintUsage();
      return 1;
    }
    return RunSend(argv[2]);
  }
  PrintUsage();
  return 1;
}
