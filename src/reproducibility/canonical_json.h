#pragma once

#include <cstdint>
#include <initializer_list>
#include <map>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace truetest::reproducibility {

class CanonicalJsonValue final
{
public:
    using Array = std::vector<CanonicalJsonValue>;
    using Object = std::map<std::string, CanonicalJsonValue, std::less<>>;
    using Storage = std::variant<std::nullptr_t, bool, std::int64_t,
                                 std::uint64_t, double, std::string,
                                 Array, Object>;

    CanonicalJsonValue() noexcept : storage_(nullptr) {}
    CanonicalJsonValue(std::nullptr_t) noexcept : storage_(nullptr) {}
    CanonicalJsonValue(bool value) noexcept : storage_(value) {}
    CanonicalJsonValue(int value) noexcept
        : storage_(static_cast<std::int64_t>(value)) {}
    CanonicalJsonValue(unsigned value) noexcept
        : storage_(static_cast<std::uint64_t>(value)) {}
    CanonicalJsonValue(std::int64_t value) noexcept : storage_(value) {}
    CanonicalJsonValue(std::uint64_t value) noexcept : storage_(value) {}
    CanonicalJsonValue(double value) noexcept : storage_(value) {}
    CanonicalJsonValue(const char* value) : storage_(std::string(value)) {}
    CanonicalJsonValue(std::string value) : storage_(std::move(value)) {}
    CanonicalJsonValue(std::string_view value) : storage_(std::string(value)) {}
    CanonicalJsonValue(Array value) : storage_(std::move(value)) {}
    CanonicalJsonValue(Object value) : storage_(std::move(value)) {}

    [[nodiscard]] static CanonicalJsonValue array(
        std::initializer_list<CanonicalJsonValue> values);
    [[nodiscard]] static CanonicalJsonValue object(
        std::initializer_list<Object::value_type> values);

    [[nodiscard]] const Storage& storage() const noexcept { return storage_; }
    [[nodiscard]] bool is_null() const noexcept;
    [[nodiscard]] bool is_bool() const noexcept;
    [[nodiscard]] bool is_integer() const noexcept;
    [[nodiscard]] bool is_number() const noexcept;
    [[nodiscard]] bool is_string() const noexcept;
    [[nodiscard]] bool is_array() const noexcept;
    [[nodiscard]] bool is_object() const noexcept;

    [[nodiscard]] bool as_bool() const;
    [[nodiscard]] std::int64_t as_i64() const;
    [[nodiscard]] std::uint64_t as_u64() const;
    [[nodiscard]] double as_double() const;
    [[nodiscard]] const std::string& as_string() const;
    [[nodiscard]] const Array& as_array() const;
    [[nodiscard]] Array& as_array();
    [[nodiscard]] const Object& as_object() const;
    [[nodiscard]] Object& as_object();

    [[nodiscard]] bool contains(std::string_view key) const;
    [[nodiscard]] const CanonicalJsonValue& at(std::string_view key) const;
    [[nodiscard]] CanonicalJsonValue& operator[](std::string key);

    friend bool operator==(const CanonicalJsonValue&,
                           const CanonicalJsonValue&) = default;

private:
    Storage storage_;
};

// UTF-8, lexicographically ordered object keys, semantic array order, no
// insignificant whitespace or trailing newline, decimal integers, finite
// binary64 round-trip values, and -0.0 normalized to 0.
[[nodiscard]] std::string serialize_canonical_json(
    const CanonicalJsonValue& value);

// Strict parser used for manifests. Duplicate keys, invalid UTF-8, excessive
// nesting, non-JSON numbers, trailing bytes, and malformed escapes fail.
[[nodiscard]] CanonicalJsonValue parse_json_strict(std::string_view text);

} // namespace truetest::reproducibility
