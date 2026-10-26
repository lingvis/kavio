#include "internal/IPCSocket.h"

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <utility>
#include <vector>

namespace kavio {
namespace {

Status ReadExact(int fd, void* buf, size_t len) {
    auto* p = static_cast<uint8_t*>(buf);
    size_t done = 0;
    while (done < len) {
        const ssize_t n = ::read(fd, p + done, len - done);
        if (n == 0) {
            return Status::Error(StatusCode::IPC_SocketError, "peer closed");
        }
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return Status::Error(StatusCode::IPC_SocketError,
                                 std::string("read failed: ") + std::strerror(errno));
        }
        done += static_cast<size_t>(n);
    }
    return Status::Ok();
}

Status WriteExact(int fd, const void* buf, size_t len) {
    const auto* p = static_cast<const uint8_t*>(buf);
    size_t done = 0;
    while (done < len) {
        const ssize_t n = ::send(fd,
                                 p + done,
                                 len - done,
#ifdef MSG_NOSIGNAL
                                 MSG_NOSIGNAL
#else
                                 0
#endif
        );
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return Status::Error(StatusCode::IPC_SocketError,
                                 std::string("write failed: ") + std::strerror(errno));
        }
        done += static_cast<size_t>(n);
    }
    return Status::Ok();
}

}  // namespace

struct IPCSocket::Connection {
    explicit Connection(int socket_fd) : fd(socket_fd) {}

    ~Connection() {
        if (fd >= 0) {
            ::close(fd);
        }
    }

    int fd = -1;
};

IPCSocket::IPCSocket() = default;

IPCSocket::~IPCSocket() {
    Disconnect();
}

std::shared_ptr<IPCSocket::Connection> IPCSocket::SnapshotConnection() const {
    std::lock_guard<std::mutex> lock(connection_mutex_);
    return connection_;
}

Status IPCSocket::Connect(const std::string& socket_path) {
    std::lock_guard<std::mutex> lock(connection_mutex_);
    if (connection_) {
        return Status::Error(StatusCode::Common_AlreadyExists, "socket already connected");
    }

    const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        return Status::Error(StatusCode::IPC_SocketError,
                             std::string("socket failed: ") + std::strerror(errno));
    }

    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    if (socket_path.empty() || socket_path.size() >= sizeof(addr.sun_path)) {
        ::close(fd);
        return Status::Error(StatusCode::Common_InvalidArgument, "invalid socket path");
    }

    std::strncpy(addr.sun_path, socket_path.c_str(), sizeof(addr.sun_path) - 1);
    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        const auto err = std::string("connect failed: ") + std::strerror(errno);
        ::close(fd);
        return Status::Error(StatusCode::IPC_SocketError, err);
    }

    connection_ = std::make_shared<Connection>(fd);
    return Status::Ok();
}

void IPCSocket::Disconnect() {
    std::shared_ptr<Connection> connection;
    {
        std::lock_guard<std::mutex> lock(connection_mutex_);
        connection.swap(connection_);
    }

    if (connection && connection->fd >= 0) {
        // shutdown wakes a blocking Receive/Send. The descriptor itself remains
        // alive until the last in-flight IO releases its Connection reference,
        // preventing fd reuse while an operation is still using it.
        ::shutdown(connection->fd, SHUT_RDWR);
    }
}

bool IPCSocket::IsConnected() const {
    return static_cast<bool>(SnapshotConnection());
}

Status IPCSocket::Send(const kavio::ipc::proto::IPCPackagePb& packet) {
    const auto connection = SnapshotConnection();
    if (!connection) {
        return Status::Error(StatusCode::Common_NotConnected, "socket is not connected");
    }

    const size_t size = packet.ByteSizeLong();
    if (size == 0 || size > KAVIO_IPC_MAX_PACKAGE_BYTES)
        return Status::Error(StatusCode::Common_ProtocolError, "IPC packet exceeds maximum size");
    std::vector<uint8_t> payload(size);
    if (!packet.SerializeToArray(payload.data(), static_cast<int>(size))) {
        return Status::Error(StatusCode::Common_ProtocolError, "protobuf serialize failed");
    }

    const uint32_t len = static_cast<uint32_t>(payload.size());
    auto st = WriteExact(connection->fd, &len, sizeof(len));
    if (!st.ok()) {
        return st;
    }
    return WriteExact(connection->fd, payload.data(), payload.size());
}

Status IPCSocket::Receive(kavio::ipc::proto::IPCPackagePb& out_packet) {
    const auto connection = SnapshotConnection();
    if (!connection) {
        return Status::Error(StatusCode::Common_NotConnected, "socket is not connected");
    }

    uint32_t len = 0;
    auto st = ReadExact(connection->fd, &len, sizeof(len));
    if (!st.ok()) {
        return st;
    }
    if (len == 0 || len > KAVIO_IPC_MAX_PACKAGE_BYTES) {
        return Status::Error(StatusCode::Common_ProtocolError, "invalid packet length");
    }

    std::vector<uint8_t> payload(len);
    st = ReadExact(connection->fd, payload.data(), payload.size());
    if (!st.ok()) {
        return st;
    }
    if (!out_packet.ParseFromArray(payload.data(), static_cast<int>(payload.size()))) {
        return Status::Error(StatusCode::Common_ProtocolError, "protobuf parse failed");
    }
    return Status::Ok();
}

}  // namespace kavio
