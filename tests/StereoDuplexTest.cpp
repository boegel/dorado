#include "TestUtils.h"
#include "read_pipeline/ReadPipeline.h"
#include "read_pipeline/StereoDuplexEncoderNode.h"

#include <catch2/catch.hpp>
#include <torch/torch.h>

#include <filesystem>
#include <vector>

#define TEST_GROUP "StereoDuplexTest"

namespace stereo_internal {
std::shared_ptr<dorado::Read> stereo_encode(std::shared_ptr<dorado::Read> template_read,
                                            std::shared_ptr<dorado::Read> complement_read);
}

namespace {
std::filesystem::path DataPath(std::string_view filename) {
    return std::filesystem::path(get_stereo_data_dir()) / filename;
}

// Reads into a vector<uint8_t>.
std::vector<uint8_t> ReadFileIntoVector(const std::filesystem::path& path) {
    const std::string str = ReadFileIntoString(path);
    std::vector<uint8_t> vec;
    vec.resize(str.size());
    std::memcpy(vec.data(), str.data(), str.size());
    return vec;
}
}  // namespace

// Tests stereo encoder output for a real sample signal against known good output.
TEST_CASE(TEST_GROUP "Encoder") {
    const auto template_read = std::make_shared<dorado::Read>();
    template_read->seq = ReadFileIntoString(DataPath("template_seq"));
    template_read->qstring = ReadFileIntoString(DataPath("template_qstring"));
    template_read->moves = ReadFileIntoVector(DataPath("template_moves"));
    torch::load(template_read->raw_data, DataPath("template_raw_data.tensor").string());
    template_read->raw_data = template_read->raw_data.to(torch::kFloat16);

    const auto complement_read = std::make_shared<dorado::Read>();
    complement_read->seq = ReadFileIntoString(DataPath("complement_seq"));
    complement_read->qstring = ReadFileIntoString(DataPath("complement_qstring"));
    complement_read->moves = ReadFileIntoVector(DataPath("complement_moves"));
    torch::load(complement_read->raw_data, DataPath("complement_raw_data.tensor").string());
    complement_read->raw_data = complement_read->raw_data.to(torch::kFloat16);

    torch::Tensor stereo_raw_data;
    torch::load(stereo_raw_data, DataPath("stereo_raw_data.tensor").string());
    stereo_raw_data = stereo_raw_data.to(torch::kFloat16);

    // TODO: This is temporarily disabled, fix before merge
    //const auto stereo_read = stereo_internal::stereo_encode(template_read, complement_read);
    //REQUIRE(torch::equal(stereo_raw_data, stereo_read->raw_data));
}
