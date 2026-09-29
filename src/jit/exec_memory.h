#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>

#if defined(_WIN32)
  #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
  #endif
  #ifndef NOMINMAX
    #define NOMINMAX
  #endif
  #include <windows.h>
#else
  #include <sys/mman.h>
#endif

// In-process executable memory. This is what makes Lithon a pure
// in-memory JIT: machine code goes straight from the encoder's byte
// vector into an OS page that is flipped to execute-only, with no
// object file, no temp file and no subprocess in between.
//
// W^X discipline: the page is mapped read/write, filled, and only then
// flipped to read/execute. It is never writable and executable at the
// same time, which is what hardened kernels (and some Windows
// configurations) require.
//
//   POSIX   : mmap(PROT_READ|PROT_WRITE) -> mprotect(PROT_READ|PROT_EXEC)
//   Windows : VirtualAlloc(PAGE_READWRITE) -> VirtualProtect(PAGE_EXECUTE_READ)
//             + FlushInstructionCache

namespace lithon::jit {

class ExecutableBuffer {
public:
    ExecutableBuffer() = default;

    explicit ExecutableBuffer(const std::vector<uint8_t>& code) {
        load(code.data(), code.size());
    }

    ExecutableBuffer(const ExecutableBuffer&) = delete;
    ExecutableBuffer& operator=(const ExecutableBuffer&) = delete;

    ExecutableBuffer(ExecutableBuffer&& other) noexcept
        : mem_(other.mem_), size_(other.size_) {
        other.mem_ = nullptr;
        other.size_ = 0;
    }

    ExecutableBuffer& operator=(ExecutableBuffer&& other) noexcept {
        if (this != &other) {
            release();
            mem_ = other.mem_;
            size_ = other.size_;
            other.mem_ = nullptr;
            other.size_ = 0;
        }
        return *this;
    }

    ~ExecutableBuffer() { release(); }

    void load(const uint8_t* data, size_t size) {
        release();
        if (size == 0) throw std::runtime_error("ExecutableBuffer: empty code");
#if defined(_WIN32)
        void* mem = VirtualAlloc(nullptr, size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (!mem) throw std::runtime_error("ExecutableBuffer: VirtualAlloc failed");
        std::memcpy(mem, data, size);
        DWORD old_protect = 0;
        if (!VirtualProtect(mem, size, PAGE_EXECUTE_READ, &old_protect)) {
            VirtualFree(mem, 0, MEM_RELEASE);
            throw std::runtime_error("ExecutableBuffer: VirtualProtect failed");
        }
        FlushInstructionCache(GetCurrentProcess(), mem, size);
#else
        void* mem = mmap(nullptr, size, PROT_READ | PROT_WRITE,
                         MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (mem == MAP_FAILED) throw std::runtime_error("ExecutableBuffer: mmap failed");
        std::memcpy(mem, data, size);
        if (mprotect(mem, size, PROT_READ | PROT_EXEC) != 0) {
            munmap(mem, size);
            throw std::runtime_error("ExecutableBuffer: mprotect(PROT_EXEC) failed");
        }
        __builtin___clear_cache(static_cast<char*>(mem), static_cast<char*>(mem) + size);
#endif
        mem_ = static_cast<uint8_t*>(mem);
        size_ = size;
    }

    uint8_t* data() const { return mem_; }
    size_t size() const { return size_; }

    template <typename Fn>
    Fn entry(size_t offset) const {
        if (!mem_ || offset >= size_) throw std::runtime_error("ExecutableBuffer: bad entry offset");
        return reinterpret_cast<Fn>(mem_ + offset);
    }

private:
    void release() {
        if (!mem_) return;
#if defined(_WIN32)
        VirtualFree(mem_, 0, MEM_RELEASE);
#else
        munmap(mem_, size_);
#endif
        mem_ = nullptr;
        size_ = 0;
    }

    uint8_t* mem_ = nullptr;
    size_t size_ = 0;
};

} // namespace lithon::jit
