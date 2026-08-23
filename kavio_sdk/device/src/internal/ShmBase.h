#pragma once

/*
 * File: ShmBase.h
 * Brief: Common POSIX SHM lifecycle base for Device SDK shared-memory types.
 *
 * ShmBase owns only the mapped-region lifecycle and common state. Concrete
 * SHM classes remain responsible for their binary layout and read/write model.
 */

#include <cstddef>
#include <cstdint>
#include <string>

namespace kavio {

enum class ShmKind : uint32_t {
    Input = 0,
    Frame = 1,
    EncodedStream = 2,
};

enum class ShmAccess : uint32_t {
    ReadOnly = 0,
    ReadWrite = 1,
};

class ShmBase {
public:
    virtual ~ShmBase();

    ShmBase(const ShmBase&) = delete;
    ShmBase& operator=(const ShmBase&) = delete;

    ShmKind Kind() const { return kind_; }
    bool IsOpen() const { return fd_ >= 0 && addr_ != nullptr && size_ > 0; }
    bool IsValid() const { return IsOpen() && layout_valid_; }
    bool IsWritable() const { return IsOpen() && access_ == ShmAccess::ReadWrite; }

    const std::string& Name() const { return name_; }
    size_t Size() const { return size_; }

    // 仅用于内部资源快照/诊断，不转移 fd 或映射区所有权。
    int NativeFd() const { return fd_; }
    void* MappedAddress() { return addr_; }
    const void* MappedAddress() const { return addr_; }

    // Close 只解除当前进程映射，不删除 POSIX SHM 对象。
    void Close();

    // 删除当前 Name() 对应的 POSIX SHM 对象；允许映射仍处于打开状态。
    bool Unlink(std::string* error = nullptr);

    // 先关闭本进程映射，再删除 POSIX SHM 对象。
    void CloseAndUnlink();

protected:
    enum class CreatePolicy : uint32_t {
        // SHM 已存在时调整为指定大小，并重新初始化映射内容。
        ResizeExisting = 0,

        // 创建时按指定大小截断对象，并重新初始化映射内容。
        Recreate = 1,
    };

    explicit ShmBase(ShmKind kind);
    ShmBase(ShmBase&& other) noexcept;
    ShmBase& operator=(ShmBase&& other) noexcept;

    bool CreateMapping(const std::string& name,
                       size_t size,
                       CreatePolicy policy,
                       bool unlink_on_failure,
                       std::string* error = nullptr);

    bool OpenMapping(const std::string& name,
                     size_t size,
                     ShmAccess access,
                     std::string* error = nullptr);

    bool OpenWholeMapping(const std::string& name,
                          size_t minimum_size,
                          ShmAccess access,
                          std::string* error = nullptr);

    void SetLayoutValid(bool valid) { layout_valid_ = valid; }

    // Create/Open 成功后设置公共状态。CreateMapping/OpenMapping 已负责 Name/Access；
    // 子类只需要完成 Attach/Validate 后调用 SetLayoutValid(true)。
    ShmAccess Access() const { return access_; }

    // 原始 fd/address 仅供具体 SHM 实现使用，不暴露给统一业务接口。
    int Fd() const { return fd_; }
    void* Address() { return addr_; }
    const void* Address() const { return addr_; }

    // 子类只清理指向 SHM 布局内部的 non-owning 指针，不处理 mmap/fd。
    virtual void ResetLayout() noexcept = 0;

    static bool UnlinkByName(const std::string& name,
                             std::string* error = nullptr);
    static void SetError(std::string* error, const std::string& message);

private:
    bool MapOwnedFd(int fd,
                    const std::string& name,
                    size_t size,
                    ShmAccess access,
                    std::string* error);
    void CloseMappingOnly() noexcept;
    void MoveFrom(ShmBase&& other) noexcept;

    static void SetErrnoError(std::string* error,
                              const std::string& prefix,
                              int error_number);

private:
    ShmKind kind_;
    int fd_ = -1;
    void* addr_ = nullptr;
    size_t size_ = 0;
    std::string name_;
    ShmAccess access_ = ShmAccess::ReadOnly;
    bool layout_valid_ = false;
    bool unlinked_ = false;
};

}  // namespace kavio
