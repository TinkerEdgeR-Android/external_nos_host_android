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

#include <chrono>
#include <iomanip>
#include <iostream>
#include <random>

#include <android-base/endian.h>
#include <android-base/logging.h>
#include <android-base/parseint.h>
#include <binder/IServiceManager.h>
#include <binder/ProcessState.h>

#include <android/hardware/citadel/ICitadeld.h>

#include <application.h>
#include <app_nugget.h>
#include <nos/debug.h>
#include <nos/CitadeldProxyClient.h>

using ::android::defaultServiceManager;
using ::android::sp;
using ::android::ProcessState;
using ::android::base::ParseUint;

using ::android::hardware::citadel::ICitadeld;

using ::nos::CitadeldProxyClient;
using ::nos::NuggetClientInterface;
using ::nos::StatusCodeString;

namespace {

std::string ToHexString(uint32_t value) {
    std::stringstream ss;
    ss << "0x" << std::hex << std::setfill('0') << std::setw(8) << value;
    return ss.str();
}

/* Read a value from a Citadel register */
bool ReadRegister(NuggetClientInterface& client, uint32_t address, uint32_t* value) {
    // Build request
    std::vector<uint8_t> buffer(sizeof(uint32_t));
    *reinterpret_cast<uint32_t*>(buffer.data()) = address;

    // Send request
    const uint32_t status = client.CallApp(APP_ID_NUGGET, NUGGET_PARAM_READ32, buffer, &buffer);
    if (status != APP_SUCCESS) {
        std::cerr << " Failed to read register: " << StatusCodeString(status)
                  << "(" << status << ")\n";
        return false;
    }

    // Process response
    if (buffer.size() != sizeof(*value)) {
        std::cerr << "Nugget gave us " << buffer.size() << " bytes instead of 4\n";
        return false;
    }
    *value = *reinterpret_cast<uint32_t*>(buffer.data());
    return true;
}

/* Write a value to a Citadel register */
bool WriteRegister(NuggetClientInterface& client, uint32_t address, uint32_t value) {
    // Build request
    std::vector<uint8_t> buffer(sizeof(nugget_app_write32));
    nugget_app_write32* w32 = reinterpret_cast<nugget_app_write32*>(buffer.data());
    w32->address = address;
    w32->value = value;

    // Send request
    const uint32_t status = client.CallApp(APP_ID_NUGGET, NUGGET_PARAM_WRITE32, buffer, nullptr);
    if (status != APP_SUCCESS) {
        std::cerr << " Failed to write register: " << StatusCodeString(status)
                  << "(" << status << ")\n";
        return false;
    }

    return true;
}

/*
 * Read a register and check the value is in the specified bounds. The bounds
 * are inclusive.
 */
bool CheckRegisterInRange(NuggetClientInterface& client, uint32_t address,
                          uint32_t min, uint32_t max){
    uint32_t value;
    if (!ReadRegister(client, address, &value)) {
        std::cerr << "Failed to read " << ToHexString(address) << "\n";
        return false;
    }
    if (value < min || value > max) {
        std::cerr << ToHexString(address) << " out of range: " << ToHexString(value) << "\n";
        return false;
    }
    return true;
}

/*
 * Read a register and check the value is ouside the specified bounds. The
 * bounds are inclusive.
 */
bool CheckRegisterNotInRange(NuggetClientInterface& client, uint32_t address,
                             uint32_t min, uint32_t max){
    uint32_t value;
    if (!ReadRegister(client, address, &value)) {
        std::cerr << "Failed to read " << ToHexString(address) << "\n";
        return false;
    }
    if (value >= min && value <= max) {
        std::cerr << ToHexString(address) << " in illegal range: " << ToHexString(value) << "\n";
        return false;
    }
    return true;
}

/* Have Nugget report the number of cycles it has been running for. */
bool CyclesSinceBoot(NuggetClientInterface & client, uint32_t* cycles) {
    std::vector<uint8_t> buffer;
    buffer.reserve(sizeof(uint32_t));
    if (client.CallApp(APP_ID_NUGGET, NUGGET_PARAM_CYCLES_SINCE_BOOT,
                       buffer, &buffer) != app_status::APP_SUCCESS) {
        std::cerr << "Failed to get cycles since boot\n";
        return false;
    }
    if (buffer.size() != sizeof(uint32_t)) {
        std::cerr << "Unexpected size of cycle count!\n";
        return false;
    }
    *cycles = le32toh(*reinterpret_cast<uint32_t *>(buffer.data()));
    return true;
}

/*
 * The current implementation of the test writes random vales to registers and
 * reads them back. This lets us check the correct values were sent across the
 * channel.
 * TODO(b/65067435): Replace with less intrusive calls.
 */
int CmdStressSpi(NuggetClientInterface& client, char** params) {
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<uint16_t> dis;

    // Parse transaction count
    uint32_t count;
    if (!ParseUint(params[0], &count)) {
        std::cerr << "Invalid count: \"" << params[0] << "\"\n";
        return EXIT_FAILURE;
    }
    if (count % 2 != 0) {
        // Make sure it is even to allow set then check tests
        std::cerr << "Count must be even\n";
        return EXIT_FAILURE;
    }

    // Each iteration does 2 transactions
    count /= 2;
    for (uint32_t i = 0; i < count; ++i) {
        constexpr uint32_t PMU_PWRDN_SCRATCH16 = 0x400000d4; // Scratch 16

        // Write a random value (SPI transaction 1)
        const uint32_t value = dis(gen);
        if (!WriteRegister(client, PMU_PWRDN_SCRATCH16, value)) {
            std::cerr << "Failed to write " << ToHexString(value) << " to scratch register 16\n";
            return EXIT_FAILURE;
        }

        // Read back the value (SPI transaction 2)
        uint32_t check_value;
        if (!ReadRegister(client, PMU_PWRDN_SCRATCH16, &check_value)) {
            std::cerr << "Failed to read scratch register 16\n";
            return EXIT_FAILURE;
        }

        // Check the value wasn't corrupted
        if (check_value != value) {
            std::cerr << "Fail: expected to read " << ToHexString(value) << " but got "
                      << ToHexString(check_value);
            return EXIT_FAILURE;
        }
    }

    return EXIT_SUCCESS;
}

/*
 * The current implementation directly reads some registers and checks they
 * contain valid values.
 * TODO(b/65067435): Replace with less intrusive calls.
 */
int CmdHealthCheck(NuggetClientInterface& client) {
    int ret = EXIT_SUCCESS;

    constexpr uint32_t TRNG_FSM_STATE = 0x4041002c;
    if (!CheckRegisterNotInRange(client, TRNG_FSM_STATE, 0x1, 0x1)) {
        std::cerr << "TRNG_FSM_STATE is not healthy\n";
        ret = EXIT_FAILURE;
    }

    constexpr uint32_t TRNG_MAX_VALUE = 0x40410040;
    if (!CheckRegisterInRange(client, TRNG_MAX_VALUE, 0x0, 0xfffe)) {
        std::cerr << "TRNG_MAX_VALUE is not healthy\n";
        ret = EXIT_FAILURE;
    }

    constexpr uint32_t TRNG_MIN_VALUE = 0x40410044;
    if (!CheckRegisterInRange(client, TRNG_MIN_VALUE, 0x10, 0x200)) {
        std::cerr << "TRNG_MIN_VALUE is not healthy\n";
        ret = EXIT_FAILURE;
    }

    constexpr uint32_t TRNG_CUR_NUM_ONES = 0x4041008c;
    if (!CheckRegisterInRange(client, TRNG_CUR_NUM_ONES, 0x334, 0x4cc)) {
        std::cerr << "TRNG_CUR_NUM_ONES is not healthy\n";
        ret = EXIT_FAILURE;
    }

    constexpr uint32_t PMU_RSTSRC = 0x40000000;
    if (!CheckRegisterInRange(client, PMU_RSTSRC, 0x0, 0x3)) {
        std::cerr << "PMU_RSTSRC is not healthy\n";
        ret = EXIT_FAILURE;
    }

    constexpr uint32_t GLOBALSEC_ALERT_INTR_STS0 = 0x40104004;
    if (!CheckRegisterInRange(client, GLOBALSEC_ALERT_INTR_STS0, 0x0, 0x0)) {
        std::cerr << "GLOBALSEC_ALERT_INTR_STS0 is not healthy\n";
        ret = EXIT_FAILURE;
    }

    constexpr uint32_t GLOBALSEC_ALERT_INTR_STS1 = 0x40104008;
    if (!CheckRegisterInRange(client, GLOBALSEC_ALERT_INTR_STS1, 0x0, 0x0)) {
        std::cerr << "GLOBALSEC_ALERT_INTR_STS1 is not healthy\n";
        ret = EXIT_FAILURE;
    }

    return ret;
}

int CmdReset(CitadeldProxyClient& client) {
    // Request a hard reset of the device
    bool success = false;
    if (!client.Citadeld().reset(&success).isOk()) {
        std::cerr << "Failed to talk to citadeld\n";
        return EXIT_FAILURE;
    }
    if (!success) {
        std::cerr << "Failed to reset Citadel\n";
        return EXIT_FAILURE;
    }

    // Check the cycle count which should have been reset by the reset. It
    // should be equivalent to around the time we just waited for but give it a
    // 5% margin.
    uint32_t cycles;
    if (!CyclesSinceBoot(client, &cycles)) {
        return EXIT_FAILURE;
    }
    const auto uptime = std::chrono::microseconds(cycles);
    const auto bringup = std::chrono::milliseconds(100);
    const auto limit = std::chrono::duration_cast<std::chrono::microseconds>(bringup) * 105 / 100;
    if (uptime > limit) {
        LOG(ERROR) << "Uptime is " << uptime.count()
                   << "us but is expected to be less than " << limit.count() << "us\n";
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}

} // namespace

/**
 * This utility invokes the citadeld device checks and reports the results.
 */
int main(int argc, char** argv) {
    // Connect to citadeld
    CitadeldProxyClient citadeldProxy;
    citadeldProxy.Open();
    if (!citadeldProxy.IsOpen()) {
      std::cerr << "Failed to open citadeld client\n";
    }

    if (argc >= 2) {
        const std::string command(argv[1]);
        char** params = &argv[2];
        const int param_count = argc - 2;

        if (command == "stress-spi" && param_count == 1) {
            return CmdStressSpi(citadeldProxy, params);
        }
        if (command == "health-check" && param_count == 0) {
            return CmdHealthCheck(citadeldProxy);
        }
        if (command == "reset" && param_count == 0) {
            return CmdReset(citadeldProxy);
        }
    }

    // Print usage if all else failed
    std::cerr << "Usage:\n";
    std::cerr << "  " << argv[0] << " stress-spi [count] -- perform count SPI transactions\n";
    std::cerr << "  " << argv[0] << " health-check       -- check Citadel's vital signs\n";
    std::cerr << "  " << argv[0] << " reset              -- pull Citadel's reset line\n";
    std::cerr << "\n";
    std::cerr << "Returns 0 on success and non-0 if any failure were detected.\n";
    return EXIT_FAILURE;
}
