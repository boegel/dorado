#include "Trimmer.h"

#include "utils/bam_utils.h"
#include "utils/barcode_kits.h"
#include "utils/trim.h"

#include <htslib/sam.h>

namespace {

const std::string UNCLASSIFIED_BARCODE = "unclassified";

}  // namespace

namespace dorado {

std::pair<int, int> Trimmer::determine_trim_interval(const BarcodeScoreResult& res, int seqlen) {
    // Initialize interval to be the whole read. Note that the interval
    // defines which portion of the read to retain.
    std::pair<int, int> trim_interval = {0, seqlen};

    if (res.kit == UNCLASSIFIED_BARCODE) {
        return trim_interval;
    }

    const float kFlankScoreThres = 0.6f;

    // Use barcode flank positions to determine trim interval
    // only if the flanks were confidently found. 1 is added to
    // the end of top barcode end value because that's the position
    // in the sequence where the barcode ends. So the actual sequence
    // because from one after that.
    auto kit_info_map = barcode_kits::get_kit_infos();
    const barcode_kits::KitInfo& kit = kit_info_map.at(res.kit);
    if (kit.double_ends) {
        float top_flank_score = res.top_flank_score;
        if (top_flank_score > kFlankScoreThres) {
            trim_interval.first = res.top_barcode_pos.second + 1;
        }

        float bottom_flank_score = res.bottom_flank_score;
        if (bottom_flank_score > kFlankScoreThres) {
            trim_interval.second = res.bottom_barcode_pos.first;
        }

        // In some cases where the read length is very small, the front
        // and rear windows could actually overlap. In that case find
        // which window was used and just grab the interval for that
        // window.
        if (trim_interval.second <= trim_interval.first) {
            if (res.use_top) {
                return {res.top_barcode_pos.first, res.top_barcode_pos.second + 1};
            } else {
                return {res.bottom_barcode_pos.first, res.bottom_barcode_pos.second + 1};
            }
        }
    } else {
        float top_flank_score = res.top_flank_score;
        if (top_flank_score > kFlankScoreThres) {
            trim_interval.first = res.top_barcode_pos.second + 1;
        }
    }

    if (trim_interval.second <= trim_interval.first) {
        // This could happen if the read is very short and the barcoding
        // algorithm determines the barcode interval to be the entire read.
        // In that case, skip trimming.
        trim_interval = {0, seqlen};
    }

    return trim_interval;
}

std::pair<int, int> Trimmer::determine_trim_interval(const AdapterScoreResult& res, int seqlen) {
    // Initialize interval to be the whole read. Note that the interval
    // defines which portion of the read to retain.
    std::pair<int, int> trim_interval = {0, seqlen};

    const float score_thres = 0.7f;

    if (res.front.name == "unclassified" || res.front.score < score_thres) {
        trim_interval.first = 0;
    } else {
        trim_interval.first = res.front.position.second + 1;
    }
    if (res.rear.name == "unclassified" || res.rear.score < score_thres) {
        trim_interval.second = seqlen;
    } else {
        trim_interval.second = res.rear.position.first;
    }

    if (trim_interval.second <= trim_interval.first) {
        // This could happen if the read is very short and the barcoding
        // algorithm determines the barcode interval to be the entire read.
        // In that case, skip trimming.
        trim_interval = {0, seqlen};
    }

    return trim_interval;
}

BamPtr Trimmer::trim_sequence(BamPtr input, std::pair<int, int> trim_interval) {
    bam1_t* input_record = input.get();

    // Fetch components that need to be trimmed.
    std::string seq = utils::extract_sequence(input_record);
    std::vector<uint8_t> qual = utils::extract_quality(input_record);
    auto [stride, move_vals] = utils::extract_move_table(input_record);
    int ts = bam_aux_get(input_record, "ts") ? int(bam_aux2i(bam_aux_get(input_record, "ts"))) : 0;
    auto [modbase_str, modbase_probs] = utils::extract_modbase_info(input_record);

    // Actually trim components.
    auto trimmed_seq = utils::trim_sequence(seq, trim_interval);
    auto trimmed_qual = utils::trim_quality(qual, trim_interval);
    auto [positions_trimmed, trimmed_moves] = utils::trim_move_table(move_vals, trim_interval);
    ts += positions_trimmed * stride;
    auto [trimmed_modbase_str, trimmed_modbase_probs] =
            utils::trim_modbase_info(seq, modbase_str, modbase_probs, trim_interval);
    auto n_cigar = input_record->core.n_cigar;
    std::vector<uint32_t> ops;
    uint32_t ref_pos_consumed = 0;
    if (n_cigar > 0) {
        auto cigar_arr = bam_get_cigar(input_record);
        ops = utils::trim_cigar(n_cigar, cigar_arr, trim_interval);
        ref_pos_consumed =
                ops.empty() ? 0 : utils::ref_pos_consumed(n_cigar, cigar_arr, trim_interval.first);
    }

    // Create a new bam record to hold the trimmed read.
    bam1_t* out_record = bam_init1();
    bam_set1(out_record, input_record->core.l_qname - input_record->core.l_extranul - 1,
             bam_get_qname(input_record), input_record->core.flag, input_record->core.tid,
             input_record->core.pos + ref_pos_consumed, input_record->core.qual, ops.size(),
             ops.empty() ? NULL : ops.data(), input_record->core.mtid, input_record->core.mpos,
             input_record->core.isize, trimmed_seq.size(), trimmed_seq.data(),
             trimmed_qual.empty() ? NULL : (char*)trimmed_qual.data(), bam_get_l_aux(input_record));
    memcpy(bam_get_aux(out_record), bam_get_aux(input_record), bam_get_l_aux(input_record));
    out_record->l_data += bam_get_l_aux(input_record);

    // Insert the new tags and delete the old ones.
    if (!trimmed_moves.empty()) {
        bam_aux_del(out_record, bam_aux_get(out_record, "mv"));
        // Move table format is stride followed by moves.
        trimmed_moves.insert(trimmed_moves.begin(), uint8_t(stride));
        bam_aux_update_array(out_record, "mv", 'c', int(trimmed_moves.size()),
                             (uint8_t*)trimmed_moves.data());
    }

    if (!trimmed_modbase_str.empty()) {
        bam_aux_del(out_record, bam_aux_get(out_record, "MM"));
        bam_aux_append(out_record, "MM", 'Z', int(trimmed_modbase_str.length() + 1),
                       (uint8_t*)trimmed_modbase_str.c_str());
        bam_aux_del(out_record, bam_aux_get(out_record, "ML"));
        bam_aux_update_array(out_record, "ML", 'C', int(trimmed_modbase_probs.size()),
                             (uint8_t*)trimmed_modbase_probs.data());
    }

    bam_aux_update_int(out_record, "ts", ts);

    return BamPtr(out_record);
}

void Trimmer::trim_sequence(SimplexRead& read, std::pair<int, int> trim_interval) {
    if (trim_interval.second - trim_interval.first == int(read.read_common.seq.length())) {
        return;
    }

    read.read_common.seq = utils::trim_sequence(read.read_common.seq, trim_interval);
    read.read_common.qstring = utils::trim_sequence(read.read_common.qstring, trim_interval);
    size_t num_positions_trimmed;
    std::tie(num_positions_trimmed, read.read_common.moves) =
            utils::trim_move_table(read.read_common.moves, trim_interval);
    read.read_common.num_trimmed_samples += read.read_common.model_stride * num_positions_trimmed;

    if (read.read_common.mod_base_info) {
        int num_modbase_channels = int(read.read_common.mod_base_info->alphabet.size());
        // The modbase probs table consists of the probability per channel per base. So when
        // trimming, we just shift everything by skipped bases * number of channels.
        std::pair<int, int> modbase_interval = {trim_interval.first * num_modbase_channels,
                                                trim_interval.second * num_modbase_channels};
        read.read_common.base_mod_probs =
                utils::trim_quality(read.read_common.base_mod_probs, modbase_interval);
    }
}

}  // namespace dorado