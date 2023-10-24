#include "PolyACalculator.h"

#include "utils/sequence_utils.h"

#include <edlib.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <utility>

namespace {

struct SignalAnchorInfo {
    // Is the strand in forward or reverse direction.
    bool is_fwd_strand = true;
    // The start or end anchor for the polyA/T signal
    // depending on whether the strand is forward or
    // reverse.
    int signal_anchor = -1;
    // Number of additional A/T bases in the polyA
    // stretch from the adapter.
    int trailing_adapter_bases = 0;
};

const int kMaxTailLength = 750;

// This algorithm walks through the signal in windows. For each window
// the avg and stdev of the signal is computed. If the stdev is below
// an empirically determined threshold, and consecutive windows have
// similar avg and stdev, then those windows are considered to be part
// of the polyA tail.
std::pair<int, int> determine_signal_bounds(int signal_anchor,
                                            bool fwd,
                                            const dorado::SimplexRead& read,
                                            int num_samples_per_base,
                                            bool is_rna) {
    const c10::Half* signal = static_cast<c10::Half*>(read.read_common.raw_data.data_ptr());
    int signal_len = read.read_common.get_raw_data_samples();

    auto calc_stats = [&](int s, int e) -> std::pair<float, float> {
        float avg = 0;
        for (int i = s; i < e; i++) {
            avg += signal[i];
        }
        avg = avg / (e - s);
        float var = 0;
        for (int i = s; i < e; i++) {
            var += (signal[i] - avg) * (signal[i] - avg);
        }
        var = var / (e - s);
        return {avg, std::sqrt(var)};
    };

    std::vector<std::pair<int, int>> intervals;
    std::pair<float, float> last_interval_stats;

    // Maximum variance between consecutive values to be
    // considered part of the same interval.
    const float kVar = 0.35f;
    // Determine the outer boundary of the signal space to
    // consider based on the anchor.
    const int kSpread = num_samples_per_base * kMaxTailLength;
    // Maximum gap between intervals that can be combined.
    const int kMaxSampleGap = num_samples_per_base * 3;
    // Minimum size of intervals considered for merge.
    const int kMinIntervalSizeForMerge = 10 * num_samples_per_base;
    // Floor for average signal value of poly tail.
    const float kMinAvgVal = (is_rna ? 0.0 : -3.0);

    int left_end = is_rna ? std::max(0, signal_anchor - 50) : std::max(0, signal_anchor - kSpread);
    int right_end = std::min(signal_len, signal_anchor + kSpread);
    spdlog::debug("Bounds left {}, right {}", left_end, right_end);

    const int kStride = 3;
    for (int s = left_end; s < right_end; s += kStride) {
        int e = std::min(s + kMaxSampleGap, right_end);
        auto [avg, stdev] = calc_stats(s, e);
        if (stdev < kVar) {
            // If a new interval overlaps with the previous interval, just extend
            // the previous interval.
            if (intervals.size() > 1 && intervals.back().second >= s &&
                std::abs(avg - last_interval_stats.first) < 0.2 && (avg > kMinAvgVal)) {
                auto& last = intervals.back();
                last.second = e;
            } else {
                // Attempt to merge the most recent interval and the one before
                // that if the gap between the intervals is small and both of the
                // intervals are longer than some threshold.
                if (intervals.size() > 2) {
                    auto& last = intervals.back();
                    auto& second_last = intervals[intervals.size() - 2];
                    if ((last.first - second_last.second < kMaxSampleGap) &&
                        (last.second - last.first > kMinIntervalSizeForMerge) &&
                        (second_last.second - second_last.first > kMinIntervalSizeForMerge)) {
                        second_last.second = last.second;
                        intervals.pop_back();
                    }
                }

                intervals.push_back({s, e});
            }
            last_interval_stats = {avg, stdev};
        }
    }

    std::string int_str = "";
    for (const auto& in : intervals) {
        int_str += std::to_string(in.first) + "-" + std::to_string(in.second) + ", ";
    }
    spdlog::debug("found intervals {}", int_str);

    std::vector<std::pair<int, int>> filtered_intervals;
    std::copy_if(intervals.begin(), intervals.end(), std::back_inserter(filtered_intervals),
                 [&](auto& i) {
                     int interval_size = i.second - i.first;
                     // Filter out any small intervals.
                     if (interval_size < (num_samples_per_base * 5)) {
                         return false;
                     }
                     // Only keep intervals that are close-ish to the signal anchor.
                     return (fwd ? std::abs(signal_anchor - i.second) < interval_size
                                 : std::abs(signal_anchor - i.first) < interval_size) ||
                            (i.first <= signal_anchor) && (signal_anchor <= i.second);
                 });

    int_str = "";
    for (auto in : filtered_intervals) {
        int_str += std::to_string(in.first) + "-" + std::to_string(in.second) + ", ";
    }
    spdlog::debug("filtered intervals {}", int_str);

    if (filtered_intervals.empty()) {
        spdlog::debug("Anchor {} No range within anchor proximity found", signal_anchor);
        return {0, 0};
    }

    // Choose the longest interval. If there is a tie for the longest interval,
    // choose the one that is closest to the anchor.
    auto best_interval = std::max_element(filtered_intervals.begin(), filtered_intervals.end(),
                                          [&](auto& l, auto& r) {
                                              auto l_size = l.second - l.first;
                                              auto r_size = r.second - r.first;
                                              if (l_size != r_size) {
                                                  return l_size < r_size;
                                              } else {
                                                  if (fwd) {
                                                      return std::abs(l.second - signal_anchor) <
                                                             std::abs(r.second - signal_anchor);
                                                  } else {
                                                      return std::abs(l.first - signal_anchor) <
                                                             std::abs(r.first - signal_anchor);
                                                  }
                                              }
                                          });

    spdlog::debug("Anchor {} Range {} {}", signal_anchor, best_interval->first,
                  best_interval->second);
    return *best_interval;
}

// Estimate the number of samples per base. For RNA, use the last 100 bases
// to get a measure of samples/base. For DNA, just taking the average across
// the whole read gives a decent estimate.
int estimate_samples_per_base(const dorado::SimplexRead& read, bool is_rna) {
    size_t num_bases = read.read_common.seq.length();
    if (is_rna && num_bases > 250) {
        const auto stride = read.read_common.model_stride;
        const auto seq_to_sig_map =
                dorado::utils::moves_to_map(read.read_common.moves, stride,
                                            read.read_common.get_raw_data_samples(), num_bases + 1);
        // Use last 100bp to estimate samples / base.
        size_t signal_len = seq_to_sig_map[num_bases] - seq_to_sig_map[num_bases - 100];
        return std::floor(static_cast<float>(signal_len) / 100);
    }
    float num_samples_per_base =
            static_cast<float>(read.read_common.get_raw_data_samples()) / num_bases;
    // The estimate is not rounded because this calculation generally overestimates
    // the samples per base. Rounding down gives better results than rounding to nearest.
    return std::floor(num_samples_per_base);
}

// In order to find the approximate location of the start/end (anchor) of the polyA
// cDNA tail, the adapter ends are aligned to the reads to find the breakpoint
// between the read and the adapter. Adapter alignment also helps determine
// the strand direction. This function returns a struct with the strand direction,
// the approximate anchor for the tail, and if there needs to be an adjustment
// made to the final polyA tail count based on the adapter sequence (e.g. because
// the adapter itself contains several As).
SignalAnchorInfo determine_signal_anchor_and_strand_cdna(const dorado::SimplexRead& read) {
    const std::string SSP = "TTTCTGTTGGTGCTGATATTGCTTT";
    const std::string SSP_rc = dorado::utils::reverse_complement(SSP);
    const std::string VNP = "ACTTGCCTGTCGCTCTATCTTCAGAGGAGAGTCCGCCGCCCGCAAGTTTT";
    const std::string VNP_rc = dorado::utils::reverse_complement(VNP);
    int trailing_Ts = dorado::utils::count_trailing_chars(VNP, 'T');

    const int kWindowSize = 150;
    std::string read_top = read.read_common.seq.substr(0, kWindowSize);
    auto bottom_start = std::max(0, (int)read.read_common.seq.length() - kWindowSize);
    std::string read_bottom = read.read_common.seq.substr(bottom_start, kWindowSize);

    EdlibAlignConfig align_config = edlibDefaultAlignConfig();
    align_config.task = EDLIB_TASK_LOC;
    align_config.mode = EDLIB_MODE_HW;

    // Check for forward strand.
    EdlibAlignResult top_v1 =
            edlibAlign(SSP.data(), SSP.length(), read_top.data(), read_top.length(), align_config);
    EdlibAlignResult bottom_v1 = edlibAlign(VNP_rc.data(), VNP_rc.length(), read_bottom.data(),
                                            read_bottom.length(), align_config);

    int dist_v1 = top_v1.editDistance + bottom_v1.editDistance;

    // Check for reverse strand.
    EdlibAlignResult top_v2 =
            edlibAlign(VNP.data(), VNP.length(), read_top.data(), read_top.length(), align_config);
    EdlibAlignResult bottom_v2 = edlibAlign(SSP_rc.data(), SSP_rc.length(), read_bottom.data(),
                                            read_bottom.length(), align_config);

    int dist_v2 = top_v2.editDistance + bottom_v2.editDistance;
    spdlog::debug("v1 dist {}, v2 dist {}", dist_v1, dist_v2);

    bool fwd = dist_v1 < dist_v2;
    bool proceed = std::min(dist_v1, dist_v2) < 30 && std::abs(dist_v1 - dist_v2) > 10;

    SignalAnchorInfo result = {false, -1, trailing_Ts};

    if (proceed) {
        int start = 0, end = 0;
        int base_anchor = 0;
        if (fwd) {
            base_anchor = bottom_start + bottom_v1.startLocations[0];
        } else {
            base_anchor = top_v2.endLocations[0];
        }

        const auto stride = read.read_common.model_stride;
        const auto seq_to_sig_map = dorado::utils::moves_to_map(
                read.read_common.moves, stride, read.read_common.get_raw_data_samples(),
                read.read_common.seq.size() + 1);
        int signal_anchor = seq_to_sig_map[base_anchor];

        result = {fwd, signal_anchor, trailing_Ts};
    } else {
        spdlog::debug("{} primer edit distance too high {}", read.read_common.read_id,
                      std::min(dist_v1, dist_v2));
    }

    edlibFreeAlignResult(top_v1);
    edlibFreeAlignResult(bottom_v1);
    edlibFreeAlignResult(top_v2);
    edlibFreeAlignResult(bottom_v2);

    return result;
}

// RNA polyA appears at the beginning of the strand. Since the adapter
// for RNA has been trimmed off already, the polyA search can begin
// from the start of the signal.
SignalAnchorInfo determine_signal_anchor_and_strand_drna(const dorado::SimplexRead& read) {
    return SignalAnchorInfo{false, 0, 0};
}

}  // namespace

namespace dorado {

void PolyACalculator::worker_thread() {
    torch::InferenceMode inference_mode_guard;

    Message message;
    while (get_input_message(message)) {
        // If this message isn't a read, just forward it to the sink.
        if (!std::holds_alternative<SimplexReadPtr>(message)) {
            send_message_to_sink(std::move(message));
            continue;
        }

        // If this message isn't a read, we'll get a bad_variant_access exception.
        auto read = std::get<SimplexReadPtr>(std::move(message));

        // Determine the strand direction, approximate base space anchor for the tail, and whether
        // the final length needs to be adjusted depending on the adapter sequence.
        auto [fwd, signal_anchor, trailing_Ts] =
                m_is_rna ? determine_signal_anchor_and_strand_drna(*read)
                         : determine_signal_anchor_and_strand_cdna(*read);

        if (signal_anchor >= 0) {
            spdlog::debug("Strand {}; poly A/T signal anchor {}", fwd ? '+' : '-', signal_anchor);

            auto num_samples_per_base = estimate_samples_per_base(*read, m_is_rna);

            // Walk through signal
            auto [signal_start, signal_end] = determine_signal_bounds(
                    signal_anchor, fwd, *read, num_samples_per_base, m_is_rna);

            int num_bases = std::round(static_cast<float>(signal_end - signal_start) /
                                       num_samples_per_base) -
                            trailing_Ts;
            if (num_bases > 0 && num_bases < kMaxTailLength) {
                spdlog::debug(
                        "{} PolyA bases {}, signal anchor {} Signal range is {} {}, "
                        "samples/base {} trim {}",
                        read->read_common.read_id, num_bases, signal_anchor, signal_start,
                        signal_end, num_samples_per_base, read->read_common.num_trimmed_samples);

                // Set tail length property in the read.
                read->read_common.rna_poly_tail_length = num_bases;

                // Update debug stats.
                total_tail_lengths_called += num_bases;
                ++num_called;
                if (spdlog::get_level() == spdlog::level::debug) {
                    std::lock_guard<std::mutex> lock(m_mutex);
                    tail_length_counts[num_bases]++;
                }
            } else {
                spdlog::debug(
                        "{} PolyA bases {}, signal anchor {} Signal range is {}, "
                        "samples/base {}, trim  {}",
                        read->read_common.read_id, num_bases, signal_anchor, signal_start,
                        signal_end, num_samples_per_base, read->read_common.num_trimmed_samples);
                num_not_called++;
            }
        } else {
            num_not_called++;
        }

        send_message_to_sink(std::move(read));
    }
}

PolyACalculator::PolyACalculator(size_t num_worker_threads, bool is_rna, size_t max_reads)
        : MessageSink(max_reads), m_num_worker_threads(num_worker_threads), m_is_rna(is_rna) {
    start_threads();
}

void PolyACalculator::start_threads() {
    for (size_t i = 0; i < m_num_worker_threads; i++) {
        m_workers.push_back(
                std::make_unique<std::thread>(std::thread(&PolyACalculator::worker_thread, this)));
    }
}

void PolyACalculator::terminate_impl() {
    terminate_input_queue();
    for (auto& m : m_workers) {
        if (m->joinable()) {
            m->join();
        }
    }
    m_workers.clear();

    spdlog::debug("Total called {}, not called {}, avg tail length {}", num_called.load(),
                  num_not_called.load(),
                  num_called.load() > 0 ? total_tail_lengths_called.load() / num_called.load() : 0);

    // Visualize a distribution of the tail lengths called.
    static bool done = false;
    if (!done && spdlog::get_level() == spdlog::level::debug) {
        int max_val = -1;
        for (auto [k, v] : tail_length_counts) {
            max_val = std::max(v, max_val);
        }
        int factor = std::max(1, 1 + max_val / 100);
        for (auto [k, v] : tail_length_counts) {
            spdlog::debug("{:03d} : {}", k, std::string(v / factor, '*'));
        }
        done = true;
    }
}

void PolyACalculator::restart() {
    restart_input_queue();
    start_threads();
}

stats::NamedStats PolyACalculator::sample_stats() const {
    stats::NamedStats stats = stats::from_obj(m_work_queue);
    stats["reads_not_estimated"] = num_not_called.load();
    stats["reads_estimated"] = num_called.load();
    stats["average_tail_length"] =
            num_called.load() > 0 ? total_tail_lengths_called.load() / num_called.load() : 0;
    return stats;
}

}  // namespace dorado
