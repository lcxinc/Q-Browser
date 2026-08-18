#pragma once

#include "EffectivePolicy.h"
#include "Manifest.h"

class PolicyEngine final
{
public:
    [[nodiscard]] static EffectivePolicy intersect(const ManifestPermissions &manifest,
                                                   const HostPolicy &host);
};
