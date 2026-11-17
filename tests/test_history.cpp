#include <gtest/gtest.h>
#include "history/HistoryService.h"
#include "platform/HistoryDirectoryLock.h"
#include "history/HistoryLibrary.h"
#include "history/DocumentCodec.h"
#include "third_party/nlohmann/json.hpp"
#include <fstream>
#include <cstring>
#include <thread>
#include <algorithm>
#include <cstdlib>
#if defined(_WIN32)
#include "platform/PlatformWin32.h"
#include "utils/ScopeExit.h"
#else
#include <sys/wait.h>
#include <unistd.h>
#endif

using namespace shotlet;
namespace fs = std::filesystem;
namespace {
class HistoryTest : public testing::Test {
protected:
    void SetUp() override { root = fs::temp_directory_path() / ("shotlet-history-test-" + NewHistoryId()); }
    void TearDown() override { std::error_code ec; fs::remove_all(root, ec); }
    CaptureDocument Document(int64_t time = 100) {
        CaptureDocument d; d.width = 8; d.height = 6; d.logicalSize = {8, 6}; d.createdAtUnixMs = time;
        d.originalRgba.resize(8 * 6 * 4);
        for (size_t i = 0; i < d.originalRgba.size(); ++i) d.originalRgba[i] = uint8_t(i);
        return d;
    }
    HistoryImage Result(const CaptureDocument& d) {
        auto p = std::shared_ptr<unsigned char>(new unsigned char[d.originalRgba.size()], std::default_delete<unsigned char[]>());
        std::memcpy(p.get(), d.originalRgba.data(), d.originalRgba.size()); return {p, d.width, d.height};
    }
    bool Write(HistoryStore& store, int64_t time) {
        auto d = Document(time); auto generation = store.RegisterCandidate(error);
        return generation && store.Write(*generation, d, Result(d), "copy", error);
    }
    fs::path root;
    std::string error;
};
}
TEST_F(HistoryTest, SevenLayersAndUnicodeRoundTripThroughJsonAndPng) {
    HistoryStore store(root); auto d = Document();
    d.layers = {{1, {1, 2, 3, 4}, 4, DocumentRectangle{{1, 2}, {4, 5}, true}},
        {2, {10, 20, 30, 40}, 2, DocumentEllipse{{2, 3}, {4, 5}, false}},
        {3, {1, 2, 3, 4}, 3, DocumentArrow{{1, 1}, {6, 5}}},
        {4, {1, 2, 3, 4}, 2, DocumentBrush{{{1, 1}, {2, 3}}}},
        {5, {1, 2, 3, 4}, 2, DocumentMosaic{{{2, 2}, {4, 3}}, 8}},
        {6, {1, 2, 3, 4}, 1, DocumentText{{1, 2}, "中文\n多行\"引号\\", 17, 65}},
        {7, {1, 2, 3, 4}, 0, DocumentNumber{{4, 3}, 24, 1}}};
    d.mosaicCache = DocumentMosaicCache{0, 0, 8, 6, d.originalRgba};
    auto generation = store.RegisterCandidate(error); ASSERT_TRUE(generation);
    ASSERT_TRUE(store.Write(*generation, d, Result(d), "save", error)) << error;
    auto list = store.List(); ASSERT_EQ(list.size(), 1u); EXPECT_EQ(list[0].action, "save");
    auto loaded = store.LoadDocument(list[0].id, error); ASSERT_TRUE(loaded) << error;
    EXPECT_EQ(loaded->originalRgba, d.originalRgba); ASSERT_EQ(loaded->layers.size(), 7u);
    EXPECT_EQ(std::get<DocumentText>(loaded->layers[5].data).text, "中文\n多行\"引号\\");
    EXPECT_EQ(loaded->mosaicCache->rgba, d.mosaicCache->rgba);
    auto image = store.LoadResult(list[0].id, false, error); ASSERT_TRUE(image) << error;
    EXPECT_EQ(std::memcmp(image.pixels.get(), d.originalRgba.data(), d.originalRgba.size()), 0);
    auto thumb = store.LoadResult(list[0].id, true, error); ASSERT_TRUE(thumb); EXPECT_LE(thumb.width, 192); EXPECT_LE(thumb.height, 128);
}

TEST_F(HistoryTest, QuickAccessRefusesTrashedRecordsButTheLibraryCanStillPreviewThem) {
    HistoryStore store(root);
    ASSERT_TRUE(Write(store, 100));
    const auto id = store.List().front().id;
    ASSERT_TRUE(store.LoadResult(id, false, error, true));
    ASSERT_TRUE(store.Trash(id, error));
    EXPECT_FALSE(store.LoadResult(id, false, error, true));
    EXPECT_FALSE(store.LoadResult(id, true, error, true));
    EXPECT_TRUE(store.LoadResult(id, false, error));
    EXPECT_TRUE(store.LoadResult(id, true, error));
    ASSERT_TRUE(store.Restore(id, error));
    EXPECT_TRUE(store.LoadResult(id, false, error, true));
}
TEST_F(HistoryTest, StorageMigrationPreservesImagesMetadataTrashAndEnabledPreference) {
    HistoryStore source(root/"source"); ASSERT_TRUE(Write(source,1)); ASSERT_TRUE(Write(source,2));
    const auto records=source.List();
    ASSERT_TRUE(source.UpdateDetails(records[0].id,[](HistoryDetails& details) { details.title="截图名称"; details.favorite=true; details.tags={"项目"}; },error));
    ASSERT_TRUE(source.Trash(records[1].id,error));
    ASSERT_TRUE(source.SetEnabled(false,error));
    const auto generation=source.GetConfig().generation;
    { std::ofstream unrelated(root/"source"/"settings.json"); unrelated<<"keep"; }
    int commits=0;
    ASSERT_TRUE(source.MigrateTo(root/fs::path(u8"新位置"),[&](std::string& message) {
        ++commits;
        HistoryStore destination(root/fs::path(u8"新位置"));
        EXPECT_EQ(destination.List(true).size(),2u); EXPECT_FALSE(destination.GetConfig().enabled);
        EXPECT_TRUE(destination.LoadDocument(records[0].id,message)); return true;
    },error))<<error;
    EXPECT_EQ(commits,1); EXPECT_TRUE(source.List(true).empty()); EXPECT_FALSE(source.GetConfig().enabled);
    EXPECT_NE(source.GetConfig().generation,generation); EXPECT_TRUE(fs::exists(root/"source"/"settings.json"));
    HistoryStore destination(root/fs::path(u8"新位置"));
    const auto migrated=destination.List(true); ASSERT_EQ(migrated.size(),2u);
    EXPECT_EQ(migrated[0].id,records[0].id); EXPECT_EQ(migrated[0].details.title,"截图名称");
    EXPECT_TRUE(migrated[0].details.favorite); EXPECT_EQ(migrated[0].details.tags,std::vector<std::string>{"项目"});
    EXPECT_GT(migrated[1].details.deletedAtUnixMs,0);
    EXPECT_EQ(destination.LoadDocument(records[0].id,error)->originalRgba,Document().originalRgba);
}
TEST_F(HistoryTest, FailedMigrationCommitRestoresOriginalGenerationAndRemovesOnlyCopiedAssets) {
    HistoryStore source(root/"source"); ASSERT_TRUE(Write(source,1));
    const auto original=source.GetConfig();
    fs::create_directories(root/"target"); { std::ofstream unrelated(root/"target"/"user.txt"); unrelated<<"keep"; }
    EXPECT_FALSE(source.MigrateTo(root/"target",[](std::string& message) { message="模拟设置保存失败"; return false; },error));
    EXPECT_EQ(error,"模拟设置保存失败"); EXPECT_EQ(source.List().size(),1u);
    EXPECT_EQ(source.GetConfig().generation,original.generation); EXPECT_TRUE(source.GetConfig().enabled);
    EXPECT_TRUE(fs::exists(root/"target"/"user.txt")); EXPECT_FALSE(fs::exists(root/"target"/"history.conf"));
    EXPECT_FALSE(fs::exists(root/"target"/"history"));
}
TEST_F(HistoryTest, MigrationRejectsOccupiedNestedAndLockedDirectoriesWithoutCallingCommit) {
    HistoryStore source(root/"source"), occupied(root/"occupied");
    ASSERT_TRUE(Write(source,1)); ASSERT_TRUE(Write(occupied,2)); int commits=0;
    const auto commit=[&](std::string&) { ++commits; return true; };
    EXPECT_FALSE(source.MigrateTo(root/"occupied",commit,error));
    EXPECT_FALSE(source.MigrateTo(root/"source"/"nested",commit,error));
    EXPECT_FALSE(source.MigrateTo(root,commit,error));
    fs::create_directory(root/"locked"); HistoryDirectoryLock lock(root/"locked"/"history.lock"); ASSERT_TRUE(lock);
    EXPECT_FALSE(source.MigrateTo(root/"locked",commit,error));
    EXPECT_EQ(commits,0); EXPECT_EQ(source.List().size(),1u); EXPECT_EQ(occupied.List().size(),1u);
}
TEST_F(HistoryTest, MigratedSourceRejectsCandidatesFromBeforeTheMoveAndNewDirectoryAcceptsScreenshots) {
    HistoryStore source(root/"source"); const auto oldGeneration=*source.RegisterCandidate(error);
    ASSERT_TRUE(source.MigrateTo(root/"target",[](std::string&) { return true; },error))<<error;
    auto document=Document(); EXPECT_FALSE(source.Write(oldGeneration,document,Result(document),"copy",error));
    EXPECT_FALSE(source.RegisterCandidate(error));
    HistoryStore destination(root/"target"); EXPECT_TRUE(Write(destination,3))<<error;
}
TEST_F(HistoryTest, HundredAndFirstRecordPrunesOldestAndCountsAllAssets) {
    HistoryStore store(root);
    for (int i = 0; i < 101; ++i) ASSERT_TRUE(Write(store, i)) << error;
    const auto records = store.List(); ASSERT_EQ(records.size(), 100u);
    EXPECT_EQ(records.front().createdAtUnixMs, 100); EXPECT_EQ(records.back().createdAtUnixMs, 1);
    uint64_t bytes = 0;
    for (const auto& entry : fs::directory_iterator(root / "history" / records[0].id)) bytes += entry.file_size();
    EXPECT_EQ(records[0].bytes, bytes);
    EXPECT_GT(bytes, fs::file_size(root / "history" / records[0].id / "result.png"));
}

TEST_F(HistoryTest, CommittedMutationsNotifyOtherInstancesAndReadsDoNotChangeRevision) {
    HistoryStore reader(root), writer(root), independent(root / "independent");
    auto revision = reader.Revision();
    auto independentRevision = independent.Revision();
    const auto expectChanged = [&] {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        auto current = reader.Revision();
        while (current == revision && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            current = reader.Revision();
        }
        EXPECT_GT(current, revision);
        revision = current;
    };
    ASSERT_TRUE(Write(writer, 1)); expectChanged();
    auto records = reader.List(); ASSERT_EQ(records.size(), 1u);
    const auto id = records.front().id;
    ASSERT_TRUE(writer.UpdateDetails(id, [](HistoryDetails& d) { d.title = "修改名称"; d.tags = {"新标签"}; }, error));
    expectChanged();
    ASSERT_TRUE(writer.UpdateDetails(id, [](HistoryDetails& d) { d.ocrIndexed = true; d.ocrText = "识别完成"; }, error));
    expectChanged();
    ASSERT_TRUE(writer.Trash(id, error)); expectChanged();
    ASSERT_TRUE(writer.Restore(id, error)); expectChanged();
    ASSERT_TRUE(writer.Delete(id, error)); expectChanged();
    ASSERT_TRUE(writer.SetEnabled(false, error)); expectChanged();
    EXPECT_FALSE(reader.GetConfig().enabled);
    ASSERT_TRUE(writer.SetEnabled(true, error)); expectChanged();
    for (int i = 0; i < 20; ++i) {
        EXPECT_TRUE(reader.List().empty());
        reader.GetConfig();
        EXPECT_EQ(reader.Revision(), revision);
        EXPECT_EQ(independent.Revision(), independentRevision);
    }
}

TEST_F(HistoryTest, SeparateProcessPublishesHistoryChangeNotification) {
    HistoryStore reader(root);
    const auto before = reader.Revision();
#if defined(_WIN32)
    constexpr auto ChildTimeout = std::chrono::seconds(10);
    const fs::path executable(SHOTLET_HISTORY_PROCESS_PROBE);
    auto command = L"\"" + executable.wstring() + L"\" \"" + root.wstring() + L"\" 1 1";
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    ASSERT_TRUE(CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, FALSE,
        0, nullptr, nullptr, &startup, &process)) << GetLastError();
    const ScopeExit closeProcess([&] { CloseHandle(process.hThread); CloseHandle(process.hProcess); });
    const auto waitResult = WaitForSingleObject(process.hProcess,
        static_cast<DWORD>(std::chrono::duration_cast<std::chrono::milliseconds>(ChildTimeout).count()));
    if (waitResult != WAIT_OBJECT_0) {
        // 子进程仍可能借用测试目录；先终止并回收，再允许 TearDown 删除目录。
        TerminateProcess(process.hProcess, EXIT_FAILURE);
        WaitForSingleObject(process.hProcess, INFINITE);
        FAIL() << "历史通知子进程超时或等待失败：" << waitResult;
    }
    DWORD exitCode = EXIT_FAILURE;
    ASSERT_TRUE(GetExitCodeProcess(process.hProcess, &exitCode));
    ASSERT_EQ(exitCode, EXIT_SUCCESS);
#else
    const pid_t child = fork(); ASSERT_GE(child, 0);
    if (child == 0) {
        // fork 后直接 exec，避免在未 exec 的子进程中调用 macOS 通知框架。
        execl(SHOTLET_HISTORY_PROCESS_PROBE, SHOTLET_HISTORY_PROCESS_PROBE, root.c_str(), "1", "1", nullptr);
        _exit(127);
    }
    int status = 0;
    ASSERT_EQ(waitpid(child, &status, 0), child);
    ASSERT_TRUE(WIFEXITED(status)); ASSERT_EQ(WEXITSTATUS(status), 0);
#endif
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    auto after = reader.Revision();
    while (after == before && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        after = reader.Revision();
    }
    EXPECT_GT(after, before);
    EXPECT_EQ(reader.List().size(), 1u);
}
TEST_F(HistoryTest, ByteQuotaPrunesOldestAndOversizedEntryKeepsOldRecords) {
    HistoryStore store(root); ASSERT_TRUE(Write(store, 1));
    auto bytes = store.List()[0].bytes;
    HistoryStore limited(root, 100, bytes * 2 + 64);
    ASSERT_TRUE(Write(limited, 2)) << error; ASSERT_TRUE(Write(limited, 3)) << error;
    auto records = limited.List(); EXPECT_LE(records.size(), 2u); EXPECT_EQ(records.front().createdAtUnixMs, 3);
    HistoryStore tiny(root, 100, 1);
    EXPECT_FALSE(Write(tiny, 4)); EXPECT_EQ(tiny.List().size(), records.size());
}
TEST_F(HistoryTest, PersistedSwitchStopsNewCandidatesButAllowsRegisteredOnes) {
    HistoryStore first(root), second(root); auto generation = first.RegisterCandidate(error); ASSERT_TRUE(generation);
    ASSERT_TRUE(second.SetEnabled(false, error)); EXPECT_FALSE(first.RegisterCandidate(error));
    auto d = Document(); EXPECT_TRUE(first.Write(*generation, d, Result(d), "copy", error));
    EXPECT_EQ(second.List().size(), 1u); EXPECT_FALSE(HistoryStore(root).GetConfig().enabled);
    ASSERT_TRUE(second.SetEnabled(true, error)); EXPECT_TRUE(first.RegisterCandidate(error));
}
TEST_F(HistoryTest, ClearInvalidatesCandidatesAcrossProcessesAndRestart) {
    HistoryService service(root); auto d = Document(); auto candidate = service.Prepare(nullptr, Result(d), "ocr");
    ASSERT_TRUE(candidate); EXPECT_GT(service.ReservedBytes(), 0u);
    const auto generation = candidate->generation;
    HistoryStore second(root); ASSERT_TRUE(second.Clear(error));
    EXPECT_FALSE(HistoryStore(root).IsCurrent(generation));
    service.Finish(std::move(candidate)); service.Drain();
    EXPECT_EQ(service.ReservedBytes(), 0u); EXPECT_TRUE(second.List().empty());
    EXPECT_TRUE(fs::exists(root / "history.conf")); EXPECT_TRUE(fs::exists(root / "history.lock"));
}
TEST_F(HistoryTest, ConfigurationFailureDoesNotDeleteExistingRecords) {
    HistoryStore store(root); ASSERT_TRUE(Write(store, 1)); auto generation = store.GetConfig().generation;
    fs::rename(root / "history.conf", root / "config-backup"); fs::create_directory(root / "history.conf");
    EXPECT_FALSE(store.Clear(error)); EXPECT_EQ(store.List().size(), 1u);
    fs::remove(root / "history.conf"); fs::rename(root / "config-backup", root / "history.conf");
    EXPECT_EQ(store.GetConfig().generation, generation);
}
TEST_F(HistoryTest, UnknownVersionOrLayerLeavesFinalImageAvailable) {
    HistoryStore store(root); ASSERT_TRUE(Write(store, 1)); auto id = store.List()[0].id;
    auto path = root / "history" / id / "document.json";
    std::ofstream(path) << "{\"formatVersion\":999,\"renderVersion\":999}";
    EXPECT_FALSE(store.LoadDocument(id, error)); EXPECT_TRUE(store.LoadResult(id, false, error));
    EXPECT_EQ(store.List().size(), 1u);
    auto d = Document(); d.layers.push_back({1, {1, 2, 3, 4}, 2, DocumentArrow{{1, 2}, {3, 4}}});
    auto text = EncodeDocument(d); text.replace(text.find("\"arrow\""), 7, "\"future-layer\"");
    EXPECT_THROW(DecodeDocument(text), std::exception);
}
TEST_F(HistoryTest, RecoverySkipsActiveTemporaryLeaseAndRemovesAbandonedAssets) {
    HistoryStore store(root); auto temporary = root / "history" / ".tmp-test"; fs::create_directory(temporary);
    {
        HistoryDirectoryLock lease(temporary / ".lease"); ASSERT_TRUE(lease);
        store.Recover(); EXPECT_TRUE(fs::exists(temporary)); EXPECT_TRUE(store.List().empty());
    }
    store.Recover(); EXPECT_FALSE(fs::exists(temporary));
}
TEST_F(HistoryTest, CandidateBudgetIsReservedBeforeCopyAndReleasedOnCancellationFailureAndDrain) {
    auto d = Document(); HistoryService limited(root, 24u * 1024u * 1024u);
    auto first = limited.Prepare(nullptr, Result(d), "copy"); ASSERT_TRUE(first);
    EXPECT_FALSE(limited.Prepare(nullptr, Result(d), "save")); EXPECT_FALSE(limited.TakeNotices().empty());
    first.reset(); EXPECT_EQ(limited.ReservedBytes(), 0u);
    auto second = limited.Prepare(nullptr, Result(d), "pin"); ASSERT_TRUE(second);
    limited.Finish(std::move(second)); limited.Drain(); EXPECT_EQ(limited.ReservedBytes(), 0u);
    EXPECT_EQ(limited.Store().List().size(), 1u);
}
TEST_F(HistoryTest, TraversalAndOversizedPngAreRejectedBeforeDecode) {
    HistoryStore store(root); EXPECT_FALSE(store.LoadResult("../history.conf", false, error));
    auto path = root / "bad.png"; const unsigned char header[] = {137,80,78,71,13,10,26,10,0,0,0,13,'I','H','D','R',127,255,255,255,127,255,255,255};
    { std::ofstream out(path, std::ios::binary); out.write(reinterpret_cast<const char*>(header), sizeof(header)); }
    EXPECT_FALSE(ReadHistoryPng(path, error));
}
#if !defined(_WIN32)
TEST_F(HistoryTest, IndependentProcessesCommitWithoutHalfRecordsOrLostWrites) {
    HistoryStore store(root);
    pid_t child = fork(); ASSERT_GE(child, 0);
    if (child == 0) {
        execl(SHOTLET_HISTORY_PROCESS_PROBE, SHOTLET_HISTORY_PROCESS_PROBE, root.c_str(), "100", "6", nullptr);
        _exit(127);
    }
    for (int i = 0; i < 6; ++i) ASSERT_TRUE(Write(store, i)) << error;
    int status = 0; ASSERT_EQ(waitpid(child, &status, 0), child); ASSERT_TRUE(WIFEXITED(status)); EXPECT_EQ(WEXITSTATUS(status), 0);
    auto records = store.List(); ASSERT_EQ(records.size(), 12u);
    for (const auto& record : records) EXPECT_TRUE(store.LoadDocument(record.id, error)) << error;
}
#endif
TEST(HistoryBudgetTest, OverflowCannotReserveAndMoveReleasesExactlyOnce) {
    HistoryBudget budget(100);
    { auto lease = budget.TryReserve(80); ASSERT_TRUE(lease); EXPECT_FALSE(budget.TryReserve(21));
      auto moved = std::move(lease); EXPECT_EQ(budget.Used(), 80u); EXPECT_FALSE(budget.TryReserve(SIZE_MAX)); }
    EXPECT_EQ(budget.Used(), 0u); EXPECT_TRUE(budget.TryReserve(100)); EXPECT_EQ(budget.Used(), 0u);
}

TEST_F(HistoryTest, MaximumLongImageSkipsSnapshotBeforeCopyWithoutChangingOutput) {
    HistoryService service(root);
    constexpr int width = 4096, height = 16384;
    auto pixels = std::shared_ptr<unsigned char>(new unsigned char[size_t(width) * height * 4], std::default_delete<unsigned char[]>());
    pixels.get()[0] = 17;
    EXPECT_FALSE(service.Prepare(nullptr, {pixels, width, height}, "copy"));
    EXPECT_EQ(service.ReservedBytes(), 0u); EXPECT_EQ(pixels.get()[0], 17);
    EXPECT_FALSE(service.TakeNotices().empty());
}

TEST_F(HistoryTest, LongImageWritesOnWorkerAndReleasesAllReservedBytesAtExit) {
    HistoryService service(root);
    constexpr int width = 40, height = 16384;
    auto pixels = std::shared_ptr<unsigned char>(new unsigned char[size_t(width) * height * 4], std::default_delete<unsigned char[]>());
    std::memset(pixels.get(), 255, size_t(width) * height * 4); pixels.get()[0] = 17;
    auto candidate = service.Prepare(nullptr, {pixels, width, height}, "copy"); ASSERT_TRUE(candidate);
    EXPECT_GE(service.ReservedBytes(), size_t(width) * height * 4 * 8);
    service.Finish(std::move(candidate)); service.Drain(); EXPECT_EQ(service.ReservedBytes(), 0u);
    auto records = service.Store().List(); ASSERT_EQ(records.size(), 1u);
    auto image = service.Store().LoadResult(records[0].id, false, error); ASSERT_TRUE(image);
    EXPECT_EQ(image.width, width); EXPECT_EQ(image.height, height); EXPECT_EQ(image.pixels.get()[0], 17);
}
#if !defined(_WIN32)
TEST_F(HistoryTest, FailureToPersistClearGenerationKeepsExistingAssets) {
    HistoryStore store(root); ASSERT_TRUE(Write(store, 1)); auto generation = store.GetConfig().generation;
    fs::permissions(root, fs::perms::owner_read | fs::perms::owner_exec);
    const bool cleared = store.Clear(error);
    fs::permissions(root, fs::perms::owner_all);
    EXPECT_FALSE(cleared); EXPECT_FALSE(error.empty()); EXPECT_EQ(store.List().size(), 1u);
    EXPECT_EQ(store.GetConfig().generation, generation);
}
#endif

TEST_F(HistoryTest, ClearDuringEncodingPreventsFinalRename) {
    HistoryStore store(root); auto d = Document(); d.width = d.height = 2048; d.logicalSize = {2048, 2048};
    d.originalRgba.resize(size_t(d.width) * d.height * 4);
    uint32_t state = 17;
    for (auto& byte : d.originalRgba) { state ^= state << 13; state ^= state >> 17; state ^= state << 5; byte = uint8_t(state); }
    auto image = Result(d); auto generation = store.RegisterCandidate(error); ASSERT_TRUE(generation);
    bool committed = false; std::string writeError;
    std::thread writer([&] { committed = store.Write(*generation, d, image, "copy", writeError); });
    bool sawTemporary = false;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!sawTemporary && std::chrono::steady_clock::now() < deadline) {
        for (const auto& entry : fs::directory_iterator(root / "history"))
            if (entry.path().filename().string().starts_with(".tmp-")) sawTemporary = true;
        std::this_thread::yield();
    }
    const bool cleared = store.Clear(error); writer.join();
    EXPECT_TRUE(sawTemporary); EXPECT_TRUE(cleared) << error; EXPECT_FALSE(committed) << writeError;
    EXPECT_TRUE(store.List().empty());
    for (const auto& entry : fs::directory_iterator(root / "history"))
        EXPECT_FALSE(entry.path().filename().string().starts_with(".tmp-"));
}

TEST_F(HistoryTest, LoadedDocumentSurvivesDeletionAndClearRemovesEveryCommittedRecord) {
    HistoryStore store(root);
    for (int i = 0; i < 5; ++i) ASSERT_TRUE(Write(store, i));
    auto records = store.List(); auto loaded = store.LoadDocument(records[0].id, error); ASSERT_TRUE(loaded);
    const auto original = loaded->originalRgba;
    ASSERT_TRUE(store.Delete(records[0].id, error)); EXPECT_EQ(loaded->originalRgba, original);
    ASSERT_TRUE(store.Clear(error)); EXPECT_TRUE(store.List().empty());
    EXPECT_EQ(loaded->originalRgba, original);
}

TEST_F(HistoryTest, RecoveryEnforcesQuotaAfterInterruptedPostCommitCleanup) {
    HistoryStore store(root);
    for (int i = 0; i < 4; ++i) ASSERT_TRUE(Write(store, i));
    HistoryStore recovered(root, 2); recovered.Recover();
    const auto records = recovered.List(); ASSERT_EQ(records.size(), 2u);
    EXPECT_EQ(records[0].createdAtUnixMs, 3); EXPECT_EQ(records[1].createdAtUnixMs, 2);
}

TEST_F(HistoryTest, LegacyMetadataDefaultsAndLibraryAttributesRoundTrip) {
    auto legacy = DecodeHistoryMetadata(R"({"id":"abc","action":"copy","sourceRecordId":"","createdAtUnixMs":100,"width":8,"height":6,"bytes":100,"formatVersion":1,"renderVersion":2})");
    EXPECT_FALSE(legacy.details.favorite); EXPECT_FALSE(legacy.details.ocrIndexed);
    EXPECT_EQ(legacy.details.captureType,"unknown"); EXPECT_TRUE(legacy.details.tags.empty());
    HistoryStore store(root); ASSERT_TRUE(Write(store,1)); const auto id=store.List()[0].id;
    ASSERT_TRUE(store.UpdateDetails(id,[](HistoryDetails& d){
        d.title="需求评审";d.captureType="window";d.sourceApplication="Chrome";d.sourceWindow="需求.pdf";
        d.favorite=true;d.tags={"设计稿","评审","设计稿"};d.ocrIndexed=true;d.ocrText="支持中文搜索与换行\n第二行";
    },error)) << error;
    auto metadata=HistoryStore(root).Get(id,error);ASSERT_TRUE(metadata);
    EXPECT_EQ(metadata->details.title,"需求评审");EXPECT_EQ(metadata->details.sourceApplication,"Chrome");
    EXPECT_EQ(metadata->details.tags.size(),2u);EXPECT_TRUE(metadata->details.favorite);EXPECT_TRUE(metadata->details.ocrIndexed);
    EXPECT_NE(metadata->details.ocrText.find("第二行"),std::string::npos);
    const auto before=EncodeHistoryMetadata(*metadata);
    EXPECT_FALSE(store.UpdateDetails(id,[](HistoryDetails& d){d.tags={std::string(129,'x')};},error));
    EXPECT_EQ(EncodeHistoryMetadata(*store.Get(id,error)),before);
}

TEST_F(HistoryTest, IndependentMetadataUpdatesDoNotLoseFavoriteOrTags) {
    HistoryStore first(root),second(root);ASSERT_TRUE(Write(first,1));auto id=first.List()[0].id;
    bool favorite=false,tag=false;std::string otherError;
    std::thread writer([&]{favorite=first.UpdateDetails(id,[](HistoryDetails& d){d.favorite=true;},otherError);});
    tag=second.UpdateDetails(id,[](HistoryDetails& d){d.tags.push_back("反馈");},error);writer.join();
    EXPECT_TRUE(favorite)<<otherError;EXPECT_TRUE(tag)<<error;
    auto result=first.Get(id,error);ASSERT_TRUE(result);EXPECT_TRUE(result->details.favorite);EXPECT_EQ(result->details.tags,std::vector<std::string>{"反馈"});
}

TEST_F(HistoryTest, RecentlyDeletedRestoresAndIsCountedAndPrunedBeforeActiveRecords) {
    HistoryStore store(root,2);ASSERT_TRUE(Write(store,1));ASSERT_TRUE(Write(store,2));
    auto records=store.List();const auto newer=records[0].id,older=records[1].id;
    ASSERT_TRUE(store.Trash(newer,error));EXPECT_EQ(store.List().size(),1u);EXPECT_EQ(store.List(true).size(),2u);
    ASSERT_TRUE(store.LoadResult(newer,false,error));ASSERT_TRUE(store.LoadDocument(newer,error));
    ASSERT_TRUE(store.Restore(newer,error));EXPECT_EQ(store.List().size(),2u);
    ASSERT_TRUE(store.Trash(newer,error));ASSERT_TRUE(Write(store,3));
    auto retained=store.List(true);ASSERT_EQ(retained.size(),2u);
    EXPECT_FALSE(store.Get(newer,error));EXPECT_TRUE(store.Get(older,error));
    ASSERT_TRUE(store.Trash(older,error));ASSERT_TRUE(store.EmptyTrash(error));
    EXPECT_EQ(store.List(true).size(),1u);EXPECT_EQ(store.List()[0].createdAtUnixMs,3);
    const auto id=store.List()[0].id;
    ASSERT_TRUE(store.UpdateDetails(id,[](HistoryDetails& d){d.deletedAtUnixMs=HistoryNow()-31ll*24*60*60*1000;},error));
    store.Recover();EXPECT_TRUE(store.List(true).empty());
}

TEST(HistoryLibraryTest, SearchFiltersSortAndLocalDateBoundariesUseRealMetadata) {
    const auto now=HistoryDayStart(HistoryNow())+12*60*60*1000;
    std::vector<HistoryMetadata> records(4);
    for(size_t i=0;i<records.size();++i){auto& r=records[i];r.id=std::to_string(i);r.createdAtUnixMs=HistoryDayStart(now,-int(i))+1000;r.width=8;r.height=6;r.bytes=100+i;}
    records[0].details.title="需求评审";records[0].details.sourceApplication="Chrome";records[0].details.tags={"设计稿"};records[0].details.favorite=true;
    records[0].details.ocrText="支持按标签搜索";records[0].details.captureType="window";
    records[3].details.deletedAtUnixMs=now;
    HistoryQuery query;query.search="需求 chrome 标签";
    EXPECT_EQ(QueryHistory(records,query,now),std::vector<size_t>{0});
    query.search.clear();query.scope=HistoryScope::Yesterday;EXPECT_EQ(QueryHistory(records,query,now),std::vector<size_t>{1});
    query.scope=HistoryScope::Favorites;EXPECT_EQ(QueryHistory(records,query,now),std::vector<size_t>{0});
    query.scope=HistoryScope::Tag;query.value="设计稿";EXPECT_EQ(QueryHistory(records,query,now),std::vector<size_t>{0});
    query.scope=HistoryScope::Type;query.value="window";EXPECT_EQ(QueryHistory(records,query,now),std::vector<size_t>{0});
    query.scope=HistoryScope::Trash;EXPECT_EQ(QueryHistory(records,query,now),std::vector<size_t>{3});
    query.scope=HistoryScope::All;query.sort=HistorySort::Oldest;EXPECT_EQ(QueryHistory(records,query,now),(std::vector<size_t>{2,1,0}));
    query.sort=HistorySort::Largest;EXPECT_EQ(QueryHistory(records,query,now),(std::vector<size_t>{2,1,0}));
    EXPECT_NE(HistoryTime(now,"%Y-%m-%d"),HistoryTime(HistoryDayStart(now,-1),"%Y-%m-%d"));
}

TEST(HistoryLibraryTest, SearchTrimsOuterWhitespaceAndMatchesAllCaseInsensitiveTerms) {
    HistoryMetadata record;
    record.details.title = "Release Notes";
    record.details.sourceApplication = "Chrome";
    record.details.sourceWindow = "Project Alpha";
    record.details.ocrText = "需求评审记录";
    record.details.tags = {"Backend"};
    HistoryQuery query;
    for (const auto* search : {"  rElEaSe   CHROME \t需求\r\n", " backend ALPHA notes ", " \t\r\n "}) {
        SCOPED_TRACE(search);
        query.search = search;
        EXPECT_TRUE(HistoryMatches(record, query, HistoryNow()));
    }
    // 中间空格仍分隔关键词，不把原文里的词语强行拼接，也不能忽略缺失的关键词。
    query.search = "ReleaseNotes";
    EXPECT_FALSE(HistoryMatches(record, query, HistoryNow()));
    query.search = "release missing";
    EXPECT_FALSE(HistoryMatches(record, query, HistoryNow()));
    query.search = "   ";
    record.details.deletedAtUnixMs = HistoryNow();
    EXPECT_FALSE(HistoryMatches(record, query, HistoryNow()));
}

TEST(HistoryLibraryTest, CaseInsensitiveSearchSupportsUtf8WithoutChangingChineseText) {
    HistoryMetadata record;
    record.details.title = "CAFÉ Straße 中文截图";
    HistoryQuery query;
    query.search = " café STRASSE 中文 ";
    EXPECT_TRUE(HistoryMatches(record, query, HistoryNow()));
    query.search = "café 其他内容";
    EXPECT_FALSE(HistoryMatches(record, query, HistoryNow()));
}

TEST_F(HistoryTest, StitchPreservesOrderCentersDifferentSizesAndRejectsDeletedInputs) {
    HistoryStore store(root);auto first=Document(1);std::fill(first.originalRgba.begin(),first.originalRgba.end(),255);
    for(size_t i=0;i<first.originalRgba.size();i+=4){first.originalRgba[i+1]=0;first.originalRgba[i+2]=0;}
    auto generation=store.RegisterCandidate(error);ASSERT_TRUE(generation);
    ASSERT_TRUE(store.Write(*generation,first,Result(first),"copy",error));auto id1=store.List()[0].id;
    auto second=Document(2);second.width=4;second.height=3;second.logicalSize={4,3};second.originalRgba.assign(4*3*4,255);
    for(size_t i=0;i<second.originalRgba.size();i+=4){second.originalRgba[i]=0;second.originalRgba[i+2]=0;}
    ASSERT_TRUE(store.Write(*generation,second,Result(second),"copy",error));auto id2=store.List()[0].id;
    auto vertical=store.Stitch({id1,id2},false,error);ASSERT_TRUE(vertical)<<error;
    EXPECT_EQ(vertical.width,8);EXPECT_EQ(vertical.height,9);
    EXPECT_EQ(vertical.pixels.get()[1],0);EXPECT_EQ(vertical.pixels.get()[(6*8+2)*4],0);
    EXPECT_EQ(vertical.pixels.get()[(6*8)*4],255);EXPECT_EQ(vertical.pixels.get()[(6*8)*4+1],255);
    auto horizontal=store.Stitch({id2,id1},true,error);ASSERT_TRUE(horizontal)<<error;
    EXPECT_EQ(horizontal.width,12);EXPECT_EQ(horizontal.height,6);EXPECT_EQ(horizontal.pixels.get()[(1*12)*4],0);
    EXPECT_EQ(horizontal.pixels.get()[(0*12+4)*4+1],0);
    ASSERT_TRUE(store.Trash(id2,error));EXPECT_FALSE(store.Stitch({id1,id2},false,error));
}

TEST_F(HistoryTest, ExportUsesSafeNamesAndNeverOverwritesExistingFiles) {
    HistoryStore store(root);ASSERT_TRUE(Write(store,1));const auto id=store.List()[0].id;
    ASSERT_TRUE(store.UpdateDetails(id,[](HistoryDetails& d){d.title="../../需求:评审";},error));
    auto folder=root/"exports";fs::create_directory(folder);
    ASSERT_TRUE(store.ExportResults({id},folder,error))<<error;ASSERT_TRUE(store.ExportResults({id},folder,error))<<error;
    size_t count=0;
    for(const auto& file:fs::directory_iterator(folder)){
        ++count;EXPECT_EQ(file.path().extension(),".png");EXPECT_EQ(file.path().parent_path(),folder);
        auto image=ReadHistoryPng(file.path(),error);ASSERT_TRUE(image);EXPECT_EQ(image.width,8);EXPECT_EQ(image.height,6);
    }
    EXPECT_EQ(count,2u);EXPECT_FALSE(store.ExportResults({"../history.conf"},folder,error));
    EXPECT_TRUE(fs::exists(root/"history.conf"));
}

TEST_F(HistoryTest, NumberLabelRoundTripsAndOldNumberDocumentsRemainCompatible) {
    auto document = Document();
    document.layers = {{1, {200, 100, 50, 255}, 0, DocumentNumber{{4, 3}, 24, 1}}};
    auto legacy = DecodeDocument(EncodeDocument(document));
    ASSERT_EQ(legacy.layers.size(), 1u);
    EXPECT_FALSE(std::get<DocumentNumber>(legacy.layers[0].data).label);
    auto& number = std::get<DocumentNumber>(document.layers[0].data);
    number.label = DocumentText{{1, 2}, "说明\n中文", 20, 100};
    number.labelFrameScale = 2;
    HistoryStore store(root);
    auto generation = store.RegisterCandidate(error);
    ASSERT_TRUE(generation);
    ASSERT_TRUE(store.Write(*generation, document, Result(document), "save", error)) << error;
    auto restored = store.LoadDocument(store.List()[0].id, error);
    ASSERT_TRUE(restored) << error;
    const auto& restoredNumber = std::get<DocumentNumber>(restored->layers[0].data);
    ASSERT_TRUE(restoredNumber.label);
    EXPECT_EQ(restoredNumber.label->text, number.label->text);
    EXPECT_FLOAT_EQ(restoredNumber.label->position.x, 1);
    EXPECT_FLOAT_EQ(restoredNumber.label->fontSize, 20);
    EXPECT_FLOAT_EQ(restoredNumber.label->wrapWidth, 100);
    EXPECT_FLOAT_EQ(restoredNumber.labelFrameScale, 2);
    number.labelFrameScale = 0;
    EXPECT_FALSE(ValidateCaptureDocument(document, error));
    number.labelFrameScale = 2;
    number.label->text.resize(CaptureDocument::MaxTextBytes + 1);
    EXPECT_FALSE(ValidateCaptureDocument(document, error));
}

TEST_F(HistoryTest, HighlightShapeAndTransparencyRoundTrip) {
    auto document = Document();
    document.highlightTransparency = 65;
    document.layers = {{1, {0, 0, 0, 255}, 1, DocumentHighlight{{1, 1}, {7, 5}, false}},
        {2, {0, 0, 0, 255}, 1, DocumentHighlight{{2, 2}, {6, 5}, true}}};
    HistoryStore store(root);
    auto generation = store.RegisterCandidate(error);
    ASSERT_TRUE(generation);
    ASSERT_TRUE(store.Write(*generation, document, Result(document), "save", error)) << error;
    auto restored = store.LoadDocument(store.List()[0].id, error);
    ASSERT_TRUE(restored) << error;
    EXPECT_EQ(restored->highlightTransparency, 65);
    ASSERT_EQ(restored->layers.size(), 2u);
    EXPECT_FALSE(std::get<DocumentHighlight>(restored->layers[0].data).ellipse);
    EXPECT_TRUE(std::get<DocumentHighlight>(restored->layers[1].data).ellipse);
}

TEST_F(HistoryTest, WatermarkRoundTripsThroughHistoryAndLegacyDocumentsStayEmpty) {
    auto document = Document();
    auto legacy = DecodeDocument(EncodeDocument(document));
    EXPECT_FALSE(legacy.watermark);
    document.watermark = DocumentWatermark{"中文Water", {10, 160, 80, 96}, 24, {-40.5f, -30.25f}};
    document.watermark->opacity = 65;
    // 旧水印文档不含透明度字段，恢复后沿用原来的 alpha=96。
    auto legacyJson = nlohmann::json::parse(EncodeDocument(document));
    legacyJson["watermark"].erase("opacity");
    const auto legacyWatermark = DecodeDocument(legacyJson.dump());
    ASSERT_TRUE(legacyWatermark.watermark);
    EXPECT_EQ(legacyWatermark.watermark->opacity, WatermarkSettings::LegacyOpacity);
    HistoryStore store(root);
    const auto generation = store.RegisterCandidate(error);
    ASSERT_TRUE(generation);
    ASSERT_TRUE(store.Write(*generation, document, Result(document), "save", error)) << error;
    const auto loaded = store.LoadDocument(store.List()[0].id, error);
    ASSERT_TRUE(loaded) << error;
    ASSERT_TRUE(loaded->watermark);
    EXPECT_EQ(loaded->watermark->text, document.watermark->text);
    EXPECT_EQ(loaded->watermark->color.g, 160);
    EXPECT_EQ(loaded->watermark->opacity, 65);
    EXPECT_FLOAT_EQ(loaded->watermark->fontSize, 24);
    EXPECT_FLOAT_EQ(loaded->watermark->origin.x, -40.5f);
    EXPECT_FLOAT_EQ(loaded->watermark->origin.y, -30.25f);
    document.watermark->text = "一二三四五六七八九十多";
    EXPECT_FALSE(ValidateCaptureDocument(document, error));
    EXPECT_THROW(DecodeDocument(EncodeDocument(document)), std::runtime_error);
    document.watermark->text = "有效";
    document.watermark->fontSize = 0;
    EXPECT_FALSE(ValidateCaptureDocument(document, error));
    document.watermark->fontSize = 0.0001f;
    EXPECT_FALSE(ValidateCaptureDocument(document, error));
    document.watermark->fontSize = 24;
    for (const int invalid : {-1, 101}) {
        document.watermark->opacity = invalid;
        EXPECT_FALSE(ValidateCaptureDocument(document, error));
        EXPECT_THROW(DecodeDocument(EncodeDocument(document)), std::runtime_error);
    }
}
