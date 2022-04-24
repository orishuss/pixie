/*
 * Copyright 2018- The Pixie Authors.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "src/common/testing/test_utils/container_runner.h"

#include "src/common/exec/exec.h"
#include "src/common/exec/subprocess.h"

namespace px {

// Number of seconds to wait between each attempt.
constexpr int kSleepSeconds = 1;

ContainerRunner::ContainerRunner(std::string_view image, std::string_view instance_name_prefix,
                                 std::string_view ready_message)
    : image_(image), instance_name_prefix_(instance_name_prefix), ready_message_(ready_message) {
  std::string out = px::Exec("docker pull " + image_).ConsumeValueOrDie();
  LOG(INFO) << out;
}

ContainerRunner::ContainerRunner(std::filesystem::path image_tar,
                                 std::string_view instance_name_prefix,
                                 std::string_view ready_message)
    : instance_name_prefix_(instance_name_prefix), ready_message_(ready_message) {
  std::string docker_load_cmd = absl::Substitute("docker load -i $0", image_tar.string());
  VLOG(1) << docker_load_cmd;
  std::string out = px::Exec(docker_load_cmd).ConsumeValueOrDie();
  LOG(INFO) << out;

  // Extract the image name.
  std::vector<std::string_view> lines = absl::StrSplit(out, "\n", absl::SkipWhitespace());
  CHECK(!lines.empty());
  std::string_view image_line = lines.back();
  constexpr std::string_view kLoadedImagePrefix = "Loaded image: ";
  CHECK(absl::StartsWith(image_line, kLoadedImagePrefix));
  image_line.remove_prefix(kLoadedImagePrefix.length());
  image_ = image_line;
}

ContainerRunner::~ContainerRunner() {
  Stop();

  std::string docker_rm_cmd = absl::StrCat("docker rm -f ", container_name_);
  LOG(INFO) << docker_rm_cmd;
  StatusOr<std::string> s = px::Exec(docker_rm_cmd);
  LOG_IF(ERROR, !s.ok()) << absl::Substitute(
      "Failed to remove the container. Container $0 may have leaked. Status: $1", container_name_,
      s.ToString());
}

namespace {
StatusOr<std::string> ContainerStatus(std::string_view container_name) {
  PL_ASSIGN_OR_RETURN(
      std::string container_status,
      px::Exec(absl::Substitute("docker inspect -f '{{.State.Status}}' $0", container_name)));
  absl::StripAsciiWhitespace(&container_status);
  return container_status;
}

StatusOr<int> ContainerPID(std::string_view container_name) {
  PL_ASSIGN_OR_RETURN(
      const std::string pid_str,
      px::Exec(absl::Substitute("docker inspect -f '{{.State.Pid}}' $0", container_name)));

  int pid;
  if (!absl::SimpleAtoi(pid_str, &pid)) {
    return error::Internal("PID was not parseable.");
  }

  if (pid == 0) {
    return error::Internal("Failed to get PID.");
  }

  return pid;
}
}  // namespace

StatusOr<std::string> ContainerRunner::Run(const std::chrono::seconds& timeout,
                                           const std::vector<std::string>& options,
                                           const std::vector<std::string>& args,
                                           const bool use_host_pid_namespace) {
  // Now run the container.
  // Run with timeout, as a backup in case we don't clean things up properly.
  container_name_ = absl::StrCat(instance_name_prefix_, "_",
                                 std::chrono::steady_clock::now().time_since_epoch().count());

  // Note that we don't add --rm to the docker run command, because we sometimes want to inspect
  // the container state after termination. Instead we explicitly remove the container on
  // ContainerRunner destruction.
  std::vector<std::string> docker_run_cmd;
  docker_run_cmd.push_back("docker");
  docker_run_cmd.push_back("run");
  docker_run_cmd.push_back("--rm");
  if (use_host_pid_namespace) {
    docker_run_cmd.push_back("--pid=host");
  }
  for (const auto& flag : options) {
    docker_run_cmd.push_back(flag);
  }
  docker_run_cmd.push_back(absl::Substitute("--name=$0", container_name_));
  docker_run_cmd.push_back(image_);
  for (const auto& arg : args) {
    docker_run_cmd.push_back(arg);
  }

  LOG(INFO) << docker_run_cmd;
  PL_RETURN_IF_ERROR(docker_.Start(docker_run_cmd, /* stderr_to_stdout */ true));

  // If the process receives a SIGKILL, then the docker run command above would leak.
  // As a safety net for such cases, we spawn off a delayed docker kill command to clean-up.
  std::string docker_kill_cmd = absl::Substitute("(sleep $0 && docker rm -f $1) 2>&1 >/dev/null",
                                                 timeout.count(), container_name_);
  FILE* pipe = popen(docker_kill_cmd.c_str(), "r");
  // We deliberately don't ever call pclose() -- even in the destructor -- otherwise, we'd block.
  // This spawned process is meant to potentially outlive the current process as a safety net.
  PL_UNUSED(pipe);

  // It may take some time for the container to come up, so we keep polling.
  // But keep count of the attempts, because we don't want to poll infinitely.
  int attempts_remaining = timeout.count();

  std::string container_status;

  // Wait for container's server to be running.
  for (; attempts_remaining > 0; --attempts_remaining) {
    const int status = docker_.GetStatus();
    if (WIFEXITED(status) || WIFSIGNALED(status)) {
      container_status = "exited";
      LOG(INFO) << absl::Substitute("The container already exited or terminated by a signal");
      break;
    }
    // We check if the container process is running before running docker inspect
    // to avoid races where the container stops running after the docker inspect.
    const bool docker_is_running = docker_.IsRunning();

    if (!docker_is_running) {
      // If docker is not running, fail early to save time.
      std::string container_out;
      PL_RETURN_IF_ERROR(docker_.Stdout(&container_out));
      return error::Internal("Container $0 docker run failed. Output:\n$1", container_name_,
                             container_out);
    }

    PL_ASSIGN_OR_RETURN(container_status, ContainerStatus(container_name_));
    LOG(INFO) << absl::Substitute("Container $0 status: $1", container_name_, container_status);

    // Status should be one of: created, restarting, running, removing, paused, exited, dead.
    if (container_status == "running" || container_status == "exited" ||
        container_status == "dead") {
      break;
    }

    // Delay before trying again.
    LOG(INFO) << absl::Substitute(
        "Container $0 not yet running, will try again ($1 attempts remaining).", container_name_,
        attempts_remaining);

    sleep(kSleepSeconds);
  }

  if (container_status != "running" && container_status != "exited") {
    std::string container_out;
    PL_RETURN_IF_ERROR(docker_.Stdout(&container_out));
    return error::Internal("Container $0 failed to start. Container output:\n$1", container_name_,
                           container_out);
  }

  // Get the PID of process within the container.
  // Note that this like won't work for short-lived containers.
  process_pid_ = ContainerPID(container_name_).ValueOr(-1);

  if (process_pid_ == -1) {
    LOG(WARNING) << absl::Substitute(
        "Container $0 may have terminated before PID could be sampled.", container_name_);
  }
  LOG(INFO) << absl::Substitute("Container $0 process PID: $1", container_name_, process_pid_);

  LOG(INFO) << absl::Substitute("Container $0 waiting for log message: $1", container_name_,
                                ready_message_);

  // Wait for container to become "ready".
  std::string container_out;
  for (; attempts_remaining > 0; --attempts_remaining) {
    // Read Stdout after reading ContainerStatus to avoid races.
    // Otherwise it is possible we don't see the container become ready,
    // but we do see its status as "exited", and we think it exited without ever becoming ready.
    PL_ASSIGN_OR_RETURN(container_status, ContainerStatus(container_name_));
    PL_RETURN_IF_ERROR(docker_.Stdout(&container_out));

    LOG(INFO) << absl::Substitute("Container $0 status: $1", container_name_, container_status);

    if (absl::StrContains(container_out, ready_message_)) {
      break;
    }

    // Early exit to save time if the container has exited.
    // Any further looping won't really help us.
    if (container_status == "exited" || container_status == "dead") {
      LOG(INFO) << absl::Substitute("Container $0 has exited.", container_name_);
      break;
    }

    LOG(INFO) << absl::Substitute(
        "Container $0 not in ready state, will try again ($1 attempts remaining).", container_name_,
        attempts_remaining);

    sleep(kSleepSeconds);
  }

  if (!absl::StrContains(container_out, ready_message_)) {
    LOG(ERROR) << absl::Substitute("Container $0 did not reach ready state.", container_name_);

    // Dump some information that may be useful for debugging.
    LOG(INFO) << "\n> docker container ls -a";
    LOG(INFO) << px::Exec("docker container ls -a").ValueOr("<docker container ls failed>");
    LOG(INFO) << "\n> docker container inspect";
    LOG(INFO) << px::Exec(absl::Substitute("docker container inspect $0", container_name_))
                     .ValueOr("<docker container failed>");
    LOG(INFO) << "\n> docker logs";
    LOG(INFO) << px::Exec(absl::Substitute("docker logs $0", container_name_))
                     .ValueOr("<docker logs failed>");

    return error::Internal("Timeout. Container $0 did not reach ready state.", container_name_);
  }

  LOG(INFO) << absl::Substitute("Container $0 is ready.", container_name_);
  return container_out;
}

void ContainerRunner::Stop() {
  // Clean-up the container.
  docker_.Signal(SIGKILL);
  docker_.Wait();
}

void ContainerRunner::Wait() { docker_.Wait(); }

}  // namespace px
