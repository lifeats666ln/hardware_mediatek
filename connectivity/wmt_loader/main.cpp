/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#define LOG_TAG "wmt_loader"

#include <android-base/logging.h>
#include <android-base/properties.h>
#include <android-base/stringprintf.h>
#include <android-base/unique_fd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

using android::base::SetProperty;
using android::base::StringPrintf;
using android::base::unique_fd;

namespace {

constexpr char kWmtDetectDev[] = "/dev/wmtdetect";

constexpr char kChipIdProp[] = "persist.vendor.connsys.chipid";
constexpr char kDriverReadyProp[] = "vendor.connsys.driver.ready";

constexpr unsigned kIocMagic = 'w';
#define COMBO_IOCTL_SET_CHIP_ID _IOW(kIocMagic, 1, int)
#define COMBO_IOCTL_GET_SOC_CHIP_ID _IOR(kIocMagic, 3, int)
#define COMBO_IOCTL_DO_MODULE_INIT _IOR(kIocMagic, 4, int)
#define COMBO_IOCTL_GET_ADIE_CHIP_ID _IOR(kIocMagic, 9, int)
#define COMBO_IOCTL_CONNSYS_SOC_HW_INIT _IOR(kIocMagic, 10, int)

constexpr std::array kAdieChipIdProps{
        "vendor.connsys.adie.chipid",    "vendor.connsys.wifi.adie.chipid",
        "vendor.connsys.bt.adie.chipid", "vendor.connsys.gps.adie.chipid",
        "vendor.connsys.fm.adie.chipid",
};

void SetHexProperty(const char* prop, int id) {
    const std::string value = StringPrintf("0x%04x", id);
    if (SetProperty(prop, value)) {
        LOG(INFO) << "Set " << prop << " to " << value;
    } else {
        LOG(ERROR) << "Failed to set " << prop << " to " << value;
    }
}

unique_fd OpenWmtDetect() {
    for (int attempt = 1;; ++attempt) {
        unique_fd fd(open(kWmtDetectDev, O_RDWR | O_NOCTTY | O_CLOEXEC));
        if (fd.get() >= 0) {
            return fd;
        }
        if (attempt == 1 || attempt % 20 == 0) {  // ~6 s between logs
            LOG(INFO) << "Can't open " << kWmtDetectDev << " (attempt " << attempt
                      << "): " << strerror(errno);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
    }
}

bool SocHwInit(int fd) {
    if (ioctl(fd, COMBO_IOCTL_CONNSYS_SOC_HW_INIT, 0) != 0) {
        PLOG(ERROR) << "Connsys hw init failed";
        return false;
    }
    return true;
}

int SetChipId(int fd) {
    const int id = ioctl(fd, COMBO_IOCTL_GET_SOC_CHIP_ID, 0);
    if (id < 0) {
        PLOG(ERROR) << "Get chip id failed";
        return id;
    }
    LOG(INFO) << StringPrintf("Chip id: 0x%x", id);
    SetHexProperty(kChipIdProp, id);
    ioctl(fd, COMBO_IOCTL_SET_CHIP_ID, id);
    return id;
}

bool DoModuleInit(int fd, int chip_id) {
    if (ioctl(fd, COMBO_IOCTL_DO_MODULE_INIT, chip_id) != 0) {
        PLOG(ERROR) << "Kernel module init failed";
        return false;
    }
    return true;
}

void SetAdieChipIds(int fd) {
    const int id = ioctl(fd, COMBO_IOCTL_GET_ADIE_CHIP_ID, 0);
    if (id < 0) {
        PLOG(ERROR) << "Get a-die chip id failed";
        return;
    }
    for (const char* prop : kAdieChipIdProps) {
        SetHexProperty(prop, id);
    }
}

}  // namespace

int main() {
    unique_fd fd = OpenWmtDetect();

    if (!SocHwInit(fd.get())) {
        return EXIT_FAILURE;
    }

    const int chip_id = SetChipId(fd.get());
    if (chip_id < 0 || !DoModuleInit(fd.get(), chip_id)) {
        return EXIT_FAILURE;
    }

    SetAdieChipIds(fd.get());

    if (!SetProperty(kDriverReadyProp, "yes")) {
        LOG(ERROR) << "Failed to set " << kDriverReadyProp;
    }

    return EXIT_SUCCESS;
}
