#include <cstddef>
#include <limits>

#include "../queuez_state_validation.h"

namespace sunrise::server::bap::encrypted::queuez {

/** Replaces an active Family-4 manifest at exactly the next peer-local version. */
bool stage_family4_refresh(const SessionState& before,
                           const middleware::queuez::Family& family,
                           SessionState& after) noexcept {
    after = before;
    if (!valid(before) || !before.family4Active || before.family4RootSoid == 0
        || before.family4Version == (std::numeric_limits<std::int32_t>::max)()
        || family.type != kAccountFamilyType || family.rootSoid != before.family4RootSoid
        || family.version != before.family4Version + 1
        || family.flags != middleware::queuez::kFullSnapshotFlag || family.objects.empty()
        || family.objects.size() > kResidentCapacity
        || family.objects.size()
               > static_cast<std::size_t>((std::numeric_limits<std::uint16_t>::max)())) {
        return false;
    }

    SessionState candidate = before;
    candidate.family4Residents = {};
    candidate.family4Version = family.version;
    candidate.family4ResidentCount = static_cast<std::uint16_t>(family.objects.size());
    for (std::size_t index = 0; index < family.objects.size(); ++index) {
        const middleware::queuez::Object& object = family.objects[index];
        if (object.id == 0 || object.version == 0) {
            return false;
        }
        for (std::size_t prior = 0; prior < index; ++prior) {
            if (family.objects[prior].version == object.version) {
                return false;
            }
        }
        candidate.family4Residents[index] = ResidentObject{object.version, object.id};
    }
    if (candidate.family4Residents.front().objectSoid != before.family4RootSoid
        || !valid(candidate)) {
        return false;
    }
    after = candidate;
    return true;
}

} // namespace sunrise::server::bap::encrypted::queuez
