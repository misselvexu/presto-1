/*
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
 */
#include <boost/algorithm/string.hpp>
#include <boost/algorithm/string/replace.hpp>
#include <boost/filesystem.hpp>
#include <gtest/gtest.h>

#include "presto_cpp/main/common/tests/test_json.h"
#include "presto_cpp/main/operators/LocalPersistentShuffle.h"
#include "presto_cpp/main/operators/PartitionAndSerialize.h"
#include "presto_cpp/main/operators/ShuffleWrite.h"
#include "presto_cpp/main/types/PrestoToVeloxQueryPlan.h"
#include "presto_cpp/presto_protocol/Connectors.h"
#include "presto_cpp/presto_protocol/presto_protocol.h"
#include "velox/exec/tests/utils/TempDirectoryPath.h"

namespace fs = boost::filesystem;

using namespace facebook::presto;
using namespace facebook::velox;

namespace {
std::string getDataPath(const std::string& fileName) {
  std::string currentPath = fs::current_path().c_str();

  if (boost::algorithm::ends_with(currentPath, "fbcode")) {
    return currentPath + "/presto_cpp/main/types/tests/data/" + fileName;
  }

  if (boost::algorithm::ends_with(currentPath, "fbsource")) {
    return currentPath + "/third-party/presto_cpp/main/types/tests/data/" +
        fileName;
  }

  // CLion runs the tests from cmake-build-release/ or cmake-build-debug/
  // directory. Hard-coded json files are not copied there and test fails with
  // file not found. Fixing the path so that we can trigger these tests from
  // CLion.
  boost::algorithm::replace_all(currentPath, "cmake-build-release/", "");
  boost::algorithm::replace_all(currentPath, "cmake-build-debug/", "");

  return currentPath + "/data/" + fileName;
}

std::shared_ptr<const core::PlanNode> assertToVeloxQueryPlan(
    const std::string& fileName) {
  std::string fragment = slurp(getDataPath(fileName));

  protocol::PlanFragment prestoPlan = json::parse(fragment);
  auto pool = memory::getDefaultMemoryPool();

  VeloxQueryPlanConverter converter(pool.get());
  return converter
      .toVeloxQueryPlan(
          prestoPlan, nullptr, "20201107_130540_00011_wrpkw.1.2.3")
      .planNode;
}

std::shared_ptr<const core::PlanNode> assertToBatchVeloxQueryPlan(
    const std::string& fileName,
    const std::string& shuffleName,
    std::shared_ptr<std::string>&& serializedShuffleWriteInfo) {
  const std::string fragment = slurp(getDataPath(fileName));

  protocol::PlanFragment prestoPlan = json::parse(fragment);
  auto pool = memory::getDefaultMemoryPool();

  VeloxQueryPlanConverter converter(pool.get());
  return converter
      .toBatchVeloxQueryPlan(
          prestoPlan,
          nullptr,
          "20201107_130540_00011_wrpkw.1.2.3",
          shuffleName,
          std::move(serializedShuffleWriteInfo))
      .planNode;
}
} // namespace

class PlanConverterTest : public ::testing::Test {};

// Leaf stage plan for select regionkey, sum(1) from nation group by 1
// Scan + Partial Agg + Repartitioning
TEST_F(PlanConverterTest, scanAgg) {
  protocol::registerConnector("hive", "hive");
  assertToVeloxQueryPlan("ScanAgg.json");

  protocol::registerConnector("hive-plus", "hive");
  assertToVeloxQueryPlan("ScanAggCustomConnectorId.json");
}

// Final Agg stage plan for select regionkey, sum(1) from nation group by 1
TEST_F(PlanConverterTest, finalAgg) {
  assertToVeloxQueryPlan("FinalAgg.json");
}

// Last stage (output) plan for select regionkey, sum(1) from nation group by 1
TEST_F(PlanConverterTest, output) {
  assertToVeloxQueryPlan("Output.json");
}

// Last stage plan for SELECT * FROM nation ORDER BY nationkey OFFSET 7 LIMIT 5.
TEST_F(PlanConverterTest, offsetLimit) {
  auto plan = assertToVeloxQueryPlan("OffsetLimit.json");

  // Look for Limit(offset = 7, count = 5) node
  bool foundLimit = false;
  auto node = plan;
  while (node) {
    node = node->sources()[0];
    if (auto limit = std::dynamic_pointer_cast<const core::LimitNode>(node)) {
      ASSERT_EQ(7, limit->offset());
      ASSERT_EQ(5, limit->count());
      foundLimit = true;
      break;
    }
  }

  ASSERT_TRUE(foundLimit);
}

TEST_F(PlanConverterTest, scanAggBatch) {
  protocol::unregisterConnector("hive");
  protocol::registerConnector("hive", "hive");
  filesystems::registerLocalFileSystem();
  auto root = assertToBatchVeloxQueryPlan(
      "ScanAggBatch.json",
      std::string(operators::LocalPersistentShuffle::kShuffleName),
      std::make_shared<std::string>(fmt::format(
          "{{\n"
          "  \"rootPath\": \"{}\",\n"
          "  \"numPartitions\": {}\n"
          "}}",
          exec::test::TempDirectoryPath::create()->path,
          10)));

  auto shuffleWrite =
      std::dynamic_pointer_cast<const operators::ShuffleWriteNode>(root);
  ASSERT_NE(shuffleWrite, nullptr);
  ASSERT_EQ(shuffleWrite->sources().size(), 1);

  auto localPartition =
      std::dynamic_pointer_cast<const core::LocalPartitionNode>(
          shuffleWrite->sources().back());
  ASSERT_NE(localPartition, nullptr);
  ASSERT_EQ(localPartition->sources().size(), 1);

  auto partitionAndSerializeNode =
      std::dynamic_pointer_cast<const operators::PartitionAndSerializeNode>(
          localPartition->sources().back());
  ASSERT_NE(partitionAndSerializeNode, nullptr);
  ASSERT_EQ(partitionAndSerializeNode->numPartitions(), 3);
}
