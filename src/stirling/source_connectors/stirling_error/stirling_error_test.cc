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

#include <absl/functional/bind_front.h>
#include <absl/strings/substitute.h>

#include "src/carnot/planner/probes/tracepoint_generator.h"
#include "src/common/base/base.h"
#include "src/common/testing/test_utils/container_runner.h"
#include "src/common/testing/testing.h"
#include "src/shared/tracepoint_translation/translation.h"
#include "src/stirling/bpf_tools/bcc_wrapper.h"
#include "src/stirling/core/output.h"
#include "src/stirling/core/types.h"
#include "src/stirling/source_connectors/perf_profiler/perf_profile_connector.h"
#include "src/stirling/source_connectors/perf_profiler/testing/testing.h"
#include "src/stirling/source_connectors/seq_gen/seq_gen_connector.h"
#include "src/stirling/source_connectors/stirling_error/probe_status_table.h"
#include "src/stirling/source_connectors/stirling_error/stirling_error_connector.h"
#include "src/stirling/source_connectors/stirling_error/stirling_error_table.h"
#include "src/stirling/stirling.h"
#include "src/stirling/testing/common.h"

DECLARE_bool(stirling_profiler_java_symbols);
DECLARE_string(stirling_profiler_java_agent_libs);
DECLARE_string(stirling_profiler_px_jattach_path);
DECLARE_uint32(stirling_profiler_table_update_period_seconds);
DECLARE_uint32(stirling_profiler_stack_trace_sample_period_ms);

namespace px {
namespace stirling {

using ::testing::Contains;
using ::testing::ElementsAre;
using ::testing::Eq;
using ::testing::Field;
using ::testing::IsEmpty;
using ::testing::SizeIs;
using ::testing::StrEq;
using ::testing::UnorderedElementsAre;

using ::px::stirling::profiler::testing::GetAgentLibsFlagValueForTesting;
using ::px::stirling::profiler::testing::GetPxJattachFlagValueForTesting;
using ::px::testing::BazelBinTestFilePath;
using ::px::types::ColumnWrapperRecordBatch;
using ::px::types::Int64Value;
using ::px::types::StringValue;
using ::px::types::Time64NSValue;
using ::px::types::UInt128Value;

using DynamicTracepointDeployment =
    ::px::stirling::dynamic_tracing::ir::logical::TracepointDeployment;
using ::px::stirling::testing::RecordBatchSizeIs;

std::vector<SourceStatusRecord> ToSourceRecordVector(
    const std::vector<std::unique_ptr<ColumnWrapperRecordBatch>>& record_batches) {
  std::vector<SourceStatusRecord> result;

  for (size_t rb_idx = 0; rb_idx < record_batches.size(); ++rb_idx) {
    auto& rb = *record_batches[rb_idx];
    for (size_t idx = 0; idx < rb.front()->Size(); ++idx) {
      SourceStatusRecord r;
      r.source_connector = rb[2]->Get<StringValue>(idx).string();
      r.status = static_cast<px::statuspb::Code>(rb[3]->Get<Int64Value>(idx).val);
      r.error = rb[4]->Get<StringValue>(idx).string();
      r.context = rb[5]->Get<StringValue>(idx).string();
      result.push_back(r);
    }
  }
  return result;
}

auto EqSourceStatusRecord(const SourceStatusRecord& x) {
  return AllOf(Field(&SourceStatusRecord::source_connector, StrEq(x.source_connector)),
               Field(&SourceStatusRecord::status, Eq(x.status)),
               Field(&SourceStatusRecord::error, StrEq(x.error)),
               Field(&SourceStatusRecord::context, StrEq(x.context)));
}

std::vector<ProbeStatusRecord> ToProbeRecordVector(
    const std::vector<std::unique_ptr<ColumnWrapperRecordBatch>>& record_batches) {
  std::vector<ProbeStatusRecord> result;

  for (size_t rb_idx = 0; rb_idx < record_batches.size(); ++rb_idx) {
    auto& rb = *record_batches[rb_idx];
    for (size_t idx = 0; idx < rb.front()->Size(); ++idx) {
      ProbeStatusRecord r;
      r.source_connector = rb[2]->Get<StringValue>(idx).string();
      r.tracepoint = rb[3]->Get<StringValue>(idx).string();
      r.status = static_cast<px::statuspb::Code>(rb[4]->Get<Int64Value>(idx).val);
      r.error = rb[5]->Get<StringValue>(idx).string();
      r.info = rb[6]->Get<StringValue>(idx).string();
      result.push_back(r);
    }
  }
  return result;
}

auto EqProbeStatusRecord(const ProbeStatusRecord& x) {
  return AllOf(Field(&ProbeStatusRecord::source_connector, StrEq(x.source_connector)),
               Field(&ProbeStatusRecord::tracepoint, StrEq(x.tracepoint)),
               Field(&ProbeStatusRecord::status, Eq(x.status)),
               Field(&ProbeStatusRecord::error, StrEq(x.error)),
               Field(&ProbeStatusRecord::info, StrEq(x.info)));
}

// A SourceConnector that fails on Init.
class FaultyConnector : public SourceConnector {
 public:
  static constexpr std::string_view kName = "faulty connector";
  static constexpr auto kSamplingPeriod = std::chrono::milliseconds{500};
  static constexpr auto kPushPeriod = std::chrono::milliseconds{1000};

  // clang-format off
  static constexpr DataElement kElements[] = {
    {"time_",
      "Timestamp when the data record was collected.",
      types::DataType::TIME64NS,
      types::SemanticType::ST_NONE,
      types::PatternType::METRIC_COUNTER},
  };
  // clang-format on
  static constexpr auto kTable0 = DataTableSchema("table0", "A test table.", kElements);

  static constexpr auto kTables = MakeArray(kTable0);

  static std::unique_ptr<SourceConnector> Create(std::string_view name) {
    return std::unique_ptr<SourceConnector>(new FaultyConnector(name));
  }

 protected:
  explicit FaultyConnector(std::string_view name) : SourceConnector(name, kTables) {}
  ~FaultyConnector() override = default;

  Status InitImpl() override {
    sampling_freq_mgr_.set_period(kSamplingPeriod);
    push_freq_mgr_.set_period(kPushPeriod);
    return error::Internal("Initialization failed on purpose.");
  };

  void TransferDataImpl(ConnectorContext* /* ctx */,
                        const std::vector<DataTable*>& /* data_tables */) override{};

  Status StopImpl() override { return Status::OK(); }
};

class StirlingErrorTest : public ::testing::Test {
 protected:
  inline static const uint32_t kNumSources = 3;
  inline static const std::string tcpdrop_bpftrace_script_ =
      "src/stirling/testing/tcpdrop.bpftrace.pxl";
  inline static const std::string pidsample_bpftrace_script_ =
      "src/stirling/testing/pidsample.bpftrace.pxl";

  template <typename TSourceType>
  std::unique_ptr<SourceRegistry> CreateSources() {
    std::unique_ptr<SourceRegistry> registry = std::make_unique<SourceRegistry>();
    for (uint32_t i = 0; i < kNumSources; ++i) {
      registry->RegisterOrDie<TSourceType>(absl::Substitute("sequences$0", i));
    }
    registry->RegisterOrDie<StirlingErrorConnector>("stirling_error");
    return registry;
  }

  void InitStirling(std::unique_ptr<SourceRegistry> registry) {
    stirling_ = Stirling::Create(std::move(registry));
    stirling_->RegisterDataPushCallback(absl::bind_front(&StirlingErrorTest::AppendData, this));

    stirlingpb::Publish publication;
    stirling_->GetPublishProto(&publication);
    IndexPublication(publication, &table_info_map_);
  }

  StatusOr<sole::uuid> DeployBPFTraceScript(const std::string& bpftrace_script) {
    // Get BPFTrace program.
    auto trace_program = std::make_unique<DynamicTracepointDeployment>();
    PL_ASSIGN_OR_RETURN(std::string program_text,
                        px::ReadFileToString(px::testing::TestFilePath(bpftrace_script)));

    // Compile tracepoint.
    PL_ASSIGN_OR_RETURN(auto compiled_tracepoint,
                        px::carnot::planner::compiler::CompileTracepoint(program_text));
    ::px::tracepoint::ConvertPlannerTracepointToStirlingTracepoint(compiled_tracepoint,
                                                                   trace_program.get());

    // Register tracepoint.
    sole::uuid trace_id = sole::uuid4();
    stirling_->RegisterTracepoint(trace_id, std::move(trace_program));

    // Wait for deployment to finish.
    StatusOr<stirlingpb::Publish> s;
    do {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
      s = stirling_->GetTracepointInfo(trace_id);
    } while (!s.ok() && s.code() == px::statuspb::Code::RESOURCE_UNAVAILABLE);

    return trace_id;
  }

  Status AppendData(uint64_t table_id, types::TabletID tablet_id,
                    std::unique_ptr<ColumnWrapperRecordBatch> record_batch) {
    PL_UNUSED(tablet_id);

    // Append stirling error table records to record_batches.
    auto iter = table_info_map_.find(table_id);
    if (iter != table_info_map_.end()) {
      const stirlingpb::InfoClass& table_info = iter->second;
      std::string table_name = table_info.schema().name();
      if (table_name == "stirling_error") {
        source_status_batches_.push_back(std::move(record_batch));
      } else if (table_name == "probe_status") {
        probe_status_batches_.push_back(std::move(record_batch));
      }
    }
    return Status::OK();
  }

  absl::flat_hash_map<uint64_t, stirlingpb::InfoClass> table_info_map_;
  std::unique_ptr<Stirling> stirling_;
  std::vector<std::unique_ptr<ColumnWrapperRecordBatch>> source_status_batches_;
  std::vector<std::unique_ptr<ColumnWrapperRecordBatch>> probe_status_batches_;
};

TEST_F(StirlingErrorTest, SourceConnectorInitOK) {
  auto registry = CreateSources<SeqGenConnector>();
  InitStirling(std::move(registry));

  ASSERT_OK(stirling_->RunAsThread());
  ASSERT_OK(stirling_->WaitUntilRunning(std::chrono::seconds(5)));
  sleep(5);
  stirling_->Stop();

  std::vector<SourceStatusRecord> records = ToSourceRecordVector(source_status_batches_);
  // Stirling Error Source Connector plus the other ones.
  EXPECT_THAT(records, SizeIs(kNumSources + 1));

  SourceStatusRecord r1{.source_connector = "stirling_error",
                        .status = px::statuspb::Code::OK,
                        .error = "",
                        .context = "Init"};

  SourceStatusRecord r2{.source_connector = "sequences0",
                        .status = px::statuspb::Code::OK,
                        .error = "",
                        .context = "Init"};

  SourceStatusRecord r3{.source_connector = "sequences1",
                        .status = px::statuspb::Code::OK,
                        .error = "",
                        .context = "Init"};

  SourceStatusRecord r4{.source_connector = "sequences2",
                        .status = px::statuspb::Code::OK,
                        .error = "",
                        .context = "Init"};

  EXPECT_THAT(records, UnorderedElementsAre(EqSourceStatusRecord(r1), EqSourceStatusRecord(r2),
                                            EqSourceStatusRecord(r3), EqSourceStatusRecord(r4)));
}

TEST_F(StirlingErrorTest, SourceConnectorInitError) {
  auto registry = CreateSources<FaultyConnector>();
  InitStirling(std::move(registry));

  ASSERT_OK(stirling_->RunAsThread());
  ASSERT_OK(stirling_->WaitUntilRunning(std::chrono::seconds(5)));
  sleep(5);
  stirling_->Stop();

  std::vector<SourceStatusRecord> records = ToSourceRecordVector(source_status_batches_);
  // Stirling Error Source Connector plus the other ones.
  EXPECT_THAT(records, SizeIs(kNumSources + 1));

  SourceStatusRecord r1{.source_connector = "stirling_error",
                        .status = px::statuspb::Code::OK,
                        .error = "",
                        .context = "Init"};

  SourceStatusRecord r2{.source_connector = "sequences0",
                        .status = px::statuspb::Code::INTERNAL,
                        .error = "Initialization failed on purpose.",
                        .context = "Init"};

  SourceStatusRecord r3{.source_connector = "sequences1",
                        .status = px::statuspb::Code::INTERNAL,
                        .error = "Initialization failed on purpose.",
                        .context = "Init"};

  SourceStatusRecord r4{.source_connector = "sequences2",
                        .status = px::statuspb::Code::INTERNAL,
                        .error = "Initialization failed on purpose.",
                        .context = "Init"};

  EXPECT_THAT(records, UnorderedElementsAre(EqSourceStatusRecord(r1), EqSourceStatusRecord(r2),
                                            EqSourceStatusRecord(r3), EqSourceStatusRecord(r4)));
}

// Deploy a dynamic BPFTrace probe and record the error messages of its deployment and removal.
// Expects one message each for deployment in progress, deployment status, and removal in progress.
TEST_F(StirlingErrorTest, BPFTraceDeploymentOK) {
  // Register StirlingErrorConnector.
  std::unique_ptr<SourceRegistry> registry = std::make_unique<SourceRegistry>();
  registry->RegisterOrDie<StirlingErrorConnector>("stirling_error");

  // Run Stirling.
  InitStirling(std::move(registry));
  ASSERT_OK(stirling_->RunAsThread());
  ASSERT_OK(stirling_->WaitUntilRunning(std::chrono::seconds(5)));

  ASSERT_OK_AND_ASSIGN(auto trace_id, DeployBPFTraceScript(tcpdrop_bpftrace_script_));
  sleep(3);

  // Remove tracepoint;
  ASSERT_OK(stirling_->RemoveTracepoint(trace_id));
  StatusOr<stirlingpb::Publish> s;
  do {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    s = stirling_->GetTracepointInfo(trace_id);
  } while (s.ok());
  sleep(3);

  stirling_->Stop();
  auto source_records = ToSourceRecordVector(source_status_batches_);
  auto probe_records = ToProbeRecordVector(probe_status_batches_);

  // Stirling Error Source Connector Initialization.
  SourceStatusRecord r1{.source_connector = "stirling_error",
                        .status = px::statuspb::Code::OK,
                        .error = "",
                        .context = "Init"};

  // TCPDrop deployed.
  ProbeStatusRecord r2{.source_connector = "dynamic_bpftrace",
                       .tracepoint = "tcp_drop_tracer",
                       .status = px::statuspb::Code::OK,
                       .error = "",
                       .info = absl::Substitute(
                           R"({"trace_id":"$0","output_table":"tcp_drop_table"})", trace_id.str())};
  // TCPDrop removal in progress.
  ProbeStatusRecord r3{.source_connector = "dynamic_bpftrace",
                       .tracepoint = "tcp_drop_tracer",
                       .status = px::statuspb::Code::RESOURCE_UNAVAILABLE,
                       .error = "Probe removal in progress.",
                       .info = absl::Substitute(R"({"trace_id":"$0"})", trace_id.str())};

  EXPECT_THAT(source_records, ElementsAre(EqSourceStatusRecord(r1)));
  EXPECT_THAT(probe_records, ElementsAre(EqProbeStatusRecord(r2), EqProbeStatusRecord(r3)));
}

TEST_F(StirlingErrorTest, BPFTraceDeploymentError) {
  // Register StirlingErrorConnector.
  std::unique_ptr<SourceRegistry> registry = std::make_unique<SourceRegistry>();
  registry->RegisterOrDie<StirlingErrorConnector>("stirling_error");

  // Run Stirling.
  InitStirling(std::move(registry));
  ASSERT_OK(stirling_->RunAsThread());
  ASSERT_OK(stirling_->WaitUntilRunning(std::chrono::seconds(5)));

  ASSERT_OK_AND_ASSIGN(auto trace_id, DeployBPFTraceScript(pidsample_bpftrace_script_));
  sleep(3);

  stirling_->Stop();
  auto source_records = ToSourceRecordVector(source_status_batches_);
  auto probe_records = ToProbeRecordVector(probe_status_batches_);

  // Stirling Error Source Connector Initialization.
  SourceStatusRecord r1{.source_connector = "stirling_error",
                        .status = px::statuspb::Code::OK,
                        .error = "",
                        .context = "Init"};
  // PidSample deployment failed.
  ProbeStatusRecord r2{.source_connector = "dynamic_bpftrace",
                       .tracepoint = "pid_sample_tracer",
                       .status = px::statuspb::Code::INTERNAL,
                       .error =
                           "Could not compile bpftrace script, Semantic pass failed: stdin:3-4: "
                           "ERROR: printf: Too many arguments "
                           "for format string (4 supplied, 3 expected)\n",
                       .info = absl::Substitute(R"({"trace_id":"$0"})", trace_id.str())};
  EXPECT_THAT(source_records, ElementsAre(EqSourceStatusRecord(r1)));
  EXPECT_THAT(probe_records, ElementsAre(EqProbeStatusRecord(r2)));
}

// TODO(rcheng/oazizi): Fix this test to work with latest clang/gcc.
TEST_F(StirlingErrorTest, DISABLED_UProbeDeploymentError) {
  // Register StirlingErrorConnector.
  std::unique_ptr<SourceRegistry> registry = std::make_unique<SourceRegistry>();
  registry->RegisterOrDie<StirlingErrorConnector>("stirling_error");

  // Run Stirling.
  InitStirling(std::move(registry));
  ASSERT_OK(stirling_->RunAsThread());
  ASSERT_OK(stirling_->WaitUntilRunning(std::chrono::seconds(5)));

  bpf_tools::BCCWrapper bcc_wrapper;
  bpf_tools::UProbeSpec spec{
      .binary_path = "/usr/lib/x86_64-linux-gnu/libssl.so.1.1",
      .symbol = "SSL_write",
      .attach_type = bpf_tools::BPFProbeAttachType::kEntry,
      .probe_fn = "probe_entry_SSL_write",
  };

  // Attempt to attach UProbe and append probe status.
  auto s = bcc_wrapper.AttachUProbe(spec);
  if (!s.ok()) {
    StirlingMonitor& monitor = *StirlingMonitor::GetInstance();
    monitor.AppendProbeStatusRecord("socket_tracer", spec.probe_fn, s, spec.ToJSON());
  }
  // Sleep so that transfer_data has time to push the records into table.
  sleep(3);

  auto source_records = ToSourceRecordVector(source_status_batches_);
  auto probe_records = ToProbeRecordVector(probe_status_batches_);

  // Stirling Error Source Connector Initialization.
  SourceStatusRecord r1{.source_connector = "stirling_error",
                        .status = px::statuspb::Code::OK,
                        .error = "",
                        .context = "Init"};
  // SSL_write Uprobe deployment failed.
  ProbeStatusRecord r2{
      .source_connector = "socket_tracer",
      .tracepoint = "probe_entry_SSL_write",
      .status = px::statuspb::Code::INTERNAL,
      .error = "Can't find start of function probe_entry_SSL_write",
      .info =
          R"({"binary":"/usr/lib/x86_64-linux-gnu/libssl.so.1.1","symbol":"SSL_write","address":0,"pid":-1,"type":"kEntry","probe_fn":"probe_entry_SSL_write"})"};

  EXPECT_THAT(source_records, ElementsAre(EqSourceStatusRecord(r1)));
  EXPECT_THAT(probe_records, ElementsAre(EqProbeStatusRecord(r2)));
}

namespace {
std::filesystem::path BazelJavaTestAppPath(const std::string_view app_name) {
  const std::filesystem::path kToyAppsPath =
      "src/stirling/source_connectors/perf_profiler/testing/java";
  const std::filesystem::path app_path = kToyAppsPath / app_name;
  const std::filesystem::path bazel_app_path = BazelBinTestFilePath(app_path);
  return bazel_app_path;
}
}  // namespace

TEST_F(StirlingErrorTest, PerfProfilerNoPreserveFramePointer) {
  PL_SET_FOR_SCOPE(FLAGS_stirling_profiler_java_agent_libs, GetAgentLibsFlagValueForTesting());
  PL_SET_FOR_SCOPE(FLAGS_stirling_profiler_px_jattach_path, GetPxJattachFlagValueForTesting());
  PL_SET_FOR_SCOPE(FLAGS_stirling_profiler_java_symbols, true);
  PL_SET_FOR_SCOPE(FLAGS_stirling_profiler_table_update_period_seconds, 5);
  PL_SET_FOR_SCOPE(FLAGS_stirling_profiler_stack_trace_sample_period_ms, 7);

  // Register StirlingErrorConnector.
  std::unique_ptr<SourceRegistry> registry = std::make_unique<SourceRegistry>();
  registry->RegisterOrDie<StirlingErrorConnector>("stirling_error");
  registry->RegisterOrDie<PerfProfileConnector>("perf_profiler");

  // Run Stirling.
  InitStirling(std::move(registry));
  ASSERT_OK(stirling_->RunAsThread());
  ASSERT_OK(stirling_->WaitUntilRunning(std::chrono::seconds(5)));

  // Run a Java container without frame pointers.
  const std::string image_name = "java_image_base-java-profiler-test-image-omit-frame-pointer";
  const std::filesystem::path image_tar_path = BazelJavaTestAppPath(image_name + ".tar");
  ASSERT_TRUE(fs::Exists(image_tar_path)) << absl::StrFormat("Missing: %s.", image_tar_path);
  ContainerRunner java_container{image_tar_path, "java", ""};
  StatusOr<std::string> result = java_container.Run(std::chrono::seconds{90});
  PL_CHECK_OK(result);

  // Wait for the java profiler to attempt symbolization.
  sleep(10);

  auto source_records = ToSourceRecordVector(source_status_batches_);
  auto probe_records = ToProbeRecordVector(probe_status_batches_);

  // Stirling Error Source Connector Initialization.
  SourceStatusRecord r1{.source_connector = "stirling_error",
                        .status = px::statuspb::Code::OK,
                        .error = "",
                        .context = "Init"};
  SourceStatusRecord r2{.source_connector = "perf_profiler",
                        .status = px::statuspb::Code::OK,
                        .error = "",
                        .context = "Init"};
  // Missing frame pointer from perf profiler.
  SourceStatusRecord r3{
      .source_connector = "perf_profiler",
      .status = px::statuspb::Code::INTERNAL,
      .error = absl::Substitute(
          "Frame pointer not available in pid: $0, cmd: \"/usr/bin/java -cp "
          "/app/px/src/stirling/source_connectors/perf_profiler/testing/java/"
          "java_image_base-java-profiler-test-image-omit-frame-pointer.binary.jar:/app/px/src/"
          "stirling/source_connectors/perf_profiler/testing/java/"
          "java_image_base-java-profiler-test-image-omit-frame-pointer.binary JavaFib\". Preserve "
          "frame pointers with the JDK option: -XX:+PreserveFramePointer.",
          java_container.process_pid()),
      .context = "Java Symbolization"};
  EXPECT_THAT(source_records, Contains(EqSourceStatusRecord(r1)));
  EXPECT_THAT(source_records, Contains(EqSourceStatusRecord(r2)));
  EXPECT_THAT(source_records, Contains(EqSourceStatusRecord(r3)));
  EXPECT_THAT(probe_records, IsEmpty());
}

}  // namespace stirling
}  // namespace px
