/*
 * Copyright (C) 2018 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <pixelstats/SysfsCollector.h>

#define LOG_TAG "pixelstats-vendor"

#include <aidl/android/frameworks/stats/IStats.h>
#include <android-base/file.h>
#include <android-base/parseint.h>
#include <android-base/properties.h>
#include <android-base/strings.h>
#include <android/binder_manager.h>
#include <hardware/google/pixel/pixelstats/pixelatoms.pb.h>
#include <utils/Log.h>
#include <utils/StrongPointer.h>
#include <utils/Timers.h>

#include <mntent.h>
#include <sys/timerfd.h>
#include <cctype>
#include <cinttypes>
#include <string>

namespace {

using aidl::android::frameworks::stats::IStats;

std::shared_ptr<IStats> getStatsService() {
    const std::string instance = std::string() + IStats::descriptor + "/default";
    if (!AServiceManager_isDeclared(instance.c_str())) {
        ALOGE("IStats service is not registered.");
        return nullptr;
    }
    return IStats::fromBinder(ndk::SpAIBinder(AServiceManager_waitForService(instance.c_str())));
}

using aidl::android::frameworks::stats::VendorAtom;
using aidl::android::frameworks::stats::VendorAtomValue;
namespace PixelAtoms = android::hardware::google::pixel::PixelAtoms;

void reportSpeakerImpedance(const std::shared_ptr<IStats> &stats_client,
                            const PixelAtoms::VendorSpeakerImpedance &speakerImpedance) {
    // Load values array
    std::vector<VendorAtomValue> values(2);
    VendorAtomValue tmp;
    tmp.set<VendorAtomValue::intValue>(speakerImpedance.speaker_location());
    values[0] = tmp;
    tmp.set<VendorAtomValue::intValue>(speakerImpedance.impedance());
    values[1] = tmp;

    // Send vendor atom to IStats HAL
    VendorAtom event = {.reverseDomainName = PixelAtoms::ReverseDomainNames().pixel(),
                        .atomId = PixelAtoms::Ids::VENDOR_SPEAKER_IMPEDANCE,
                        .values = values};
    const ndk::ScopedAStatus ret = stats_client->reportVendorAtom(event);
    if (!ret.isOk())
        ALOGE("Unable to report VendorSpeakerImpedance to Stats service");
}

}  // namespace

namespace android {
namespace hardware {
namespace google {
namespace pixel {

using aidl::android::frameworks::stats::VendorAtom;
using aidl::android::frameworks::stats::VendorAtomValue;
using android::base::ReadFileToString;
using android::base::WriteStringToFile;
using android::base::StartsWith;
using android::frameworks::stats::V1_0::ChargeCycles;
using android::frameworks::stats::V1_0::HardwareFailed;
using android::frameworks::stats::V1_0::SlowIo;
using android::frameworks::stats::V1_0::SpeechDspStat;
using android::hardware::google::pixel::PixelAtoms::BatteryCapacity;
using android::hardware::google::pixel::PixelAtoms::BootStatsInfo;
using android::hardware::google::pixel::PixelAtoms::F2fsStatsInfo;
using android::hardware::google::pixel::PixelAtoms::F2fsCompressionInfo;
using android::hardware::google::pixel::PixelAtoms::PixelMmMetricsPerDay;
using android::hardware::google::pixel::PixelAtoms::PixelMmMetricsPerHour;
using android::hardware::google::pixel::PixelAtoms::StorageUfsHealth;
using android::hardware::google::pixel::PixelAtoms::StorageUfsResetCount;
using android::hardware::google::pixel::PixelAtoms::VendorSpeakerImpedance;
using android::hardware::google::pixel::PixelAtoms::ZramBdStat;
using android::hardware::google::pixel::PixelAtoms::ZramMmStat;

const std::vector<SysfsCollector::MmMetricsInfo> SysfsCollector::kMmMetricsPerHourInfo = {
        {"nr_free_pages", PixelMmMetricsPerHour::kFreePagesFieldNumber, false},
        {"nr_anon_pages", PixelMmMetricsPerHour::kAnonPagesFieldNumber, false},
        {"nr_file_pages", PixelMmMetricsPerHour::kFilePagesFieldNumber, false},
        {"nr_slab_reclaimable", PixelMmMetricsPerHour::kSlabReclaimableFieldNumber, false},
        {"nr_zspages", PixelMmMetricsPerHour::kZspagesFieldNumber, false},
        {"nr_unevictable", PixelMmMetricsPerHour::kUnevictableFieldNumber, false},
};

const std::vector<SysfsCollector::MmMetricsInfo> SysfsCollector::kMmMetricsPerDayInfo = {
        {"workingset_refault", PixelMmMetricsPerDay::kWorkingsetRefaultFieldNumber, true},
        {"workingset_refault_file", PixelMmMetricsPerDay::kWorkingsetRefaultFieldNumber, true},
        {"pswpin", PixelMmMetricsPerDay::kPswpinFieldNumber, true},
        {"pswpout", PixelMmMetricsPerDay::kPswpoutFieldNumber, true},
        {"allocstall_dma", PixelMmMetricsPerDay::kAllocstallDmaFieldNumber, true},
        {"allocstall_dma32", PixelMmMetricsPerDay::kAllocstallDma32FieldNumber, true},
        {"allocstall_normal", PixelMmMetricsPerDay::kAllocstallNormalFieldNumber, true},
        {"allocstall_movable", PixelMmMetricsPerDay::kAllocstallMovableFieldNumber, true},
        {"pgalloc_dma", PixelMmMetricsPerDay::kPgallocDmaFieldNumber, true},
        {"pgalloc_dma32", PixelMmMetricsPerDay::kPgallocDma32FieldNumber, true},
        {"pgalloc_normal", PixelMmMetricsPerDay::kPgallocNormalFieldNumber, true},
        {"pgalloc_movable", PixelMmMetricsPerDay::kPgallocMovableFieldNumber, true},
        {"pgsteal_kswapd", PixelMmMetricsPerDay::kPgstealKswapdFieldNumber, true},
        {"pgsteal_direct", PixelMmMetricsPerDay::kPgstealDirectFieldNumber, true},
        {"pgscan_kswapd", PixelMmMetricsPerDay::kPgscanKswapdFieldNumber, true},
        {"pgscan_direct", PixelMmMetricsPerDay::kPgscanDirectFieldNumber, true},
        {"oom_kill", PixelMmMetricsPerDay::kOomKillFieldNumber, true},
};

SysfsCollector::SysfsCollector(const struct SysfsPaths &sysfs_paths)
    : kSlowioReadCntPath(sysfs_paths.SlowioReadCntPath),
      kSlowioWriteCntPath(sysfs_paths.SlowioWriteCntPath),
      kSlowioUnmapCntPath(sysfs_paths.SlowioUnmapCntPath),
      kSlowioSyncCntPath(sysfs_paths.SlowioSyncCntPath),
      kCycleCountBinsPath(sysfs_paths.CycleCountBinsPath),
      kImpedancePath(sysfs_paths.ImpedancePath),
      kCodecPath(sysfs_paths.CodecPath),
      kCodec1Path(sysfs_paths.Codec1Path),
      kSpeechDspPath(sysfs_paths.SpeechDspPath),
      kBatteryCapacityCC(sysfs_paths.BatteryCapacityCC),
      kBatteryCapacityVFSOC(sysfs_paths.BatteryCapacityVFSOC),
      kUFSLifetimeA(sysfs_paths.UFSLifetimeA),
      kUFSLifetimeB(sysfs_paths.UFSLifetimeB),
      kUFSLifetimeC(sysfs_paths.UFSLifetimeC),
      kUFSHostResetPath(sysfs_paths.UFSHostResetPath),
      kF2fsStatsPath(sysfs_paths.F2fsStatsPath),
      kZramMmStatPath("/sys/block/zram0/mm_stat"),
      kZramBdStatPath("/sys/block/zram0/bd_stat"),
      kEEPROMPath(sysfs_paths.EEPROMPath),
      kVmstatPath("/proc/vmstat"),
      kIonTotalPoolsPath("/sys/kernel/dma_heap/total_pools_kb"),
      kIonTotalPoolsPathForLegacy("/sys/kernel/ion/total_pools_kb") {}

bool SysfsCollector::ReadFileToInt(const std::string &path, int *val) {
    return ReadFileToInt(path.c_str(), val);
}

bool SysfsCollector::ReadFileToInt(const char *const path, int *val) {
    std::string file_contents;

    if (!ReadFileToString(path, &file_contents)) {
        ALOGE("Unable to read %s - %s", path, strerror(errno));
        return false;
    } else if (StartsWith(file_contents, "0x")) {
        if (sscanf(file_contents.c_str(), "0x%x", val) != 1) {
            ALOGE("Unable to convert %s to hex - %s", path, strerror(errno));
            return false;
        }
    } else if (sscanf(file_contents.c_str(), "%d", val) != 1) {
        ALOGE("Unable to convert %s to int - %s", path, strerror(errno));
        return false;
    }
    return true;
}

bool SysfsCollector::ReadFileToUint(const char *const path, uint64_t *val) {
    std::string file_contents;

    if (!ReadFileToString(path, &file_contents)) {
        ALOGI("Unable to read %s - %s", path, strerror(errno));
        return false;
    } else {
        file_contents = android::base::Trim(file_contents);
        if (!android::base::ParseUint(file_contents, val)) {
            ALOGI("Unable to convert %s to uint - %s", path, strerror(errno));
            return false;
        }
    }
    return true;
}

/**
 * Read the contents of kCycleCountBinsPath and report them via IStats HAL.
 * The contents are expected to be N buckets total, the nth of which indicates the
 * number of times battery %-full has been increased with the n/N% full bucket.
 */
void SysfsCollector::logBatteryChargeCycles() {
    std::string file_contents;
    int val;
    std::vector<int> charge_cycles;
    if (kCycleCountBinsPath == nullptr || strlen(kCycleCountBinsPath) == 0) {
        ALOGV("Battery charge cycle path not specified");
        return;
    }
    if (!ReadFileToString(kCycleCountBinsPath, &file_contents)) {
        ALOGE("Unable to read battery charge cycles %s - %s", kCycleCountBinsPath, strerror(errno));
        return;
    }

    std::stringstream stream(file_contents);
    while (stream >> val) {
        charge_cycles.push_back(val);
    }
    ChargeCycles cycles;
    cycles.cycleBucket = charge_cycles;

    std::replace(file_contents.begin(), file_contents.end(), ' ', ',');
    stats_->reportChargeCycles(cycles);
}

/**
 * Read the contents of kEEPROMPath and report them.
 */
void SysfsCollector::logBatteryEEPROM() {
    if (kEEPROMPath == nullptr || strlen(kEEPROMPath) == 0) {
        ALOGV("Battery EEPROM path not specified");
        return;
    }

    battery_EEPROM_reporter_.checkAndReport(kEEPROMPath);
}

/**
 * Check the codec for failures over the past 24hr.
 */
void SysfsCollector::logCodecFailed() {
    std::string file_contents;
    if (kCodecPath == nullptr || strlen(kCodecPath) == 0) {
        ALOGV("Audio codec path not specified");
        return;
    }
    if (!ReadFileToString(kCodecPath, &file_contents)) {
        ALOGE("Unable to read codec state %s - %s", kCodecPath, strerror(errno));
        return;
    }
    if (file_contents == "0") {
        return;
    } else {
        HardwareFailed failed = {.hardwareType = HardwareFailed::HardwareType::CODEC,
                                 .hardwareLocation = 0,
                                 .errorCode = HardwareFailed::HardwareErrorCode::COMPLETE};
        stats_->reportHardwareFailed(failed);
    }
}

/**
 * Check the codec1 for failures over the past 24hr.
 */
void SysfsCollector::logCodec1Failed() {
    std::string file_contents;
    if (kCodec1Path == nullptr || strlen(kCodec1Path) == 0) {
        ALOGV("Audio codec1 path not specified");
        return;
    }
    if (!ReadFileToString(kCodec1Path, &file_contents)) {
        ALOGE("Unable to read codec1 state %s - %s", kCodec1Path, strerror(errno));
        return;
    }
    if (file_contents == "0") {
        return;
    } else {
        ALOGE("%s report hardware fail", kCodec1Path);
        HardwareFailed failed = {.hardwareType = HardwareFailed::HardwareType::CODEC,
                                 .hardwareLocation = 1,
                                 .errorCode = HardwareFailed::HardwareErrorCode::COMPLETE};
        stats_->reportHardwareFailed(failed);
    }
}

void SysfsCollector::reportSlowIoFromFile(const char *path,
                                          const SlowIo::IoOperation &operation_s) {
    std::string file_contents;
    if (path == nullptr || strlen(path) == 0) {
        ALOGV("slow_io path not specified");
        return;
    }
    if (!ReadFileToString(path, &file_contents)) {
        ALOGE("Unable to read slowio %s - %s", path, strerror(errno));
        return;
    } else {
        int32_t slow_io_count = 0;
        if (sscanf(file_contents.c_str(), "%d", &slow_io_count) != 1) {
            ALOGE("Unable to parse %s from file %s to int.", file_contents.c_str(), path);
        } else if (slow_io_count > 0) {
            SlowIo slowio = {.operation = operation_s, .count = slow_io_count};
            stats_->reportSlowIo(slowio);
        }
        // Clear the stats
        if (!android::base::WriteStringToFile("0", path, true)) {
            ALOGE("Unable to clear SlowIO entry %s - %s", path, strerror(errno));
        }
    }
}

/**
 * Check for slow IO operations.
 */
void SysfsCollector::logSlowIO() {
    reportSlowIoFromFile(kSlowioReadCntPath, SlowIo::IoOperation::READ);
    reportSlowIoFromFile(kSlowioWriteCntPath, SlowIo::IoOperation::WRITE);
    reportSlowIoFromFile(kSlowioUnmapCntPath, SlowIo::IoOperation::UNMAP);
    reportSlowIoFromFile(kSlowioSyncCntPath, SlowIo::IoOperation::SYNC);
}

/**
 * Report the last-detected impedance of left & right speakers.
 */
void SysfsCollector::logSpeakerImpedance(const std::shared_ptr<IStats> &stats_client) {
    std::string file_contents;
    if (kImpedancePath == nullptr || strlen(kImpedancePath) == 0) {
        ALOGV("Audio impedance path not specified");
        return;
    }
    if (!ReadFileToString(kImpedancePath, &file_contents)) {
        ALOGE("Unable to read impedance path %s", kImpedancePath);
        return;
    }

    float left, right;
    if (sscanf(file_contents.c_str(), "%g,%g", &left, &right) != 2) {
        ALOGE("Unable to parse speaker impedance %s", file_contents.c_str());
        return;
    }
    VendorSpeakerImpedance left_obj;
    left_obj.set_speaker_location(0);
    left_obj.set_impedance(static_cast<int32_t>(left * 1000));

    VendorSpeakerImpedance right_obj;
    right_obj.set_speaker_location(1);
    right_obj.set_impedance(static_cast<int32_t>(right * 1000));

    reportSpeakerImpedance(stats_client, left_obj);
    reportSpeakerImpedance(stats_client, right_obj);
}

/**
 * Report the Speech DSP state.
 */
void SysfsCollector::logSpeechDspStat() {
    std::string file_contents;
    if (kSpeechDspPath == nullptr || strlen(kSpeechDspPath) == 0) {
        ALOGV("Speech DSP path not specified");
        return;
    }
    if (!ReadFileToString(kSpeechDspPath, &file_contents)) {
        ALOGE("Unable to read speech dsp path %s", kSpeechDspPath);
        return;
    }

    int32_t uptime = 0, downtime = 0, crashcount = 0, recovercount = 0;
    if (sscanf(file_contents.c_str(), "%d,%d,%d,%d", &uptime, &downtime, &crashcount,
               &recovercount) != 4) {
        ALOGE("Unable to parse speech dsp stat %s", file_contents.c_str());
        return;
    }

    ALOGD("SpeechDSP uptime %d downtime %d crashcount %d recovercount %d", uptime, downtime,
          crashcount, recovercount);
    SpeechDspStat dspstat = {.totalUptimeMillis = uptime,
                             .totalDowntimeMillis = downtime,
                             .totalCrashCount = crashcount,
                             .totalRecoverCount = recovercount};

    stats_->reportSpeechDspStat(dspstat);
}

void SysfsCollector::logBatteryCapacity(const std::shared_ptr<IStats> &stats_client) {
    std::string file_contents;
    if (kBatteryCapacityCC == nullptr || strlen(kBatteryCapacityCC) == 0) {
        ALOGV("Battery Capacity CC path not specified");
        return;
    }
    if (kBatteryCapacityVFSOC == nullptr || strlen(kBatteryCapacityVFSOC) == 0) {
        ALOGV("Battery Capacity VFSOC path not specified");
        return;
    }
    int delta_cc_sum, delta_vfsoc_sum;
    if (!ReadFileToInt(kBatteryCapacityCC, &delta_cc_sum) ||
            !ReadFileToInt(kBatteryCapacityVFSOC, &delta_vfsoc_sum))
        return;

    // Load values array
    std::vector<VendorAtomValue> values(2);
    VendorAtomValue tmp;
    tmp.set<VendorAtomValue::intValue>(delta_cc_sum);
    values[BatteryCapacity::kDeltaCcSumFieldNumber - kVendorAtomOffset] = tmp;
    tmp.set<VendorAtomValue::intValue>(delta_vfsoc_sum);
    values[BatteryCapacity::kDeltaVfsocSumFieldNumber - kVendorAtomOffset] = tmp;

    // Send vendor atom to IStats HAL
    VendorAtom event = {.reverseDomainName = PixelAtoms::ReverseDomainNames().pixel(),
                        .atomId = PixelAtoms::Ids::BATTERY_CAPACITY,
                        .values = values};
    const ndk::ScopedAStatus ret = stats_client->reportVendorAtom(event);
    if (!ret.isOk())
        ALOGE("Unable to report ChargeStats to Stats service");
}

void SysfsCollector::logUFSLifetime(const std::shared_ptr<IStats> &stats_client) {
    std::string file_contents;
    if (kUFSLifetimeA == nullptr || strlen(kUFSLifetimeA) == 0) {
        ALOGV("UFS lifetimeA path not specified");
        return;
    }
    if (kUFSLifetimeB == nullptr || strlen(kUFSLifetimeB) == 0) {
        ALOGV("UFS lifetimeB path not specified");
        return;
    }
    if (kUFSLifetimeC == nullptr || strlen(kUFSLifetimeC) == 0) {
        ALOGV("UFS lifetimeC path not specified");
        return;
    }

    int lifetimeA = 0, lifetimeB = 0, lifetimeC = 0;
    if (!ReadFileToInt(kUFSLifetimeA, &lifetimeA) ||
        !ReadFileToInt(kUFSLifetimeB, &lifetimeB) ||
        !ReadFileToInt(kUFSLifetimeC, &lifetimeC)) {
        ALOGE("Unable to read UFS lifetime : %s", strerror(errno));
        return;
    }

    // Load values array
    std::vector<VendorAtomValue> values(3);
    VendorAtomValue tmp;
    tmp.set<VendorAtomValue::intValue>(lifetimeA);
    values[StorageUfsHealth::kLifetimeAFieldNumber - kVendorAtomOffset] = tmp;
    tmp.set<VendorAtomValue::intValue>(lifetimeB);
    values[StorageUfsHealth::kLifetimeBFieldNumber - kVendorAtomOffset] = tmp;
    tmp.set<VendorAtomValue::intValue>(lifetimeC);
    values[StorageUfsHealth::kLifetimeCFieldNumber - kVendorAtomOffset] = tmp;

    // Send vendor atom to IStats HAL
    VendorAtom event = {.reverseDomainName = PixelAtoms::ReverseDomainNames().pixel(),
                        .atomId = PixelAtoms::Ids::STORAGE_UFS_HEALTH,
                        .values = values};
    const ndk::ScopedAStatus ret = stats_client->reportVendorAtom(event);
    if (!ret.isOk()) {
        ALOGE("Unable to report UfsHealthStat to Stats service");
    }
}

void SysfsCollector::logUFSErrorStats(const std::shared_ptr<IStats> &stats_client) {
    int host_reset_count;

    if (kUFSHostResetPath == nullptr || strlen(kUFSHostResetPath) == 0) {
        ALOGV("UFS host reset count specified");
        return;
    }

    if (!ReadFileToInt(kUFSHostResetPath, &host_reset_count)) {
        ALOGE("Unable to read host reset count");
        return;
    }

    // Load values array
    std::vector<VendorAtomValue> values(1);
    VendorAtomValue tmp;
    tmp.set<VendorAtomValue::intValue>(host_reset_count);
    values[StorageUfsResetCount::kHostResetCountFieldNumber - kVendorAtomOffset] = tmp;

    // Send vendor atom to IStats HAL
    VendorAtom event = {.reverseDomainName = PixelAtoms::ReverseDomainNames().pixel(),
                        .atomId = PixelAtoms::Ids::UFS_RESET_COUNT,
                        .values = values};
    const ndk::ScopedAStatus ret = stats_client->reportVendorAtom(event);
    if (!ret.isOk()) {
        ALOGE("Unable to report UFS host reset count to Stats service");
    }
}

static std::string getUserDataBlock() {
    std::unique_ptr<std::FILE, int (*)(std::FILE*)> fp(setmntent("/proc/mounts", "re"), endmntent);
    if (fp == nullptr) {
        ALOGE("Error opening /proc/mounts");
        return "";
    }

    mntent* mentry;
    while ((mentry = getmntent(fp.get())) != nullptr) {
        if (strcmp(mentry->mnt_dir, "/data") == 0) {
            return std::string(basename(mentry->mnt_fsname));
        }
    }
    return "";
}

void SysfsCollector::logF2fsStats(const std::shared_ptr<IStats> &stats_client) {
    int dirty, free, cp_calls_fg, gc_calls_fg, moved_block_fg, vblocks;
    int cp_calls_bg, gc_calls_bg, moved_block_bg;

    if (kF2fsStatsPath == nullptr) {
        ALOGE("F2fs stats path not specified");
        return;
    }

    const std::string userdataBlock = getUserDataBlock();
    const std::string kF2fsStatsDir = kF2fsStatsPath + userdataBlock;

    if (!ReadFileToInt(kF2fsStatsDir + "/dirty_segments", &dirty)) {
        ALOGV("Unable to read dirty segments");
    }

    if (!ReadFileToInt(kF2fsStatsDir + "/free_segments", &free)) {
        ALOGV("Unable to read free segments");
    }

    if (!ReadFileToInt(kF2fsStatsDir + "/cp_foreground_calls", &cp_calls_fg)) {
        ALOGV("Unable to read cp_foreground_calls");
    }

    if (!ReadFileToInt(kF2fsStatsDir + "/cp_background_calls", &cp_calls_bg)) {
        ALOGV("Unable to read cp_background_calls");
    }

    if (!ReadFileToInt(kF2fsStatsDir + "/gc_foreground_calls", &gc_calls_fg)) {
        ALOGV("Unable to read gc_foreground_calls");
    }

    if (!ReadFileToInt(kF2fsStatsDir + "/gc_background_calls", &gc_calls_bg)) {
        ALOGV("Unable to read gc_background_calls");
    }

    if (!ReadFileToInt(kF2fsStatsDir + "/moved_blocks_foreground", &moved_block_fg)) {
        ALOGV("Unable to read moved_blocks_foreground");
    }

    if (!ReadFileToInt(kF2fsStatsDir + "/moved_blocks_background", &moved_block_bg)) {
        ALOGV("Unable to read moved_blocks_background");
    }

    if (!ReadFileToInt(kF2fsStatsDir + "/avg_vblocks", &vblocks)) {
        ALOGV("Unable to read avg_vblocks");
    }

    // Load values array
    std::vector<VendorAtomValue> values(9);
    VendorAtomValue tmp;
    tmp.set<VendorAtomValue::intValue>(dirty);
    values[F2fsStatsInfo::kDirtySegmentsFieldNumber - kVendorAtomOffset] = tmp;
    tmp.set<VendorAtomValue::intValue>(free);
    values[F2fsStatsInfo::kFreeSegmentsFieldNumber - kVendorAtomOffset] = tmp;
    tmp.set<VendorAtomValue::intValue>(cp_calls_fg);
    values[F2fsStatsInfo::kCpCallsFgFieldNumber - kVendorAtomOffset] = tmp;
    tmp.set<VendorAtomValue::intValue>(cp_calls_bg);
    values[F2fsStatsInfo::kCpCallsBgFieldNumber - kVendorAtomOffset] = tmp;
    tmp.set<VendorAtomValue::intValue>(gc_calls_fg);
    values[F2fsStatsInfo::kGcCallsFgFieldNumber - kVendorAtomOffset] = tmp;
    tmp.set<VendorAtomValue::intValue>(gc_calls_bg);
    values[F2fsStatsInfo::kGcCallsBgFieldNumber - kVendorAtomOffset] = tmp;
    tmp.set<VendorAtomValue::intValue>(moved_block_fg);
    values[F2fsStatsInfo::kMovedBlocksFgFieldNumber - kVendorAtomOffset] = tmp;
    tmp.set<VendorAtomValue::intValue>(moved_block_bg);
    values[F2fsStatsInfo::kMovedBlocksBgFieldNumber - kVendorAtomOffset] = tmp;
    tmp.set<VendorAtomValue::intValue>(vblocks);
    values[F2fsStatsInfo::kValidBlocksFieldNumber - kVendorAtomOffset] = tmp;

    // Send vendor atom to IStats HAL
    VendorAtom event = {.reverseDomainName = PixelAtoms::ReverseDomainNames().pixel(),
                        .atomId = PixelAtoms::Ids::F2FS_STATS,
                        .values = values};
    const ndk::ScopedAStatus ret = stats_client->reportVendorAtom(event);
    if (!ret.isOk()) {
        ALOGE("Unable to report F2fs stats to Stats service");
    }
}

void SysfsCollector::logF2fsCompressionInfo(const std::shared_ptr<IStats> &stats_client) {
    int compr_written_blocks, compr_saved_blocks, compr_new_inodes;

    if (kF2fsStatsPath == nullptr) {
        ALOGV("F2fs stats path not specified");
        return;
    }

    std::string userdataBlock = getUserDataBlock();

    std::string path = kF2fsStatsPath + (userdataBlock + "/compr_written_block");
    if (!ReadFileToInt(path, &compr_written_blocks)) {
        ALOGE("Unable to read compression written blocks");
        return;
    }

    path = kF2fsStatsPath + (userdataBlock + "/compr_saved_block");
    if (!ReadFileToInt(path, &compr_saved_blocks)) {
        ALOGE("Unable to read compression saved blocks");
        return;
    } else {
        if (!WriteStringToFile(std::to_string(0), path)) {
            ALOGE("Failed to write to file %s", path.c_str());
            return;
        }
    }

    path = kF2fsStatsPath + (userdataBlock + "/compr_new_inode");
    if (!ReadFileToInt(path, &compr_new_inodes)) {
        ALOGE("Unable to read compression new inodes");
        return;
    } else {
        if (!WriteStringToFile(std::to_string(0), path)) {
            ALOGE("Failed to write to file %s", path.c_str());
            return;
        }
    }

    // Load values array
    std::vector<VendorAtomValue> values(3);
    VendorAtomValue tmp;
    tmp.set<VendorAtomValue::intValue>(compr_written_blocks);
    values[F2fsCompressionInfo::kComprWrittenBlocksFieldNumber - kVendorAtomOffset] = tmp;
    tmp.set<VendorAtomValue::intValue>(compr_saved_blocks);
    values[F2fsCompressionInfo::kComprSavedBlocksFieldNumber - kVendorAtomOffset] = tmp;
    tmp.set<VendorAtomValue::intValue>(compr_new_inodes);
    values[F2fsCompressionInfo::kComprNewInodesFieldNumber - kVendorAtomOffset] = tmp;

    // Send vendor atom to IStats HAL
    VendorAtom event = {.reverseDomainName = PixelAtoms::ReverseDomainNames().pixel(),
                        .atomId = PixelAtoms::Ids::F2FS_COMPRESSION_INFO,
                        .values = values};
    const ndk::ScopedAStatus ret = stats_client->reportVendorAtom(event);
    if (!ret.isOk()) {
        ALOGE("Unable to report F2fs compression info to Stats service");
    }
}

void SysfsCollector::reportZramMmStat(const std::shared_ptr<IStats> &stats_client) {
    std::string file_contents;
    if (!kZramMmStatPath) {
        ALOGV("ZramMmStat path not specified");
        return;
    }

    if (!ReadFileToString(kZramMmStatPath, &file_contents)) {
        ALOGE("Unable to ZramMmStat %s - %s", kZramMmStatPath, strerror(errno));
        return;
    } else {
        int64_t orig_data_size = 0;
        int64_t compr_data_size = 0;
        int64_t mem_used_total = 0;
        int64_t mem_limit = 0;
        int64_t max_used_total = 0;
        int64_t same_pages = 0;
        int64_t pages_compacted = 0;
        int64_t huge_pages = 0;
        int64_t huge_pages_since_boot = 0;

        // huge_pages_since_boot may not exist according to kernel version.
        // only check if the number of collected data is equal or larger then 8
        if (sscanf(file_contents.c_str(),
                   "%" SCNd64 " %" SCNd64 " %" SCNd64 " %" SCNd64 " %" SCNd64 " %" SCNd64
                   " %" SCNd64 " %" SCNd64 " %" SCNd64,
                   &orig_data_size, &compr_data_size, &mem_used_total, &mem_limit, &max_used_total,
                   &same_pages, &pages_compacted, &huge_pages, &huge_pages_since_boot) < 8) {
            ALOGE("Unable to parse ZramMmStat %s from file %s to int.",
                    file_contents.c_str(), kZramMmStatPath);
        }

        // Load values array.
        // The size should be the same as the number of fields in ZramMmStat
        std::vector<VendorAtomValue> values(6);
        VendorAtomValue tmp;
        tmp.set<VendorAtomValue::intValue>(orig_data_size);
        values[ZramMmStat::kOrigDataSizeFieldNumber - kVendorAtomOffset] = tmp;
        tmp.set<VendorAtomValue::intValue>(compr_data_size);
        values[ZramMmStat::kComprDataSizeFieldNumber - kVendorAtomOffset] = tmp;
        tmp.set<VendorAtomValue::intValue>(mem_used_total);
        values[ZramMmStat::kMemUsedTotalFieldNumber - kVendorAtomOffset] = tmp;
        tmp.set<VendorAtomValue::intValue>(same_pages);
        values[ZramMmStat::kSamePagesFieldNumber - kVendorAtomOffset] = tmp;
        tmp.set<VendorAtomValue::intValue>(huge_pages);
        values[ZramMmStat::kHugePagesFieldNumber - kVendorAtomOffset] = tmp;

        // Skip the first data to avoid a big spike in this accumulated value.
        if (prev_huge_pages_since_boot_ == -1)
            tmp.set<VendorAtomValue::intValue>(0);
        else
            tmp.set<VendorAtomValue::intValue>(huge_pages_since_boot - prev_huge_pages_since_boot_);

        values[ZramMmStat::kHugePagesSinceBootFieldNumber - kVendorAtomOffset] = tmp;
        prev_huge_pages_since_boot_ = huge_pages_since_boot;

        // Send vendor atom to IStats HAL
        VendorAtom event = {.reverseDomainName = PixelAtoms::ReverseDomainNames().pixel(),
            .atomId = PixelAtoms::Ids::ZRAM_MM_STAT,
            .values = values};
        const ndk::ScopedAStatus ret = stats_client->reportVendorAtom(event);
        if (!ret.isOk())
            ALOGE("Zram Unable to report ZramMmStat to Stats service");
    }
}

void SysfsCollector::reportZramBdStat(const std::shared_ptr<IStats> &stats_client) {
    std::string file_contents;
    if (!kZramBdStatPath) {
        ALOGV("ZramBdStat path not specified");
        return;
    }

    if (!ReadFileToString(kZramBdStatPath, &file_contents)) {
        ALOGE("Unable to ZramBdStat %s - %s", kZramBdStatPath, strerror(errno));
        return;
    } else {
        int64_t bd_count = 0;
        int64_t bd_reads = 0;
        int64_t bd_writes = 0;

        if (sscanf(file_contents.c_str(), "%" SCNd64 " %" SCNd64 " %" SCNd64,
                                &bd_count, &bd_reads, &bd_writes) != 3) {
            ALOGE("Unable to parse ZramBdStat %s from file %s to int.",
                    file_contents.c_str(), kZramBdStatPath);
        }

        // Load values array
        std::vector<VendorAtomValue> values(3);
        VendorAtomValue tmp;
        tmp.set<VendorAtomValue::intValue>(bd_count);
        values[ZramBdStat::kBdCountFieldNumber - kVendorAtomOffset] = tmp;
        tmp.set<VendorAtomValue::intValue>(bd_reads);
        values[ZramBdStat::kBdReadsFieldNumber - kVendorAtomOffset] = tmp;
        tmp.set<VendorAtomValue::intValue>(bd_writes);
        values[ZramBdStat::kBdWritesFieldNumber - kVendorAtomOffset] = tmp;

        // Send vendor atom to IStats HAL
        VendorAtom event = {.reverseDomainName = PixelAtoms::ReverseDomainNames().pixel(),
            .atomId = PixelAtoms::Ids::ZRAM_BD_STAT,
            .values = values};
        const ndk::ScopedAStatus ret = stats_client->reportVendorAtom(event);
        if (!ret.isOk())
            ALOGE("Zram Unable to report ZramBdStat to Stats service");
    }
}

void SysfsCollector::logZramStats(const std::shared_ptr<IStats> &stats_client) {
    reportZramMmStat(stats_client);
    reportZramBdStat(stats_client);
}

void SysfsCollector::logBootStats(const std::shared_ptr<IStats> &stats_client) {
    int mounted_time_sec = 0;

    if (kF2fsStatsPath == nullptr) {
        ALOGE("F2fs stats path not specified");
        return;
    }

    std::string userdataBlock = getUserDataBlock();

    if (!ReadFileToInt(kF2fsStatsPath + (userdataBlock + "/mounted_time_sec"), &mounted_time_sec)) {
        ALOGV("Unable to read mounted_time_sec");
        return;
    }

    int fsck_time_ms = android::base::GetIntProperty("ro.boottime.init.fsck.data", 0);
    int checkpoint_time_ms = android::base::GetIntProperty("ro.boottime.init.mount.data", 0);

    if (fsck_time_ms == 0 && checkpoint_time_ms == 0) {
        ALOGV("Not yet initialized");
        return;
    }

    // Load values array
    std::vector<VendorAtomValue> values(3);
    VendorAtomValue tmp;
    tmp.set<VendorAtomValue::intValue>(mounted_time_sec);
    values[BootStatsInfo::kMountedTimeSecFieldNumber - kVendorAtomOffset] = tmp;
    tmp.set<VendorAtomValue::intValue>(fsck_time_ms / 1000);
    values[BootStatsInfo::kFsckTimeSecFieldNumber - kVendorAtomOffset] = tmp;
    tmp.set<VendorAtomValue::intValue>(checkpoint_time_ms / 1000);
    values[BootStatsInfo::kCheckpointTimeSecFieldNumber - kVendorAtomOffset] = tmp;

    // Send vendor atom to IStats HAL
    VendorAtom event = {.reverseDomainName = PixelAtoms::ReverseDomainNames().pixel(),
                        .atomId = PixelAtoms::Ids::BOOT_STATS,
                        .values = values};
    const ndk::ScopedAStatus ret = stats_client->reportVendorAtom(event);
    if (!ret.isOk()) {
        ALOGE("Unable to report Boot stats to Stats service");
    } else {
        log_once_reported = true;
    }
}

/**
 * Parse the output of /proc/vmstat or the sysfs having the same output format.
 * The map containing pairs of {field_string, data} will be returned.
 */
std::map<std::string, uint64_t> SysfsCollector::readVmStat(const char *path) {
    std::string file_contents;
    std::map<std::string, uint64_t> vmstat_data;

    if (path == nullptr) {
        ALOGI("vmstat path is not specified");
        return vmstat_data;
    }

    if (!android::base::ReadFileToString(path, &file_contents)) {
        ALOGE("Unable to read vmstat from %s, err: %s", path, strerror(errno));
        return vmstat_data;
    }

    std::istringstream data(file_contents);
    std::string line;
    while (std::getline(data, line)) {
        std::vector<std::string> words = android::base::Split(line, " ");
        if (words.size() != 2)
            continue;

        uint64_t i;
        if (!android::base::ParseUint(words[1], &i))
            continue;

        vmstat_data[words[0]] = i;
    }
    return vmstat_data;
}

uint64_t SysfsCollector::getIonTotalPools() {
    uint64_t res;

    if (kIonTotalPoolsPath == nullptr && kIonTotalPoolsPathForLegacy == nullptr) {
        ALOGI("ion_total_pools path is not specified");
        return 0;
    }

    if (!ReadFileToUint(kIonTotalPoolsPathForLegacy, &res) || (res == 0)) {
        if (!ReadFileToUint(kIonTotalPoolsPath, &res)) {
            return 0;
        }
    }

    return res;
}

/**
 * fillAtomValues() is used to copy Mm metrics to values
 * metrics_info: This is a vector of MmMetricsInfo {field_string, atom_key, update_diff}
 *               field_string is used to get the data from mm_metrics.
 *               atom_key is the position where the data should be put into values.
 *               update_diff will be true if this is an accumulated data.
 *               metrics_info may have multiple entries with the same atom_key,
 *               e.g. workingset_refault and workingset_refault_file.
 * mm_metrics: This map contains pairs of {field_string, cur_value} collected
 *             from /proc/vmstat or the sysfs for the pixel specific metrics.
 *             e.g. {"nr_free_pages", 200000}
 *             Some data in mm_metrics are accumulated, e.g. pswpin.
 *             We upload the difference instead of the accumulated value
 *             when update_diff of the field is true.
 * prev_mm_metrics: The pointer to the metrics we collected last time.
 * atom_values: The atom values that will be reported later.
 */
void SysfsCollector::fillAtomValues(const std::vector<MmMetricsInfo> &metrics_info,
                                    const std::map<std::string, uint64_t> &mm_metrics,
                                    std::map<std::string, uint64_t> *prev_mm_metrics,
                                    std::vector<VendorAtomValue> *atom_values) {
    VendorAtomValue tmp;
    tmp.set<VendorAtomValue::longValue>(0);
    // resize atom_values to add all fields defined in metrics_info
    int max_idx = 0;
    for (auto &entry : metrics_info) {
        if (max_idx < entry.atom_key)
            max_idx = entry.atom_key;
    }
    int size = max_idx - kVendorAtomOffset + 1;
    if (atom_values->size() < size)
        atom_values->resize(size, tmp);

    for (auto &entry : metrics_info) {
        int atom_idx = entry.atom_key - kVendorAtomOffset;

        auto data = mm_metrics.find(entry.name);
        if (data == mm_metrics.end())
            continue;

        uint64_t cur_value = data->second;
        uint64_t prev_value = 0;
        if (prev_mm_metrics->size() != 0) {
            auto prev_data = prev_mm_metrics->find(entry.name);
            if (prev_data != prev_mm_metrics->end())
                prev_value = prev_data->second;
        }

        if (entry.update_diff) {
            tmp.set<VendorAtomValue::longValue>(cur_value - prev_value);
        } else {
            tmp.set<VendorAtomValue::longValue>(cur_value);
        }
        (*atom_values)[atom_idx] = tmp;
    }
    (*prev_mm_metrics) = mm_metrics;
}

void SysfsCollector::logPixelMmMetricsPerHour(const std::shared_ptr<IStats> &stats_client) {
    std::map<std::string, uint64_t> vmstat = readVmStat(kVmstatPath);
    if (vmstat.size() == 0)
        return;

    uint64_t ion_total_pools = getIonTotalPools();

    std::vector<VendorAtomValue> values;
    bool is_first_atom = (prev_hour_vmstat_.size() == 0) ? true : false;
    fillAtomValues(kMmMetricsPerHourInfo, vmstat, &prev_hour_vmstat_, &values);

    // resize values to add the following fields
    VendorAtomValue tmp;
    tmp.set<VendorAtomValue::longValue>(0);
    int size = PixelMmMetricsPerHour::kIonTotalPoolsFieldNumber - kVendorAtomOffset + 1;
    if (values.size() < size) {
        values.resize(size, tmp);
    }
    tmp.set<VendorAtomValue::longValue>(ion_total_pools);
    values[PixelMmMetricsPerHour::kIonTotalPoolsFieldNumber - kVendorAtomOffset] = tmp;

    // Don't report the first atom to avoid big spike in accumulated values.
    if (!is_first_atom) {
        // Send vendor atom to IStats HAL
        VendorAtom event = {.reverseDomainName = PixelAtoms::ReverseDomainNames().pixel(),
                            .atomId = PixelAtoms::Ids::PIXEL_MM_METRICS_PER_HOUR,
                            .values = values};
        const ndk::ScopedAStatus ret = stats_client->reportVendorAtom(event);
        if (!ret.isOk())
            ALOGE("Unable to report PixelMmMetricsPerHour to Stats service");
    }
}

void SysfsCollector::logPixelMmMetricsPerDay(const std::shared_ptr<IStats> &stats_client) {
    std::map<std::string, uint64_t> vmstat = readVmStat(kVmstatPath);
    if (vmstat.size() == 0)
        return;

    std::vector<VendorAtomValue> values;
    bool is_first_atom = (prev_day_vmstat_.size() == 0) ? true : false;
    fillAtomValues(kMmMetricsPerDayInfo, vmstat, &prev_day_vmstat_, &values);

    // Don't report the first atom to avoid big spike in accumulated values.
    if (!is_first_atom) {
        // Send vendor atom to IStats HAL
        VendorAtom event = {.reverseDomainName = PixelAtoms::ReverseDomainNames().pixel(),
                            .atomId = PixelAtoms::Ids::PIXEL_MM_METRICS_PER_DAY,
                            .values = values};
        const ndk::ScopedAStatus ret = stats_client->reportVendorAtom(event);
        if (!ret.isOk())
            ALOGE("Unable to report MEMORY_MANAGEMENT_INFO to Stats service");
    }
}

void SysfsCollector::logPerDay() {
    stats_ = android::frameworks::stats::V1_0::IStats::tryGetService();
    if (!stats_) {
        ALOGE("Unable to connect to Stats service");
    } else {
        logBatteryChargeCycles();
        logCodec1Failed();
        logCodecFailed();
        logSlowIO();
        logSpeechDspStat();
        stats_.clear();
    }

    const std::shared_ptr<IStats> stats_client = getStatsService();
    if (!stats_client) {
        ALOGE("Unable to get AIDL Stats service");
        return;
    }
    // Collect once per service init; can be multiple due to service reinit
    if (!log_once_reported) {
        logBootStats(stats_client);
    }
    logBatteryCapacity(stats_client);
    logBatteryEEPROM();
    logF2fsStats(stats_client);
    logF2fsCompressionInfo(stats_client);
    logPixelMmMetricsPerDay(stats_client);
    logSpeakerImpedance(stats_client);
    logUFSLifetime(stats_client);
    logUFSErrorStats(stats_client);
    logZramStats(stats_client);
}

void SysfsCollector::logPerHour() {
    const std::shared_ptr<IStats> stats_client = getStatsService();
    if (!stats_client) {
        ALOGE("Unable to get AIDL Stats service");
        return;
    }

    logPixelMmMetricsPerHour(stats_client);
}

/**
 * Loop forever collecting stats from sysfs nodes and reporting them via
 * IStats.
 */
void SysfsCollector::collect(void) {
    int timerfd = timerfd_create(CLOCK_BOOTTIME, 0);
    if (timerfd < 0) {
        ALOGE("Unable to create timerfd - %s", strerror(errno));
        return;
    }

    // Sleep for 30 seconds on launch to allow codec driver to load.
    sleep(30);

    // Collect first set of stats on boot.
    logPerHour();
    logPerDay();

    // Set an one-hour timer.
    struct itimerspec period;
    const int kSecondsPerHour = 60 * 60;
    int hours = 0;
    period.it_interval.tv_sec = kSecondsPerHour;
    period.it_interval.tv_nsec = 0;
    period.it_value.tv_sec = kSecondsPerHour;
    period.it_value.tv_nsec = 0;

    if (timerfd_settime(timerfd, 0, &period, NULL)) {
        ALOGE("Unable to set one hour timer - %s", strerror(errno));
        return;
    }

    while (1) {
        int readval;
        do {
            char buf[8];
            errno = 0;
            readval = read(timerfd, buf, sizeof(buf));
        } while (readval < 0 && errno == EINTR);
        if (readval < 0) {
            ALOGE("Timerfd error - %s\n", strerror(errno));
            return;
        }

        hours++;
        logPerHour();
        if (hours == 24) {
            // Collect stats every 24hrs after.
            logPerDay();
            hours = 0;
        }
    }
}

}  // namespace pixel
}  // namespace google
}  // namespace hardware
}  // namespace android
