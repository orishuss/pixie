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

#include "src/stirling/utils/detect_application.h"

#include <string>
#include <vector>

#include "src/common/testing/testing.h"
#include "src/stirling/source_connectors/socket_tracer/testing/container_images.h"
#include "src/stirling/utils/proc_path_tools.h"

namespace px {
namespace stirling {

using ::px::system::ProcParser;
using ::testing::StrEq;

// Tests that we can execute the executable of container process (with the set of
// permissions granted through our requires_bpf tag, although the exact permission might be more
// limited, perhaps only need 'root' permission to have access to the file).
//
// NOTE: Disabled to reduce flakiness. The mechanism tested here is replaced by the mount namespace
// execution. Didn't remove it because it's an interesting case that might be useful.
TEST(NodeVersionTest, DISABLED_ResultsAreAsExpected) {
  constexpr std::string_view kNode15_0ImageTar =
      "src/stirling/source_connectors/socket_tracer/testing/containers/node_15_0_image.tar";
  ContainerRunner node_server(px::testing::BazelRunfilePath(kNode15_0ImageTar), "node_server", "");
  ASSERT_OK_AND_ASSIGN(std::string output, node_server.Run(std::chrono::seconds{60}));
  pid_t node_server_pid = node_server.process_pid();

  ProcParser proc_parser(system::Config::GetInstance());
  LazyLoadedFPResolver fp_resolver;

  ASSERT_OK_AND_ASSIGN(const std::filesystem::path proc_exe_path,
                       ProcExe(node_server_pid, &proc_parser, &fp_resolver));
  ASSERT_OK_AND_THAT(px::Exec(absl::StrCat(proc_exe_path.string(), " --version")),
                     StrEq("v15.0.1\n"));
}

// Tests that the mntexec cli can execute into the alpine container.
TEST(AlpineNodeExecTest, MountNSSubprocessWorks) {
  ContainerRunner node_server(px::testing::BazelRunfilePath(
                                  "src/stirling/source_connectors/socket_tracer/testing/containers/"
                                  "node_14_18_1_alpine_image.tar"),
                              "node_server", "");
  ASSERT_OK_AND_ASSIGN(std::string output, node_server.Run(std::chrono::seconds{60}));
  pid_t node_server_pid = node_server.process_pid();

  ProcParser proc_parser(system::Config::GetInstance());
  ASSERT_OK_AND_ASSIGN(std::filesystem::path exe, proc_parser.GetExePath(node_server_pid));

  SubProcess proc(node_server_pid);
  ASSERT_OK(proc.Start({exe.string(), "--version"}));
  ASSERT_EQ(proc.Wait(/*close_pipe*/ false), 0) << "Subprocess' exit code should be 0";

  std::string node_proc_stdout;
  ASSERT_OK(proc.Stdout(&node_proc_stdout));
  EXPECT_THAT(node_proc_stdout, StrEq("v14.18.1\n"));
}

}  // namespace stirling
}  // namespace px
