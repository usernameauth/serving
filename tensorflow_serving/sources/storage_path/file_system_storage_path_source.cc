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

#include "tensorflow_serving/sources/storage_path/file_system_storage_path_source.h"

#include <algorithm>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#include "absl/status/status.h"
#include "absl/strings/str_cat.h"
#include "xla/tsl/platform/errors.h"
#include "xla/tsl/platform/macros.h"
#include "tensorflow/core/lib/core/errors.h"
#include "tensorflow/core/lib/io/path.h"
#include "tensorflow/core/lib/strings/numbers.h"
#include "tensorflow/core/platform/env.h"
#include "tensorflow_serving/core/servable_data.h"
#include "tensorflow_serving/core/servable_id.h"

namespace tensorflow {
namespace serving {

FileSystemStoragePathSource::~FileSystemStoragePathSource() {
  // Note: Deletion of 'fs_polling_thread_' will block until our underlying
  // thread closure stops. Hence, destruction of this object will not proceed
  // until the thread has terminated.
  fs_polling_thread_.reset();
}

namespace {

// Returns the names of servables that appear in 'old_config' but not in
// 'new_config'.
std::set<string> GetDeletedServables(
    const FileSystemStoragePathSourceConfig& old_config,
    const FileSystemStoragePathSourceConfig& new_config) {
  std::set<string> new_servables;
  for (const FileSystemStoragePathSourceConfig::ServableToMonitor& servable :
       new_config.servables()) {
    new_servables.insert(servable.servable_name());
  }

  std::set<string> deleted_servables;
  for (const FileSystemStoragePathSourceConfig::ServableToMonitor&
           old_servable : old_config.servables()) {
    if (new_servables.find(old_servable.servable_name()) ==
        new_servables.end()) {
      deleted_servables.insert(old_servable.servable_name());
    }
  }
  return deleted_servables;
}

// Adds a new ServableData for the servable version to the vector of versions to
// aspire.
void AspireVersion(
    const FileSystemStoragePathSourceConfig::ServableToMonitor& servable,
    const string& version_relative_path, const int64_t version_number,
    std::vector<ServableData<StoragePath>>* versions) {
  const ServableId servable_id = {servable.servable_name(), version_number};
  const string full_path =
      io::JoinPath(servable.base_path(), version_relative_path);
  versions->emplace_back(ServableData<StoragePath>(servable_id, full_path));
}

// Converts the string version path to an integer.
// Returns false if the input is invalid.
bool ParseVersionNumber(const string& version_path, int64_t* version_number) {
  return absl::SimpleAtoi(version_path.c_str(), version_number);
}

// Update the servable data to include all the servable versions found in the
// base path as aspired versions.
// The argument 'children' represents a list of base-path children from the file
// system.
// Returns true if one or more valid servable version paths are found, otherwise
// returns false.
bool AspireAllVersions(
    const FileSystemStoragePathSourceConfig::ServableToMonitor& servable,
    const std::vector<string>& children,
    std::vector<ServableData<StoragePath>>* versions) {
  bool at_least_one_version_found = false;
  for (const string& child : children) {
    // Identify all the versions, among children that can be interpreted as
    // version numbers.
    int64_t version_number;
    if (ParseVersionNumber(child, &version_number)) {
      // Emit all the aspired-versions data.
      AspireVersion(servable, child, version_number, versions);
      at_least_one_version_found = true;
    }
  }

  return at_least_one_version_found;
}

// Helper that indexes a list of the given "children" (where child is the
// name of the directory corresponding to a servable version). Note that strings
// that cannot be parsed as a number are skipped (no error is returned).
std::map<int64_t /* servable version */, string /* child */>
IndexChildrenByVersion(const std::vector<string>& children) {
  std::map<int64_t, string> children_by_version;
  for (int i = 0; i < children.size(); ++i) {
    int64_t version_number;
    if (!ParseVersionNumber(children[i], &version_number)) {
      continue;
    }

    if (children_by_version.count(version_number) > 0) {
      LOG(WARNING) << "Duplicate version directories detected. Version "
                   << version_number << " will be loaded from " << children[i]
                   << ", " << children_by_version[version_number]
                   << " will be ignored.";
    }
    children_by_version[version_number] = children[i];
  }
  return children_by_version;
}

// Aspire versions for a servable configured with the "latest" version policy.
//
// 'children' represents a list of base-path children from the file system.
//
// Returns true iff it winds up aspiring at least one version.
bool AspireLatestVersions(
    const FileSystemStoragePathSourceConfig::ServableToMonitor& servable,
    const std::map<int64_t, string>& children_by_version,
    std::vector<ServableData<StoragePath>>* versions) {
  const int32 num_servable_versions_to_serve =
      std::max(servable.servable_version_policy().latest().num_versions(), 1U);
  // Identify 'num_servable_versions_to_serve' latest version(s) among children
  // that can be interpreted as version numbers and emit as aspired versions.
  int num_versions_emitted = 0;
  for (auto rit = children_by_version.rbegin();
       rit != children_by_version.rend(); ++rit) {
    if (num_versions_emitted == num_servable_versions_to_serve) {
      break;
    }
    const int64_t version = rit->first;
    const string& child = rit->second;
    AspireVersion(servable, child, version, versions);
    num_versions_emitted++;
  }

  return !children_by_version.empty();
}

// Like `AspireSpecificVersions` but use `FileExists` instead of GetChildren to
// remove unnecessary directory listings. Note that this function has to
// fallback to the general case when there are directories that *parse as* the
// version number via `strtod` but aren't equivalent (e.g., "base_dir/00001"
// rather than "base_dir/1").
//
// Returns true if all the models are loaded.
bool AspireSpecificVersionsFastPath(
    const FileSystemStoragePathSourceConfig::ServableToMonitor& servable,
    std::vector<ServableData<StoragePath>>* versions) {
  if (servable.servable_version_policy().specific().versions().empty()) {
    // There aren't any requested versions, WARN loudly and explicitly, since
    // this is a likely configuration error. Return *true*, since we are done
    // with processing this servable.
    LOG(WARNING) << "No specific versions requested for servable "
                 << servable.servable_name() << ".";
    return true;
  }

  // First ensure that we find *all* the requested versions, so that we can use
  // this fast path. If not, we'll call the general AspireSpecificVersions after
  // a GetChildren call.
  for (const int64_t version :
       servable.servable_version_policy().specific().versions()) {
    const string version_dir = absl::StrCat(version);
    const string child_dir = io::JoinPath(servable.base_path(), version_dir);

    const absl::Status status = Env::Default()->FileExists(child_dir);
    if (!status.ok()) {
      return false;
    }
  }

  // We've found them all. Aspire them one by one.
  for (const int64_t version :
       servable.servable_version_policy().specific().versions()) {
    const string version_dir = absl::StrCat(version);
    AspireVersion(servable, version_dir, version, versions);
  }

  return true;
}

// Aspire versions for a servable configured with the "specific" version policy.
//
// 'children' represents a list of base-path children from the file system.
//
// Returns true iff it winds up aspiring at least one version.
bool AspireSpecificVersions(
    const FileSystemStoragePathSourceConfig::ServableToMonitor& servable,
    const std::map<int64_t, string>& children_by_version,
    std::vector<ServableData<StoragePath>>* versions) {
  const std::unordered_set<int64_t> versions_to_serve(
      servable.servable_version_policy().specific().versions().begin(),
      servable.servable_version_policy().specific().versions().end());
  // Identify specific version to serve (as specified by 'versions_to_serve')
  // among children that can be interpreted as version numbers and emit as
  // aspired versions.
  std::unordered_set<int64_t> aspired_versions;
  for (auto it = children_by_version.begin(); it != children_by_version.end();
       ++it) {
    const int64_t version = it->first;
    if (versions_to_serve.count(version) == 0) {
      continue;  // Current version is not specified by policy for serving.
    }
    const string& child = it->second;
    AspireVersion(servable, child, version, versions);
    aspired_versions.insert(version);
  }
  for (const int64_t version : versions_to_serve) {
    if (aspired_versions.count(version) == 0) {
      LOG(WARNING)
          << "Version " << version << " of servable "
          << servable.servable_name() << ", which was requested to be served "
          << "as a 'specific' version in the servable's version policy, was "
          << "not found in the file system";
    }
  }

  return !aspired_versions.empty();
}

// Like PollFileSystemForConfig(), but for a single servable.
absl::Status PollFileSystemForServable(
    const FileSystemStoragePathSourceConfig::ServableToMonitor& servable,
    std::vector<ServableData<StoragePath>>* versions) {
  // First, determine whether the base path exists. This check guarantees that
  // we don't emit an empty aspired-versions list for a non-existent (or
  // transiently unavailable) base-path. (On some platforms, GetChildren()
  // returns an empty list instead of erring if the base path isn't found.)
  absl::Status status = Env::Default()->FileExists(servable.base_path());
  if (!status.ok()) {
    return errors::InvalidArgument(
        "Could not find base path ", servable.base_path(), " for servable ",
        servable.servable_name(), " with error ", status.ToString());
  }

  if (servable.servable_version_policy().policy_choice_case() ==
      FileSystemStoragePathSourceConfig::ServableVersionPolicy::kSpecific) {
    // Special case the specific handler, to avoid GetChildren in the case where
    // all of the directories match their version number.
    if (AspireSpecificVersionsFastPath(servable, versions)) {
      // We found them all, exit early.
      return absl::OkStatus();
    }
  }

  // Retrieve a list of base-path children from the file system.
  std::vector<string> children;
  TF_RETURN_IF_ERROR(
      Env::Default()->GetChildren(servable.base_path(), &children));

  // GetChildren() returns all descendants instead for cloud storage like GCS.
  // In such case we should filter out all non-direct descendants.
  std::set<string> real_children;
  for (int i = 0; i < children.size(); ++i) {
    const string& child = children[i];
    real_children.insert(child.substr(0, child.find_first_of('/')));
  }
  children.clear();
  children.insert(children.begin(), real_children.begin(), real_children.end());
  const std::map<int64_t /* version */, string /* child */>
      children_by_version = IndexChildrenByVersion(children);

  bool at_least_one_version_found = false;
  switch (servable.servable_version_policy().policy_choice_case()) {
    case FileSystemStoragePathSourceConfig::ServableVersionPolicy::
        POLICY_CHOICE_NOT_SET:
      TF_FALLTHROUGH_INTENDED;  // Default policy is kLatest.
    case FileSystemStoragePathSourceConfig::ServableVersionPolicy::kLatest:
      at_least_one_version_found =
          AspireLatestVersions(servable, children_by_version, versions);
      break;
    case FileSystemStoragePathSourceConfig::ServableVersionPolicy::kAll:
      at_least_one_version_found =
          AspireAllVersions(servable, children, versions);
      break;
    case FileSystemStoragePathSourceConfig::ServableVersionPolicy::kSpecific:
      at_least_one_version_found =
          AspireSpecificVersions(servable, children_by_version, versions);
      break;
    default:
      return errors::Internal("Unhandled servable version_policy: ",
                              servable.servable_version_policy().DebugString());
  }

  if (!at_least_one_version_found) {
    LOG(WARNING) << "No versions of servable " << servable.servable_name()
                 << " found under base path " << servable.base_path()
                 << ". Did you forget to name your leaf directory as a number "
                    "(eg. '/1/')?";
  }

  return absl::Status();
}

// Polls the file system, and populates 'versions_by_servable_name' with the
// aspired-versions data FileSystemStoragePathSource should emit based on what
// was found, indexed by servable name.
absl::Status PollFileSystemForConfig(
    const FileSystemStoragePathSourceConfig& config,
    std::map<string, std::vector<ServableData<StoragePath>>>*
        versions_by_servable_name) {
  for (const FileSystemStoragePathSourceConfig::ServableToMonitor& servable :
       config.servables()) {
    std::vector<ServableData<StoragePath>> versions;
    TF_RETURN_IF_ERROR(PollFileSystemForServable(servable, &versions));
    versions_by_servable_name->insert(
        {servable.servable_name(), std::move(versions)});
  }
  return absl::Status();
}

// Determines if, for any servables in 'config', the file system doesn't
// currently contain at least one version under its base path.
absl::Status FailIfZeroVersions(
    const FileSystemStoragePathSourceConfig& config) {
  std::map<string, std::vector<ServableData<StoragePath>>>
      versions_by_servable_name;
  TF_RETURN_IF_ERROR(
      PollFileSystemForConfig(config, &versions_by_servable_name));

  std::map<string, string> servable_name_to_base_path_map;
  for (const FileSystemStoragePathSourceConfig::ServableToMonitor& servable :
       config.servables()) {
    servable_name_to_base_path_map.insert(
        {servable.servable_name(), servable.base_path()});
  }

  for (const auto& entry : versions_by_servable_name) {
    const string& servable = entry.first;
    const std::vector<ServableData<StoragePath>>& versions = entry.second;
    if (versions.empty()) {
      return errors::NotFound(
          "Unable to find a numerical version path for servable ", servable,
          " at: ", servable_name_to_base_path_map[servable]);
    }
  }
  return absl::Status();
}

}  // namespace

absl::Status FileSystemStoragePathSource::Create(
    const FileSystemStoragePathSourceConfig& config,
    std::unique_ptr<FileSystemStoragePathSource>* result) {
  result->reset(new FileSystemStoragePathSource());
  return (*result)->UpdateConfig(config);
}

absl::Status FileSystemStoragePathSource::UpdateConfig(
    const FileSystemStoragePathSourceConfig& config) {
  mutex_lock l(mu_);

  if (fs_polling_thread_ != nullptr &&
      config.file_system_poll_wait_seconds() !=
          config_.file_system_poll_wait_seconds()) {
    return errors::InvalidArgument(
        "Changing file_system_poll_wait_seconds is not supported");
  }

  if (config.fail_if_zero_versions_at_startup() ||  // NOLINT
      config.servable_versions_always_present()) {
    TF_RETURN_IF_ERROR(FailIfZeroVersions(config));
  }

  if (aspired_versions_callback_) {
    TF_RETURN_IF_ERROR(UnaspireServables(GetDeletedServables(config_, config)));
  }
  config_ = config;

  return absl::Status();
}

void FileSystemStoragePathSource::SetAspiredVersionsCallback(
    AspiredVersionsCallback callback) {
  mutex_lock l(mu_);

  if (fs_polling_thread_ != nullptr) {
    LOG(ERROR) << "SetAspiredVersionsCallback() called multiple times; "
                  "ignoring this call";
    DCHECK(false);
    return;
  }
  aspired_versions_callback_ = callback;

  const auto thread_fn = [this](void) {
    absl::Status status = this->PollFileSystemAndInvokeCallback();
    if (!status.ok()) {
      LOG(ERROR) << "FileSystemStoragePathSource encountered a "
                    "filesystem access error: "
                 << status.message();
    }
  };

  if (config_.file_system_poll_wait_seconds() == 0) {
    // Start a thread to poll filesystem once and call the callback.
    fs_polling_thread_.reset(new FileSystemStoragePathSource::ThreadType(
        absl::in_place_type_t<std::unique_ptr<Thread>>(),
        Env::Default()->StartThread(
            ThreadOptions(),
            "FileSystemStoragePathSource_filesystem_oneshot_thread",
            thread_fn)));
  } else if (config_.file_system_poll_wait_seconds() > 0) {
    // Start a thread to poll the filesystem periodically and call the callback.
    PeriodicFunction::Options pf_options;
    pf_options.thread_name_prefix =
        "FileSystemStoragePathSource_filesystem_polling_thread";
    fs_polling_thread_.reset(new FileSystemStoragePathSource::ThreadType(
        absl::in_place_type_t<PeriodicFunction>(), thread_fn,
        config_.file_system_poll_wait_seconds() * 1000000, pf_options));
  }
}

absl::Status FileSystemStoragePathSource::PollFileSystemAndInvokeCallback() {
  mutex_lock l(mu_);
  std::map<string, std::vector<ServableData<StoragePath>>>
      versions_by_servable_name;
  TF_RETURN_IF_ERROR(
      PollFileSystemForConfig(config_, &versions_by_servable_name));
  for (const auto& entry : versions_by_servable_name) {
    const string& servable = entry.first;
    const std::vector<ServableData<StoragePath>>& versions = entry.second;
    if (versions.empty() && config_.servable_versions_always_present()) {
      LOG(ERROR) << "Refusing to unload all versions for Servable: "
                 << servable;
      continue;
    }
    for (const ServableData<StoragePath>& version : versions) {
      if (version.status().ok()) {
        VLOG(1) << "File-system polling update: Servable:" << version.id()
                << "; Servable path: " << version.DataOrDie()
                << "; Polling frequency: "
                << config_.file_system_poll_wait_seconds();
      }
    }
    CallAspiredVersionsCallback(servable, versions);
  }
  return absl::Status();
}

absl::Status FileSystemStoragePathSource::UnaspireServables(
    const std::set<string>& servable_names) {
  for (const string& servable_name : servable_names) {
    CallAspiredVersionsCallback(servable_name,
                                std::vector<ServableData<StoragePath>>{});
  }
  return absl::Status();
}

}  // namespace serving
}  // namespace tensorflow
