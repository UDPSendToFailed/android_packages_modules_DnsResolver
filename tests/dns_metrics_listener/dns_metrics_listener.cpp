/**
 * Copyright (c) 2019, The Android Open Source Project
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
 */

#include "dns_metrics_listener.h"

#include <thread>

#include <android-base/chrono_utils.h>
#include <android-base/format.h>

namespace android {
namespace net {
namespace metrics {

using android::base::ScopedLockAssertion;
using std::chrono::milliseconds;

constexpr milliseconds kRetryIntervalMs{20};
constexpr milliseconds kEventTimeoutMs{5000};

bool DnsMetricsListener::DnsEvent::operator==(const DnsMetricsListener::DnsEvent& o) const {
    return std::tie(netId, eventType, returnCode, hostname, ipAddresses, ipAddressesCount) ==
           std::tie(o.netId, o.eventType, o.returnCode, o.hostname, o.ipAddresses,
                    o.ipAddressesCount);
}

std::ostream& operator<<(std::ostream& os, const DnsMetricsListener::DnsEvent& data) {
    return os << fmt::format("[{}, {}, {}, {}, [{}], {}]", data.netId, data.eventType,
                             data.returnCode, data.hostname, fmt::join(data.ipAddresses, ", "),
                             data.ipAddressesCount);
}

::ndk::ScopedAStatus DnsMetricsListener::onNat64PrefixEvent(int32_t netId, bool added,
                                                            const std::string& prefixString,
                                                            int32_t /*prefixLength*/) {
    std::lock_guard lock(mMutex);
    mUnexpectedNat64PrefixUpdates++;
    if (netId == mNetId) mNat64Prefix = added ? prefixString : "";
    return ::ndk::ScopedAStatus::ok();
}

::ndk::ScopedAStatus DnsMetricsListener::onPrivateDnsValidationEvent(
        int32_t netId, const std::string& ipAddress, const std::string& /*hostname*/,
        bool validated) {
    {
        std::lock_guard lock(mMutex);
        // keep updating the server to have latest validation status.
        mValidationRecords.insert_or_assign({netId, ipAddress}, validated);
    }
    mCv.notify_one();
    return ::ndk::ScopedAStatus::ok();
}

::ndk::ScopedAStatus DnsMetricsListener::onDnsEvent(int32_t netId, int32_t eventType,
                                                    int32_t returnCode, int32_t /*latencyMs*/,
                                                    const std::string& hostname,
                                                    const std::vector<std::string>& ipAddresses,
                                                    int32_t ipAddressesCount, int32_t /*uid*/) {
    std::lock_guard lock(mMutex);
    if (netId == mNetId) {
        mDnsEventRecords.push(
                {netId, eventType, returnCode, hostname, ipAddresses, ipAddressesCount});
    }
    return ::ndk::ScopedAStatus::ok();
}

bool DnsMetricsListener::waitForNat64Prefix(ExpectNat64PrefixStatus status, milliseconds timeout) {
    android::base::Timer t;
    while (t.duration() < timeout) {
        {
            std::lock_guard lock(mMutex);
            if ((status == EXPECT_FOUND && !mNat64Prefix.empty()) ||
                (status == EXPECT_NOT_FOUND && mNat64Prefix.empty())) {
                mUnexpectedNat64PrefixUpdates--;
                return true;
            }
        }
        std::this_thread::sleep_for(kRetryIntervalMs);
    }
    return false;
}

bool DnsMetricsListener::waitForPrivateDnsValidation(const std::string& serverAddr,
                                                     const bool validated) {
    std::unique_lock lock(mMutex);
    return mCv.wait_for(lock, kEventTimeoutMs, [&]() REQUIRES(mMutex) {
        return findAndRemoveValidationRecord({mNetId, serverAddr}, validated);
    });
}

bool DnsMetricsListener::findAndRemoveValidationRecord(const ServerKey& key, const bool value) {
    auto it = mValidationRecords.find(key);
    if (it != mValidationRecords.end() && it->second == value) {
        mValidationRecords.erase(it);
        return true;
    }
    return false;
}

std::optional<DnsMetricsListener::DnsEvent> DnsMetricsListener::popDnsEvent() {
    // Wait until the queue is not empty or timeout.
    android::base::Timer t;
    while (t.duration() < milliseconds{1000}) {
        {
            std::lock_guard lock(mMutex);
            if (!mDnsEventRecords.empty()) break;
        }
        std::this_thread::sleep_for(kRetryIntervalMs);
    }

    std::lock_guard lock(mMutex);
    if (mDnsEventRecords.empty()) return std::nullopt;

    auto ret = mDnsEventRecords.front();
    mDnsEventRecords.pop();
    return ret;
}

}  // namespace metrics
}  // namespace net
}  // namespace android
