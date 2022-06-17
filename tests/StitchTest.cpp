#include "../dorado/utils/stitch.h"

#include "../dorado/read_pipeline/ReadPipeline.h"

#include <catch2/catch.hpp>

#define TEST_GROUP "[utils]"

// clang-format off
const std::vector<std::string> SEQS(7, "ACGT");
const std::vector<std::string> QSTR(7, "!&.-");
const std::vector<std::vector<uint8_t>> MOVES{
        {1, 0, 0, 1, 0, 0, 1, 0, 1, 0}, 
        {1, 0, 0, 1, 0, 0, 0, 1, 0, 1},
        {1, 0, 0, 1, 0, 1, 1, 0, 0, 0},
        {1, 0, 0, 1, 0, 0, 1, 0, 1, 0},
        {0, 1, 0, 1, 0, 0, 1, 0, 1, 0},
        {1, 0, 0, 0, 0, 0, 1, 0, 1, 1},
        {1, 0, 0, 1, 0, 0, 1, 0, 1, 0}};
/*
A        C        G     T
1, 0, 0, 1, 0, 0, 1, 0, 1, 0
                     A        C           G     T
                     1, 0, 0, 1, 0, 0, 0, 1, 0, 1
                                          A        C     G  T
                                          1, 0, 0, 1, 0, 1, 1, 0, 0, 0
                                                               A        C        G     T
                                                               1, 0, 0, 1, 0, 0, 1, 0, 1, 0
                                                                                       A     C        G     T
                                                                                    0, 1, 0, 1, 0, 0, 1, 0, 1, 0
                                                                                                         A                 C     G  T
                                                                                                         1, 0, 0, 0, 0, 0, 1, 0, 1, 1
                                                                                                                        A        C        G     T
                                                                                                                        1, 0, 0, 1, 0, 0, 1, 0, 1, 0
=
A        C        G     T     C           G        C     G  T           C        G     T     C        G     T              C     C        G     T
1, 0, 0, 1, 0, 0, 1, 0, 1, 0, 1, 0, 0, 0, 1, 0, 0, 1, 0, 1, 1, 0, 0, 0, 1, 0, 0, 1, 0, 1, 0, 1, 0, 0, 1, 0, 1, 0, 0, 0, 0, 1, 0, 1, 0, 0, 1, 0, 1, 0 
*/
// clang-format on
TEST_CASE("Test stitch_chunks", TEST_GROUP) {
    constexpr size_t CHUNK_SIZE = 10;
    constexpr size_t OVERLAP = 3;

    auto read = std::make_shared<Read>();
    read->num_chunks = 0;

    std::vector<float> floatTmp(50, 0.f);
    auto options = torch::TensorOptions().dtype(torch::kFloat32);
    read->raw_data = torch::from_blob(floatTmp.data(), floatTmp.size(), options);
    size_t raw_size = read->raw_data.size(0);
    size_t offset = 0;
    size_t chunk_in_read_idx = 0;
    size_t signal_chunk_step = CHUNK_SIZE - OVERLAP;
    auto chunk = std::make_shared<Chunk>(read, offset, chunk_in_read_idx++, CHUNK_SIZE);
    chunk->qstring = QSTR[read->num_chunks];
    chunk->seq = SEQS[read->num_chunks];
    chunk->moves = MOVES[read->num_chunks];
    read->called_chunks.push_back(chunk);
    read->num_chunks++;
    while (offset + CHUNK_SIZE < raw_size) {
        offset = std::min(offset + signal_chunk_step, raw_size - CHUNK_SIZE);
        chunk = std::make_shared<Chunk>(read, offset, chunk_in_read_idx++, CHUNK_SIZE);
        chunk->qstring = QSTR[read->num_chunks];
        chunk->seq = SEQS[read->num_chunks];
        chunk->moves = MOVES[read->num_chunks];
        read->called_chunks.push_back(chunk);
        read->num_chunks++;
    }

    REQUIRE_NOTHROW(stitch_chunks(read));

    const std::string expected_sequence = "ACGTCGCGTCGTCGTCCGT";
    const std::string expected_qstring = "!&.-&.&.-&.-&.-&&.-";
    const std::vector<uint8_t> expected_moves = {1, 0, 0, 1, 0, 0, 1, 0, 1, 0, 1, 0, 0, 0, 1, 0, 0,
                                                 1, 0, 1, 1, 0, 0, 0, 1, 0, 0, 1, 0, 1, 0, 1, 0, 0,
                                                 1, 0, 1, 0, 0, 0, 0, 1, 0, 1, 0, 0, 1, 0, 1, 0};

    REQUIRE(read->seq == expected_sequence);
    REQUIRE(read->qstring == expected_qstring);
    REQUIRE(read->moves == expected_moves);
}