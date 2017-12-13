/*
* Copyright (C) 2017 The Android Open Source Project
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

#define DEBUG true  // STOPSHIP if true
#include "Log.h"

#include "GaugeMetricProducer.h"
#include "guardrail/StatsdStats.h"
#include "stats_util.h"

#include <cutils/log.h>
#include <limits.h>
#include <stdlib.h>

using android::util::FIELD_COUNT_REPEATED;
using android::util::FIELD_TYPE_BOOL;
using android::util::FIELD_TYPE_FLOAT;
using android::util::FIELD_TYPE_INT32;
using android::util::FIELD_TYPE_INT64;
using android::util::FIELD_TYPE_MESSAGE;
using android::util::FIELD_TYPE_STRING;
using android::util::ProtoOutputStream;
using std::map;
using std::string;
using std::unordered_map;
using std::vector;

namespace android {
namespace os {
namespace statsd {

// for StatsLogReport
const int FIELD_ID_NAME = 1;
const int FIELD_ID_START_REPORT_NANOS = 2;
const int FIELD_ID_END_REPORT_NANOS = 3;
const int FIELD_ID_GAUGE_METRICS = 8;
// for GaugeMetricDataWrapper
const int FIELD_ID_DATA = 1;
// for GaugeMetricData
const int FIELD_ID_DIMENSION = 1;
const int FIELD_ID_BUCKET_INFO = 2;
// for KeyValuePair
const int FIELD_ID_KEY = 1;
const int FIELD_ID_VALUE_STR = 2;
const int FIELD_ID_VALUE_INT = 3;
const int FIELD_ID_VALUE_BOOL = 4;
const int FIELD_ID_VALUE_FLOAT = 5;
// for GaugeBucketInfo
const int FIELD_ID_START_BUCKET_NANOS = 1;
const int FIELD_ID_END_BUCKET_NANOS = 2;
const int FIELD_ID_GAUGE = 3;

GaugeMetricProducer::GaugeMetricProducer(const ConfigKey& key, const GaugeMetric& metric,
                                         const int conditionIndex,
                                         const sp<ConditionWizard>& wizard, const int pullTagId,
                                         const int64_t startTimeNs)
    : MetricProducer(key, startTimeNs, conditionIndex, wizard),
      mMetric(metric),
      mPullTagId(pullTagId) {
    if (metric.has_bucket() && metric.bucket().has_bucket_size_millis()) {
        mBucketSizeNs = metric.bucket().bucket_size_millis() * 1000 * 1000;
    } else {
        mBucketSizeNs = kDefaultGaugemBucketSizeNs;
    }

    // TODO: use UidMap if uid->pkg_name is required
    mDimension.insert(mDimension.begin(), metric.dimension().begin(), metric.dimension().end());

    if (metric.links().size() > 0) {
        mConditionLinks.insert(mConditionLinks.begin(), metric.links().begin(),
                               metric.links().end());
        mConditionSliced = true;
    }

    // Kicks off the puller immediately.
    if (mPullTagId != -1) {
        mStatsPullerManager.RegisterReceiver(mPullTagId, this,
                                             metric.bucket().bucket_size_millis());
    }

    VLOG("metric %s created. bucket size %lld start_time: %lld", metric.name().c_str(),
         (long long)mBucketSizeNs, (long long)mStartTimeNs);
}

GaugeMetricProducer::~GaugeMetricProducer() {
    VLOG("~GaugeMetricProducer() called");
    if (mPullTagId != -1) {
        mStatsPullerManager.UnRegisterReceiver(mPullTagId, this);
    }
}

void GaugeMetricProducer::onDumpReportLocked(const uint64_t dumpTimeNs,
                                             ProtoOutputStream* protoOutput) {
    VLOG("gauge metric %s dump report now...", mMetric.name().c_str());

    flushIfNeededLocked(dumpTimeNs);

    protoOutput->write(FIELD_TYPE_STRING | FIELD_ID_NAME, mMetric.name());
    protoOutput->write(FIELD_TYPE_INT64 | FIELD_ID_START_REPORT_NANOS, (long long)mStartTimeNs);
    long long protoToken = protoOutput->start(FIELD_TYPE_MESSAGE | FIELD_ID_GAUGE_METRICS);

    for (const auto& pair : mPastBuckets) {
        const HashableDimensionKey& hashableKey = pair.first;
        auto it = mDimensionKeyMap.find(hashableKey);
        if (it == mDimensionKeyMap.end()) {
            ALOGE("Dimension key %s not found?!?! skip...", hashableKey.c_str());
            continue;
        }

        VLOG("  dimension key %s", hashableKey.c_str());
        long long wrapperToken =
                protoOutput->start(FIELD_TYPE_MESSAGE | FIELD_COUNT_REPEATED | FIELD_ID_DATA);

        // First fill dimension (KeyValuePairs).
        for (const auto& kv : it->second) {
            long long dimensionToken = protoOutput->start(
                    FIELD_TYPE_MESSAGE | FIELD_COUNT_REPEATED | FIELD_ID_DIMENSION);
            protoOutput->write(FIELD_TYPE_INT32 | FIELD_ID_KEY, kv.key());
            if (kv.has_value_str()) {
                protoOutput->write(FIELD_TYPE_STRING | FIELD_ID_VALUE_STR, kv.value_str());
            } else if (kv.has_value_int()) {
                protoOutput->write(FIELD_TYPE_INT64 | FIELD_ID_VALUE_INT, kv.value_int());
            } else if (kv.has_value_bool()) {
                protoOutput->write(FIELD_TYPE_BOOL | FIELD_ID_VALUE_BOOL, kv.value_bool());
            } else if (kv.has_value_float()) {
                protoOutput->write(FIELD_TYPE_FLOAT | FIELD_ID_VALUE_FLOAT, kv.value_float());
            }
            protoOutput->end(dimensionToken);
        }

        // Then fill bucket_info (GaugeBucketInfo).
        for (const auto& bucket : pair.second) {
            long long bucketInfoToken = protoOutput->start(
                    FIELD_TYPE_MESSAGE | FIELD_COUNT_REPEATED | FIELD_ID_BUCKET_INFO);
            protoOutput->write(FIELD_TYPE_INT64 | FIELD_ID_START_BUCKET_NANOS,
                               (long long)bucket.mBucketStartNs);
            protoOutput->write(FIELD_TYPE_INT64 | FIELD_ID_END_BUCKET_NANOS,
                               (long long)bucket.mBucketEndNs);
            protoOutput->write(FIELD_TYPE_INT64 | FIELD_ID_GAUGE, (long long)bucket.mGauge);
            protoOutput->end(bucketInfoToken);
            VLOG("\t bucket [%lld - %lld] count: %lld", (long long)bucket.mBucketStartNs,
                 (long long)bucket.mBucketEndNs, (long long)bucket.mGauge);
        }
        protoOutput->end(wrapperToken);
    }
    protoOutput->end(protoToken);
    protoOutput->write(FIELD_TYPE_INT64 | FIELD_ID_END_REPORT_NANOS, (long long)dumpTimeNs);

    mPastBuckets.clear();
    mStartTimeNs = mCurrentBucketStartTimeNs;
    // TODO: Clear mDimensionKeyMap once the report is dumped.
}

void GaugeMetricProducer::onConditionChangedLocked(const bool conditionMet,
                                                   const uint64_t eventTime) {
    VLOG("Metric %s onConditionChanged", mMetric.name().c_str());
    flushIfNeededLocked(eventTime);
    mCondition = conditionMet;

    // Push mode. No need to proactively pull the gauge data.
    if (mPullTagId == -1) {
        return;
    }
    if (!mCondition) {
        return;
    }
    // Already have gauge metric for the current bucket, do not do it again.
    if (mCurrentSlicedBucket->size() > 0) {
        return;
    }
    vector<std::shared_ptr<LogEvent>> allData;
    if (!mStatsPullerManager.Pull(mPullTagId, &allData)) {
        ALOGE("Stats puller failed for tag: %d", mPullTagId);
        return;
    }
    for (const auto& data : allData) {
        onMatchedLogEventLocked(0, *data, false /*scheduledPull*/);
    }
    flushIfNeededLocked(eventTime);
}

void GaugeMetricProducer::onSlicedConditionMayChangeLocked(const uint64_t eventTime) {
    VLOG("Metric %s onSlicedConditionMayChange", mMetric.name().c_str());
}

int64_t GaugeMetricProducer::getGauge(const LogEvent& event) {
    status_t err = NO_ERROR;
    int64_t val = event.GetLong(mMetric.gauge_field(), &err);
    if (err == NO_ERROR) {
        return val;
    } else {
        VLOG("Can't find value in message.");
        return -1;
    }
}

void GaugeMetricProducer::onDataPulled(const std::vector<std::shared_ptr<LogEvent>>& allData) {
    std::lock_guard<std::mutex> lock(mMutex);

    for (const auto& data : allData) {
        onMatchedLogEventLocked(0, *data, true /*scheduledPull*/);
    }
}

bool GaugeMetricProducer::hitGuardRailLocked(const HashableDimensionKey& newKey) {
    if (mCurrentSlicedBucket->find(newKey) != mCurrentSlicedBucket->end()) {
        return false;
    }
    // 1. Report the tuple count if the tuple count > soft limit
    if (mCurrentSlicedBucket->size() > StatsdStats::kDimensionKeySizeSoftLimit - 1) {
        size_t newTupleCount = mCurrentSlicedBucket->size() + 1;
        StatsdStats::getInstance().noteMetricDimensionSize(mConfigKey, mMetric.name(),
                                                           newTupleCount);
        // 2. Don't add more tuples, we are above the allowed threshold. Drop the data.
        if (newTupleCount > StatsdStats::kDimensionKeySizeHardLimit) {
            ALOGE("GaugeMetric %s dropping data for dimension key %s", mMetric.name().c_str(),
                  newKey.c_str());
            return true;
        }
    }

    return false;
}

void GaugeMetricProducer::onMatchedLogEventInternalLocked(
        const size_t matcherIndex, const HashableDimensionKey& eventKey,
        const map<string, HashableDimensionKey>& conditionKey, bool condition,
        const LogEvent& event, bool scheduledPull) {
    if (condition == false) {
        return;
    }
    uint64_t eventTimeNs = event.GetTimestampNs();
    if (eventTimeNs < mCurrentBucketStartTimeNs) {
        VLOG("Skip event due to late arrival: %lld vs %lld", (long long)eventTimeNs,
             (long long)mCurrentBucketStartTimeNs);
        return;
    }

    // When the event happens in a new bucket, flush the old buckets.
    if (eventTimeNs >= mCurrentBucketStartTimeNs + mBucketSizeNs) {
        flushIfNeededLocked(eventTimeNs);
    }

    // For gauge metric, we just simply use the first gauge in the given bucket.
    if (!mCurrentSlicedBucket->empty()) {
        return;
    }
    const long gauge = getGauge(event);
    if (gauge >= 0) {
        if (hitGuardRailLocked(eventKey)) {
            return;
        }
        (*mCurrentSlicedBucket)[eventKey] = gauge;
    }
    for (auto& tracker : mAnomalyTrackers) {
        tracker->detectAndDeclareAnomaly(eventTimeNs, mCurrentBucketNum, eventKey, gauge);
    }
}

// When a new matched event comes in, we check if event falls into the current
// bucket. If not, flush the old counter to past buckets and initialize the new
// bucket.
// if data is pushed, onMatchedLogEvent will only be called through onConditionChanged() inside
// the GaugeMetricProducer while holding the lock.
void GaugeMetricProducer::flushIfNeededLocked(const uint64_t& eventTimeNs) {
    if (eventTimeNs < mCurrentBucketStartTimeNs + mBucketSizeNs) {
        return;
    }

    GaugeBucket info;
    info.mBucketStartNs = mCurrentBucketStartTimeNs;
    info.mBucketEndNs = mCurrentBucketStartTimeNs + mBucketSizeNs;
    info.mBucketNum = mCurrentBucketNum;

    for (const auto& slice : *mCurrentSlicedBucket) {
        info.mGauge = slice.second;
        auto& bucketList = mPastBuckets[slice.first];
        bucketList.push_back(info);
        VLOG("gauge metric %s, dump key value: %s -> %lld", mMetric.name().c_str(),
             slice.first.c_str(), (long long)slice.second);
    }

    // Reset counters
    for (auto& tracker : mAnomalyTrackers) {
        tracker->addPastBucket(mCurrentSlicedBucket, mCurrentBucketNum);
    }

    mCurrentSlicedBucket = std::make_shared<DimToValMap>();

    // Adjusts the bucket start time
    int64_t numBucketsForward = (eventTimeNs - mCurrentBucketStartTimeNs) / mBucketSizeNs;
    mCurrentBucketStartTimeNs = mCurrentBucketStartTimeNs + numBucketsForward * mBucketSizeNs;
    mCurrentBucketNum += numBucketsForward;
    VLOG("metric %s: new bucket start time: %lld", mMetric.name().c_str(),
         (long long)mCurrentBucketStartTimeNs);
}

size_t GaugeMetricProducer::byteSizeLocked() const {
    size_t totalSize = 0;
    for (const auto& pair : mPastBuckets) {
        totalSize += pair.second.size() * kBucketSize;
    }
    return totalSize;
}

}  // namespace statsd
}  // namespace os
}  // namespace android
