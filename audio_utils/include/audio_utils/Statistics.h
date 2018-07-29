/*
 * Copyright 2018 The Android Open Source Project
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

#ifndef ANDROID_AUDIO_UTILS_STATISTICS_H
#define ANDROID_AUDIO_UTILS_STATISTICS_H

#include <cmath>
#include <deque> // for ReferenceStatistics implementation
#include <math.h>
#include <sstream>

namespace android {

/**
 * Statistics provides a running weighted average, variance, and standard deviation of
 * a sample stream. It is more numerically stable for floating point computation than a
 * naive sum of values, sum of values squared.
 *
 * The weighting is like an IIR filter, with the most recent sample weighted as 1, and decaying
 * by alpha (between 0 and 1).  With alpha == 1. this is rectangular weighting, reducing to
 * Welford's algorithm.
 *
 * The IIR filter weighting emphasizes more recent samples, has low overhead updates,
 * constant storage, and constant computation (per update or variance read).
 *
 * This is a variant of the weighted mean and variance algorithms described here:
 * https://en.wikipedia.org/wiki/Moving_average
 * https://en.wikipedia.org/wiki/Algorithms_for_calculating_variance#Online_algorithm
 * https://en.wikipedia.org/wiki/Weighted_arithmetic_mean
 *
 * weight = sum_{i=1}^n \alpha^{n-i}
 * mean = 1/weight * sum_{i=1}^n \alpha^{n-i}x_i
 * var = 1/weight * sum_{i=1}^n alpha^{n-i}(x_i- mean)^2
 *
 * The Statistics class is safe to call from a SCHED_FIFO thread with the exception of
 * the toString() method, which uses std::stringstream to format data for printing.
 *
 * Long term data accumulation and constant alpha:
 * If the alpha weight is 1 (or not specified) then statistics objects with float
 * summation types (D, S) should NOT add more than the mantissa-bits elements
 * without reset to prevent variance increases due to weight precision underflow.
 * This is 1 << 23 elements for float and 1 << 52 elements for double.
 *
 * Setting alpha less than 1 avoids this underflow problem.
 * Alpha < 1 - (epsilon * 32), where epsilon is std::numeric_limits<D>::epsilon()
 * is recommended for continuously running statistics (alpha <= 0.999996
 * for float summation precision).
 *
 * Alpha may also change on-the-fly, based on the reliability of
 * new information.  In that case, alpha may be set temporarily greater
 * than 1.
 *
 * https://en.wikipedia.org/wiki/Weighted_arithmetic_mean#Reliability_weights_2
 *
 * TODO:
 * 1) Alternative versions of Kahan/Neumaier sum that better preserve precision.
 * 2) Add binary math ops to corrected sum classes for better precision in lieu of long double.
 */

/**
 * Mean may have catastrophic cancellation of positive and negative sample values,
 * so we use Kahan summation in the algorithms below (or substitute "D" if not needed).
 *
 * https://en.wikipedia.org/wiki/Kahan_summation_algorithm
 */

template <typename T>
struct KahanSum {
    T mSum{};
    T mCorrection{}; // negative low order bits of mSum.

    constexpr KahanSum<T>& operator+=(const T& rhs) { // takes T not KahanSum<T>
        const T y = rhs - mCorrection;
        const T t = mSum + y;

#ifdef __FAST_MATH__
#warning "fast math enabled, could optimize out KahanSum correction"
#endif

        mCorrection = (t - mSum) - y; // compiler, please do not optimize this out with /fp:fast

        mSum = t;
        return *this;
    }

    constexpr operator T() const {
        return mSum;
    }

    constexpr void reset() {
        mSum = {};
        mCorrection = {};
    }
};

// A more robust version of Kahan summation for input greater than sum.
// TODO: investigate variants that reincorporate mCorrection bits into mSum if possible.
template <typename T>
struct NeumaierSum {
    T mSum{};
    T mCorrection{}; // low order bits of mSum.

    constexpr NeumaierSum<T>& operator+=(const T& rhs) { // takes T not NeumaierSum<T>
        const T t = mSum + rhs;

        if (const_abs(mSum) >= const_abs(rhs)) {
            mCorrection += (mSum - t) + rhs;
        } else {
            mCorrection += (rhs - t) + mSum;
        }

        mSum = t;
        return *this;
    }

    static constexpr T const_abs(T x) {
        return x < T{} ? -x : x;
    }

    constexpr operator T() const {
        return mSum + mCorrection;
    }

    constexpr void reset() {
        mSum = {};
        mCorrection = {};
    }
};

template <typename T>
struct StatisticsConstants {
    // value closest to negative infinity for type T
    static constexpr T mNegativeInfinity {
        std::numeric_limits<T>::has_infinity ?
                -std::numeric_limits<T>::infinity() : std::numeric_limits<T>::min()
    };

    // value closest to positive infinity for type T
    static constexpr T mPositiveInfinity {
        std::numeric_limits<T>::has_infinity ?
                std::numeric_limits<T>::infinity() : std::numeric_limits<T>::max()
    };
};

template <typename T, typename D = double, typename S = KahanSum<D>>
class Statistics {
public:
    /** alpha is the weight (if alpha == 1. we use a rectangular window) */
    explicit constexpr Statistics(D alpha = D(1.))
        : mAlpha(alpha)
    { }

    template <size_t N>
    explicit constexpr Statistics(const T (&a)[N], D alpha = D(1.))
        : mAlpha(alpha)
    {
        for (const auto &data : a) {
            add(data);
        }
    }

    constexpr void setAlpha(D alpha) {
        mAlpha = alpha;
    }

    constexpr void add(const T &value) {
        // Note: fastest implementation uses fmin fminf but would not be constexpr

        mMax = std::max(mMax, value); // order important: reject NaN
        mMin = std::min(mMin, value); // order important: reject NaN
        ++mN;
        const D delta = value - mMean;
        /* if (mAlpha == 1.) we have Welford's algorithm
            ++mN;
            mMean += delta / mN;
            mM2 += delta * (value - mMean);

            Note delta * (value - mMean) should be non-negative.
        */
        mWeight = D(1.) + mAlpha * mWeight;
        mWeight2 = D(1.) + mAlpha * mAlpha * mWeight2;
        mMean += delta / mWeight;
        mM2 = mAlpha * mM2 + delta * (value - mMean);

        /*
           Alternate variant related to:
           http://mathworld.wolfram.com/SampleVarianceComputation.html

           const double sweight = mAlpha * mWeight;
           mWeight = 1. + sweight;
           const double dmean = delta / mWeight;
           mMean += dmean;
           mM2 = mAlpha * mM2 + mWeight * sweight * dmean * dmean;

           The update is slightly different than Welford's algorithm
           showing a by-construction non-negative update to M2.
        */
    }

    constexpr int64_t getN() const {
        return mN;
    }

    constexpr void reset() {
        mMin = StatisticsConstants<T>::mPositiveInfinity;
        mMax = StatisticsConstants<T>::mNegativeInfinity;
        mN = 0;
        mWeight = {};
        mWeight2 = {};
        mMean = {};
        mM2 = {};
    }

    constexpr D getWeight() const {
        return mWeight;
    }

    constexpr D getMean() const {
        return mMean;
    }

    constexpr D getVariance() const {
        if (mN < 2) {
            // must have 2 samples for sample variance.
            return {};
        } else {
            return mM2 / getSampleWeight();
        }
    }

    constexpr D getPopVariance() const {
        if (mN < 1) {
            return {};
        } else {
            return mM2 / mWeight;
        }
    }

    // explicitly use audio_utils_sqrt if you need a constexpr version
    D getStdDev() const {
        return sqrt(getVariance());
    }

    D getPopStdDev() const {
        return sqrt(getPopVariance());
    }

    constexpr T getMin() const {
        return mMin;
    }

    constexpr T getMax() const {
        return mMax;
    }

    std::string toString() const {
        const int64_t N = getN();
        if (N == 0) return "unavail";

        std::stringstream ss;
        ss << "ave=" << getMean();
        if (N > 1) {
            // we use the sample standard deviation (not entirely unbiased,
            // though the sample variance is unbiased).
            ss << " std=" << getStdDev();
        }
        ss << " min=" << getMin();
        ss << " max=" << getMax();
        return ss.str();
    }

private:
    D mAlpha;
    T mMin{StatisticsConstants<T>::mPositiveInfinity};
    T mMax{StatisticsConstants<T>::mNegativeInfinity};

    int64_t mN = 0;  // running count of samples.
    D mWeight{};     // sum of weights.
    D mWeight2{};    // sum of weights squared.
    S mMean{};       // running mean.
    D mM2{};         // running unnormalized variance.

    // Reliability correction for unbiasing variance, since mean is estimated
    // from same sample stream as variance.
    // if mAlpha == 1 this is mWeight - 1;
    //
    // TODO: consider exposing the correction factor.
    constexpr D getSampleWeight() const {
        // if mAlpha is constant then the mWeight2 member variable is not
        // needed, one can use instead:
        // return (mWeight - D(1.)) * D(2.) / (D(1.) + mAlpha);

        return mWeight - mWeight2 / mWeight;
    }
};

/**
 * ReferenceStatistics is a naive implementation of the weighted running variance,
 * which consumes more space and is slower than Statistics.  It is provided for
 * comparison and testing purposes.  Do not call from a SCHED_FIFO thread!
 *
 * Note: Common code not combined for implementation clarity.
 *       We don't invoke Kahan summation or other tricks.
 */
template <typename T, typename D = double>
class ReferenceStatistics {
public:
    /** alpha is the weight (alpha == 1. is rectangular window) */
    explicit ReferenceStatistics(D alpha = D(1.))
        : mAlpha(alpha)
    { }

    constexpr void setAlpha(D alpha) {
        mAlpha = alpha;
    }

    // For independent testing, have intentionally slightly different behavior
    // of min and max than Statistics with respect to Nan.
    constexpr void add(const T &value) {
        if (getN() == 0) {
            mMax = value;
            mMin = value;
        } else if (value > mMax) {
            mMax = value;
        } else if (value < mMin) {
            mMin = value;
        }

        mData.push_front(value);
        mAlphaList.push_front(mAlpha);
    }

    int64_t getN() const {
        return mData.size();
    }

    void reset() {
        mMin = {};
        mMax = {};
        mData.clear();
        mAlphaList.clear();
    }

    D getWeight() const {
        D weight{};
        D alpha_i(1.);
        for (size_t i = 0; i < mData.size(); ++i) {
            weight += alpha_i;
            alpha_i *= mAlphaList[i];
        }
        return weight;
    }

    D getWeight2() const {
        D weight2{};
        D alpha2_i(1.);
        for (size_t i = 0; i < mData.size(); ++i) {
            weight2 += alpha2_i;
            alpha2_i *= mAlphaList[i] * mAlphaList[i];
        }
        return weight2;
    }

    D getMean() const {
        D wsum{};
        D alpha_i(1.);
        for (size_t i = 0; i < mData.size(); ++i) {
            wsum += alpha_i * mData[i];
            alpha_i *= mAlphaList[i];
        }
        return wsum / getWeight();
    }

    // Should always return a positive value.
    D getVariance() const {
        return getUnweightedVariance() / (getWeight() - getWeight2() / getWeight());
    }

    // Should always return a positive value.
    D getPopVariance() const {
        return getUnweightedVariance() / getWeight();
    }

    D getStdDev() const {
        return sqrt(getVariance());
    }

    D getPopStdDev() const {
        return sqrt(getPopVariance());
    }

    T getMin() const {
        return mMin;
    }

    T getMax() const {
        return mMax;
    }

    std::string toString() const {
        const auto N = getN();
        if (N == 0) return "unavail";

        std::stringstream ss;
        ss << "ave=" << getMean();
        if (N > 1) {
            // we use the sample standard deviation (not entirely unbiased,
            // though the sample variance is unbiased).
            ss << " std=" << getStdDev();
        }
        ss << " min=" << getMin();
        ss << " max=" << getMax();
        return ss.str();
    }

private:
    T mMin{};
    T mMax{};

    D mAlpha;                 // current alpha value
    std::deque<T> mData;      // store all the data for exact summation, mData[0] most recent.
    std::deque<D> mAlphaList; // alpha value for the data added.

    D getUnweightedVariance() const {
        const D mean = getMean();
        D wsum{};
        D alpha_i(1.);
        for (size_t i = 0; i < mData.size(); ++i) {
            const D diff = mData[i] - mean;
            wsum += alpha_i * diff * diff;
            alpha_i *= mAlphaList[i];
        }
        return wsum;
    }
};

//
// constexpr statistics functions
//
// Form: algorithm(forward_iterator begin, forward_iterator end)

// returns max of elements, or if no elements negative infinity.
template <typename T>
constexpr auto audio_utils_max(T begin, T end) {
    using S = typename std::remove_cv_t<typename std::remove_reference_t<
            decltype(*begin)>>;
    S maxValue = StatisticsConstants<S>::mNegativeInfinity;
    for (auto it = begin; it != end; ++it) {
        maxValue = std::max(maxValue, *it);
    }
    return maxValue;
}

// returns min of elements, or if no elements positive infinity.
template <typename T>
constexpr auto audio_utils_min(T begin, T end) {
    using S = typename std::remove_cv_t<typename std::remove_reference_t<
            decltype(*begin)>>;
    S minValue = StatisticsConstants<S>::mPositiveInfinity;
    for (auto it = begin; it != end; ++it) {
        minValue = std::min(minValue, *it);
    }
    return minValue;
}

template <typename D = double, typename S = KahanSum<D>, typename T>
constexpr auto audio_utils_sum(T begin, T end) {
    S sum{};
    for (auto it = begin; it != end; ++it) {
        sum += D(*it);
    }
    return sum;
}

template <typename D = double, typename S = KahanSum<D>, typename T>
constexpr auto audio_utils_sumSqDiff(T begin, T end, D x = {}) {
    S sum{};
    for (auto it = begin; it != end; ++it) {
        const D diff = *it - x;
        sum += diff * diff;
    }
    return sum;
}

// Form: algorithm(array[]), where array size is known to the compiler.

template <typename T, size_t N>
constexpr T audio_utils_max(const T (&a)[N]) {
    return audio_utils_max(&a[0], &a[N]);
}

template <typename T, size_t N>
constexpr T audio_utils_min(const T (&a)[N]) {
    return audio_utils_min(&a[0], &a[N]);
}

template <typename D = double, typename S = KahanSum<D>, typename T, size_t N>
constexpr D audio_utils_sum(const T (&a)[N]) {
    return audio_utils_sum<D, S>(&a[0], &a[N]);
}

template <typename D = double, typename S = KahanSum<D>, typename T, size_t N>
constexpr D audio_utils_sumSqDiff(const T (&a)[N], D x = {}) {
    return audio_utils_sumSqDiff<D, S>(&a[0], &a[N], x);
}

// TODO: remove when std::isnan is constexpr
template <typename T>
constexpr T audio_utils_isnan(T x) {
    return __builtin_isnan(x);
}

// constexpr sqrt computed by the Babylonian (Newton's) method.
// Please use math libraries for non-constexpr cases.
// TODO: remove when there is some std::sqrt which is constexpr.
//
// https://en.wikipedia.org/wiki/Methods_of_computing_square_roots

// watch out using the unchecked version, use the checked version below.
template <typename T>
constexpr T audio_utils_sqrt_unchecked(T x, T prev) {
    static_assert(std::is_floating_point<T>::value, "must be floating point type");
    const T next = T(0.5) * (prev + x / prev);
    return next == prev ? next : audio_utils_sqrt_unchecked(x, next);
}

// checked sqrt
template <typename T>
constexpr T audio_utils_sqrt(T x) {
    static_assert(std::is_floating_point<T>::value, "must be floating point type");
    if (x < T{}) { // negative values return nan
        return std::numeric_limits<T>::quiet_NaN();
    } else if (audio_utils_isnan(x)
            || x == std::numeric_limits<T>::infinity()
            || x == T{}) {
        return x;
    } else { // good to go.
        return audio_utils_sqrt_unchecked(x, T(1.));
    }
}

} // namespace android

#endif // !ANDROID_AUDIO_UTILS_STATISTICS_H
