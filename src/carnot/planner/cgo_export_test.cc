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

#include "src/carnot/planner/cgo_export.h"

#include <google/protobuf/text_format.h>
#include <google/protobuf/util/message_differencer.h>
#include <string>
#include <vector>

#include <absl/strings/str_join.h>
#include "src/carnot/planner/compiler/test_utils.h"
#include "src/carnot/planner/compilerpb/compiler_status.pb.h"
#include "src/carnot/planner/distributedpb/distributed_plan.pb.h"
#include "src/carnot/planner/plannerpb/func_args.pb.h"
#include "src/carnot/planner/test_utils.h"
#include "src/carnot/planpb/plan.pb.h"
#include "src/carnot/udf_exporter/udf_exporter.h"
#include "src/common/base/error.h"
#include "src/common/base/macros.h"
#include "src/common/base/statusor.h"
#include "src/common/testing/protobuf.h"
#include "src/common/testing/testing.h"

namespace px {
namespace carnot {
namespace planner {

using px::testing::proto::EqualsProto;
using px::testing::proto::Partially;

class PlannerExportTest : public ::testing::Test {
 protected:
  void SetUp() override {
    // TODO(philkuz/zasgar) need to import the udf_info str here once we have the genrule.
    // or figure out a different way to handle this.
    udfexporter::ExportUDFInfo().ConsumeValueOrDie()->info_pb().SerializeToString(&udf_info_str_);
  }

  plannerpb::QueryRequest MakeQueryRequest(const std::string& query) {
    plannerpb::QueryRequest query_request;
    query_request.set_query_str(query);
    return query_request;
  }

  plannerpb::CompileMutationsRequest MakeCompileMutationsRequest(const std::string& query) {
    plannerpb::CompileMutationsRequest mutations_request;
    mutations_request.set_query_str(query);
    return mutations_request;
  }

  void TearDown() override { PlannerFree(planner_); }
  PlannerPtr MakePlanner() { return PlannerNew(udf_info_str_.c_str(), udf_info_str_.length()); }
  PlannerPtr planner_;
  std::string udf_info_str_;
};

TEST_F(PlannerExportTest, one_pem_one_kelvin_query_test) {
  planner_ = MakePlanner();
  int result_len;
  std::string logical_planner_state;
  ASSERT_TRUE(
      testutils::CreateOnePEMOneKelvinPlannerState().SerializeToString(&logical_planner_state));
  std::string query = "import px\npx.display(px.DataFrame('table1'), 'out')";
  std::string query_request;
  ASSERT_TRUE(MakeQueryRequest(query).SerializeToString(&query_request));

  auto interface_result =
      PlannerPlan(planner_, logical_planner_state.c_str(), logical_planner_state.length(),
                  query_request.c_str(), query_request.length(), &result_len);
  ASSERT_GT(result_len, 0);

  distributedpb::LogicalPlannerResult planner_result;
  ASSERT_TRUE(
      planner_result.ParseFromString(std::string(interface_result, interface_result + result_len)));
  delete[] interface_result;
  ASSERT_OK(planner_result.status());
  EXPECT_EQ(planner_result.plan().qb_address_to_plan_size(), 2);
}

TEST_F(PlannerExportTest, bad_queries) {
  planner_ = MakePlanner();
  int result_len;
  std::string logical_planner_state;
  ASSERT_TRUE(testutils::CreateTwoPEMsPlannerState().SerializeToString(&logical_planner_state));
  // Bad table name query that should yield a compiler error.
  std::string bad_table_query =
      "import px\n"
      "df = px.DataFrame(table='bad_table_name')\n"
      "px.display(df, 'out')";
  std::string query_request;
  ASSERT_TRUE(MakeQueryRequest(bad_table_query).SerializeToString(&query_request));
  auto interface_result =
      PlannerPlan(planner_, logical_planner_state.c_str(), logical_planner_state.length(),
                  query_request.c_str(), query_request.length(), &result_len);
  // The compiler should successfully compile and a proto should be returned.
  ASSERT_GT(result_len, 0);
  distributedpb::LogicalPlannerResult planner_result_pb;
  ASSERT_TRUE(planner_result_pb.ParseFromString(
      std::string(interface_result, interface_result + result_len)));
  delete[] interface_result;
  EXPECT_NOT_OK(planner_result_pb.status());
  EXPECT_THAT(planner_result_pb.status(), HasCompilerError("Table 'bad_table_name' not found."));
}

constexpr char kUDFQuery[] = R"query(
import px
t1 = px.DataFrame(table='table1', start_time='-30s')
t1 = t1[t1['cpu_cycles'] >= 0]
px.display(t1)
)query";

// Previously had an issue where the UDF registry's memory was improperly handled, and this query
// would cause a segfault. If this unit test passes, then that bug should be gone.
TEST_F(PlannerExportTest, udf_in_query) {
  planner_ = MakePlanner();
  int result_len;
  std::string logical_planner_state;
  ASSERT_TRUE(
      testutils::CreateTwoPEMsOneKelvinPlannerState().SerializeToString(&logical_planner_state));
  std::string query_request;
  ASSERT_TRUE(MakeQueryRequest(kUDFQuery).SerializeToString(&query_request));
  auto interface_result =
      PlannerPlan(planner_, logical_planner_state.c_str(), logical_planner_state.length(),
                  query_request.c_str(), query_request.length(), &result_len);
  // The compiler should successfully compile and a proto should be returned.
  ASSERT_GT(result_len, 0);
  distributedpb::LogicalPlannerResult planner_result_pb;
  ASSERT_TRUE(planner_result_pb.ParseFromString(
      std::string(interface_result, interface_result + result_len)));
  delete[] interface_result;
  EXPECT_OK(planner_result_pb.status());
}

TEST_F(PlannerExportTest, pass_query_string_instead_of_req_should_fail) {
  planner_ = MakePlanner();
  std::string logical_planner_state;
  ASSERT_TRUE(
      testutils::CreateTwoPEMsOneKelvinPlannerState().SerializeToString(&logical_planner_state));
  int result_len;
  // Pass in kUDFQuery instead of query_request object here.
  auto interface_result =
      PlannerPlan(planner_, logical_planner_state.c_str(), logical_planner_state.length(),
                  kUDFQuery, sizeof(kUDFQuery), &result_len);
  // The compiler should successfully compile and a proto should be returned.
  ASSERT_GT(result_len, 0);
  distributedpb::LogicalPlannerResult planner_result_pb;
  ASSERT_TRUE(planner_result_pb.ParseFromString(
      std::string(interface_result, interface_result + result_len)));
  delete[] interface_result;
  ASSERT_NOT_OK(planner_result_pb.status());
  EXPECT_THAT(planner_result_pb.status().msg(),
              ::testing::ContainsRegex("Failed to process the query request.*"));
}

constexpr char kPxTraceQuery[] = R"pxl(
import pxtrace
import px

@pxtrace.probe("MyFunc")
def probe_func():
    id = pxtrace.ArgExpr('id')
    return [{'id': id},
            {'err': pxtrace.RetExpr('$0.a')},
            {'latency': pxtrace.FunctionLatency()}]

pxtrace.UpsertTracepoint('http_return',
                         "http_return_table",
                         probe_func,
                         px.uint128("123e4567-e89b-12d3-a456-426655440000"),
                         "5m")
)pxl";

constexpr char kExpectedTracePb[] = R"proto(
name: "http_return"
ttl {
  seconds: 300
}
deployment_spec {
  upid {
    asid: 306070887 pid: 3902477011 ts_ns: 11841725277501915136
  }
}
programs {
  table_name: "http_return_table"
  spec {
    outputs {
      name: "http_return_table"
      fields: "id"
      fields: "err"
      fields: "latency"
    }
    probe {
      name: "http_return"
      tracepoint {
        symbol: "MyFunc"
      }
      args {
        id: "arg0"
        expr: "id"
      }
      ret_vals {
        id: "ret0"
        expr: "$0.a"
      }
      function_latency {
        id: "lat0"
      }
      output_actions {
        output_name: "http_return_table"
        variable_names: "arg0"
        variable_names: "ret0"
        variable_names: "lat0"
      }
    }
  }
}
)proto";

TEST_F(PlannerExportTest, compile_probe_def) {
  planner_ = MakePlanner();
  int result_len;
  std::string logical_planner_state;
  ASSERT_TRUE(
      testutils::CreateTwoPEMsOneKelvinPlannerState().SerializeToString(&logical_planner_state));
  std::string mutation_request;
  ASSERT_TRUE(MakeCompileMutationsRequest(kPxTraceQuery).SerializeToString(&mutation_request));
  auto interface_result = PlannerCompileMutations(
      planner_, logical_planner_state.c_str(), logical_planner_state.length(),
      mutation_request.c_str(), mutation_request.length(), &result_len);

  ASSERT_GT(result_len, 0);
  plannerpb::CompileMutationsResponse mutations_response_pb;
  ASSERT_TRUE(mutations_response_pb.ParseFromString(
      std::string(interface_result, interface_result + result_len)));
  delete[] interface_result;
  ASSERT_OK(mutations_response_pb.status());
  ASSERT_EQ(mutations_response_pb.mutations().size(), 1);
  EXPECT_THAT(mutations_response_pb.mutations()[0].trace(), EqualsProto(kExpectedTracePb));
}

constexpr char kExpectedDeleteTracepointsPxl[] = R"pxl(
import pxtrace
pxtrace.DeleteTracepoint('http_probe')
pxtrace.DeleteTracepoint('cool_func')
)pxl";
constexpr char kExpectedDeleteTracepointsMutationPb[] = R"proto(
status{}
mutations {
  delete_tracepoint {
    name: "http_probe"
  }
}
mutations {
  delete_tracepoint {
    name: "cool_func"
  }
}
)proto";

TEST_F(PlannerExportTest, compile_delete_tracepoint) {
  planner_ = MakePlanner();
  int result_len;
  std::string logical_planner_state;
  ASSERT_TRUE(
      testutils::CreateTwoPEMsOneKelvinPlannerState().SerializeToString(&logical_planner_state));
  std::string mutation_request;
  ASSERT_TRUE(MakeCompileMutationsRequest(kExpectedDeleteTracepointsPxl)
                  .SerializeToString(&mutation_request));
  auto interface_result = PlannerCompileMutations(
      planner_, logical_planner_state.c_str(), logical_planner_state.length(),
      mutation_request.c_str(), mutation_request.length(), &result_len);

  ASSERT_GT(result_len, 0);
  plannerpb::CompileMutationsResponse mutations_response_pb;
  ASSERT_TRUE(mutations_response_pb.ParseFromString(
      std::string(interface_result, interface_result + result_len)));
  delete[] interface_result;
  ASSERT_OK(mutations_response_pb.status());
  EXPECT_THAT(mutations_response_pb, EqualsProto(kExpectedDeleteTracepointsMutationPb));
}

}  // namespace planner
}  // namespace carnot
}  // namespace px
