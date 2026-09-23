#pragma once

// Umbrella header for the DPU Service Fabric public API.
//
// The runtime is a standalone, vendor-neutral C++20 library. Everything in this
// header is stable public surface: identities, model types, engines, the runtime
// facade, durability and the framed transport.

#include "dpu/fabric/core/archive.hpp"
#include "dpu/fabric/core/bounded.hpp"
#include "dpu/fabric/core/digest.hpp"
#include "dpu/fabric/core/json.hpp"
#include "dpu/fabric/core/reason.hpp"
#include "dpu/fabric/core/resources.hpp"
#include "dpu/fabric/core/result.hpp"
#include "dpu/fabric/core/types.hpp"
#include "dpu/fabric/engine/authority.hpp"
#include "dpu/fabric/engine/dependency.hpp"
#include "dpu/fabric/engine/eligibility.hpp"
#include "dpu/fabric/engine/planner.hpp"
#include "dpu/fabric/engine/runtime.hpp"
#include "dpu/fabric/model/capability.hpp"
#include "dpu/fabric/model/event.hpp"
#include "dpu/fabric/model/lifecycle.hpp"
#include "dpu/fabric/model/plan.hpp"
#include "dpu/fabric/model/service.hpp"
#include "dpu/fabric/model/topology.hpp"
#include "dpu/fabric/persist/store.hpp"
#include "dpu/fabric/transport/client.hpp"
#include "dpu/fabric/transport/frame.hpp"
#include "dpu/fabric/transport/server.hpp"
#include "dpu/fabric/version.hpp"
