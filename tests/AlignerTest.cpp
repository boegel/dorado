#include "MessageSinkUtils.h"
#include "TestUtils.h"
#include "alignment/Minimap2Aligner.h"
#include "read_pipeline/AlignerNode.h"
#include "read_pipeline/ClientAccess.h"
#include "read_pipeline/HtsReader.h"
#include "utils/bam_utils.h"
#include "utils/sequence_utils.h"

#include <catch2/catch.hpp>
#include <htslib/sam.h>

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#define TEST_GROUP "[bam_utils][aligner]"

namespace fs = std::filesystem;

namespace {

class TestClientAccess : public dorado::ClientAccess {
    const dorado::AlignmentInfo m_align_info;

public:
    TestClientAccess(dorado::AlignmentInfo align_info) : m_align_info(std::move(align_info)) {}

    uint32_t client_id() const override { return 1; }

    const dorado::AlignmentInfo& alignment_info() const override { return m_align_info; }
};

template <class... Args>
std::unique_ptr<dorado::Pipeline> create_pipeline(std::vector<dorado::Message>& output_messages,
                                                  Args&&... args) {
    dorado::PipelineDescriptor pipeline_desc;
    auto sink = pipeline_desc.add_node<MessageSinkToVector>({}, 100, output_messages);
    auto aligner = pipeline_desc.add_node<dorado::AlignerNode>({sink}, args...);
    return dorado::Pipeline::create(std::move(pipeline_desc));
}

template <class... Args>
std::vector<dorado::BamPtr> RunAlignmentPipeline(dorado::HtsReader& reader, Args&&... args) {
    std::vector<dorado::Message> messages;
    auto index_file_access = std::make_shared<dorado::alignment::IndexFileAccess>();
    auto pipeline = create_pipeline(messages, index_file_access, args...);
    reader.read(*pipeline, 100);
    pipeline.reset();
    return ConvertMessages<dorado::BamPtr>(std::move(messages));
}

}  // namespace

TEST_CASE("AlignerTest: Check standard alignment", TEST_GROUP) {
    using Catch::Matchers::Contains;

    fs::path aligner_test_dir = fs::path(get_aligner_data_dir());
    auto ref = aligner_test_dir / "target.fq";
    auto query = aligner_test_dir / "target.fq";

    auto options = dorado::alignment::dflt_options;
    options.kmer_size = options.window_size = 15;
    options.index_batch_size = 1e9;
    dorado::HtsReader reader(query.string());
    auto bam_records = RunAlignmentPipeline(reader, ref.string(), options, 10);
    REQUIRE(bam_records.size() == 1);

    bam1_t* rec = bam_records[0].get();
    bam1_t* in_rec = reader.record.get();

    // Check input/output reads are matching.
    std::string orig_read = dorado::utils::extract_sequence(in_rec);
    std::string aligned_read = dorado::utils::extract_sequence(rec);

    // Check quals are matching.
    std::vector<uint8_t> orig_qual = dorado::utils::extract_quality(in_rec);
    std::vector<uint8_t> aligned_qual = dorado::utils::extract_quality(rec);
    CHECK(orig_qual == aligned_qual);

    // Check aux tags.
    uint32_t l_aux = bam_get_l_aux(rec);
    std::string aux((char*)bam_get_aux(rec), (char*)(bam_get_aux(rec) + l_aux));
    std::string tags[] = {"NMi", "msi", "ASi", "nni", "def", "tpA", "cmi", "s1i", "rli"};
    for (auto tag : tags) {
        CHECK_THAT(aux, Contains(tag));
    }
}

TEST_CASE("AlignerTest: Check supplementary alignment", TEST_GROUP) {
    using Catch::Matchers::Contains;

    fs::path aligner_test_dir = fs::path(get_aligner_data_dir());
    auto ref = aligner_test_dir / "supplementary_aln_target.fa";
    auto query = aligner_test_dir / "supplementary_aln_query.fa";

    auto options = dorado::alignment::dflt_options;
    options.kmer_size = options.window_size = 15;
    options.index_batch_size = 1e9;
    dorado::HtsReader reader(query.string());
    auto bam_records = RunAlignmentPipeline(reader, ref.string(), options, 10);
    REQUIRE(bam_records.size() == 2);

    // Check first alignment is primary.
    {
        bam1_t* rec = bam_records[0].get();

        // Check aux tags.
        uint32_t l_aux = bam_get_l_aux(rec);
        std::string aux((char*)bam_get_aux(rec), (char*)(bam_get_aux(rec) + l_aux));
        CHECK_THAT(aux, Contains("tpAP"));
        CHECK(rec->core.l_qseq > 0);  // Primary alignment should have SEQ.
    }

    // Check second alignment is secondary.
    {
        bam1_t* rec = bam_records[1].get();

        // Check aux tags.
        uint32_t l_aux = bam_get_l_aux(rec);
        std::string aux((char*)bam_get_aux(rec), (char*)(bam_get_aux(rec) + l_aux));
        CHECK_THAT(aux, Contains("tpAS"));
        CHECK(rec->core.l_qseq == 0);  // Secondary alignment doesn't need SEQ.
    }
}

TEST_CASE("AlignerTest: Check reverse complement alignment", TEST_GROUP) {
    using Catch::Matchers::Contains;

    fs::path aligner_test_dir = fs::path(get_aligner_data_dir());
    auto ref = aligner_test_dir / "target.fq";
    auto query = aligner_test_dir / "rev_target.fq";

    auto options = dorado::alignment::dflt_options;
    options.kmer_size = options.window_size = 15;
    options.index_batch_size = 1e9;
    dorado::HtsReader reader(query.string());
    auto bam_records = RunAlignmentPipeline(reader, ref.string(), options, 10);
    REQUIRE(bam_records.size() == 1);

    bam1_t* rec = bam_records[0].get();
    bam1_t* in_rec = reader.record.get();

    // Check flag.
    CHECK(rec->core.flag & 0x10);

    // Check read reverse complementing.
    std::string orig_read = dorado::utils::extract_sequence(in_rec);
    std::string aligned_read = dorado::utils::extract_sequence(rec);
    CHECK(orig_read == dorado::utils::reverse_complement(aligned_read));

    // Check qual reversal.
    std::vector<uint8_t> orig_qual = dorado::utils::extract_quality(in_rec);
    std::vector<uint8_t> aligned_qual = dorado::utils::extract_quality(rec);
    std::reverse(aligned_qual.begin(), aligned_qual.end());
    CHECK(orig_qual == aligned_qual);
}

TEST_CASE("AlignerTest: Check dorado tags are retained", TEST_GROUP) {
    using Catch::Matchers::Contains;

    fs::path aligner_test_dir = fs::path(get_aligner_data_dir());
    auto ref = aligner_test_dir / "basecall_target.fa";
    auto query = aligner_test_dir / "basecall.sam";

    auto options = dorado::alignment::dflt_options;
    options.kmer_size = options.window_size = 15;
    options.index_batch_size = 1e9;
    dorado::HtsReader reader(query.string());
    auto bam_records = RunAlignmentPipeline(reader, ref.string(), options, 10);
    REQUIRE(bam_records.size() == 1);

    bam1_t* rec = bam_records[0].get();

    // Check aux tags.
    uint32_t l_aux = bam_get_l_aux(rec);
    std::string aux((char*)bam_get_aux(rec), (char*)(bam_get_aux(rec) + l_aux));
    std::string tags[] = {"RGZ", "MMZ", "MLB"};
    for (auto tag : tags) {
        CHECK_THAT(aux, Contains(tag));
    }
}

TEST_CASE("AlignerTest: Verify impact of updated aligner args", TEST_GROUP) {
    fs::path aligner_test_dir = fs::path(get_aligner_data_dir());
    auto ref = aligner_test_dir / "target.fq";
    auto query = aligner_test_dir / "query.fa";

    // Run alignment with one set of k/w.
    {
        auto options = dorado::alignment::dflt_options;
        options.kmer_size = options.window_size = 28;
        options.index_batch_size = 1e9;
        dorado::HtsReader reader(query.string());
        auto bam_records = RunAlignmentPipeline(reader, ref.string(), options, 2);
        CHECK(bam_records.size() == 2);  // Generates 2 alignments.
    }

    // Run alignment with another set of k/w.
    {
        auto options = dorado::alignment::dflt_options;
        options.kmer_size = options.window_size = 5;
        options.index_batch_size = 1e9;
        dorado::HtsReader reader(query.string());
        auto bam_records = RunAlignmentPipeline(reader, ref.string(), options, 2);
        CHECK(bam_records.size() == 1);  // Generates 1 alignment.
    }
}

TEST_CASE("AlignerTest: Check AlignerNode crashes if multi index encountered", TEST_GROUP) {
    fs::path aligner_test_dir = fs::path(get_aligner_data_dir());
    auto ref = aligner_test_dir / "long_target.fa";

    auto options = dorado::alignment::dflt_options;
    options.kmer_size = options.window_size = 5;
    options.index_batch_size = 1e3;
    auto index_file_access = std::make_shared<dorado::alignment::IndexFileAccess>();
    CHECK_THROWS(dorado::AlignerNode(index_file_access, ref.string(), options, 1));
}

SCENARIO("AlignerNode push SimplexRead", TEST_GROUP) {
    GIVEN("AlgnerNode constructed with populated index file collection") {
        const std::string read_id{"aligner_node_test_simplex"};
        fs::path aligner_test_dir{get_aligner_data_dir()};
        auto ref = aligner_test_dir / "target.fq";

        dorado::AlignmentInfo align_info{};
        align_info.minimap_options = dorado::alignment::dflt_options;
        align_info.reference_file = ref.string();
        auto index_file_access = std::make_shared<dorado::alignment::IndexFileAccess>();
        CHECK(index_file_access->load_index(align_info.reference_file, align_info.minimap_options,
                                            2) == dorado::alignment::IndexLoadResult::success);
        std::vector<dorado::Message> messages;
        auto pipeline = create_pipeline(messages, index_file_access, 2);

        AND_GIVEN("client with no alignment requirements") {
            const std::string read_id{"aligner_node_test_simplex"};
            auto simplex_read = std::make_unique<dorado::SimplexRead>();
            simplex_read->read_common.read_id = read_id;
            auto client_without_align = std::make_shared<TestClientAccess>(dorado::AlignmentInfo{});
            simplex_read->read_common.client_access = client_without_align;

            WHEN("push simplex read to pipeline") {
                const std::string read_id{"aligner_node_test_simplex"};
                simplex_read->read_common.seq = "ACGTACGTACGTACGT";

                pipeline->push_message(std::move(simplex_read));
                pipeline.reset();

                THEN("Single simplex read is output") {
                    REQUIRE((messages.size() == 1 &&
                             std::holds_alternative<dorado::SimplexReadPtr>(messages[0])));
                }

                THEN("Output simplex read has empty alignment_string") {
                    simplex_read = std::get<dorado::SimplexReadPtr>(std::move(messages[0]));
                    REQUIRE(simplex_read->read_common.alignment_string.empty());
                }
            }
        }

        AND_GIVEN("client requiring alignment") {
            const std::string read_id{"aligner_node_test_simplex"};
            auto simplex_read = std::make_unique<dorado::SimplexRead>();
            simplex_read->read_common.read_id = read_id;
            auto client_requiring_alignment = std::make_shared<TestClientAccess>(align_info);
            simplex_read->read_common.client_access = client_requiring_alignment;

            WHEN("push simplex read with no alignment matches to pipeline") {
                simplex_read->read_common.seq = "ACGTACGTACGTACGT";

                pipeline->push_message(std::move(simplex_read));
                pipeline.reset();

                THEN("Single simplex read is output") {
                    REQUIRE((messages.size() == 1 &&
                             std::holds_alternative<dorado::SimplexReadPtr>(messages[0])));
                }

                THEN("Output simplex read has alignment_string populated") {
                    simplex_read = std::get<dorado::SimplexReadPtr>(std::move(messages[0]));
                    REQUIRE_FALSE(simplex_read->read_common.alignment_string.empty());
                }

                THEN("Output simplex read has alignment_string containing unmapped sam line") {
                    simplex_read = std::get<dorado::SimplexReadPtr>(std::move(messages[0]));
                    const std::string expected{read_id +
                                               dorado::alignment::UNMAPPED_SAM_LINE_STRIPPED};
                    REQUIRE(simplex_read->read_common.alignment_string == expected);
                }
            }

            WHEN("push simplex read with alignment matches to pipeline") {
                dorado::HtsReader reader(ref.string());
                reader.read();
                auto sequence = dorado::utils::extract_sequence(reader.record.get());
                simplex_read->read_common.seq = sequence;

                pipeline->push_message(std::move(simplex_read));
                pipeline.reset();

                simplex_read = std::get<dorado::SimplexReadPtr>(std::move(messages[0]));
                THEN("Output sam line has read_id as QNAME") {
                    const std::string expected{read_id +
                                               dorado::alignment::UNMAPPED_SAM_LINE_STRIPPED};
                    REQUIRE(simplex_read->read_common.alignment_string.substr(0, read_id.size()) ==
                            read_id);
                }

                THEN("Output sam line contains sequence string") {
                    REQUIRE_FALSE(simplex_read->read_common.alignment_string.find(sequence) ==
                                  std::string::npos);
                }
            }
        }
    }
}
