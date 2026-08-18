#pragma once

#include <QString>
#include <QtGlobal>

#include <optional>
#include <utility>

enum class SandboxNativeErrorKind : quint8 {
    None,
    Win32,
    HResult,
};

struct SandboxNativeError final
{
    SandboxNativeErrorKind kind = SandboxNativeErrorKind::None;
    quint32 value = 0;

    [[nodiscard]] static SandboxNativeError win32(const quint32 error) noexcept
    {
        return {SandboxNativeErrorKind::Win32, error};
    }

    [[nodiscard]] static SandboxNativeError hresult(const qint32 error) noexcept
    {
        return {SandboxNativeErrorKind::HResult,
                static_cast<quint32>(error)};
    }

    [[nodiscard]] bool operator==(
        const SandboxNativeError &other) const noexcept = default;
};

template<typename T>
struct SandboxValueResult final
{
    std::optional<T> value;
    QString errorCode;
    SandboxNativeError nativeError;

    [[nodiscard]] bool has_value() const noexcept { return value.has_value(); }
    [[nodiscard]] explicit operator bool() const noexcept { return has_value(); }
    [[nodiscard]] T *operator->() noexcept { return &*value; }
    [[nodiscard]] const T *operator->() const noexcept { return &*value; }
    [[nodiscard]] T &operator*() & noexcept { return *value; }
    [[nodiscard]] const T &operator*() const & noexcept { return *value; }
    [[nodiscard]] T &&operator*() && noexcept { return std::move(*value); }
    void reset() noexcept { value.reset(); }
};
