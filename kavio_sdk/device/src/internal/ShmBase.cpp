#include "internal/ShmBase.h"

#include <cerrno>
#include <cstring>
#include <limits>
#include <utility>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

namespace kavio {
namespace {

int OpenFlagsForAccess(ShmAccess access) {
    return access == ShmAccess::ReadWrite ? O_RDWR : O_RDONLY;
}

int ProtectionForAccess(ShmAccess access) {
    return access == ShmAccess::ReadWrite ? (PROT_READ | PROT_WRITE) : PROT_READ;
}

}  // namespace

ShmBase::ShmBase(ShmKind kind)
    : kind_(kind) {
}

ShmBase::~ShmBase() {
    // 析构阶段不调用虚函数。派生类的布局指针随对象一起销毁，只需释放 owning 资源。
    CloseMappingOnly();
}

ShmBase::ShmBase(ShmBase&& other) noexcept
    : kind_(other.kind_) {
    MoveFrom(std::move(other));
}

ShmBase& ShmBase::operator=(ShmBase&& other) noexcept {
    if (this != &other) {
        CloseMappingOnly();
        kind_ = other.kind_;
        MoveFrom(std::move(other));
    }
    return *this;
}

bool ShmBase::CreateMapping(const std::string& name,
                            size_t size,
                            CreatePolicy policy,
                            bool unlink_on_failure,
                            std::string* error) {
    Close();
    name_.clear();
    unlinked_ = false;

    if (name.empty() || size == 0) {
        SetError(error, "invalid shared memory create arguments");
        return false;
    }

    int flags = O_CREAT | O_RDWR;
    if (policy == CreatePolicy::Recreate) {
        flags |= O_TRUNC;
    }

    const int fd = ::shm_open(name.c_str(), flags, 0666);
    if (fd < 0) {
        SetErrnoError(error, "shm_open failed: " + name, errno);
        return false;
    }

    if (::ftruncate(fd, static_cast<off_t>(size)) != 0) {
        const int saved_errno = errno;
        ::close(fd);
        if (unlink_on_failure) {
            ::shm_unlink(name.c_str());
        }
        SetErrnoError(error, "ftruncate failed: " + name, saved_errno);
        return false;
    }

    if (!MapOwnedFd(fd, name, size, ShmAccess::ReadWrite, error)) {
        if (unlink_on_failure) {
            ::shm_unlink(name.c_str());
        }
        return false;
    }

    unlinked_ = false;
    return true;
}

bool ShmBase::OpenMapping(const std::string& name,
                          size_t size,
                          ShmAccess access,
                          std::string* error) {
    Close();
    name_.clear();
    unlinked_ = false;

    if (name.empty() || size == 0) {
        SetError(error, "invalid shared memory open arguments");
        return false;
    }

    const int fd = ::shm_open(name.c_str(), OpenFlagsForAccess(access), 0);
    if (fd < 0) {
        SetErrnoError(error, "shm_open failed: " + name, errno);
        return false;
    }

    struct stat st {};
    if (::fstat(fd, &st) != 0) {
        const int saved_errno = errno;
        ::close(fd);
        SetErrnoError(error, "fstat failed: " + name, saved_errno);
        return false;
    }
    if (st.st_size < 0 || static_cast<uint64_t>(st.st_size) < static_cast<uint64_t>(size)) {
        ::close(fd);
        SetError(error, "shared memory size is smaller than requested mapping: " + name);
        return false;
    }

    if (!MapOwnedFd(fd, name, size, access, error)) {
        return false;
    }

    unlinked_ = false;
    return true;
}

bool ShmBase::OpenWholeMapping(const std::string& name,
                               size_t minimum_size,
                               ShmAccess access,
                               std::string* error) {
    Close();
    name_.clear();
    unlinked_ = false;

    if (name.empty()) {
        SetError(error, "invalid shared memory open arguments");
        return false;
    }

    const int fd = ::shm_open(name.c_str(), OpenFlagsForAccess(access), 0);
    if (fd < 0) {
        SetErrnoError(error, "shm_open failed: " + name, errno);
        return false;
    }

    struct stat st {};
    if (::fstat(fd, &st) != 0) {
        const int saved_errno = errno;
        ::close(fd);
        SetErrnoError(error, "fstat failed: " + name, saved_errno);
        return false;
    }
    if (st.st_size < 0 || static_cast<uint64_t>(st.st_size) < minimum_size) {
        ::close(fd);
        SetError(error, "shared memory size is too small: " + name);
        return false;
    }
    if (static_cast<uint64_t>(st.st_size) >
        static_cast<uint64_t>(std::numeric_limits<size_t>::max())) {
        ::close(fd);
        SetError(error, "shared memory size exceeds process addressable size: " + name);
        return false;
    }

    if (!MapOwnedFd(fd, name, static_cast<size_t>(st.st_size), access, error)) {
        return false;
    }

    unlinked_ = false;
    return true;
}

void ShmBase::Close() {
    ResetLayout();
    layout_valid_ = false;
    CloseMappingOnly();
}

bool ShmBase::Unlink(std::string* error) {
    if (name_.empty() || unlinked_) {
        return false;
    }
    if (!UnlinkByName(name_, error)) {
        return false;
    }
    unlinked_ = true;
    return true;
}

void ShmBase::CloseAndUnlink() {
    const std::string name = name_;
    Close();
    if (!name.empty() && !unlinked_) {
        (void)UnlinkByName(name, nullptr);
    }
    unlinked_ = true;
    name_.clear();
}

bool ShmBase::UnlinkByName(const std::string& name, std::string* error) {
    if (name.empty()) {
        SetError(error, "shared memory name is empty");
        return false;
    }
    if (::shm_unlink(name.c_str()) != 0) {
        SetErrnoError(error, "shm_unlink failed: " + name, errno);
        return false;
    }
    return true;
}

void ShmBase::SetError(std::string* error, const std::string& message) {
    if (error) {
        *error = message;
    }
}

bool ShmBase::MapOwnedFd(int fd,
                         const std::string& name,
                         size_t size,
                         ShmAccess access,
                         std::string* error) {
    void* addr = ::mmap(nullptr, size, ProtectionForAccess(access), MAP_SHARED, fd, 0);
    if (addr == MAP_FAILED) {
        const int saved_errno = errno;
        ::close(fd);
        SetErrnoError(error, "mmap failed: " + name, saved_errno);
        return false;
    }

    fd_ = fd;
    addr_ = addr;
    size_ = size;
    name_ = name;
    access_ = access;
    layout_valid_ = false;
    return true;
}

void ShmBase::CloseMappingOnly() noexcept {
    if (addr_ != nullptr) {
        ::munmap(addr_, size_);
    }
    if (fd_ >= 0) {
        ::close(fd_);
    }
    fd_ = -1;
    addr_ = nullptr;
    size_ = 0;
    access_ = ShmAccess::ReadOnly;
    layout_valid_ = false;
}

void ShmBase::MoveFrom(ShmBase&& other) noexcept {
    fd_ = other.fd_;
    addr_ = other.addr_;
    size_ = other.size_;
    name_ = std::move(other.name_);
    access_ = other.access_;
    layout_valid_ = other.layout_valid_;
    unlinked_ = other.unlinked_;

    other.fd_ = -1;
    other.addr_ = nullptr;
    other.size_ = 0;
    other.name_.clear();
    other.access_ = ShmAccess::ReadOnly;
    other.layout_valid_ = false;
    other.unlinked_ = false;
}

void ShmBase::SetErrnoError(std::string* error,
                            const std::string& prefix,
                            int error_number) {
    if (error) {
        *error = prefix + ": " + std::strerror(error_number) +
                 " (errno=" + std::to_string(error_number) + ")";
    }
}

}  // namespace kavio
