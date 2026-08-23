#pragma once

#include <kavio/Types.h>
#include "ipc.pb.h"

#include <memory>
#include <mutex>
#include <string>

namespace kavio {

class IPCSocket {
public:
    IPCSocket();
    ~IPCSocket();

    IPCSocket(const IPCSocket&) = delete;
    IPCSocket& operator=(const IPCSocket&) = delete;

    Status Connect(const std::string& socket_path = DefaultIpcSocketPath());
    void Disconnect();

    bool IsConnected() const;

    Status Send(const kavio::ipc::proto::IPCPackagePb& packet);
    Status Receive(kavio::ipc::proto::IPCPackagePb& out_packet);

private:
    struct Connection;

    std::shared_ptr<Connection> SnapshotConnection() const;

    mutable std::mutex connection_mutex_;
    std::shared_ptr<Connection> connection_;
};

}  // namespace kavio