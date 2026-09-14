/*
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "ns3/test.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <utility>

using namespace ns3;

namespace
{

std::pair<int, std::string>
RunMtpExample(const std::string& testFile,
              const std::string& program,
              const std::string& arguments = "")
{
    const std::filesystem::path repoRoot = PROJECT_SOURCE_PATH;
    const std::string pythonCommand =
        std::system("command -v python3.12 >/dev/null 2>&1") == 0 ? "python3.12" : "python3";

    std::string command = "cd \"" + repoRoot.string() + "\" && " + pythonCommand + " ./ns3 run " +
                          program + " --no-build --command-template=\"%s --test";
    if (!arguments.empty())
    {
        command += " " + arguments;
    }
    command += "\" > \"" + testFile + "\" 2>&1";

    const int status = std::system(command.c_str());
    std::ifstream input(testFile);
    std::stringstream buffer;
    buffer << input.rdbuf();
    return {status, buffer.str()};
}

class MtpExpectedFailureSystemTestCase : public TestCase
{
  public:
    MtpExpectedFailureSystemTestCase(const std::string& name,
                                     const std::string& program,
                                     const std::string& expectedDiagnostic,
                                     const std::string& arguments = "")
        : TestCase(name),
          m_program(program),
          m_expectedDiagnostic(expectedDiagnostic),
          m_arguments(arguments)
    {
    }

    void DoRun() override
    {
        const std::string testFile = CreateTempDirFilename(GetName() + ".log");
        const auto [status, output] = RunMtpExample(testFile, m_program, m_arguments);

        NS_TEST_ASSERT_MSG_NE(status, 0, "invalid MTP runtime input must fail fast");
        NS_TEST_ASSERT_MSG_NE(output.find(m_expectedDiagnostic),
                              std::string::npos,
                              "failure must report the violated MTP invariant: " << output);
    }

  private:
    std::string m_program;
    std::string m_expectedDiagnostic;
    std::string m_arguments;
};

class MtpManualPartitionValidSystemTestCase : public TestCase
{
  public:
    MtpManualPartitionValidSystemTestCase()
        : TestCase("MTP - manual partition schedules a node on its worker partition")
    {
    }

    void DoRun() override
    {
        const std::string testFile = CreateTempDirFilename(GetName() + ".log");
        const auto [status, output] =
            RunMtpExample(testFile, "src/mtp/examples/mtp-manual-partition-validation", "--valid");

        NS_TEST_ASSERT_MSG_EQ(status, 0, "valid manual MTP partition must run: " << output);
        NS_TEST_ASSERT_MSG_NE(output.find("lp=1"),
                              std::string::npos,
                              "node event must execute on worker partition 1: " << output);
    }
};

class MtpRemoteEventOrderingSystemTestCase : public TestCase
{
  public:
    MtpRemoteEventOrderingSystemTestCase()
        : TestCase("MTP - same-time remote events preserve sender order")
    {
    }

    void DoRun() override
    {
        const std::string testFile = CreateTempDirFilename(GetName() + ".log");
        const auto [status, output] =
            RunMtpExample(testFile, "src/mtp/examples/mtp-remote-event-ordering");

        NS_TEST_ASSERT_MSG_EQ(status, 0, "same-time remote events must run: " << output);
        NS_TEST_ASSERT_MSG_NE(output.find("order=11,12,21,22, wrong-lp=0"),
                              std::string::npos,
                              "remote events must preserve cross-sender and sender-local order on "
                              "LP 3: "
                                  << output);
    }
};

class MtpOrderedGlobalEventsSystemTestCase : public TestCase
{
  public:
    MtpOrderedGlobalEventsSystemTestCase()
        : TestCase("MTP - keyed public events have stable cross-LP ordering")
    {
    }

    void DoRun() override
    {
        const std::string testFile = CreateTempDirFilename(GetName() + ".log");
        const auto [status, output] =
            RunMtpExample(testFile, "src/mtp/examples/mtp-ordered-global-events");

        NS_TEST_ASSERT_MSG_EQ(status, 0, "keyed public events must run: " << output);
        NS_TEST_ASSERT_MSG_NE(
            output.find("TEST : 0 : PASSED concurrent=100 staggered=100 wrong-lp=0 "
                        "public-followup=1"),
            std::string::npos,
            "concurrent and staggered batches plus public follow-ups must execute on the public "
            "LP in stable key order: "
                << output);
    }
};

class MtpSameTimeAbsoluteEventSystemTestCase : public TestCase
{
  public:
    MtpSameTimeAbsoluteEventSystemTestCase()
        : TestCase("MTP - absolute-time delivery accepts the current LP time")
    {
    }

    void DoRun() override
    {
        const std::string testFile = CreateTempDirFilename(GetName() + ".log");
        const auto [status, output] =
            RunMtpExample(testFile, "src/mtp/examples/mtp-past-event-validation", "--same-time");

        NS_TEST_ASSERT_MSG_EQ(status, 0, "current-time absolute event must run: " << output);
        NS_TEST_ASSERT_MSG_NE(output.find("event-time-ns=10"),
                              std::string::npos,
                              "absolute event must execute at 10ns: " << output);
    }
};

class MtpManualPartitionValidationSystemTestSuite : public TestSuite
{
  public:
    MtpManualPartitionValidationSystemTestSuite()
        : TestSuite("mtp-manual-partition-validation", Type::SYSTEM)
    {
        AddTestCase(new MtpManualPartitionValidSystemTestCase(), TestCase::Duration::QUICK);
        AddTestCase(new MtpExpectedFailureSystemTestCase(
                        "MTP - manual partition rejects an unmapped node",
                        "src/mtp/examples/mtp-manual-partition-validation",
                        "MTP worker partition ownership is missing for node"),
                    TestCase::Duration::QUICK);
    }
};

MtpManualPartitionValidationSystemTestSuite g_manualPartitionValidation;

class MtpRemoteEventOrderingSystemTestSuite : public TestSuite
{
  public:
    MtpRemoteEventOrderingSystemTestSuite()
        : TestSuite("mtp-remote-event-ordering", Type::SYSTEM)
    {
        AddTestCase(new MtpRemoteEventOrderingSystemTestCase(), TestCase::Duration::QUICK);
    }
};

MtpRemoteEventOrderingSystemTestSuite g_remoteEventOrdering;

class MtpOrderedGlobalEventsSystemTestSuite : public TestSuite
{
  public:
    MtpOrderedGlobalEventsSystemTestSuite()
        : TestSuite("mtp-ordered-global-events", Type::SYSTEM)
    {
        AddTestCase(new MtpOrderedGlobalEventsSystemTestCase(), TestCase::Duration::QUICK);
    }
};

MtpOrderedGlobalEventsSystemTestSuite g_orderedGlobalEvents;

class MtpZeroDelayPartitionSystemTestCase : public TestCase
{
  public:
    MtpZeroDelayPartitionSystemTestCase()
        : TestCase("MTP - zero-delay neighbors stay in one worker partition")
    {
    }

    void DoRun() override
    {
        const std::string testFile = CreateTempDirFilename(GetName() + ".log");
        const auto [status, output] =
            RunMtpExample(testFile, "src/mtp/examples/mtp-zero-delay-partition");

        NS_TEST_ASSERT_MSG_EQ(status, 0, "zero-delay partition case must run: " << output);
        NS_TEST_ASSERT_MSG_NE(
            output.find("TEST : 0 : PASSED"),
            std::string::npos,
            "zero-delay neighbors must share a non-public worker partition: " << output);
    }
};

class MtpZeroDelayPartitionSystemTestSuite : public TestSuite
{
  public:
    MtpZeroDelayPartitionSystemTestSuite()
        : TestSuite("mtp-zero-delay-partition", Type::SYSTEM)
    {
        AddTestCase(new MtpZeroDelayPartitionSystemTestCase(), TestCase::Duration::QUICK);
    }
};

MtpZeroDelayPartitionSystemTestSuite g_zeroDelayPartition;

class MtpPastEventValidationSystemTestSuite : public TestSuite
{
  public:
    MtpPastEventValidationSystemTestSuite()
        : TestSuite("mtp-past-event-validation", Type::SYSTEM)
    {
        AddTestCase(new MtpSameTimeAbsoluteEventSystemTestCase(), TestCase::Duration::QUICK);
        AddTestCase(new MtpExpectedFailureSystemTestCase(
                        "MTP - absolute-time delivery rejects a past event",
                        "src/mtp/examples/mtp-past-event-validation",
                        "MTP cannot schedule an event in the past"),
                    TestCase::Duration::QUICK);
        AddTestCase(new MtpExpectedFailureSystemTestCase(
                        "MTP - absolute-time delivery rejects a negative event time",
                        "src/mtp/examples/mtp-past-event-validation",
                        "targetTs=-1",
                        "--negative-time"),
                    TestCase::Duration::QUICK);
        AddTestCase(new MtpExpectedFailureSystemTestCase(
                        "MTP - remote mailbox rejects an event from the past",
                        "src/mtp/examples/mtp-past-event-validation",
                        "MTP received a remote event from the past",
                        "--remote-past"),
                    TestCase::Duration::QUICK);
        AddTestCase(new MtpExpectedFailureSystemTestCase(
                        "MTP - local relative scheduling rejects a negative delay",
                        "src/mtp/examples/mtp-past-event-validation",
                        "MTP cannot schedule an event with a negative delay",
                        "--negative-local-delay"),
                    TestCase::Duration::QUICK);
        AddTestCase(new MtpExpectedFailureSystemTestCase(
                        "MTP - remote relative scheduling rejects a negative delay",
                        "src/mtp/examples/mtp-past-event-validation",
                        "MTP cannot schedule an event with a negative delay",
                        "--negative-remote-delay"),
                    TestCase::Duration::QUICK);
    }
};

MtpPastEventValidationSystemTestSuite g_pastEventValidation;

} // namespace
