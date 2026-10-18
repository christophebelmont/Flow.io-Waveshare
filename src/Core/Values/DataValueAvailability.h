#pragma once
#include "Core/Generated/DataValueCatalog_Generated.h"
#include "ValueRegistry.h"

/** Availability is independent of exposure preferences and takes precedence over them. */
inline bool readAvailableDataValue(const ValueRegistry& values, ValueId id,
                                   ValueSnapshot& value, ValueMetadata& metadata) {
    const auto* descriptor = dataValueDescriptor(id);
    return descriptor && values.read(id, value, &metadata) &&
        dataValueCompatible(*descriptor, metadata) && value.quality == ValueQuality::Valid;
}

/** Catalog membership and the active history policy, independent of the current sample.
 *  History pages use it so stored records stay reachable while a measure is unavailable. */
inline bool dataValueHistorized(const ValueRegistry& values, ValueId id) {
    return dataValueDescriptor(id) != nullptr && values.history(id);
}
