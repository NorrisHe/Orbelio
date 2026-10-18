#pragma once
// Orbelio compatibility types used only inside the imported GPL renderer.
// Preserve error propagation without importing LichtFeld's CUDA/core runtime.
#include "export.hpp"
#include "source_site.hpp"
#include <expected>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
namespace lfs {
enum class ErrorCode { Internal, ContractViolation, InvalidArgument, NotFound, ResourceExhausted,
    Unavailable, Cancelled, DeadlineExceeded, DeviceLost, FailedPrecondition };
enum class ErrorDomain { Vulkan, IO };
enum class Severity { Error, Fatal };
enum class Retryability { NotRetryable, Retryable, RetryableWithBackoff };
struct NativeError { ErrorDomain domain{}; std::int64_t code{}; std::string name; };
struct ErrorInit {
    ErrorCode code = ErrorCode::Internal;
    ErrorDomain domain = ErrorDomain::Vulkan;
    Severity severity = Severity::Error;
    Retryability retryability = Retryability::NotRetryable;
    std::string user_message;
    std::string detail;
    core::SourceSite detection{"", 0, ""};
    std::optional<NativeError> native = {};
};
class Error {
public:
    Error() = default;
    explicit Error(ErrorInit init) : init_(std::move(init)) {}
    std::string_view detail() const { return init_.detail; }
    Error with_context(std::string_view message, core::SourceSite site) && {
        init_.detail += "\n" + std::string(message) + " (" + site.file_name() + ":" + std::to_string(site.line()) + ")";
        return std::move(*this);
    }
private:
    ErrorInit init_;
};
inline Error make_error(ErrorInit init) { return Error(std::move(init)); }
class Exception : public std::runtime_error {
public:
    explicit Exception(Error error) : std::runtime_error(std::string(error.detail())), error_(std::move(error)) {}
    const Error& error() const { return error_; }
private:
    Error error_;
};
template<class T> class Result : public std::expected<T, Error> {
    using Base = std::expected<T, Error>;
public:
    using Base::Base;
    Result(Error error) : Base(std::unexpected(std::move(error))) {}
};
}
