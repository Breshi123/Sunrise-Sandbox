#include "entity_name_build.h"
#include "localized_aliases.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <span>
#include <string_view>
#include <vector>

#include "../../../core/logging/log.h"
#include "../../../middleware/content/packages/tables/definition_index_table.h"
#include "../../../state/build_data/runtime.h"

namespace sunrise::client::content::entity_names {
namespace {

namespace package_reader = middleware::content::packages::reader;
namespace package_tables = middleware::content::packages::tables;
namespace names_state = state::build_data::entity_names;

constexpr std::uint32_t kNamedBagClass = 0x80809478U;
constexpr std::uint32_t kBudgetHeaderClass = 0x808099D1U;
constexpr std::uint32_t kBudgetBodyClass = 0x80809F10U;
constexpr std::uint32_t kEntityClass = 0x80809C0FU;
constexpr std::size_t kNamedBagArrayOffset = 0x08;
constexpr std::size_t kBudgetBodyArrayOffset = 0x20;
constexpr std::size_t kRowSize = 0x10;
constexpr std::size_t kRowTagOffset = 0x08;
constexpr std::size_t kMaximumRows = 1U << 20U;

using Name = names_state::Name;

struct Collection {
    std::vector<Name> names{};
    std::vector<std::uint32_t> related{};
    std::vector<std::uint32_t> validEntities{};
    localized_aliases::Result aliases{};
    std::size_t packages{};
    std::size_t bags{};
};

template <typename Value>
[[nodiscard]] bool read_value(std::span<const std::byte> blob,
                              std::size_t offset,
                              Value& output) noexcept {
    if (offset > blob.size() || sizeof(Value) > blob.size() - offset) {
        return false;
    }
    std::memcpy(&output, blob.data() + offset, sizeof output);
    return true;
}

[[nodiscard]] bool read_name(std::span<const std::byte> blob,
                             std::size_t pointerOffset,
                             Name& output) noexcept {
    std::int64_t relative = 0;
    if (!read_value(blob, pointerOffset, relative) || relative == 0) {
        return false;
    }
    const std::int64_t startSigned = static_cast<std::int64_t>(pointerOffset) + relative;
    if (startSigned < 0 || static_cast<std::uint64_t>(startSigned) >= blob.size()) {
        return false;
    }

    std::array<char, names_state::kNameLength> path{};
    std::size_t length = 0;
    bool terminated = false;
    for (std::size_t cursor = static_cast<std::size_t>(startSigned);
         cursor < blob.size() && length + 1 < path.size();
         ++cursor) {
        const char value = static_cast<char>(blob[cursor]);
        if (value == '\0') {
            terminated = true;
            break;
        }
        path[length++] = value;
    }
    if (!terminated || length == 0) {
        return false;
    }

    std::size_t begin = 0;
    for (std::size_t index = 0; index < length; ++index) {
        if (path[index] == '\\' || path[index] == '/') {
            begin = index + 1;
        }
    }
    std::size_t end = length;
    for (std::size_t index = begin; index < length; ++index) {
        if (path[index] == '.') {
            end = index;
            break;
        }
    }
    if (begin >= end || end - begin >= output.text.size()) {
        return false;
    }
    output.length = static_cast<std::uint8_t>(end - begin);
    std::memcpy(output.text.data(), path.data() + begin, output.length);
    return true;
}

void append_rows(std::span<const std::byte> blob,
                 std::size_t descriptorOffset,
                 Collection& output) noexcept {
    std::int32_t count = 0;
    std::int64_t relative = 0;
    if (!read_value(blob, descriptorOffset, count)
        || !read_value(blob, descriptorOffset + 0x08, relative) || count <= 0
        || static_cast<std::size_t>(count) > kMaximumRows) {
        return;
    }
    const std::int64_t startSigned = static_cast<std::int64_t>(descriptorOffset + 0x18)
                                   + relative;
    if (startSigned < 0) {
        return;
    }
    const std::size_t start = static_cast<std::size_t>(startSigned);
    const std::size_t rows = static_cast<std::size_t>(count);
    if (start > blob.size() || rows > (blob.size() - start) / kRowSize) {
        return;
    }
    for (std::size_t index = 0; index < rows; ++index) {
        const std::size_t row = start + index * kRowSize;
        std::uint32_t tag = 0;
        if (!read_value(blob, row + kRowTagOffset, tag)
            || package_tables::package_of(tag) == package_tables::kAbsentPackageId) {
            continue;
        }
        output.related.push_back(tag);
        Name name{};
        name.tag = tag;
        if (read_name(blob, row, name)) {
            output.names.push_back(name);
        }
    }
}

void sort_unique(std::vector<std::uint32_t>& values) {
    std::sort(values.begin(), values.end());
    values.erase(std::unique(values.begin(), values.end()), values.end());
}

[[nodiscard]] bool collect_tag(void* context, std::uint32_t tag) noexcept {
    static_cast<std::vector<std::uint32_t>*>(context)->push_back(tag);
    return true;
}

[[nodiscard]] bool extract(const package_reader::Source& source,
                           package_reader::Scratch& scratch,
                           Collection& output) noexcept {
    std::vector<std::uint32_t> bags{};
    package_reader::ScanResult scan{};
    if (!package_reader::scan_class(source.directory, kNamedBagClass, &collect_tag, &bags, scan)) {
        output.packages = scan.packages;
        return false;
    }
    output.packages = scan.packages;
    output.bags = bags.size();

    std::vector<std::byte> blob{};
    std::uint32_t classId = 0;
    for (const std::uint32_t tag : bags) {
        if (package_reader::read_tag(source, scratch, tag, blob, classId)
            && classId == kNamedBagClass) {
            append_rows(blob, kNamedBagArrayOffset, output);
        }
    }
    sort_unique(output.related);

    std::vector<std::uint32_t> budgetHeaders{};
    for (const std::uint32_t tag : output.related) {
        if (!package_reader::read_tag(source, scratch, tag, blob, classId)) {
            continue;
        }
        if (classId == kEntityClass) {
            output.validEntities.push_back(tag);
        } else if (classId == kBudgetHeaderClass) {
            budgetHeaders.push_back(tag);
        }
    }

    std::vector<std::uint32_t> budgetBodies{};
    for (const std::uint32_t tag : budgetHeaders) {
        if (!package_reader::read_tag(source, scratch, tag, blob, classId)
            || classId != kBudgetHeaderClass) {
            continue;
        }
        std::uint32_t bodyTag = 0;
        if (read_value(blob, 0, bodyTag)
            && package_tables::package_of(bodyTag) != package_tables::kAbsentPackageId) {
            budgetBodies.push_back(bodyTag);
        }
    }
    sort_unique(budgetBodies);
    for (const std::uint32_t tag : budgetBodies) {
        if (package_reader::read_tag(source, scratch, tag, blob, classId)
            && classId == kBudgetBodyClass) {
            append_rows(blob, kBudgetBodyArrayOffset, output);
        }
    }
    sort_unique(output.related);
    sort_unique(output.validEntities);
    const std::vector<std::uint32_t> directEntities = output.validEntities;
    for (const std::uint32_t tag : output.related) {
        if (!std::binary_search(directEntities.begin(), directEntities.end(), tag)
            && package_reader::read_tag(source, scratch, tag, blob, classId)
            && classId == kEntityClass) {
            output.validEntities.push_back(tag);
        }
    }
    sort_unique(output.validEntities);

    output.names.erase(std::remove_if(output.names.begin(),
                                      output.names.end(),
                                      [&output](const Name& name) {
                                          return !std::binary_search(output.validEntities.begin(),
                                                                     output.validEntities.end(),
                                                                     name.tag);
                                      }),
                       output.names.end());
    (void)localized_aliases::append(source, scratch, output.names, output.aliases);
    std::sort(output.names.begin(), output.names.end(), [](const Name& left, const Name& right) {
        if (left.tag != right.tag) {
            return left.tag < right.tag;
        }
        return std::string_view(left.text.data(), left.length)
             < std::string_view(right.text.data(), right.length);
    });
    output.names.erase(std::unique(output.names.begin(),
                                   output.names.end(),
                                   [](const Name& left, const Name& right) {
                                       return left.tag == right.tag && left.length == right.length
                                           && std::memcmp(left.text.data(),
                                                          right.text.data(),
                                                          left.length) == 0;
                                   }),
                       output.names.end());
    return output.names.size() <= names_state::kNameCapacity;
}

void report(const Collection& collection, const char* result) noexcept {
    std::array<char, 384> line{};
    const int length = std::snprintf(line.data(),
                                     line.size(),
                                     "ev=build_data stage=entity_names packages=%zu bags=%zu "
                                     "wrappers=%zu placements=%zu aliases=%zu names=%zu result=%s",
                                     collection.packages,
                                     collection.bags,
                                     collection.aliases.wrappers,
                                     collection.aliases.placements,
                                     collection.aliases.resolved,
                                     collection.names.size(),
                                     result);
    if (length > 0) {
        core::log::write(core::log::Channel::client,
                         result[0] == 'o' ? core::log::Level::info : core::log::Level::warn,
                         {line.data(), static_cast<std::size_t>(length)});
    }
}

} // namespace

bool build(const package_reader::Source& source, package_reader::Scratch& scratch) noexcept {
    if (state::build_data::entity_names_ready()) {
        return true;
    }
    Collection collection{};
    if (!extract(source, scratch, collection)) {
        report(collection, "extract");
        return false;
    }
    const bool published = state::build_data::publish_entity_names(collection.names);
    report(collection, published ? "ok" : "publish");
    return published;
}

} // namespace sunrise::client::content::entity_names
