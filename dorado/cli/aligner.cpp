#include "Version.h"
#include "minimap.h"
#include "utils/bam_utils.h"
#include "utils/cli_utils.h"
#include "utils/log_utils.h"

#include <argparse.hpp>
#include <spdlog/spdlog.h>

#include <chrono>
#include <string>
#include <thread>
#include <vector>
using namespace std::chrono_literals;

#ifndef _WIN32
#include <unistd.h>
#endif

namespace dorado {

int aligner(int argc, char* argv[]) {
    utils::InitLogging();

    argparse::ArgumentParser parser("dorado", DORADO_VERSION, argparse::default_arguments::help);
    parser.add_description(
            "Alignment using minimap2. The outputs are expected to be equivalent to minimap2.\n"
            "The default parameters use the map-ont preset.\n"
            "NOTE: Not all arguments from minimap2 are currently available. Additionally, "
            "parameter names are not finalized and may change.");
    parser.add_argument("index").help("reference in (fastq/fasta/mmi).");
    parser.add_argument("reads").help("any HTS format.").nargs(argparse::nargs_pattern::any);
    parser.add_argument("-at", "--aligner-threads")
            .help("number of threads for alignment.")
            .default_value(0)
            .scan<'i', int>();
    parser.add_argument("-wt", "--writer-threads")
            .help("number of threads for BAM generation.")
            .default_value(0)
            .scan<'i', int>();
    parser.add_argument("-n", "--max-reads")
            .help("maxium number of reads to process (for debugging).")
            .default_value(1000000)
            .scan<'i', int>();
    parser.add_argument("-k").help("k-mer size (maximum 28).").default_value(15).scan<'i', int>();
    parser.add_argument("-w").help("minimizer window size.").default_value(10).scan<'i', int>();
    parser.add_argument("-v", "--verbose").default_value(false).implicit_value(true);

    try {
        parser.parse_args(argc, argv);
    } catch (const std::exception& e) {
        std::ostringstream parser_stream;
        parser_stream << parser;
        spdlog::error("{}\n{}", e.what(), parser_stream.str());
        std::exit(1);
    }

    if (parser.get<bool>("--verbose")) {
        mm_verbose = 3;
        spdlog::set_level(spdlog::level::debug);
    }

    auto index(parser.get<std::string>("index"));
    auto reads(parser.get<std::vector<std::string>>("reads"));
    auto aligner_threads(parser.get<int>("aligner-threads"));
    auto writer_threads(parser.get<int>("writer-threads"));
    auto max_reads(parser.get<int>("max-reads"));
    auto kmer_size(parser.get<int>("k"));
    auto window_size(parser.get<int>("w"));

    int available_threads = std::thread::hardware_concurrency();
    // Heuristically use 25% of threads for alignment and rest for BAM generation.
    // BAM generation is surprisingly expensive...
    std::tie(aligner_threads, writer_threads) = utils::aligner_writer_thread_allocation(
            aligner_threads, writer_threads, available_threads, 0.25f);
    spdlog::debug("> aligner threads {}, writer threads {}", aligner_threads, writer_threads);

    if (reads.size() == 0) {
#ifndef _WIN32
        if (isatty(fileno(stdin))) {
            std::cout << parser << std::endl;
            return 1;
        }
#endif
        reads.push_back("-");
    } else if (reads.size() > 1) {
        spdlog::error("> multi file input not yet handled");
        return 1;
    }

    spdlog::info("> loading index {}", index);

    utils::BamWriter writer("-", writer_threads);
    utils::Aligner aligner(writer, index, kmer_size, window_size, aligner_threads);
    utils::BamReader reader(reads[0]);

    spdlog::debug("> input fmt: {} aligned: {}", reader.format, reader.is_aligned);
    writer.write_header(reader.header, aligner.sq());

    spdlog::info("> starting alignment");
    reader.read(aligner, max_reads);
    writer.join();

    spdlog::info("> finished alignment");
    spdlog::info("> total/primary/unmapped {}/{}/{}", writer.total, writer.primary,
                 writer.unmapped);

    return 0;
}

}  // namespace dorado
