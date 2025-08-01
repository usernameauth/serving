/* Copyright 2016 Google Inc. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/

#include "tensorflow_serving/core/load_servables_fast.h"

#include <algorithm>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "absl/types/optional.h"
#include "tensorflow/core/lib/core/errors.h"
#include "tensorflow/core/lib/strings/strcat.h"
#include "tensorflow_serving/core/servable_state.h"
#include "tensorflow_serving/core/source.h"
#include "tensorflow_serving/core/target.h"

namespace tensorflow {
namespace serving {

namespace internal {

uint32 GetManagerNumLoadThreads(AspiredVersionsManager* manager) {
  return manager->num_load_threads();
}

std::function<void(const uint32)> SetManagerNumLoadThreadsNotifier(
    AspiredVersionsManager* manager) {
  return manager->set_num_load_threads_observer_->Notifier();
}

absl::Status ConnectSourcesWithFastInitialLoad(
    AspiredVersionsManager* manager,
    std::vector<Source<std::unique_ptr<Loader>>*> sources,
    const std::function<absl::Status()>& wait_until_loaded_fn,
    const uint32 num_threads) {
  const uint32 prev_num_load_threads = GetManagerNumLoadThreads(manager);
  std::function<void(const uint32)> set_manager_num_load_threads =
      SetManagerNumLoadThreadsNotifier(manager);
  set_manager_num_load_threads(num_threads);
  for (Source<std::unique_ptr<Loader>>* source : sources) {
    ConnectSourceToTarget(source, manager);
  }
  const absl::Status status = wait_until_loaded_fn();
  set_manager_num_load_threads(prev_num_load_threads);
  return status;
}

}  // namespace internal

absl::Status ConnectSourceWithFastInitialLoad(
    AspiredVersionsManager* manager, Source<std::unique_ptr<Loader>>* source,
    ServableStateMonitor* servable_state_monitor,
    const std::vector<ServableRequest>& initial_servables,
    const uint32 num_threads) {
  return ConnectSourcesWithFastInitialLoad(manager, {source},
                                           servable_state_monitor,
                                           initial_servables, num_threads);
}

absl::Status ConnectSourcesWithFastInitialLoad(
    AspiredVersionsManager* manager,
    std::vector<Source<std::unique_ptr<Loader>>*> sources,
    ServableStateMonitor* servable_state_monitor,
    const std::vector<ServableRequest>& initial_servables,
    const uint32 num_threads) {
  return internal::ConnectSourcesWithFastInitialLoad(
      manager, sources,
      [&]() {
        std::map<ServableId, ServableState::ManagerState> states_reached;
        const bool all_servables_available =
            servable_state_monitor->WaitUntilServablesReachState(
                initial_servables, ServableState::ManagerState::kAvailable,
                &states_reached);
        if (!all_servables_available) {
          const int num_unavailable_servables = std::count_if(
              states_reached.begin(), states_reached.end(),
              [](const std::pair<ServableId, ServableState::ManagerState>&
                     id_and_state) {
                return id_and_state.second !=
                       ServableState::ManagerState::kAvailable;
              });
          string message =
              strings::StrCat(num_unavailable_servables,
                              " servable(s) did not become available: {");
          for (const auto& id_and_state : states_reached) {
            if (id_and_state.second !=
                ServableState::ManagerState::kAvailable) {
              absl::optional<ServableState> maybe_state =
                  servable_state_monitor->GetState(id_and_state.first);
              const string error_msg =
                  maybe_state && !maybe_state.value().health.ok()
                      ? " due to error: " +
                            maybe_state.value().health.ToString()
                      : "";
              strings::StrAppend(&message, "{",
                                 id_and_state.first.DebugString(), error_msg,
                                 "}, ");
            }
          }
          strings::StrAppend(&message, "}");
          return errors::Unknown(message);
        }
        return absl::OkStatus();
      },
      num_threads);
}

}  // namespace serving
}  // namespace tensorflow
